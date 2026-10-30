/* POSIX 2008 is exactly what this file needs: `newlocale`, `freelocale`, `nl_langinfo_l`,
 * `strcoll_l` and `towupper_l` are all in it, so the file asks for that rather than for
 * `_GNU_SOURCE` -- which the collected build passes but `PLAIN_CFLAGS` (the `nogc` build) does
 * not, so relying on it compiled one configuration and not the other. */
#define _POSIX_C_SOURCE 200809L
#include "native_locale.h"

#include <langinfo.h>
#include <wctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "utf8.h"
#include "value.h"
#include "xalloc.h"

/* ---- what a `Locale` is -----------------------------------------------------------------------
 *
 * A POSIX `locale_t`, opened by name, plus the name it actually resolved to. The data comes from
 * the operating system rather than from a table in this repository: measured on 2026-10-23,
 * glibc answers correct month and day names, decimal and grouping separators, per-locale case
 * mapping (including the Turkish dotted/dotless `i`) and real collation (German sorts `ärger`
 * beside `apple`, Swedish after `Zulu`) for every locale the host has generated. See
 * docs/roadmap/'s Locale entry for the numbers and for what this cannot do -- chiefly
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

/* The same, from a Ziman `Locale` rather than from its bare handle: field 0 by position, as
 * `mutable_string_handle_of` reads a builder's. The prelude's locale-aware overloads are handed the
 * struct, the raw natives in this file are handed the int. */
static LocaleHandle *handle_of_struct(Value v, int line, int col) {
    return handle_of(v.as.st->fields[0], line, col);
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

/* ---- case mapping, for src/prelude.c ---------------------------------------------------------
 *
 * **One code point in, one code point out.** `towupper_l` and `towlower_l` take and answer a single
 * wide character, so this is *simple* case mapping and not *full*: German `ß` stays `ß` where the
 * language wants `SS`, the Greek final sigma is not contextual, and `ﬁ` does not become `FI`. That
 * is a real limit of the POSIX interface rather than of this code, stated in locale.zm's header and
 * in docs/roadmap/, and the thing ICU would buy. What it *does* get right is the mapping that
 * actually bites: `towupper_l('i')` is `İ` and `towlower_l('I')` is `ı` under `tr_TR`, measured.
 *
 * A code point that has no mapping comes back unchanged, which is `towupper_l`'s own contract, so
 * every byte of the input is accounted for and the output is always well-formed UTF-8.
 *
 * Invalid UTF-8 is a runtime error naming the byte, the same judgement `chars` and `len` make --
 * reachable only through `append_byte`, since that is the one route left to such a string. */
static locale_t locale_for(const Value *loc, int line, int col) {
    if (loc != NULL) return handle_of_struct(*loc, line, col)->loc;
    LocaleHandle *h = __atomic_load_n(&g_default, __ATOMIC_ACQUIRE);
    if (h != NULL) return h->loc;
    return root_handle(line, col)->loc;
}

int ziman_locale_default_is_root(void) {
    LocaleHandle *d = __atomic_load_n(&g_default, __ATOMIC_ACQUIRE);
    if (d == NULL) return 1; /* unset means root */
    return d == __atomic_load_n(&g_root, __ATOMIC_ACQUIRE);
}

Value ziman_locale_case_map(Value s, const Value *loc, int upper, int line, int col) {
    locale_t L = locale_for(loc, line, col);
    const uint8_t *in = (const uint8_t *)s.as.str.ptr;
    size_t n = s.as.str.len;
    /* A mapped code point can encode longer than the one it replaces (U+00DF is 2 bytes, U+1E9E is
     * 3), so the buffer is sized for the worst case rather than for `n`: four bytes per input code
     * point, and a code point is at least one byte. */
    char *out = zalloc_atomic(n * 4 + 1);
    size_t pos = 0, w = 0;
    while (pos < n) {
        uint32_t cp;
        int adv = ziman_utf8_decode_strict(in, n, pos, &cp);
        if (adv == 0) {
            ziman_error("runtime", line, col, "%s: string is not valid UTF-8 (invalid sequence at byte %zu)",
                        upper ? "to_upper" : "to_lower", pos);
        }
        wint_t mapped = upper ? towupper_l((wint_t)cp, L) : towlower_l((wint_t)cp, L);
        w += (size_t)ziman_utf8_encode((uint32_t)mapped, out + w);
        pos += (size_t)adv;
    }
    out[w] = '\0';
    return value_string_view(out, w, 1);
}

int ziman_locale_equals_fold(Value a, Value b, const Value *loc, int line, int col) {
    locale_t L = locale_for(loc, line, col);
    const uint8_t *x = (const uint8_t *)a.as.str.ptr;
    const uint8_t *y = (const uint8_t *)b.as.str.ptr;
    size_t nx = a.as.str.len, ny = b.as.str.len;
    size_t px = 0, py = 0;
    /* Folded a code point at a time rather than by building two folded strings: equal-length is not
     * a precondition here, because simple case mapping can change a string's byte length. */
    while (px < nx && py < ny) {
        uint32_t cx, cy;
        int ax = ziman_utf8_decode_strict(x, nx, px, &cx);
        int ay = ziman_utf8_decode_strict(y, ny, py, &cy);
        if (ax == 0 || ay == 0) {
            ziman_error("runtime", line, col,
                        "equals_ignore_case: string is not valid UTF-8 (invalid sequence at byte %zu)",
                        ax == 0 ? px : py);
        }
        if (towlower_l((wint_t)cx, L) != towlower_l((wint_t)cy, L)) return 0;
        px += (size_t)ax;
        py += (size_t)ay;
    }
    return px == nx && py == ny;
}

/* ---- month and day names ---------------------------------------------------------------------
 *
 * `nl_langinfo_l(MON_1 + i, L)` and friends, which is where a localised date formatter's names come
 * from: `Ocak`, `Januar`, `1月`, `يناير`, measured across six locales on 2026-10-23.
 *
 * **Trimmed, deliberately.** glibc pads some abbreviated month names for column alignment -- the
 * `ja_JP` set is " 1月", " 2月" with a leading space, which suits `ls -l` and not a formatted date.
 * Trimming is this module altering the host's data, which is why it is said here rather than
 * discovered in output.
 *
 * The pointer `nl_langinfo_l` answers belongs to the locale, so the bytes are copied into a string
 * of our own; the codeset was verified UTF-8 when the locale was opened, which is what makes that
 * copy a well-formed Ziman string. */
static Value langinfo_string(locale_t L, nl_item item) {
    const char *raw = nl_langinfo_l(item, L);
    if (raw == NULL) return value_string(ziman_xstrdup(""));
    size_t a = 0, b = strlen(raw);
    while (a < b && (raw[a] == ' ' || raw[a] == '\t')) a++;
    while (b > a && (raw[b - 1] == ' ' || raw[b - 1] == '\t')) b--;
    char *out = ziman_xmalloc(b - a + 1);
    memcpy(out, raw + a, b - a);
    out[b - a] = '\0';
    return value_string(out);
}

/* `month_name_raw(handle, index, full)`: `index` is 0-11, `full` picks MON over ABMON. */
static Value native_locale_month_name_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    LocaleHandle *h = handle_of(args[0], line, col);
    int64_t i = args[1].as.i;
    if (i < 0 || i > 11) ziman_error("runtime", line, col, "locale: month index %lld is not 0-11", (long long)i);
    nl_item base = args[2].as.b ? MON_1 : ABMON_1;
    return langinfo_string(h->loc, base + (int)i);
}

