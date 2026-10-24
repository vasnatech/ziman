/* POSIX 2008 is exactly what this file needs: `newlocale`, `freelocale`, `nl_langinfo_l`,
 * `strcoll_l` and `towupper_l` are all in it, so the file asks for that rather than for
 * `_GNU_SOURCE` -- which the collected build passes but `PLAIN_CFLAGS` (the `nogc` build) does
 * not, so relying on it compiled one configuration and not the other. */
#define _POSIX_C_SOURCE 200809L
#include "native_locale.h"

#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "value.h"
#include "xalloc.h"

/* ---- what a `Locale` is -----------------------------------------------------------------------
 *
 * A POSIX `locale_t`, opened by name, plus the name it actually resolved to. The data comes from
 * the operating system rather than from a table in this repository: measured on 2026-10-23,
 * glibc answers correct month and day names, decimal and grouping separators, per-locale case
 * mapping (including the Turkish dotted/dotless `i`) and real collation (German sorts `ärger`
 * beside `apple`, Swedish after `Zulu`) for every locale the host has generated. See
 * docs/ROADMAP.md's Locale entry for the numbers and for what this cannot do -- chiefly
 * one-to-many case mapping, since `towupper_l` maps one character to one character and so cannot
 * turn `ß` into `SS`.
 *
 * **Every function used here takes an explicit `locale_t`.** `nl_langinfo_l`, `towupper_l` and
 * `strcoll_l` are thread-safe that way; `setlocale` and `localeconv` are not, and are deliberately
 * never called. Two threads holding two different locales was measured working, 20,000 reads each.
 *
 * The handle is `ziman_xmalloc`ed, which under the collector is `GC_MALLOC`, so the `int` a Ziman
 * `Locale` carries is the address and holding one is what keeps it alive -- the same arrangement
 * `native_regex.c` documents. Unlike a compiled regex, a `locale_t` is libc's to free, so the
 * finalizer calls `freelocale` as well as forgetting the handle. */
typedef struct {
    locale_t loc;
    const char *name; /* the resolved POSIX name, NUL-terminated, owned here */
} LocaleHandle;

static HandleSet g_locales = HANDLE_SET_INIT;

#ifdef ZIMAN_GC
static void locale_drop_finalizer(void *obj, void *client_data) {
    (void)client_data;
    LocaleHandle *h = (LocaleHandle *)obj;
    if (handleset_remove(&g_locales, obj) && h->loc != (locale_t)0) {
        freelocale(h->loc);
        h->loc = (locale_t)0;
    }
}
#endif

static void locale_register_drop(LocaleHandle *h) {
#ifdef ZIMAN_GC
    GC_REGISTER_FINALIZER_NO_ORDER(h, locale_drop_finalizer, NULL, NULL, NULL);
#else
    (void)h;
#endif
}

/* **The codeset has to be UTF-8 or ASCII, and that is not a detail.** `newlocale("tr_TR")`
 * succeeds on this host with codeset ISO-8859-9, and `nl_langinfo_l` would then answer `Şubat` as
 * Latin-5 bytes -- which is not valid UTF-8, so it would poison a Ziman string the moment anything
 * counted its characters. Rejecting a non-Unicode codeset here is what makes every name this
 * module hands back a well-formed `string` by construction. */
static int codeset_is_unicode(locale_t loc) {
    const char *cs = nl_langinfo_l(CODESET, loc);
    if (cs == NULL) return 0;
    return strcmp(cs, "UTF-8") == 0 || strcmp(cs, "utf8") == 0 || strcmp(cs, "UTF8") == 0 ||
           strcmp(cs, "ANSI_X3.4-1968") == 0 || strcmp(cs, "US-ASCII") == 0 ||
           strcmp(cs, "ASCII") == 0;
}

static locale_t open_exact(const char *name) {
    locale_t loc = newlocale(LC_ALL_MASK, name, (locale_t)0);
    if (loc == (locale_t)0) return (locale_t)0;
    if (!codeset_is_unicode(loc)) {
        freelocale(loc);
        return (locale_t)0;
    }
    return loc;
}

/* `name` with `.UTF-8` tried first, because a bare `tr_TR` resolves to Latin-5 (see above). A name
 * that already carries a codeset is taken as written -- a caller who asked for one gets what they
 * asked for or the fallback, not a silent substitution. */
static locale_t open_with_codeset(const char *name, char **resolved) {
    if (strchr(name, '.') != NULL) {
        locale_t loc = open_exact(name);
        if (loc != (locale_t)0) *resolved = ziman_xstrdup(name);
        return loc;
    }
    static const char *suffixes[] = {".UTF-8", ".utf8", ""};
    for (unsigned i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        size_t n = strlen(name) + strlen(suffixes[i]) + 1;
        char *candidate = ziman_xmalloc(n);
        snprintf(candidate, n, "%s%s", name, suffixes[i]);
        locale_t loc = open_exact(candidate);
        if (loc != (locale_t)0) {
            *resolved = candidate;
            return loc;
        }
    }
    return (locale_t)0;
}

static LocaleHandle *root_handle(int line, int col); /* defined below; `of`'s fallback needs it */

static Value handle_value(LocaleHandle *h) {
    return value_int((int64_t)(intptr_t)h);
}

