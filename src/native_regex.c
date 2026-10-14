#define _POSIX_C_SOURCE 200809L
#include "native_regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"
#include <pthread.h>

#include "handles.h"
#include "regex.h"
#include "value.h"
#include "xalloc.h"

/* A compiled pattern is immutable, so it is shared freely between threads and
 * never freed (like every other heap object in the default build); the
 * `handle` Ziman holds is the address of this wrapper.
 *
 * Validated by membership in `g_regexes`, not by reading a magic word out of the address. The
 * magic word was the same mistake `src/handles.h` exists to describe: it answers "is this a
 * pattern" by dereferencing the thing whose validity is in question, so a forged handle is a
 * read at an address the caller chose. `regex.Regex { handle: 1, ... }` is ordinary Ziman and
 * segfaulted the process. `handleset_has` answers from a table of addresses we put there.
 *
 * Nothing is ever removed from the set, because a compiled pattern is never freed. That also
 * means a handle cannot go stale, so this is a membership test and not a liveness one -- unlike
 * `native_net.c`'s, which needs `handleset_has_then` to close the window before a concurrent
 * close. Being in the set roots nothing (the slots are unscanned); what keeps a `RegexHandle`
 * alive is the Ziman `Regex` value holding its address, exactly as `src/handles.c` describes. */
typedef struct {
    Regex *re;
} RegexHandle;

static HandleSet g_regexes = HANDLE_SET_INIT;

static RegexHandle *handle_of(Value v, int line, int col) {
    RegexHandle *h = (RegexHandle *)(intptr_t)v.as.i;
    if (!handleset_has(&g_regexes, h)) ziman_error("runtime", line, col, "regex: not a valid compiled pattern");
    return h;
}

/* A small cache of compiled patterns, so compiling the same pattern text again (in a loop, or
 * on every request) costs a lookup instead of a compile. A compiled pattern is immutable, so
 * sharing one handle between callers is safe; the oldest entry is replaced when the cache is
 * full. Only successful compiles are cached. */
enum { CACHE_SIZE = 256 };
typedef struct {
    char *pattern;
    size_t len;
    RegexHandle *handle;
} CacheEntry;
static CacheEntry g_cache[CACHE_SIZE];
static int g_cache_next = 0;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static RegexHandle *cache_lookup(const char *pattern, size_t len) {
    RegexHandle *found = NULL;
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (g_cache[i].handle != NULL && g_cache[i].len == len && memcmp(g_cache[i].pattern, pattern, len) == 0) {
            found = g_cache[i].handle;
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_mu);
    return found;
}

static void cache_store(const char *pattern, size_t len, RegexHandle *h) {
    pthread_mutex_lock(&g_cache_mu);
    CacheEntry *slot = &g_cache[g_cache_next];
    g_cache_next = (g_cache_next + 1) % CACHE_SIZE;
    /* The evicted entry's pattern text, which nothing outside this table ever sees -- cache_lookup
     * only memcmps it -- so this is the one part of an eviction that is safe to release. It was
     * leaked before (docs/reviews/tasks.md A35).
     *
     * The compiled pattern itself is deliberately *not* freed here, and `regex_free` exists without
     * being called from this line. Two reasons, one per build. Under the default build the Regex is
     * collector memory, so an evicted one that no Ziman value still names is reclaimed on its own
     * and freeing it by hand would be the no-op `free` anyway. Under `nogc` freeing it would be a
     * use-after-free: a handle is handed to Ziman as an integer and cache_lookup hands the same one
     * to every caller, so a program can still be holding a pattern whose slot has been reused, and
     * nothing in that build can tell -- which is the same reason nothing else in this interpreter
     * is freed there either (docs/DESIGN.md's "Memory reclamation"). */
    free(slot->pattern);
    slot->pattern = NULL;
    char *copy = ziman_xmalloc(len + 1);
    memcpy(copy, pattern, len);
    copy[len] = '\0';
    slot->pattern = copy;
    slot->len = len;
    slot->handle = h;
    pthread_mutex_unlock(&g_cache_mu);
}

static Value native_regex_compile_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    Type rt = type_result(type_scalar(TY_INT), str);
    const char *pattern = args[0].as.str.ptr;
    size_t plen = args[0].as.str.len;
    RegexHandle *cached = cache_lookup(pattern, plen);
    if (cached != NULL) return value_result(rt, result_obj_new(1, value_int((int64_t)(intptr_t)cached)));
    char err[256];
    Regex *re = regex_compile(pattern, plen, err, sizeof err);
    if (re == NULL) return value_result(rt, result_obj_new(0, value_string(ziman_xstrdup(err))));
    RegexHandle *h = ziman_xmalloc(sizeof(RegexHandle));
    h->re = re;
    handleset_add(&g_regexes, h);
    cache_store(pattern, plen, h);
    return value_result(rt, result_obj_new(1, value_int((int64_t)(intptr_t)h)));
}