/* `day_name_raw(handle, index, full)`: `index` is 0-6 with 0 = Sunday, which is POSIX's own order
 * (DAY_1 is Sunday), so the caller does no rotation. */
static Value native_locale_day_name_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    LocaleHandle *h = handle_of(args[0], line, col);
    int64_t i = args[1].as.i;
    if (i < 0 || i > 6) ziman_error("runtime", line, col, "locale: day index %lld is not 0-6", (long long)i);
    nl_item base = args[2].as.b ? DAY_1 : ABDAY_1;
    return langinfo_string(h->loc, base + (int)i);
}

/* ---- collation and number separators ---------------------------------------------------------
 *
 * `strcoll_l`, which is real collation and not code points: measured on 2026-10-23, `de_DE` orders
 * `ärger` beside `apple` and `sv_SE` puts it after `Zulu`. That difference is the whole reason a
 * locale-aware order cannot replace the one `compare<T: Ord>` gives -- a `Map`'s key order must not
 * depend on anyone's language -- so this is a separate comparator, reached as `compare(a, b, l)`
 * and usable with `sort_with`.
 *
 * `strcoll_l` wants NUL-terminated strings, which `value_cstr` gives without copying when the view
 * already is one. A Ziman string cannot hold a 0 byte (`append_byte` refuses one), so nothing is
 * truncated by passing it as a C string. */
int ziman_locale_collate(Value a, Value b, const Value *loc, int line, int col) {
    locale_t L = locale_for(loc, line, col);
    int c = strcoll_l(value_cstr(a), value_cstr(b), L);
    /* Only the sign is meaningful, which is `compare`'s own contract, but normalising keeps a
     * caller from depending on a magnitude libc never promised. */
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static Value native_locale_collate_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    return value_int(ziman_locale_collate(args[0], args[1], &args[2], line, col));
}

/* The decimal point and the digit-grouping separator. `nl_langinfo_l(RADIXCHAR|THOUSEP)` rather
 * than `localeconv`, which has no `_l` form and reads thread state: these two take an explicit
 * locale and so are thread-safe, which is the property this whole file is built on.
 *
 * A locale may group with nothing at all -- `ar_SA` answers an empty THOUSEP -- which is a real
 * answer and not a failure. */
static Value native_locale_radix_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    return langinfo_string(handle_of(args[0], line, col)->loc, RADIXCHAR);
}

static Value native_locale_thousep_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    return langinfo_string(handle_of(args[0], line, col)->loc, THOUSEP);
}

void native_locale_register(void) {
    native_register("locale::collate_raw", native_locale_collate_raw);
    native_register("locale::radix_raw", native_locale_radix_raw);
    native_register("locale::thousep_raw", native_locale_thousep_raw);
    native_register("locale::month_name_raw", native_locale_month_name_raw);
    native_register("locale::day_name_raw", native_locale_day_name_raw);
    native_register("locale::open_raw", native_locale_open_raw);
    native_register("locale::root_raw", native_locale_root_raw);
    native_register("locale::name_raw", native_locale_name_raw);
    native_register("locale::env_name_raw", native_locale_env_name_raw);
    native_register("locale::set_default_raw", native_locale_set_default_raw);
    native_register("locale::default_raw", native_locale_default_raw);
}
