/* See xalloc.h for why these exist and why they are functions. */
#define _POSIX_C_SOURCE 200809L /* strdup, in the nogc build where zgc.h is not force-included */
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ZIMAN_ALLOC_PROFILE
/* zgc.h's profiling macros, called by name so the caller's own file and line reach the counters
 * rather than this file's. */
void *ziman_alloc_malloc(const char *file, int line, size_t n);
void *ziman_alloc_realloc(const char *file, int line, void *p, size_t n);
char *ziman_alloc_strdup(const char *file, int line, const char *s);
#endif

/* Reported with write(2) and not fprintf: stdio buffers, and the one thing known about the process
 * at this point is that it could not get memory. Exits 1, the same code every other fatal error
 * here uses (see error.c). */
static _Noreturn void die_oom(const char *what, size_t n, const char *file, int line) {
    char msg[256];
    /* `n` of 0 means the size is not known here -- ziman_xcheck_at wraps an allocator that was
     * already called, so it has the result and not the request. */
    int k = n != 0
        ? snprintf(msg, sizeof msg, "ziman: out of memory: %s of %zu bytes failed at %s:%d\n",
                   what, n, file, line)
        : snprintf(msg, sizeof msg, "ziman: out of memory: %s failed at %s:%d\n", what, file, line);
    if (k > 0) {
        size_t len = (size_t)k < sizeof msg ? (size_t)k : sizeof msg - 1;
        ssize_t written = write(2, msg, len);
        (void)written;
    }
    exit(1);
}

/* An array whose own size does not fit a size_t: not an allocation failure but an impossible
 * request, so it says so rather than reporting a wrapped byte count. */
static _Noreturn void die_overflow(size_t count, size_t size, const char *file, int line) {
    char msg[256];
    int k = snprintf(msg, sizeof msg,
                     "ziman: refusing an array of %zu elements of %zu bytes at %s:%d: the size overflows\n",
                     count, size, file, line);
    if (k > 0) {
        size_t len = (size_t)k < sizeof msg ? (size_t)k : sizeof msg - 1;
        ssize_t written = write(2, msg, len);
        (void)written;
    }
    exit(1);
}

#ifdef ZIMAN_GC
/* zgc.h's `calloc`; see its comment there. Defined here because this is where allocation policy
 * lives, and only in the collector builds, since zgc.h is what redirects `calloc` at all. */
void *ziman_gc_calloc_at(const char *file, int line, size_t count, size_t size) {
    if (count != 0 && size > (size_t)-1 / count) die_overflow(count, size, file, line);
    size_t want = count * size;
    if (want == 0) want = 1;
#ifdef ZIMAN_ALLOC_PROFILE
    void *p = ziman_alloc_malloc(file, line, want);
    if (p != NULL) memset(p, 0, want);
#else
    void *p = GC_MALLOC(want);
#endif
    if (p == NULL) die_oom("a zeroed allocation", want, file, line);
    return p;
}
#endif

void *ziman_xcheck_at(void *p, const char *file, int line) {
    if (p == NULL) die_oom("an allocation", 0, file, line);
    return p;
}

void *ziman_xmalloc_at(const char *file, int line, size_t n) {
    size_t want = n != 0 ? n : 1;
#ifdef ZIMAN_ALLOC_PROFILE
    void *p = ziman_alloc_malloc(file, line, want);
#else
    void *p = malloc(want);
#endif
    if (p == NULL) die_oom("an allocation", want, file, line);
    return p;
}

void *ziman_xcalloc_at(const char *file, int line, size_t count, size_t size) {
    /* The overflow check libc's calloc makes and zgc.h's `calloc` macro does not: it is
     * `GC_MALLOC((a) * (b))`, where the product wraps silently (docs/reviews/tasks/ F15). */
    if (count != 0 && size > (size_t)-1 / count) die_overflow(count, size, file, line);
    size_t want = count * size;
    if (want == 0) want = 1;
#ifdef ZIMAN_ALLOC_PROFILE
    void *p = ziman_alloc_malloc(file, line, want);
    if (p != NULL) memset(p, 0, want);
#else
    void *p = calloc(1, want);
#endif
    if (p == NULL) die_oom("a zeroed allocation", want, file, line);
    return p;
}

void *ziman_xrealloc_at(const char *file, int line, void *p, size_t n) {
    size_t want = n != 0 ? n : 1;
#ifdef ZIMAN_ALLOC_PROFILE
    void *fresh = ziman_alloc_realloc(file, line, p, want);
#else
    void *fresh = realloc(p, want);
#endif
    if (fresh == NULL) die_oom("a resize", want, file, line);
    return fresh;
}

char *ziman_xstrdup_at(const char *file, int line, const char *s) {
#ifdef ZIMAN_ALLOC_PROFILE
    char *copy = ziman_alloc_strdup(file, line, s);
#else
    char *copy = strdup(s);
#endif
    if (copy == NULL) die_oom("a string copy", strlen(s) + 1, file, line);
    return copy;
}