static Value native_regex_group_count_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    return value_int(regex_group_count(handle_of(args[0], line, col)->re));
}

static Value int_array(int *vals, int n) {
    int64_t *wide = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int i = 0; i < n; i++) wide[i] = vals[i];
    Value v = value_int_array(wide, (size_t)n);
    free(wide);
    return v;
}

/* search_raw(handle, text, from) -> [int]: the capture offsets of the first match at or after `from` (see regex_search), or [] for none. */
static Value native_regex_search_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    RegexHandle *h = handle_of(args[0], line, col);
    size_t len = args[1].as.str.len;
    int64_t from = args[2].as.i;
    if (from < 0 || (size_t)from > len) {
        ziman_error("runtime", line, col, "regex: start offset %lld is out of range for a text of length %zu", (long long)from, len);
    }
    int n = 2 * (regex_group_count(h->re) + 1);
    int *caps = ziman_xmalloc((size_t)n * sizeof(int));
    Value result;
    int found = regex_search(h->re, args[1].as.str.ptr, len, (size_t)from, caps);
    /* Neither negative return means "no match", and they mean different things: -1 is a failed
     * allocation inside the matcher, whose size regex_compile has already bounded, so it is a real
     * out-of-memory; -2 is the work ceiling, which a pattern reaches by being expensive per input
     * byte rather than by being large (regex.c's MATCH_WORK_FLOOR, docs/reviews/tasks.md A28). */
    if (found == -2) ziman_error("runtime", line, col, "regex: this pattern needs too much work to match a subject of %zu bytes", len);
    if (found < 0) ziman_error("runtime", line, col, "regex: out of memory while matching");
    if (found) result = int_array(caps, n);
    else result = int_array(caps, 0);
    free(caps);
    return result;
}

/* search_all_raw(handle, text) -> [int]: the capture offsets of every non-overlapping match, concatenated. An empty
 * match is followed by a step of one character so the scan always advances. */
static Value native_regex_search_all_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    RegexHandle *h = handle_of(args[0], line, col);
    const char *text = args[1].as.str.ptr;
    size_t len = args[1].as.str.len;
    int n = 2 * (regex_group_count(h->re) + 1);
    int *caps = ziman_xmalloc((size_t)n * sizeof(int));
    int cap = 64, count = 0;
    int *all = ziman_xmalloc((size_t)cap * sizeof(int));
    size_t from = 0;
    /* One budget for the whole scan, not one per match: see regex_search_budgeted. */
    uint64_t spent = 0;
    for (;;) {
        if (from > len) break;
        int found = regex_search_budgeted(h->re, text, len, from, caps, &spent);
        if (found == -2) ziman_error("runtime", line, col, "regex: this pattern needs too much work to match a subject of %zu bytes", len); /* see search_raw */
        if (found < 0) ziman_error("runtime", line, col, "regex: out of memory while matching");
        if (!found) break;
        if (count + n > cap) {
            while (count + n > cap) cap *= 2;
            all = ziman_xrealloc(all, (size_t)cap * sizeof(int));
        }
        memcpy(all + count, caps, (size_t)n * sizeof(int));
        count += n;
        size_t end = (size_t)caps[1];
        if (caps[1] == caps[0]) {
            end++;
            while (end < len && ((unsigned char)text[end] & 0xC0) == 0x80) end++; /* step over a whole UTF-8 character */
        }
        from = end;
    }
    Value result = int_array(all, count);
    free(caps);
    free(all);
    return result;
}

/* group_names_raw(handle) -> [string]: the name of each group 1..n, "" for an unnamed one. */
static Value native_regex_group_names_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    RegexHandle *h = handle_of(args[0], line, col);
    int n = regex_group_count(h->re);
    Value *items = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(Value));
    for (int g = 1; g <= n; g++) {
        const char *name = regex_group_name(h->re, g);
        items[g - 1] = value_string(ziman_xstrdup(name != NULL ? name : ""));
    }
    return value_array(type_array(type_scalar(TY_STRING)), array_obj_new(items, n));
}

void native_regex_register(void) {
    native_register("regex::group_names_raw", native_regex_group_names_raw);
    native_register("regex::compile_raw", native_regex_compile_raw);
    native_register("regex::group_count_raw", native_regex_group_count_raw);
    native_register("regex::search_raw", native_regex_search_raw);
    native_register("regex::search_all_raw", native_regex_search_all_raw);
}