static LocaleHandle *make_handle(locale_t loc, const char *name) {
    LocaleHandle *h = ziman_xmalloc(sizeof(LocaleHandle));
    h->loc = loc;
    h->name = name;
    handleset_add(&g_locales, h);
    locale_register_drop(h);
    return h;
}

/* `locale_open_raw(name) -> int`: a handle, always. A name the host cannot provide falls back to
 * the root locale rather than failing, which is the contract `locale.of` states -- and
 * `locale_name_raw` is how a caller sees which one they got, since there is no Err to inspect.
 *
 * The fallback is root and deliberately *not* the runtime default or the host's own locale: those
 * would make the same call answer differently on two machines, or leak a server's configuration
 * into a user's output. */
static Value native_locale_open_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *name = value_cstr(args[0]);
    char *resolved = NULL;
    locale_t loc = open_with_codeset(name, &resolved);
    if (loc == (locale_t)0) {
        /* The shared root, not a private copy of it: a program that resolves many unavailable names
         * should not accumulate one `locale_t` per miss. */
        return handle_value(root_handle(line, col));
    }
    return handle_value(make_handle(loc, resolved));
}

/* Root, opened once and kept forever. `locale.root()` is what protocol and identifier code passes,
 * which puts it in loops that fold one character at a time -- yaml's hex digits, an HTTP header
 * prefix -- so it must not open a `locale_t` per call. One handle, shared: it is never freed, never
 * collected, and nothing about a locale is mutable, so sharing it is safe.
 *
 * The double-checked store is an acquire/release pair rather than a mutex for the same reason the
 * default below is: read constantly, written once. Two threads racing to be first both open a
 * locale and one loses, which leaks a single `locale_t` for the life of the process -- cheaper than
 * a lock on the read path, and bounded at one. */
static LocaleHandle *g_root = NULL;

static LocaleHandle *root_handle(int line, int col) {
    LocaleHandle *h = __atomic_load_n(&g_root, __ATOMIC_ACQUIRE);
    if (h != NULL) return h;
    locale_t loc = open_exact("C.UTF-8");
    const char *name = "C.UTF-8";
    if (loc == (locale_t)0) {
        loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
        name = "C";
    }
    if (loc == (locale_t)0) {
        ziman_error("runtime", line, col, "locale: the host provides no usable locale, not even 'C'");
    }
    LocaleHandle *fresh = make_handle(loc, ziman_xstrdup(name));
    __atomic_store_n(&g_root, fresh, __ATOMIC_RELEASE);
    return fresh;
}

static Value native_locale_root_raw(Value *args, int argc, int line, int col) {
    (void)args;
    (void)argc;
    return handle_value(root_handle(line, col));
}

static LocaleHandle *handle_of(Value v, int line, int col) {
    LocaleHandle *h = (LocaleHandle *)(intptr_t)v.as.i;
    if (!handleset_has(&g_locales, h)) ziman_error("runtime", line, col, "locale: not a valid Locale");
    return h;
}

/* `locale_name_raw(handle) -> string`: the name the open actually resolved to, which is how a
 * caller tells `locale.of("xx")` fell back to root without `of` having to answer a Result. */
static Value native_locale_name_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    return value_string((char *)handle_of(args[0], line, col)->name);
}

/* `locale_env_name_raw() -> string`: what the environment asks for, in POSIX's own precedence, or
 * "" when it says nothing. Read here rather than passed to `newlocale("")` so that `locale.system`
 * can report the name it used -- a `locale_t` cannot be asked what it was opened as. */
static Value native_locale_env_name_raw(Value *args, int argc, int line, int col) {
    (void)args;
    (void)argc;
    (void)line;
    (void)col;
    const char *names[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        const char *v = getenv(names[i]);
        if (v != NULL && v[0] != '\0') return value_string(ziman_xstrdup(v));
    }
    return value_string(ziman_xstrdup(""));
}

/* The process-wide default, which is what a locale-aware function uses when given no locale.
 *
 * One pointer, read far more often than written -- every `to_upper(s)` with no locale reads it --
 * so it is an acquire/release atomic rather than a mutex, the idiom `src/handleslot.c` already
 * uses. `make tsan` covers this. Zero means "not set yet", and `locale.zm` reads that as root. */
static LocaleHandle *g_default = NULL;

static Value native_locale_set_default_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    LocaleHandle *h = handle_of(args[0], line, col);
    __atomic_store_n(&g_default, h, __ATOMIC_RELEASE);
    return value_int(0);
}

/* 0 when nothing has been set, which `locale.zm` turns into root. */
static Value native_locale_default_raw(Value *args, int argc, int line, int col) {
    (void)args;
    (void)argc;
    (void)line;
    (void)col;
    LocaleHandle *h = __atomic_load_n(&g_default, __ATOMIC_ACQUIRE);
    return value_int((int64_t)(intptr_t)h);
}

void native_locale_register(void) {
    native_register("locale::open_raw", native_locale_open_raw);
    native_register("locale::root_raw", native_locale_root_raw);
    native_register("locale::name_raw", native_locale_name_raw);
    native_register("locale::env_name_raw", native_locale_env_name_raw);
    native_register("locale::set_default_raw", native_locale_set_default_raw);
    native_register("locale::default_raw", native_locale_default_raw);
}
