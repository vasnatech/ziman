#define _POSIX_C_SOURCE 200809L
#include "prelude.h"
#include "utf8.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "handles.h"
#include "mapobj.h"
#include "maptable.h"
#include "decimal.h"
#include "error.h"
#include "fmtstr.h"
#include "gc_guard_roots.h"
#include "gc_mark.h"
#include "gc_thread_heap.h"
#include "native.h"
#include "native_locale.h"
#include "value.h"

/* Generated from lib/prelude/<part>.zm by the Makefile's own prelude rule --
 * real .zm source, never a hand-escaped C string literal (see
 * lib/prelude/prelude.zm's own header comment for why these files,
 * uniquely among corelib's modules, are never registered in corelib.c's
 * CORE_MODULES table). */
#include "prelude_prelude_source.h"
#include "prelude_array_source.h"
#include "prelude_channel_source.h"
#include "prelude_iter_source.h"
#include "prelude_map_source.h"
#include "prelude_mutable_source.h"
#include "prelude_option_source.h"
#include "prelude_result_source.h"
#include "prelude_set_source.h"
#include "prelude_string_source.h"
#include "xalloc.h"

/* Each PRELUDE_*_SOURCE is one long string literal, and several still
 * exceed the C99-mandated minimum translation limit of 4095 characters
 * on their own -- harmless on every compiler this project targets.
 * Silenced narrowly here, right where the macros are expanded, the same
 * way corelib.c already does for its own module sources, rather than
 * loosening -Wpedantic project-wide.
 *
 * Order is the order `--api builtins` prints and the order the loader
 * merges: the universal verbs first, then the cross-type questions, then
 * the per-type files alphabetically. Nothing depends on it semantically
 * -- declarations are looked up by name -- so it is chosen to read well.
 * Adding a part means a file in lib/prelude/, its name in the Makefile's
 * PRELUDE_PARTS, and *two* lines here -- the #include above as well as the row
 * below. This said "one line" until 2026-10-22, when adding a part and leaving
 * the #include out stopped the build with `PRELUDE_MATH_SOURCE undeclared`.
 * There are as many of each as there are parts, which is the cheap check: ten
 * #includes, ten rows, ten names in PRELUDE_PARTS. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
static const struct {
    const char *name;
    const char *source;
} PARTS[] = {
    {"lib/prelude/prelude.zm", PRELUDE_PRELUDE_SOURCE},
    {"lib/prelude/array.zm", PRELUDE_ARRAY_SOURCE},
    {"lib/prelude/channel.zm", PRELUDE_CHANNEL_SOURCE},
    {"lib/prelude/iter.zm", PRELUDE_ITER_SOURCE},
    {"lib/prelude/map.zm", PRELUDE_MAP_SOURCE},
    {"lib/prelude/mutable.zm", PRELUDE_MUTABLE_SOURCE},
    {"lib/prelude/option.zm", PRELUDE_OPTION_SOURCE},
    {"lib/prelude/result.zm", PRELUDE_RESULT_SOURCE},
    {"lib/prelude/set.zm", PRELUDE_SET_SOURCE},
    {"lib/prelude/string.zm", PRELUDE_STRING_SOURCE},
};
#pragma GCC diagnostic pop

int prelude_part_count(void) { return (int)(sizeof PARTS / sizeof PARTS[0]); }
const char *prelude_part_name(int i) { return PARTS[i].name; }
const char *prelude_part_source(int i) { return PARTS[i].source; }

/* Both number parsers used to hand the text straight to strtoll/strtod, which accept what a strict parser must not:
 * leading whitespace, a leading `+`, and (strtod) hex floats and `infinity`/`nan(...)`. The text is the whole number or
 * it is an Err: an optional `-` and digits (an int); for a float also a point and an exponent, and exactly `nan`, `inf`
 * and `-inf`, the spellings `str` prints them as. `parse_bigint` and `parse_decimal` have their own parsers and were
 * already this strict. */
static int int_text_ok(const char *s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    return i < n && s[i] >= '0' && s[i] <= '9';
}

static int float_text_ok(const char *s, size_t n) {
    if ((n == 3 && (memcmp(s, "nan", 3) == 0 || memcmp(s, "inf", 3) == 0)) || (n == 4 && memcmp(s, "-inf", 4) == 0)) return 1;
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i >= n || !((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) return 0;
    for (; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) return 0;
    }
    return 1;
}

static Value native_parse_int(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type result_type = type_result(type_scalar(TY_INT), type_scalar(TY_STRING));
    const char *str = value_cstr(args[0]);
    char *endptr;
    errno = 0;
    long long v = strtoll(str, &endptr, 10);
    if (!int_text_ok(str, args[0].as.str.len) || endptr == str || *endptr != '\0' || errno == ERANGE || strlen(str) != args[0].as.str.len) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as int", str);
        return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    return value_result(type_ref(result_type), result_obj_new(1, value_int((int64_t)v)));
}

static Value native_parse_float(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type result_type = type_result(type_scalar(TY_FLOAT), type_scalar(TY_STRING));
    const char *str = value_cstr(args[0]);
    char *endptr;
    errno = 0;
    double v = strtod(str, &endptr);
    /* ERANGE alone is not a parse failure, because strtod reports two different things with it. On
     * *overflow* the result is infinity and the number genuinely has no double: rejected, as before.
     * On *underflow* the result is the correctly-rounded subnormal, which is an ordinary double --
     * and rejecting those meant parse_float could not read back what str had just written.
     * `str(5e-324)` is "5e-324" and parse_float refused it, breaking the round-trip agreement the
     * docs promise; tests/lang/number_limits.out has been printing that very value all along. In
     * a server it turned a legitimate client number into a 500.
     *
     * A value too small for even a subnormal (1e-400) rounds to 0.0 and is accepted: that is the same
     * rounding that makes 0.1 inexact, only more extreme, and it is what strtod, Python, Java and
     * JavaScript all do. Overflow stays rejected, which leaves a deliberate asymmetry worth naming --
     * `parse_float("inf")` is Ok, because that text asks for infinity, while "1e400" is a finite
     * number this type cannot hold and is better refused than silently turned into infinity. */
    int range_error = errno == ERANGE && isinf(v);
    if (!float_text_ok(str, args[0].as.str.len) || endptr == str || *endptr != '\0' || range_error || strlen(str) != args[0].as.str.len) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as float", str);
        return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    return value_result(type_ref(result_type), result_obj_new(1, value_float(v)));
}

static Value native_parse_bigint(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type result_type = type_result(type_scalar(TY_BIGINT), type_scalar(TY_STRING));
    int ok;
    const char *big_text = value_cstr(args[0]);
    BigNum n = bignum_from_string(big_text, &ok);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as bigint", big_text);
        return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    return value_result(type_ref(result_type), result_obj_new(1, value_bigint(n)));
}

static Value native_parse_decimal(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type result_type = type_result(type_scalar(TY_DECIMAL), type_scalar(TY_STRING));
    int ok;
    const char *dec_text = value_cstr(args[0]);
    Decimal d = decimal_from_string(dec_text, &ok);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as decimal", dec_text);
        return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    return value_result(type_ref(result_type), result_obj_new(1, value_decimal(d)));
}


#define SPTR(v) ((v).as.str.ptr)
#define SLEN(v) ((size_t)(v).as.str.len)

#define SASCII(v) (((v).as.str.flags & ZSTR_ASCII) != 0)

/* A slice of `parent` (which must be a string value): `len` bytes starting
 * `start` bytes in. No copy -- the result points into the parent's buffer,
 * and is NUL-terminated only if it runs to the end of a terminated parent.
 *
 * ZSTR_ASCII is inherited when the parent has it, because a slice of text with
 * no high bit set has no high bit set either. That inheritance is what keeps
 * `chars`, `split` and `lines` linear overall: they produce one view per piece,
 * and a rescan per view would make walking a string quadratic again by the back
 * door. A slice of non-ASCII text is scanned, but only over its own bytes --
 * for `char_at` that is one character, and summed over a whole split it is one
 * pass across the parent. */
static Value string_slice(Value parent, size_t start, size_t len) {
    uint32_t flags = 0;
    if ((parent.as.str.flags & ZSTR_TERM) && start + len == SLEN(parent)) flags |= ZSTR_TERM;
    if (SASCII(parent) || ziman_str_is_ascii(SPTR(parent) + start, len)) flags |= ZSTR_ASCII;
    return value_string_flags(SPTR(parent) + start, len, flags);
}

/* Takes the array directly because `concat`'s arm assembles the pieces itself rather than letting
 * the variadic tail be collapsed for it -- the arm having to decide which of the four declarations
 * a call means before anything can be assembled. That was true while `concat` was a builtin
 * (2026-10-18) and stayed true when it became four `#[compiler_resolved]` declarations on
 * 2026-10-21: what the conversion moved was the dispatch, not the decision. */
Value ziman_concat_strings(const ArrayObj *pieces) {
    size_t total = 0;
    for (int i = 0; i < pieces->count; i++) {
        total += SLEN(array_get(pieces, i));
    }
    char *buf = zalloc_atomic(total + 1);
    size_t pos = 0;
    for (int i = 0; i < pieces->count; i++) {
        Value piece = array_get(pieces, i);
        size_t piece_len = SLEN(piece);
        memcpy(buf + pos, SPTR(piece), piece_len);
        pos += piece_len;
    }
    buf[total] = '\0';
    return value_string_view(buf, total, 1);
}

/* The code points in `n` bytes of UTF-8, which is what `len` answers for text as of 2026-10-23.
 *
 * It was `char_count`'s whole body until then, `len` being the byte count. The two swapped places:
 * `len` is the character question now and `byte_len` is the byte one, so this is reached by `len`
 * and `char_count` is gone. `who` names the caller in the error, since both reach it.
 *
 * O(n) where the byte length is a field read -- that is the cost of the change, taken knowingly:
 * `len` on text is a scan now. Invalid UTF-8 is a runtime error naming the byte offset, reachable
 * only by chaining a byte-level cut into this, since `bytes_to_string` rejects it at the boundary. */
static int64_t utf8_code_points(const uint8_t *s, size_t len, const char *who, int line, int col) {
    size_t pos = 0;
    int64_t count = 0;
    while (pos < len) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) {
            ziman_error("runtime", line, col, "%s: string is not valid UTF-8 (invalid sequence at byte %zu)", who, pos);
        }
        pos += (size_t)n;
        count++;
    }
    return count;
}

/* ---- character indexing ------------------------------------------------------------------
 *
 * **The whole read family counts characters as of 2026-10-23**: `substring`, `char_at`,
 * `is_char_at`, `index_of` and `last_index_of` take and answer code-point indices, where they took
 * and answered byte offsets before. `len` had already moved; these had not, which left
 * `substring(s, 0, index_of(s, ",", 0))` -- the core parsing idiom, 102 call sites -- pairing a
 * character count against a byte offset.
 *
 * Moving them together is what keeps that composition correct: both ends are characters now, so
 * the idiom needs no change at all. What stays in bytes is named for it -- `byte_len`, `to_bytes`,
 * and a MutableString's `set`, which writes one byte and says so.
 *
 * **Each of these is two implementations, picked by ZSTR_ASCII.** When the string has no byte with
 * its high bit set, a character index *is* a byte index, so each function below is exactly the
 * arithmetic it was when these took byte offsets -- O(1), no walk, no second pass. The scanning
 * code is the fallback for text that actually has multi-byte characters.
 *
 * That split is not an optimisation bolted on afterwards; it is what makes the unit affordable. A
 * character offset without it is a walk from the start of the string, which turns every one of the
 * library's ~86 index-scanning loops quadratic -- measured at 128 ms for a 16,000-character scan,
 * and a timeout on yaml's 100,000-character nesting case, whose input is pure ASCII.
 *
 * **The cost that remains**: non-ASCII text still walks, so random access into it is O(n) and a
 * scanner over it is quadratic. Accepted knowingly. The known remedy -- a per-thread one-entry
 * cursor memo, which would need no space in a Value -- is deferred until a benchmark asks for it.
 * `byte_len` stays for the loops that genuinely want storage positions. */

/* The byte offset where character `ci` begins. `ci == <char count>` answers the end of the string,
 * which is what an exclusive bound needs; past that is a runtime error naming the unit. */
static size_t utf8_byte_of_char(const uint8_t *s, size_t len, int64_t ci, const char *who, int line, int col) {
    if (ci < 0) {
        ziman_error("runtime", line, col, "%s: character index must not be negative (got %lld)", who, (long long)ci);
    }
    size_t pos = 0;
    int64_t seen = 0;
    while (seen < ci) {
        if (pos >= len) {
            ziman_error("runtime", line, col, "%s: character index %lld is past the end (%lld characters)", who,
                        (long long)ci, (long long)seen);
        }
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) {
            ziman_error("runtime", line, col, "%s: string is not valid UTF-8 (invalid sequence at byte %zu)", who, pos);
        }
        pos += (size_t)n;
        seen++;
    }
    return pos;
}

/* The character index of a byte offset, which must be a character boundary -- every caller here
 * passes one back from a search, which can only match at a boundary for valid UTF-8. */
static int64_t utf8_char_of_byte(const uint8_t *s, size_t len, size_t bo, const char *who, int line, int col) {
    size_t pos = 0;
    int64_t ci = 0;
    while (pos < bo) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) {
            ziman_error("runtime", line, col, "%s: string is not valid UTF-8 (invalid sequence at byte %zu)", who, pos);
        }
        pos += (size_t)n;
        ci++;
    }
    return ci;
}

/* How many bytes the character at `bo` occupies. */
static int utf8_char_bytes(const uint8_t *s, size_t len, size_t bo, const char *who, int line, int col) {
    uint32_t cp;
    int n = ziman_utf8_decode_strict(s, len, bo, &cp);
    if (n == 0) {
        ziman_error("runtime", line, col, "%s: string is not valid UTF-8 (invalid sequence at byte %zu)", who, bo);
    }
    return n;
}

static Value native_substring(Value *args, int argc, int line, int col) {
    (void)argc;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t blen = SLEN(args[0]);
    int64_t st = args[1].as.i, en = args[2].as.i;
    if (SASCII(args[0])) {
        if (st < 0 || en > (int64_t)blen || st > en) {
            ziman_error("runtime", line, col,
                        "substring index out of bounds (start=%lld, end=%lld, length=%lld characters)",
                        (long long)st, (long long)en, (long long)blen);
        }
        return string_slice(args[0], (size_t)st, (size_t)(en - st));
    }
    int64_t chars = utf8_code_points(s, blen, "substring", line, col);
    if (st < 0 || en > chars || st > en) {
        ziman_error("runtime", line, col,
                    "substring index out of bounds (start=%lld, end=%lld, length=%lld characters)",
                    (long long)st, (long long)en, (long long)chars);
    }
    size_t bst = utf8_byte_of_char(s, blen, st, "substring", line, col);
    size_t ben = utf8_byte_of_char(s, blen, en, "substring", line, col);
    return string_slice(args[0], bst, ben - bst);
}

static Value native_char_at(Value *args, int argc, int line, int col) {
    (void)argc;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t blen = SLEN(args[0]);
    int64_t i = args[1].as.i;
    if (SASCII(args[0])) {
        if (i < 0 || i >= (int64_t)blen) {
            ziman_error("runtime", line, col, "char_at index out of bounds (index=%lld, length=%lld characters)",
                        (long long)i, (long long)blen);
        }
        return string_slice(args[0], (size_t)i, 1);
    }
    int64_t chars = utf8_code_points(s, blen, "char_at", line, col);
    if (i < 0 || i >= chars) {
        ziman_error("runtime", line, col, "char_at index out of bounds (index=%lld, length=%lld characters)",
                    (long long)i, (long long)chars);
    }
    size_t bo = utf8_byte_of_char(s, blen, i, "char_at", line, col);
    return string_slice(args[0], bo, (size_t)utf8_char_bytes(s, blen, bo, "char_at", line, col));
}

static Value native_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t blen = SLEN(args[0]);
    int64_t from = args[2].as.i;
    if (SASCII(args[0])) {
        if (from < 0 || from > (int64_t)blen) {
            ziman_error("runtime", line, col, "index_of start out of bounds (from=%lld, length=%lld characters)",
                        (long long)from, (long long)blen);
        }
        /* The byte offset value_str_find answers *is* the character index here, so unlike the
         * walking path below this needs no conversion pass over the match. */
        return value_int(value_str_find(SPTR(args[0]), blen, SPTR(args[1]), SLEN(args[1]), (size_t)from));
    }
    int64_t chars = utf8_code_points(s, blen, "index_of", line, col);
    if (from < 0 || from > chars) {
        ziman_error("runtime", line, col, "index_of start out of bounds (from=%lld, length=%lld characters)",
                    (long long)from, (long long)chars);
    }
    size_t bfrom = utf8_byte_of_char(s, blen, from, "index_of", line, col);
    int64_t at = value_str_find(SPTR(args[0]), blen, SPTR(args[1]), SLEN(args[1]), bfrom);
    if (at < 0) return value_int(-1);
    return value_int(utf8_char_of_byte(s, blen, (size_t)at, "index_of", line, col));
}

static Value native_last_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    size_t ls = SLEN(args[0]), ln = SLEN(args[1]);
    if (ln > ls) return value_int(-1);
    for (size_t i = ls - ln + 1; i-- > 0; ) {
        if (memcmp(SPTR(args[0]) + i, SPTR(args[1]), ln) == 0) {
            if (SASCII(args[0])) return value_int((int64_t)i);
            return value_int(utf8_char_of_byte((const uint8_t *)SPTR(args[0]), ls, i, "last_index_of", line, col));
        }
    }
    return value_int(-1);
}

static Value native_is_char_at(Value *args, int argc, int line, int col) {
    (void)argc;
    /* No error path at all, which is the contract: out of range is false, and so is invalid UTF-8
       rather than the runtime error its siblings raise. That is what lets it replace a guarded
       `substring` without a bounds check in front of it. */
    (void)line;
    (void)col;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t blen = SLEN(args[0]);
    int64_t i = args[1].as.i;
    size_t lc = SLEN(args[2]);
    /* False, never an error, for an index out of range -- the property that lets this replace a
     * guarded `substring`. Kept across the move to character indices, so the walk has to stop at
     * the end rather than raise there. */
    if (i < 0) return value_bool(0);
    if (SASCII(args[0])) {
        if (i > (int64_t)blen || blen - (size_t)i < lc) return value_bool(0);
        return value_bool(memcmp((const char *)s + i, SPTR(args[2]), lc) == 0);
    }
    size_t pos = 0;
    int64_t seen = 0;
    while (seen < i && pos < blen) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, blen, pos, &cp);
        if (n == 0) return value_bool(0);
        pos += (size_t)n;
        seen++;
    }
    if (seen < i || blen - pos < lc) return value_bool(0);
    return value_bool(memcmp((const char *)s + pos, SPTR(args[2]), lc) == 0);
}

static Value native_equals_ignore_case(Value *args, int argc, int line, int col) {
    if (argc >= 3) return value_bool(ziman_locale_equals_fold(args[0], args[1], &args[2], line, col));
    if (!ziman_locale_default_is_root()) {
        return value_bool(ziman_locale_equals_fold(args[0], args[1], NULL, line, col));
    }
    size_t n = SLEN(args[0]);
    if (n != SLEN(args[1])) return value_bool(0);
    const unsigned char *a = (const unsigned char *)SPTR(args[0]);
    const unsigned char *b = (const unsigned char *)SPTR(args[1]);
    for (size_t i = 0; i < n; i++) {
        unsigned char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return value_bool(0);
    }
    return value_bool(1);
}

static Value native_starts_with(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t ls = SLEN(args[0]), lp = SLEN(args[1]);
    return value_bool(lp <= ls && memcmp(SPTR(args[0]), SPTR(args[1]), lp) == 0);
}

static Value native_ends_with(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t ls = SLEN(args[0]), lx = SLEN(args[1]);
    return value_bool(lx <= ls && memcmp(SPTR(args[0]) + ls - lx, SPTR(args[1]), lx) == 0);
}

static Value native_replace(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *str = SPTR(args[0]);
    size_t ls = SLEN(args[0]), lf = SLEN(args[1]), lt = SLEN(args[2]);
    if (lf == 0) {
        ziman_error("runtime", line, col, "replace: the text to replace must not be empty");
    }
    size_t hits = 0;
    for (long at = value_str_find(str, ls, SPTR(args[1]), lf, 0); at >= 0; at = value_str_find(str, ls, SPTR(args[1]), lf, (size_t)at + lf)) hits++;
    size_t total = ls + hits * lt - hits * lf;
    char *buf = zalloc_atomic(total + 1);
    size_t out = 0, cursor = 0;
    for (long at = value_str_find(str, ls, SPTR(args[1]), lf, 0); at >= 0; at = value_str_find(str, ls, SPTR(args[1]), lf, cursor)) {
        memcpy(buf + out, str + cursor, (size_t)at - cursor);
        out += (size_t)at - cursor;
        memcpy(buf + out, SPTR(args[2]), lt);
        out += lt;
        cursor = (size_t)at + lf;
    }
    memcpy(buf + out, str + cursor, ls - cursor);
    out += ls - cursor;
    buf[out] = '\0';
    return value_string_view(buf, out, 1);
}

/* The pieces are slices of the input, not copies. */
static Value native_split(Value *args, int argc, int line, int col) {
    (void)argc;
    size_t ls = SLEN(args[0]), ld = SLEN(args[1]);
    if (ld == 0) {
        ziman_error("runtime", line, col, "split: separator must not be empty");
    }
    Value *items = NULL;
    int count = 0, cap = 0;
    size_t cursor = 0;
    for (;;) {
        long found = value_str_find(SPTR(args[0]), ls, SPTR(args[1]), ld, cursor);
        size_t piece_len = found >= 0 ? (size_t)found - cursor : ls - cursor;
        if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            items = ziman_xrealloc(items, (size_t)cap * sizeof(Value));
        }
        items[count++] = string_slice(args[0], cursor, piece_len);
        if (found < 0) break;
        cursor = (size_t)found + ld;
    }
    return value_array(type_ref(type_array(type_scalar(TY_STRING))), array_obj_new(items, count));
}

static Value native_trim(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = SPTR(args[0]);
    size_t len = SLEN(args[0]);
    size_t start = 0, end = len;
    while (start < end && isspace((unsigned char)str[start])) start++;
    while (end > start && isspace((unsigned char)str[end - 1])) end--;
    return string_slice(args[0], start, end - start);
}

static Value native_is_empty(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_bool(SLEN(args[0]) == 0);
}

static Value native_is_blank(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = SPTR(args[0]);
    size_t len = SLEN(args[0]);
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)str[i])) return value_bool(0);
    }
    return value_bool(1);
}

/* ---- the ASCII character-class predicates -----------------------------------------------------
 *
 * `is_alpha`, `is_numeric`, `is_alpha_numeric`, `is_digit` and `is_char`, each over a `string` and
 * a `MutableString`.
 *
 * **These are deliberately ASCII, and that is a decision rather than a shortcut.** Six private
 * helpers across `lib/` -- in `csv`, `toml`, `yaml`, `regex`, `mysql` and `zone` -- each spelled
 * the same `byte_len(c) == 1 && index_of("0123456789", c, 0) >= 0`, and all six meant ASCII: they
 * are reading CSV cells, TOML keys and zone offsets, where a non-ASCII digit is not a digit but a
 * syntax error. Promoting them with Unicode semantics would have changed what every one of them
 * asked. The Unicode questions have an answer already, and a better one: the regex engine's
 * `\p{L}` and `\p{Nd}` read the general-category tables in `src/regex_unicode.h`, which is where a
 * character-property question with a table behind it belongs.
 *
 * Because the classes are ASCII, **no UTF-8 decoding is needed at all**: any byte >= 0x80 is
 * outside every class, so one byte scan answers each predicate whatever the encoding. That makes
 * these the only members of the string family whose cost does not depend on the ASCII bit.
 *
 * `is_char` is the exception, being about length rather than class: it asks whether the text is
 * exactly one character. That is `len(s) == 1`, which for ASCII is one byte and otherwise is "the
 * first character's own sequence is the whole string" -- no scan past the first byte either way.
 *
 * Empty is false for all five. A predicate over "every character" is vacuously true of no
 * characters, which is `is_empty`'s question and a trap in this family: `is_digit("")` answering
 * true would make `is_digit(c)` unusable as the guard every one of those six helpers used it as. */

static int ascii_is_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int ascii_is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/* Every byte in the class, and at least one byte. `kind` is 0 alpha, 1 digit, 2 either. */
static int all_bytes_in_class(const char *s, size_t len, int kind) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = kind == 0 ? ascii_is_alpha(c) : kind == 1 ? ascii_is_digit(c) : (ascii_is_alpha(c) || ascii_is_digit(c));
        if (!ok) return 0;
    }
    return 1;
}

/* Whether `len` bytes hold exactly one character. A lead byte's own length says how many bytes
 * the first character takes, so this needs no decode and no validation: text that is not valid
 * UTF-8 simply is not one character, which is the honest answer rather than an error. */
static int is_one_character(const char *s, size_t len) {
    if (len == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    size_t want = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    return want != 0 && want == len;
}

static Value native_string_is_alpha(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(all_bytes_in_class(SPTR(args[0]), SLEN(args[0]), 0));
}

static Value native_string_is_numeric(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(all_bytes_in_class(SPTR(args[0]), SLEN(args[0]), 1));
}

static Value native_string_is_alpha_numeric(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(all_bytes_in_class(SPTR(args[0]), SLEN(args[0]), 2));
}

static Value native_string_is_digit(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(SLEN(args[0]) == 1 && ascii_is_digit((unsigned char)SPTR(args[0])[0]));
}

static Value native_string_is_char(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(is_one_character(SPTR(args[0]), SLEN(args[0])));
}

/* ---- case mapping, two arities --------------------------------------------------------------
 *
 * One registration serves `to_upper(s)` and `to_upper(s, locale)`, the way `len`'s nine members
 * share one function: the declarations are separate and the dispatch is on `argc`.
 *
 * **The one-argument form keeps the ASCII path whenever the default is root**, which it is until a
 * program sets otherwise. That is not an optimisation so much as the thing that makes this change
 * safe: root folding *is* ASCII folding, so a program that never touches the default sees byte-for-
 * byte what it saw before -- including the ~50 places in lib/ that fold a header, a hex digit or an
 * environment-variable name and must not depend on anyone's language.
 *
 * `toupper` below is ASCII because the C global locale is never changed: `src/native_locale.c`
 * deliberately never calls `setlocale`, using `towupper_l` with an explicit locale instead. If that
 * ever stopped being true these two loops would quietly start following it. */
static Value native_to_upper(Value *args, int argc, int line, int col) {
    if (argc >= 2) return ziman_locale_case_map(args[0], &args[1], 1, line, col);
    if (!ziman_locale_default_is_root()) return ziman_locale_case_map(args[0], NULL, 1, line, col);
    const char *str = SPTR(args[0]);
    size_t len = SLEN(args[0]);
    char *buf = zalloc_atomic(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string_view(buf, len, 1);
}

static Value native_to_lower(Value *args, int argc, int line, int col) {
    if (argc >= 2) return ziman_locale_case_map(args[0], &args[1], 0, line, col);
    if (!ziman_locale_default_is_root()) return ziman_locale_case_map(args[0], NULL, 0, line, col);
    const char *str = SPTR(args[0]);
    size_t len = SLEN(args[0]);
    char *buf = zalloc_atomic(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string_view(buf, len, 1);
}

/* An Option<string> argument's own payload, or NULL for None -- typecheck
 * already proved `opt` is genuinely an Option (see check_value_against_
 * param_type in typecheck.c), so this only ever has to look at which
 * variant it turned out to be at runtime, the same way unwrap_Some
 * itself would. */
static int option_string_value(Value opt, Value *out) {
    EnumObj *obj = opt.as.en;
    if (strcmp(obj->variant_name, "Some") == 0) {
        *out = obj->payload;
        return 1;
    }
    return 0;
}

/* args[3] is always the collected `[string]` array, regardless of how
 * many pieces the call site wrote out individually vs. passed as one
 * already-built array -- see ast.h's Param comment and interpreter.c's
 * collapse_variadic_args, which is what guarantees this native
 * function never has to know the difference. args[0..2] are
 * prefix/suffix/separator, each already resolved to a concrete
 * Option<string> by the time this runs -- an omitted one (only legal in
 * a named call) already became None, and a bare string became
 * Some(...), before this native function was ever reached (see
 * typecheck.c's canonicalize_named_args and check_value_against_param_type).
 * Still two linear passes overall (sum the lengths, then copy), one
 * allocation -- the whole point of this function existing natively: a
 * pure-Ziman version accumulating via repeated `concat` would be the
 * same O(N^2) pattern this was written to replace (see docs/DESIGN.md's
 * "join" entry). */
static Value native_join(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value pv, sv, dv;
    memset(&pv, 0, sizeof pv);
    memset(&sv, 0, sizeof sv);
    memset(&dv, 0, sizeof dv);
    int has_prefix = option_string_value(args[0], &pv);
    int has_suffix = option_string_value(args[1], &sv);
    int has_sep = option_string_value(args[2], &dv);
    ArrayObj *pieces = args[3].as.arr;

    size_t prefix_len = has_prefix ? SLEN(pv) : 0;
    size_t suffix_len = has_suffix ? SLEN(sv) : 0;
    size_t separator_len = has_sep ? SLEN(dv) : 0;

    size_t total = prefix_len + suffix_len;
    for (int i = 0; i < pieces->count; i++) {
        total += SLEN(array_get(pieces, i));
    }
    if (pieces->count > 1) {
        total += separator_len * (size_t)(pieces->count - 1);
    }

    char *buf = zalloc_atomic(total + 1);
    size_t pos = 0;
    if (has_prefix) {
        memcpy(buf + pos, SPTR(pv), prefix_len);
        pos += prefix_len;
    }
    for (int i = 0; i < pieces->count; i++) {
        if (i > 0 && has_sep) {
            memcpy(buf + pos, SPTR(dv), separator_len);
            pos += separator_len;
        }
        Value piece = array_get(pieces, i);
        size_t piece_len = SLEN(piece);
        memcpy(buf + pos, SPTR(piece), piece_len);
        pos += piece_len;
    }
    if (has_suffix) {
        memcpy(buf + pos, SPTR(sv), suffix_len);
        pos += suffix_len;
    }
    buf[total] = '\0';
    return value_string_view(buf, total, 1);
}

/* --- MutableString -------------------------------------------------
 * A mutable string-accumulation handle, backed entirely here, native-
 * side -- Ziman code never sees anything but an opaque `handle: int`
 * (see lib/prelude/string.zm's MutableString struct). The handle IS the address of
 * an individually heap-allocated MutableStringBuf, not an index into a
 * global table: a table would keep every builder ever made reachable
 * forever (each json_to_string node makes one), which is exactly the kind
 * of leak a collector cannot fix. As a plain int in a Value the address is
 * still visible to the conservative collector, so the buffer lives exactly
 * as long as the MutableString value that names it. A forged or stale handle is caught by
 * membership in the per-kind `HandleSet` below rather than by a magic word read out of the
 * caller's own address -- see that comment.
 * A builder belongs to the thread that made it (`owner`): every operation
 * checks the calling thread, so sharing one -- by a channel, a closure, a
 * global -- is a clear runtime error and never a data race (the compiler
 * refuses the obvious ways first: see `#[thread_bound]` in lib/prelude/string.zm). */
/* A builder's handle is the address of its buffer, and `MutableArray`, `MutableMap`,
 * `MutableSet`, `MutableString` and `Mutable` are all constructible from Ziman as ordinary struct
 * literals over an `int` field. So each of these was validated by reading a magic word out of the
 * address the caller supplied -- the same mistake A40 fixed for `regex`, which `src/handles.h`
 * exists to describe, and `MutableArray<int> { handle: 1 }` followed by a push segfaulted the
 * process. All five did.
 *
 * One `HandleSet` per kind, so membership proves both that the address is ours and which kind it
 * is, which is what the magic word was doing for the second half only. The sets are never removed
 * from: a builder buffer is freed by the collector when nothing names it, and these sets are
 * unscanned so being in one keeps nothing alive (see src/handles.c). A stale address could in
 * principle be reused by a later builder of the same kind, which was equally true of the magic word
 * and is a wrong-object bug rather than memory unsafety -- unlike a forged integer, which this
 * stops outright (A50). */
static HandleSet g_string_builders = HANDLE_SET_INIT;
static HandleSet g_array_builders = HANDLE_SET_INIT;
static HandleSet g_map_builders = HANDLE_SET_INIT;
static HandleSet g_cells = HANDLE_SET_INIT;

#ifdef ZIMAN_GC
/* Takes a dead builder out of its validation set.
 *
 * Without this the four sets above only ever grow: nothing else removes from them, because unlike a
 * socket or a cursor a builder owns no descriptor and so had no reason to be closed. The entry is
 * not a root (see src/handles.c), so the pointer in it dangles the moment the collector takes the
 * object -- the same shape `iter_drop_finalizer` describes as leaving a pointer behind "for the life
 * of the process", except that builders are the handles a program churns through fastest. Measured
 * before this existed: 16,000,000 short-lived `mutable_string_new()` calls left a 268 MB table
 * (one permanent entry each, ~16.8 bytes), and the growth is linear and unbounded.
 *
 * Removal only, and NO_ORDER: there is nothing to close, the set is the only thing that has to
 * forget this object, and no other finalizable object needs to be kept alive to do it. */
static void builder_drop_finalizer(void *obj, void *client_data) {
    handleset_remove((HandleSet *)client_data, obj);
}

static void builder_register_drop(void *buf, HandleSet *set) {
    GC_REGISTER_FINALIZER_NO_ORDER(buf, builder_drop_finalizer, set, NULL, NULL);
}
#else
static void builder_register_drop(void *buf, HandleSet *set) { (void)buf; (void)set; }
#endif

/* The check every builder operation makes: `what` names the builder type for the message. */
static void builder_owner_check(pthread_t owner, const char *what) {
    if (!pthread_equal(owner, pthread_self())) {
        ziman_error("runtime", 0, 0,
                    "%s belongs to the thread that made it and was used from another thread (a builder cannot be shared between threads; hand over the finished value instead)",
                    what);
    }
}

typedef struct {
    pthread_t owner;
    char *data;
    size_t length;
    size_t capacity;
    double grow_factor;
    /* The ZSTR_ASCII of an immutable string, kept here because a builder has no Value to carry it
     * in: no byte in `data` has its high bit set, so a character index is a byte index. Maintained
     * by the five writers (from/new/append/append_byte/set) and reset by clear, never recomputed on
     * read -- which is what makes `len(sb)` a field read and the four readers below O(1) on ASCII
     * text, exactly as their immutable twins are. */
    int ascii;
} MutableStringBuf;

static int64_t mutable_string_table_register(MutableStringBuf *buf) {
    buf->owner = pthread_self();
    handleset_add(&g_string_builders, buf);
    builder_register_drop(buf, &g_string_builders);
    return (int64_t)(intptr_t)buf;
}

static MutableStringBuf *mutable_string_table_get(int64_t handle) {
    MutableStringBuf *buf = (MutableStringBuf *)(intptr_t)handle;
    if (!handleset_has(&g_string_builders, buf)) ziman_error("runtime", 0, 0, "not a valid MutableString handle");
    builder_owner_check(buf->owner, "a MutableString");
    return buf;
}

/* An Option<int>/Option<float> argument's own payload, or `fallback` for
 * None -- deliberately never reads a None's payload field even though
 * eval_enum_access happens to store a harmless `value_int(0)` there for
 * every no-payload variant construction: that placeholder is always an
 * *int*, so blindly reading it as a float here (for grow_factor) would
 * silently reinterpret the wrong union member. Checking variant_name
 * first avoids relying on that unrelated implementation detail at all. */
static int64_t option_int_or(Value opt, int64_t fallback) {
    EnumObj *obj = opt.as.en;
    if (strcmp(obj->variant_name, "Some") == 0) {
        return obj->payload.as.i;
    }
    return fallback;
}

static double option_float_or(Value opt, double fallback) {
    EnumObj *obj = opt.as.en;
    if (strcmp(obj->variant_name, "Some") == 0) {
        return obj->payload.as.f;
    }
    return fallback;
}

static char *STRINGBUILDER_FIELD_NAMES[] = {"handle"};

static int64_t mutable_string_handle_of(Value sb) {
    return sb.as.st->fields[0].as.i;
}

static void mutable_string_ensure_capacity(MutableStringBuf *buf, size_t additional) {
    size_t needed = buf->length + additional;
    if (needed <= buf->capacity) {
        return;
    }
    size_t new_capacity = buf->capacity > 0 ? buf->capacity : 1;
    while (new_capacity < needed) {
        new_capacity = (size_t)((double)new_capacity * buf->grow_factor) + 1;
    }
    buf->data = ziman_xrealloc(buf->data, new_capacity);
    buf->capacity = new_capacity;
}

/* `mutable(s)` over text: a MutableString holding what `s` holds, which is the fourth corner of a
 * table that had three.
 *
 * **Added 2026-10-23**, reversing a decision `mutable_cell_fallback` had recorded: text answered a
 * `Mutable<string>` cell, because "a MutableString is *filled* rather than converted and so is not
 * this conversion". A MutableArray is filled too and `mutable([1, 2])` converts anyway, so the
 * distinction did not hold; what settled it is that the old answer compiled and failed later.
 *
 * Capacity is the content plus the default headroom, so the common `mutable(s)` then append does
 * not reallocate on its first write. `source_is_handle` copies a builder instead -- the shape
 * `$mutable_array_from` has for its world, and unreachable from `mutable` for the same reason it
 * is there: a builder argument answers a cell, so only a `string` is ever routed here. */
Value ziman_mutable_string_from(Value source, int source_is_handle) {
    const char *src;
    size_t len;
    if (source_is_handle) {
        const MutableStringBuf *from = mutable_string_table_get(mutable_string_handle_of(source));
        src = from->data;
        len = from->length;
    } else {
        src = SPTR(source);
        len = SLEN(source);
    }
    MutableStringBuf *buf = ziman_xmalloc(sizeof(MutableStringBuf));
    buf->capacity = len + 20;
    buf->length = 0;
    buf->grow_factor = 2.0;
    buf->data = zalloc_atomic(buf->capacity);
    /* Inherited from a builder source, scanned from a string one -- the copy is byte for byte
     * either way, so the answer is the source's. */
    buf->ascii = source_is_handle ? mutable_string_table_get(mutable_string_handle_of(source))->ascii
                                  : ziman_str_is_ascii(src, len);
    if (len > 0) {
        memcpy(buf->data, src, len);
        buf->length = len;
    }
    int64_t handle = mutable_string_table_register(buf);
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int(handle);
    return value_struct(type_ref(type_struct("MutableString")), struct_obj_new(STRINGBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_mutable_string_new(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    int64_t capacity = option_int_or(args[0], 20);
    double grow_factor = option_float_or(args[1], 2.0);
    if (capacity < 0) {
        capacity = 0;
    }
    if (grow_factor <= 1.0) {
        /* A factor at or below 1.0 would never actually grow the buffer
         * -- mutable_string_ensure_capacity's while loop would spin forever. Falls
         * back to the same default mutable_string_new's own signature declares,
         * rather than erroring, since a MutableString is otherwise
         * perfectly usable with this one parameter simply ignored. */
        grow_factor = 2.0;
    }

    MutableStringBuf *buf = ziman_xmalloc(sizeof(MutableStringBuf));
    buf->capacity = (size_t)capacity;
    buf->length = 0;
    buf->grow_factor = grow_factor;
    buf->ascii = 1; /* vacuously, and it stays true until something non-ASCII is appended */
    buf->data = buf->capacity > 0 ? zalloc_atomic(buf->capacity) : NULL;

    int64_t handle = mutable_string_table_register(buf);

    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int(handle);
    return value_struct(type_ref(type_struct("MutableString")), struct_obj_new(STRINGBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_mutable_string_append(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    ArrayObj *pieces = args[1].as.arr;
    for (int i = 0; i < pieces->count; i++) {
        Value piece = array_get(pieces, i);
        const char *s = SPTR(piece);
        size_t len = SLEN(piece);
        mutable_string_ensure_capacity(buf, len);
        memcpy(buf->data + buf->length, s, len);
        buf->length += len;
        /* One-way: once a builder holds a multi-byte character it holds one until `clear`. The
         * piece's own bit is reused when it has one, so appending strings never rescans them. */
        if (buf->ascii && !(piece.as.str.flags & ZSTR_ASCII)) buf->ascii = 0;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

/* mutable_string_append_byte(sb, bytes...) -> bool: raw bytes, so text can be assembled
 * from decoded bytes without a [byte] in between. A string can't hold a 0
 * byte, so if any of them is 0b nothing at all is appended and the result is
 * false. */
static Value native_mutable_string_append_byte(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    ArrayObj *bytes = args[1].as.arr;
    if (bytes->bytes != NULL) {
        if (memchr(bytes->bytes, 0, (size_t)bytes->count) != NULL) return value_bool(0);
        mutable_string_ensure_capacity(buf, (size_t)bytes->count);
        memcpy(buf->data + buf->length, bytes->bytes, (size_t)bytes->count);
        buf->length += (size_t)bytes->count;
        /* A high bit here may not even be valid UTF-8 -- this is the one route by which a builder
         * can hold text that is not (nothing else accepts raw bytes). Clearing the flag is right
         * either way: it only ever claims "every byte is below 0x80", which is a fact about bytes
         * and needs no opinion about whether they decode. */
        if (buf->ascii && !ziman_str_is_ascii((const char *)bytes->bytes, (size_t)bytes->count)) buf->ascii = 0;
        return value_bool(1);
    }
    for (int i = 0; i < bytes->count; i++) {
        if (array_get(bytes, i).as.by == 0) return value_bool(0);
    }
    mutable_string_ensure_capacity(buf, (size_t)bytes->count);
    for (int i = 0; i < bytes->count; i++) {
        unsigned char b = (unsigned char)array_get(bytes, i).as.by;
        if (b >= 0x80u) buf->ascii = 0; /* same reasoning as the packed branch above */
        buf->data[buf->length++] = (char)b;
    }
    return value_bool(1);
}

/* Reading and writing one byte of the buffer, and searching it -- the four a MutableString was
 * missing, so that a string being built can be inspected without `snapshot`, a full copy per call.
 *
 * **Character-indexed since 2026-10-23**, where all four took byte offsets before. They had to
 * move: `len(sb)` had already become a character count, so `char_at(sb, len(sb) - 1)` was pairing
 * one unit against the other, and `char_at(sb, 1) == char_at(s, 1)` could be false for the same
 * text in the two worlds. A builder is the mutable twin of a string, and a twin that counts
 * differently is not one.
 *
 * Each takes the buffer's own ZSTR_ASCII equivalent (`buf->ascii`) as its fast path, so the common
 * case is the byte arithmetic it always was -- see the character-indexing block above `substring`
 * for why that split is what makes the unit affordable at all. */
static Value native_mutable_string_get(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    int64_t i = args[1].as.i;
    if (buf->ascii) {
        if (i < 0 || (size_t)i >= buf->length) {
            ziman_error("runtime", line, col, "char_at index %lld out of bounds (MutableString length %lld characters)",
                        (long long)i, (long long)buf->length);
        }
        return value_string_copy(&buf->data[i], 1);
    }
    const uint8_t *d = (const uint8_t *)buf->data;
    int64_t chars = utf8_code_points(d, buf->length, "char_at", line, col);
    if (i < 0 || i >= chars) {
        ziman_error("runtime", line, col, "char_at index %lld out of bounds (MutableString length %lld characters)",
                    (long long)i, (long long)chars);
    }
    size_t bo = utf8_byte_of_char(d, buf->length, i, "char_at", line, col);
    return value_string_copy(&buf->data[bo], (size_t)utf8_char_bytes(d, buf->length, bo, "char_at", line, col));
}

/* Replaces one **character**, mirroring the immutable `set(arr, i, x)`: one element in, one element
 * out. A `value` that is not exactly one character is refused, because `set` nowhere else in the
 * language changes a container's length.
 *
 * It does move bytes, though, which it did not when it replaced a byte: one character in UTF-8 is
 * one to four bytes, so writing 'e' over 'é' shortens the buffer and 'é' over 'e' lengthens it.
 * That is a splice of the *bytes* and not of the characters -- the character count is what `len`
 * answers and what `set` leaves alone, so "one element in, one element out" stays true in the unit
 * the function now speaks. Refusing unequal byte lengths instead would make `set` usable on ASCII
 * and not on anything else, which is the kind of quiet asymmetry this whole change is removing. */
static Value native_mutable_string_set(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    int64_t i = args[1].as.i;
    const uint8_t *d = (const uint8_t *)buf->data;
    size_t vlen = args[2].as.str.len;
    /* Exactly one character of replacement, checked before the index so the message is about the
     * value when both are wrong. An ASCII value is one character iff it is one byte. */
    int64_t vchars = (args[2].as.str.flags & ZSTR_ASCII)
                         ? (int64_t)vlen
                         : utf8_code_points((const uint8_t *)args[2].as.str.ptr, vlen, "set", line, col);
    if (vchars != 1) {
        ziman_error("runtime", line, col,
                    "set replaces exactly one character, and was given %lld (append or rebuild to change the length)",
                    (long long)vchars);
    }
    size_t bo, old_bytes;
    if (buf->ascii) {
        if (i < 0 || (size_t)i >= buf->length) {
            ziman_error("runtime", line, col, "set index %lld out of bounds (MutableString length %lld characters)",
                        (long long)i, (long long)buf->length);
        }
        bo = (size_t)i;
        old_bytes = 1;
    } else {
        int64_t chars = utf8_code_points(d, buf->length, "set", line, col);
        if (i < 0 || i >= chars) {
            ziman_error("runtime", line, col, "set index %lld out of bounds (MutableString length %lld characters)",
                        (long long)i, (long long)chars);
        }
        bo = utf8_byte_of_char(d, buf->length, i, "set", line, col);
        old_bytes = (size_t)utf8_char_bytes(d, buf->length, bo, "set", line, col);
    }
    if (vlen > old_bytes) mutable_string_ensure_capacity(buf, vlen - old_bytes);
    size_t tail = buf->length - bo - old_bytes;
    if (vlen != old_bytes) memmove(buf->data + bo + vlen, buf->data + bo + old_bytes, tail);
    memcpy(buf->data + bo, args[2].as.str.ptr, vlen);
    buf->length = buf->length - old_bytes + vlen;
    if (buf->ascii && !(args[2].as.str.flags & ZSTR_ASCII)) buf->ascii = 0;
    return value_int(0); /* void return -- the caller never uses this */
}

/* Takes a `from` because the immutable `index_of(s, needle, from)` does, and validates it the same
 * way: mirroring the twin exactly matters more than saving an argument. `last_index_of` has no
 * `from` for the same reason -- its own twin does not. */
static Value native_mutable_string_is_alpha(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    return value_bool(all_bytes_in_class(buf->data, buf->length, 0));
}

static Value native_mutable_string_is_numeric(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    return value_bool(all_bytes_in_class(buf->data, buf->length, 1));
}

static Value native_mutable_string_is_alpha_numeric(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    return value_bool(all_bytes_in_class(buf->data, buf->length, 2));
}

static Value native_mutable_string_is_digit(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    return value_bool(buf->length == 1 && ascii_is_digit((unsigned char)buf->data[0]));
}

static Value native_mutable_string_is_char(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    return value_bool(is_one_character(buf->data, buf->length));
}

static Value native_mutable_string_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    int64_t from = args[2].as.i;
    if (buf->ascii) {
        if (from < 0 || (size_t)from > buf->length) {
            ziman_error("runtime", line, col, "index_of 'from' %lld is outside 0..%lld (the MutableString's length in characters)",
                        (long long)from, (long long)buf->length);
        }
        return value_int(value_str_find(buf->data, buf->length, args[1].as.str.ptr, args[1].as.str.len, (size_t)from));
    }
    const uint8_t *d = (const uint8_t *)buf->data;
    int64_t chars = utf8_code_points(d, buf->length, "index_of", line, col);
    if (from < 0 || from > chars) {
        ziman_error("runtime", line, col, "index_of 'from' %lld is outside 0..%lld (the MutableString's length in characters)",
                    (long long)from, (long long)chars);
    }
    size_t bfrom = utf8_byte_of_char(d, buf->length, from, "index_of", line, col);
    long at = value_str_find(buf->data, buf->length, args[1].as.str.ptr, args[1].as.str.len, bfrom);
    if (at < 0) return value_int(-1);
    return value_int(utf8_char_of_byte(d, buf->length, (size_t)at, "index_of", line, col));
}

/* The last occurrence: searched forward, keeping the most recent hit, since value_str_find only
 * scans one way. An empty needle is found at the length, which is where `last_index_of` on a
 * string puts it. */
static Value native_mutable_string_last_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    const uint8_t *d = (const uint8_t *)buf->data;
    size_t needle_len = args[1].as.str.len;
    if (needle_len == 0) {
        /* An empty needle is found at the length, which is where `last_index_of` on a string puts
         * it -- in characters now, so the two worlds still agree on the answer. */
        if (buf->ascii) return value_int((int64_t)buf->length);
        return value_int(utf8_code_points(d, buf->length, "last_index_of", line, col));
    }
    int64_t best = -1;
    size_t from = 0;
    while (from + needle_len <= buf->length) {
        long at = value_str_find(buf->data, buf->length, args[1].as.str.ptr, needle_len, from);
        if (at < 0) break;
        best = (int64_t)at;
        from = (size_t)at + 1;
    }
    if (best < 0 || buf->ascii) return value_int(best);
    return value_int(utf8_char_of_byte(d, buf->length, (size_t)best, "last_index_of", line, col));
}

static Value native_mutable_string_to_string(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(args[0]));
    char *out = zalloc_atomic(buf->length + 1);
    memcpy(out, buf->data, buf->length);
    out[buf->length] = '\0';
    return value_string(out);
}


/* --- MutableArray<T> -------------------------------------------------
 * Identical shape to MutableString's own table/handle machinery above,
 * just storing `Value` per slot instead of `char` -- since an ordinary
 * Ziman ArrayObj is already exactly this (a generic, type-erased `Value
 * *items` buffer), the native side needs no per-T dispatch at all: a
 * `Value` copies by plain assignment regardless of what type it holds.
 * The one thing a native function genuinely can't do on its own is
 * stamp the *concrete* T onto a result's own `.type` (MutableArray<T>
 * carries no argument that would reveal it, and NativeFn's signature
 * carries no type-argument information at all) -- every `.type` built
 * below is a placeholder for exactly this reason, immediately
 * overwritten with the call's own already-substituted e->resolved_type
 * right after each of these returns (see eval_expr's EXPR_CALL case). */
typedef struct {
    pthread_t owner;
    Value *items;
    size_t length;
    size_t capacity;
    double grow_factor;
    /* Packed mode: once the first appended element is a byte, bool, int or float,
     * elements are kept as raw values in `pdata` (with `items` unused), so an
     * array of them under construction doesn't cost a Value each and snapshot
     * is one memcpy. */
    PackKind pkind;
    unsigned char *pdata;
    /* A capacity asked for but not yet allocated. Nothing can be allocated at `new` time,
     * because which of the two representations to allocate is decided by the first element --
     * so reserving eagerly meant allocating `capacity * sizeof(Value)` of boxed storage and,
     * for a packed element type, throwing all of it away unused on that first push. */
    size_t reserved;
} MutableArrayBuf;

/* Same pointer-as-handle scheme as MutableString above, for the same reason. */
static int64_t mutable_array_table_register(MutableArrayBuf *buf) {
    buf->owner = pthread_self();
    handleset_add(&g_array_builders, buf);
    builder_register_drop(buf, &g_array_builders);
    return (int64_t)(intptr_t)buf;
}

static MutableArrayBuf *mutable_array_table_get(int64_t handle) {
    MutableArrayBuf *buf = (MutableArrayBuf *)(intptr_t)handle;
    if (!handleset_has(&g_array_builders, buf)) ziman_error("runtime", 0, 0, "not a valid MutableArray handle");
    builder_owner_check(buf->owner, "a MutableArray");
    return buf;
}

static char *ARRAYBUILDER_FIELD_NAMES[] = {"handle"};

static int64_t mutable_array_handle_of(Value ab) {
    return ab.as.st->fields[0].as.i;
}

static void mutable_array_ensure_capacity(MutableArrayBuf *buf, size_t additional) {
    size_t needed = buf->length + additional;
    if (needed <= buf->capacity) {
        return;
    }
    size_t new_capacity = buf->capacity > 0 ? buf->capacity : (buf->reserved > 0 ? buf->reserved : 1);
    while (new_capacity < needed) {
        new_capacity = (size_t)((double)new_capacity * buf->grow_factor) + 1;
    }
    if (buf->pkind != PK_NONE) buf->pdata = ziman_xrealloc(buf->pdata, new_capacity * pack_elem_size(buf->pkind));
    else buf->items = ziman_xrealloc(buf->items, new_capacity * sizeof(Value));
    buf->capacity = new_capacity;
}

/* An empty MutableArray with room for `capacity` elements. Factored out of native_mutable_array_new so that
 * `mutable_array_from` can size the buffer to its source in one allocation, where `new` plus a
 * loop of pushes would regrow it as it went. */
static Value mutable_array_new_value(size_t capacity, double grow_factor) {
    MutableArrayBuf *buf = ziman_xmalloc(sizeof(MutableArrayBuf));
    buf->length = 0;
    buf->grow_factor = grow_factor;
    buf->capacity = 0;
    buf->reserved = capacity;
    buf->items = NULL;
    buf->pdata = NULL;
    buf->pkind = PK_NONE;
    int64_t handle = mutable_array_table_register(buf);
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int(handle);
    return value_struct(type_ref(type_struct("MutableArray")), struct_obj_new(ARRAYBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_mutable_array_new(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    int64_t capacity = option_int_or(args[0], 20);
    double grow_factor = option_float_or(args[1], 2.0);
    if (capacity < 0) {
        capacity = 0;
    }
    if (grow_factor <= 1.0) {
        grow_factor = 2.0;
    }

    return mutable_array_new_value((size_t)capacity, grow_factor);
}

/* Picks the packed representation from the first element, when the builder is still empty and has
 * not already chosen one. Factored out of native_mutable_array_append so that inserting into a *fresh*
 * builder takes the same decision -- without it, `insert(b, 0, 1)` on a new builder
 * would box every int for the rest of its life while an append-first builder packed them. */
static void mutable_array_adopt_pack_kind(MutableArrayBuf *buf, Value first_element) {
    if (buf->pkind != PK_NONE || buf->length != 0) return;
    PackKind first = pack_kind_of_type(first_element.type->kind);
    if (first == PK_NONE) return;
    size_t cap = buf->capacity > 0 ? buf->capacity : (buf->reserved > 0 ? buf->reserved : 16);
    buf->items = NULL;
    buf->pdata = zalloc_atomic(cap * pack_elem_size(first));
    buf->capacity = cap;
    buf->pkind = first;
}

/* Shifts `count` elements from `from` to `to`, in whichever representation this builder uses. One
 * helper because insert and remove differ only in direction, and getting the stride wrong for a
 * packed buffer would silently corrupt it rather than fail. */
static void mutable_array_move_elements(MutableArrayBuf *buf, size_t to, size_t from, size_t count) {
    if (count == 0) return;
    if (buf->pkind != PK_NONE) {
        size_t w = pack_elem_size(buf->pkind);
        memmove((unsigned char *)buf->pdata + to * w, (unsigned char *)buf->pdata + from * w, count * w);
    } else {
        memmove(&buf->items[to], &buf->items[from], count * sizeof(Value));
    }
}

/* The one-element `push(b, x)`, which is how almost every call is written. The
 * general path collapses the variadic tail into a one-element array first: three allocations per
 * call, two of them from a collector's heap, and for a *reference* element type that temporary is a
 * scanned allocation, so its cost compounds with the size of the live buffer rather than staying
 * per-call. Measured at 2,000,000 pushes of a string: 4.80s one at a time against 0.22s in batches
 * of a thousand -- the same elements, the same final memory, 22x the time -- while the same loop
 * with `int` elements is linear, because an int builder's storage is packed and unscanned. This
 * appends the Value itself and allocates nothing (D18).
 *
 * `interpreter.c`'s call_function picks it by comparing the resolved native pointer, which is why
 * `ziman_mutable_array_push` is no longer static. */
void ziman_mutable_array_push_one(Value ab, Value element) {
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(ab));
    mutable_array_adopt_pack_kind(buf, element);
    mutable_array_ensure_capacity(buf, 1);
    if (buf->pkind != PK_NONE) {
        pack_store(buf->pkind, buf->pdata, (int)buf->length, element);
    } else {
        buf->items[buf->length] = element;
    }
    buf->length++;
}

Value ziman_mutable_array_push(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    ArrayObj *pieces = args[1].as.arr;
    if (pieces->count == 0) return value_int(0);
    mutable_array_adopt_pack_kind(buf, array_get(pieces, 0));
    mutable_array_ensure_capacity(buf, (size_t)pieces->count);
    if (buf->pkind != PK_NONE) {
        size_t esz = pack_elem_size(buf->pkind);
        if (array_pack_kind(pieces) == buf->pkind) {
            const void *src = buf->pkind == PK_BYTE ? (const void *)pieces->bytes : buf->pkind == PK_BOOL ? (const void *)pieces->bools :
                              buf->pkind == PK_INT ? (const void *)pieces->ints : (const void *)pieces->floats;
            memcpy(buf->pdata + esz * buf->length, src, esz * (size_t)pieces->count);
        } else {
            for (int i = 0; i < pieces->count; i++) pack_store(buf->pkind, buf->pdata, (int)buf->length + i, array_get(pieces, i));
        }
        buf->length += (size_t)pieces->count;
        return value_int(0);
    }
    for (int i = 0; i < pieces->count; i++) {
        buf->items[buf->length] = array_get(pieces, i);
        buf->length++;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

/* Defined further down with the other element accessors; declared here because `remove` has to
 * read the element before it shifts over it. */
static Value mutable_array_element_at(const MutableArrayBuf *buf, size_t i);

/* O(n): everything after the index moves. `0..len` inclusive, exactly the immutable
 * `insert(arr, i, x)` rule, so inserting at the length appends and "add at the end" needs no second
 * function. A loop of `insert(xs, 0, v)` is quadratic, which is why there is no `push_front` --
 * see docs/DESIGN.md. */
static Value native_mutable_array_insert(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    int64_t i = args[1].as.i;
    if (i < 0 || (size_t)i > buf->length) {
        ziman_error("runtime", line, col,
                    "insert index %lld out of bounds (MutableArray length %lld; 0..length is allowed, and inserting at the length appends)",
                    (long long)i, (long long)buf->length);
    }
    mutable_array_adopt_pack_kind(buf, args[2]);
    mutable_array_ensure_capacity(buf, 1);
    mutable_array_move_elements(buf, (size_t)i + 1, (size_t)i, buf->length - (size_t)i);
    if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)i, args[2]);
    else buf->items[i] = args[2];
    buf->length++;
    return value_int(0); /* void return -- the caller never uses this */
}

/* O(n), and it answers what it removed -- which is why there is no `pop`:
 * `remove(b, len(b) - 1)` is one. The element is otherwise unrecoverable without a
 * `get` first. */
static Value native_mutable_array_remove(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    int64_t i = args[1].as.i;
    if (i < 0 || (size_t)i >= buf->length) {
        ziman_error("runtime", line, col, "remove index %lld out of bounds (MutableArray length %lld)", (long long)i,
                    (long long)buf->length);
    }
    Value removed = mutable_array_element_at(buf, (size_t)i);
    mutable_array_move_elements(buf, (size_t)i, (size_t)i + 1, buf->length - (size_t)i - 1);
    buf->length--;
    return removed;
}

/* The only cheap shrink: drops the tail and keeps the buffer. A length at or above the current one
 * leaves it alone rather than erroring, because "keep at most n" is the useful meaning and there is
 * no immutable counterpart to disagree with. `clear(xs)` is this with 0. */
/* Sorts the buffer in place with **no copy of the elements at all**: a merge sort over positions,
 * then the permutation applied by following its cycles. What it allocates is two int64 order
 * buffers and an n-byte "placed" map -- 17 bytes per element against the 80 a boxed `Value` costs,
 * twice, which is what the snapshot-sort-write-back version paid.
 *
 * The comparator is called on the *elements*, not on positions as `sort_positions` does, because
 * the caller's `cmp` has the element type's signature. Stability comes from the same `< 0` test the
 * other merge sorts here use: equal elements keep the left run's order.
 *
 * Why only `sort_with` and not `sort`/`sort_desc`: those two can use `order_at`'s native comparison
 * for a packed int/float/byte buffer, with no interpreter call per comparison, which is worth more
 * than the copies it costs. Telling "the type's own order, possibly reversed" from "an arbitrary
 * comparator" needs a direction, and the public signature of `sort_with` has nowhere to put one. */
static void mutable_array_permute_in_place(MutableArrayBuf *buf, const int64_t *order, size_t n) {
    unsigned char *placed = zalloc_atomic(n > 0 ? n : 1);
    for (size_t i = 0; i < n; i++) placed[i] = 0;
    for (size_t i = 0; i < n; i++) {
        if (placed[i]) continue;
        /* Walk this cycle, carrying the element displaced at its start until it lands. Nothing is
         * read after it has been overwritten: the write is always to `j` and the next read is from
         * `src`, which has not been written yet. */
        Value carried = mutable_array_element_at(buf, i);
        size_t j = i;
        for (;;) {
            size_t src = (size_t)order[j];
            placed[j] = 1;
            Value landing = src == i ? carried : mutable_array_element_at(buf, src);
            if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)j, landing);
            else buf->items[j] = landing;
            if (src == i) break;
            j = src;
        }
    }
}

static Value native_mutable_array_sort_with(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    size_t n = buf->length;
    if (n < 2) return value_int(0);
    int64_t *order = zalloc_atomic(n * sizeof(int64_t));
    int64_t *tmp = zalloc_atomic(n * sizeof(int64_t));
    for (size_t i = 0; i < n; i++) order[i] = (int64_t)i;
    for (size_t width = 1; width < n; width *= 2) {
        size_t k = 0;
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid;
            while (i < mid && j < hi) {
                Value pair[2] = {mutable_array_element_at(buf, (size_t)order[j]), mutable_array_element_at(buf, (size_t)order[i])};
                if (native_call_closure(args[1], pair, 2, line, col).as.i < 0) tmp[k++] = order[j++];
                else tmp[k++] = order[i++];
            }
            while (i < mid) tmp[k++] = order[i++];
            while (j < hi) tmp[k++] = order[j++];
        }
        int64_t *swap = order;
        order = tmp;
        tmp = swap;
    }
    mutable_array_permute_in_place(buf, order, n);
    return value_int(0); /* void return -- the caller never uses this */
}

/* Compacts the buffer in place: one walk with a write cursor trailing the read cursor, so the kept
 * elements end up at the front and the length shrinks. **No allocation at all**, which is the whole
 * reason this exists rather than `array_filter(snapshot(xs), keep)` -- that makes two copies, and
 * this is the one filter in the language that makes none.
 *
 * `keep` is user code, so it could in principle change the builder while the walk is under way.
 * That is survivable rather than supported: `buf` points at the stable handle record and its
 * `items`/`pdata` are re-read through it, so a reallocation inside the predicate cannot leave a
 * dangling pointer -- but what the rest of the walk then visits is the buffer mid-compaction, which
 * is the same class of thing as appending during a `for` and is documented the same way. */
static Value native_mutable_array_filter(Value *args, int argc, int line, int col) {
    (void)argc;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    size_t write = 0;
    for (size_t read = 0; read < buf->length; read++) {
        Value element = mutable_array_element_at(buf, read);
        if (!native_call_closure(args[1], &element, 1, line, col).as.b) continue;
        if (write != read) {
            if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)write, element);
            else buf->items[write] = element;
        }
        write++;
    }
    buf->length = write;
    return value_int(0); /* void return -- the caller never uses this */
}

static Value native_string_to_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    return value_byte_array((const uint8_t *)s, n);
}

/* ---- UTF-8: a strict validator/decoder, used by bytes_to_string and the codepoint functions
 * below (chars, len over text, truncate) -- see docs/DESIGN.md's UTF-8 correctness entry.
 *
 * Deliberately not shared with regex.c's own decode(): that one is lenient on purpose (an
 * invalid sequence falls back to one raw byte, so a regex never fails to match just because the
 * text it's given happens to be malformed) -- exactly wrong here, where the whole point is to
 * *detect* malformed input, not paper over it. */

/* The byte offset of the first invalid sequence in s[0..len), or -1 if all of it is valid. */
static long utf8_find_invalid(const uint8_t *s, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) return (long)pos;
        pos += (size_t)n;
    }
    return -1;
}

static Value native_bytes_to_string(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str_type = type_scalar(TY_STRING);
    ArrayObj *arr = args[0].as.arr;
    int n = 0;
    uint8_t *tmp = array_copy_bytes(arr, &n);
    long bad = utf8_find_invalid(tmp, (size_t)n);
    if (bad >= 0) {
        char msg[80];
        snprintf(msg, sizeof msg, "bytes are not valid UTF-8 (invalid sequence at byte %ld)", bad);
        free(tmp);
        return value_result(type_ref(type_result(str_type, str_type)), result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    if (memchr(tmp, 0, (size_t)n) != NULL) {
        free(tmp);
        return value_result(type_ref(type_result(str_type, str_type)), result_obj_new(0, value_string(ziman_xstrdup("bytes contain a NUL byte and cannot be a string"))));
    }
    char *s = (char *)tmp; /* already NUL-terminated by array_copy_bytes */
    return value_result(type_ref(type_result(str_type, str_type)), result_obj_new(1, value_string(s)));
}

/* One string per code point of `s` (never a raw byte): `chars("héllo")` is `[h, é, l, l, o]`,
 * not `[h, \xc3, \xa9, l, l, o]`. A runtime error naming the byte offset if `s` isn't valid
 * UTF-8 -- reachable only by chaining a byte-level function (`substring`, `concat` on a slice)
 * into a code-point one, since bytes_to_string above already rejects invalid UTF-8 at the one
 * real boundary untrusted bytes become a string. */
static Value native_chars(Value *args, int argc, int line, int col) {
    (void)argc;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t len = SLEN(args[0]);
    Value *items = NULL;
    int count = 0, cap = 0;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) {
            ziman_error("runtime", line, col, "chars: string is not valid UTF-8 (invalid sequence at byte %zu)", pos);
        }
        if (count == cap) {
            cap = cap == 0 ? 8 : cap * 2;
            items = ziman_xrealloc(items, (size_t)cap * sizeof(Value));
        }
        items[count++] = string_slice(args[0], pos, (size_t)n);
        pos += (size_t)n;
    }
    return value_array(type_ref(type_array(type_scalar(TY_STRING))), array_obj_new(items, count));
}

/* The number of code points in `s` -- `char_count("héllo")` is 5, where `len("héllo")` (bytes)
 * is 6. Cheaper than `len(chars(s))` when only the count is wanted: no array, no per-character
 * string views. Same UTF-8 validity requirement and error as `chars`. */
/* `byte_len` over text in either world: the storage size, which `len` answered until 2026-10-23.
 *
 * Registered bare, as `len` is, because one implementation serves both members -- a `string` keeps
 * its length beside its pointer and a `MutableString` keeps it in the handle's table. The symbol
 * fallback is never consulted for either name. */
static Value native_byte_len(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value v = args[0];
    if (v.type->kind == TY_STRING) return value_int((int64_t)v.as.str.len);
    return value_int(ziman_builder_length(v, ziman_mutable_kind(v.type)));
}

/* `s`, shortened to at most `max_chars` code points -- the safe way to truncate user-facing text
 * for display, where `substring` raises on a `max_chars` past the end instead of clamping. (It also
 * cut multi-byte characters in half until 2026-10-23; counting characters is what both do now, so
 * clamping is the whole of the difference.) Stops at the earlier of `max_chars` code points or the
 * end of `s`; `max_chars >= len(s)` returns `s` itself, unchanged. Same UTF-8 validity requirement
 * and error as `chars`. */
static Value native_truncate(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t max_chars = args[1].as.i;
    if (max_chars < 0) {
        ziman_error("runtime", line, col, "truncate: max_chars must not be negative (got %lld)", (long long)max_chars);
    }
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t len = SLEN(args[0]);
    size_t pos = 0;
    int64_t count = 0;
    while (pos < len && count < max_chars) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        if (n == 0) {
            ziman_error("runtime", line, col, "truncate: string is not valid UTF-8 (invalid sequence at byte %zu)", pos);
        }
        pos += (size_t)n;
        count++;
    }
    return string_slice(args[0], 0, pos);
}

/* `s` with its code points in the opposite order -- code points, not bytes, because reversing the
 * bytes of UTF-8 is the classic way to corrupt it.
 *
 * **Was ordinary Ziman until it was measured.** It read `chars(s)` into an array and folded it with
 * `concat` in a loop, which is the quadratic string build: each `concat` allocates a fresh buffer
 * and copies everything accumulated so far, so reversing 16,000 characters took 54 ms and 8,000
 * took 22. It also paid n+1 allocations for the `chars` array and its one-character views before
 * the fold even started. This is one allocation of exactly the right size and one pass.
 *
 * The ASCII path does not decode at all: one byte per character means reversing the characters *is*
 * reversing the bytes. Off it, each character's bytes are copied forward into a position measured
 * from the end -- the whole trick of a UTF-8 reversal is that the sequence stays in order while its
 * position moves. The result inherits ZSTR_ASCII either way, since a reversal changes no byte. */
static Value native_reverse_string(Value *args, int argc, int line, int col) {
    (void)argc;
    const uint8_t *s = (const uint8_t *)SPTR(args[0]);
    size_t n = SLEN(args[0]);
    if (n == 0) return args[0]; /* its own reverse, and worth not allocating for */
    char *buf = zalloc_atomic(n + 1);
    buf[n] = '\0';
    if (SASCII(args[0])) {
        for (size_t i = 0; i < n; i++) buf[n - 1 - i] = (char)s[i];
    } else {
        size_t pos = 0, out = n;
        while (pos < n) {
            uint32_t cp;
            int w = ziman_utf8_decode_strict(s, n, pos, &cp);
            if (w == 0) {
                ziman_error("runtime", line, col, "reverse: string is not valid UTF-8 (invalid sequence at byte %zu)",
                            pos);
            }
            out -= (size_t)w;
            memcpy(buf + out, s + pos, (size_t)w);
            pos += (size_t)w;
        }
    }
    return value_string_flags(buf, n, ZSTR_TERM | (args[0].as.str.flags & ZSTR_ASCII));
}

/* ---- sorting scalars natively (array_sort and friends) -------------------------------
 * A packed [int] / [float] / [byte], or an array of strings, is sorted by a stable merge
 * sort over positions, avoiding a per-comparison interpreter call. Anything else (a struct,
 * an enum, bigint, ...) is left to collections' generic sort. */
static int scalar_sortable(const ArrayObj *a) {
    if (array_pack_kind(a) == PK_INT || array_pack_kind(a) == PK_FLOAT || array_pack_kind(a) == PK_BYTE) return 1;
    if (array_pack_kind(a) != PK_NONE) return 0; /* [bool] has no order */
    for (int i = 0; i < a->count; i++) {
        if (a->boxed_items[i].type->kind != TY_STRING) return 0;
    }
    return 1;
}

/* Same order as `compare`: a NaN sorts after every other float and equals itself. */
static int order_float(double x, double y) {
    int nx = x != x, ny = y != y;
    if (nx || ny) return nx == ny ? 0 : (nx ? 1 : -1);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int order_at(const ArrayObj *a, int i, int j) {
    switch (array_pack_kind(a)) {
        case PK_INT: return a->ints[i] < a->ints[j] ? -1 : (a->ints[i] > a->ints[j] ? 1 : 0);
        case PK_FLOAT: return order_float(a->floats[i], a->floats[j]);
        case PK_BYTE: return (int)a->bytes[i] - (int)a->bytes[j];
        default: {
            Value x = a->boxed_items[i], y = a->boxed_items[j];
            uint32_t n = x.as.str.len < y.as.str.len ? x.as.str.len : y.as.str.len;
            int c = n > 0 ? memcmp(x.as.str.ptr, y.as.str.ptr, n) : 0;
            if (c != 0) return c < 0 ? -1 : 1;
            return x.as.str.len < y.as.str.len ? -1 : (x.as.str.len > y.as.str.len ? 1 : 0);
        }
    }
}

static Value native_is_sortable_scalars(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(scalar_sortable(args[0].as.arr));
}

/* The positions of the array's elements in sorted order, as a fresh int64 buffer (the
 * caller's to use). Stable: equal elements keep their input order, in descending order
 * too (it is not the ascending result reversed). */
static int64_t *scalar_order(const ArrayObj *a, int desc) {
    int n = a->count;
    int64_t *order = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t *tmp = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int i = 0; i < n; i++) order[i] = i;
    for (int width = 1; width < n; width *= 2) {
        int k = 0;
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n;
            int hi = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo, j = mid;
            while (i < mid && j < hi) {
                int c = order_at(a, (int)order[j], (int)order[i]);
                if (desc ? c > 0 : c < 0) tmp[k++] = order[j++];
                else tmp[k++] = order[i++];
            }
            while (i < mid) tmp[k++] = order[i++];
            while (j < hi) tmp[k++] = order[j++];
        }
        int64_t *swap = order; order = tmp; tmp = swap;
    }
    return order;
}

/* sort_positions(n, cmp): the positions 0..n ordered by `cmp(a, b)` (negative, zero or
 * positive, over two positions) -- a stable merge sort in two reused buffers, calling back
 * into Ziman only for the comparisons. */
static Value native_sort_positions(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t n = args[0].as.i;
    Value cmp = args[1];
    if (n < 0) ziman_error("runtime", line, col, "sort_positions: n must be non-negative, found %lld", (long long)n);
    int64_t *order = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t *tmp = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t width = 1; width < n; width *= 2) {
        int64_t k = 0;
        for (int64_t lo = 0; lo < n; lo += 2 * width) {
            int64_t mid = lo + width < n ? lo + width : n;
            int64_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            int64_t i = lo, j = mid;
            while (i < mid && j < hi) {
                Value pair[2] = {value_int(order[j]), value_int(order[i])};
                if (native_call_closure(cmp, pair, 2, line, col).as.i < 0) tmp[k++] = order[j++];
                else tmp[k++] = order[i++];
            }
            while (i < mid) tmp[k++] = order[i++];
            while (j < hi) tmp[k++] = order[j++];
        }
        int64_t *swap = order; order = tmp; tmp = swap;
    }
    return value_int_array(order, (size_t)n);
}

static Value native_sort_scalar_order(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const ArrayObj *a = args[0].as.arr;
    return value_int_array(scalar_order(a, args[1].as.b), (size_t)a->count);
}

/* `a` rearranged: element i of the result is a's element order[i]. One allocation, and a
 * packed array stays packed. Positions are 0-based and must be in range. */
static Value permute_array(const Type *type, const ArrayObj *a, const int64_t *order, int n, int line, int col) {
    for (int i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] >= a->count) {
            ziman_error("runtime", line, col, "permute: position %lld out of range for an array of length %d", (long long)order[i], a->count);
        }
    }
    PackKind kind = array_pack_kind(a);
    if (kind == PK_NONE) {
        Value *items = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(Value));
        for (int i = 0; i < n; i++) items[i] = a->boxed_items[order[i]];
        return value_array(type, array_obj_new(items, n));
    }
    size_t esz = pack_elem_size(kind);
    unsigned char *buf = zalloc_atomic(esz * (size_t)(n > 0 ? n : 1));
    const unsigned char *src = kind == PK_BYTE ? (const unsigned char *)a->bytes : kind == PK_BOOL ? (const unsigned char *)a->bools
                               : kind == PK_INT ? (const unsigned char *)a->ints : (const unsigned char *)a->floats;
    for (int i = 0; i < n; i++) memcpy(buf + esz * (size_t)i, src + esz * (size_t)order[i], esz);
    return value_array(type, array_obj_new_packed(kind, buf, n));
}

/* sort_scalars<T>(items, descending): the array sorted, without an index array in between. */
static Value native_sort_scalars(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    return permute_array(args[0].type, a, scalar_order(a, args[1].as.b), a->count, line, col);
}

/* permute<T>(items, order): items[order[0]], items[order[1]], ... */
static Value native_permute(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    const ArrayObj *ord = args[1].as.arr;
    int n = ord->count;
    int64_t *order = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int i = 0; i < n; i++) order[i] = array_get(ord, i).as.i;
    return permute_array(args[0].type, a, order, n, line, col);
}

/* The same, for the array form: one allocation over all the parts, packed when every part is
 * packed the same way. */
Value ziman_concat_arrays(const ArrayObj *parts) {
    int total = 0;
    PackKind kind = PK_NONE;
    int all_packed = 1;
    for (int i = 0; i < parts->count; i++) {
        ArrayObj *part = array_get(parts, i).as.arr;
        total += part->count;
        if (part->count == 0) continue;
        PackKind pk = array_pack_kind(part);
        if (pk == PK_NONE || (kind != PK_NONE && pk != kind)) all_packed = 0;
        else if (kind == PK_NONE) kind = pk;
    }
    if (total > 0 && all_packed && kind != PK_NONE) {
        size_t esz = pack_elem_size(kind);
        unsigned char *buf = zalloc_atomic(esz * (size_t)total);
        size_t at = 0;
        for (int i = 0; i < parts->count; i++) {
            ArrayObj *part = array_get(parts, i).as.arr;
            if (part->count == 0) continue;
            const void *src = kind == PK_BYTE ? (const void *)part->bytes : kind == PK_BOOL ? (const void *)part->bools : kind == PK_INT ? (const void *)part->ints : (const void *)part->floats;
            memcpy(buf + at, src, esz * (size_t)part->count);
            at += esz * (size_t)part->count;
        }
        return value_array(type_scalar_ref(TY_VOID), array_obj_new_packed(kind, buf, total));
    }
    Value *items = total > 0 ? ziman_xmalloc((size_t)total * sizeof(Value)) : NULL;
    int pos = 0;
    for (int i = 0; i < parts->count; i++) {
        ArrayObj *part = array_get(parts, i).as.arr;
        for (int j = 0; j < part->count; j++) items[pos++] = array_get(part, j);
    }
    ArrayObj *packed = array_try_pack(items, total);
    if (packed != NULL) return value_array(type_scalar_ref(TY_VOID), packed);
    return value_array(type_scalar_ref(TY_VOID), array_obj_new(items, total));
}

/* slice<T>(a, start, end) over an array: a zero-copy view. Arrays are immutable once built
 * (see ArrayObj), so the result can share the parent's element storage: a new
 * header pointing into `a`'s items. The same trade-off as string views: the
 * slice keeps the whole parent alive.
 *
 * The caller sets the result's own type from the call's resolved type, `slice` being a builtin
 * dispatched on its first argument since 2026-10-18. */
Value ziman_slice_array(Value a, int64_t start, int64_t end, int line, int col) {
    ArrayObj *arr = a.as.arr;
    if (start < 0 || end < start || end > arr->count) {
        ziman_error("runtime", line, col, "slice: range %lld..%lld is out of bounds for an array of length %d",
                    (long long)start, (long long)end, arr->count);
    }
    return value_array(type_scalar_ref(TY_VOID), array_obj_view(arr, (int)start, (int)end));
}

/* One element of a builder, whichever representation it is in -- `array_get`'s shape against the
 * builder's own buffer rather than an ArrayObj. */
static Value mutable_array_element_at(const MutableArrayBuf *buf, size_t i) {
    switch (buf->pkind) {
        case PK_BYTE: return value_byte(((unsigned char *)buf->pdata)[i]);
        case PK_BOOL: return value_bool(((unsigned char *)buf->pdata)[i] != 0);
        case PK_INT: return value_int(((int64_t *)buf->pdata)[i]);
        case PK_FLOAT: return value_float(((double *)buf->pdata)[i]);
        case PK_NONE: break;
    }
    return buf->items[i];
}

/* Reading and writing a builder in place, so that a loop that needs what it has already appended does not
 * have to call snapshot -- which copies the whole buffer every time it is called, and in a loop is the
 * quadratic cost MutableArray exists to avoid (`--lint` already flags that shape; see
 * lint_builder_read_by_conversion). MutableMap and MutableSet have had `get`/`contains` from the start; this
 * is the same thing for the one builder that was append-only. */
/* `b[i]` and `b[i] = value`, reached straight from the interpreter's index paths rather than
 * through a call. They were `native_mutable_array_get`/`_set` until F209, registered natives that
 * the checker rewrote `b[i]` onto -- which is why `mutable_array_get` and `mutable_array_set` had
 * to be declared and advertised, and why a traceback for `b[5]` carried a frame for a function the
 * author never wrote. Both still take `mutable_array_table_get`'s ownership check, same as every
 * other builder operation.
 *
 * The message says "index", not "get index"/"set index": the prefix existed to tell two function
 * names apart, and the position now names itself the way an array's `a[5]` always has. */
Value ziman_mutable_array_index_get(Value ab, int64_t i, int line, int col) {
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(ab));
    if (i < 0 || (size_t)i >= buf->length) {
        ziman_error("runtime", line, col, "index %lld out of bounds (MutableArray length %lld)",
                    (long long)i, (long long)buf->length);
    }
    return mutable_array_element_at(buf, (size_t)i);
}

void ziman_mutable_array_index_set(Value ab, int64_t i, Value value, int line, int col) {
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(ab));
    if (i < 0 || (size_t)i >= buf->length) {
        ziman_error("runtime", line, col, "index %lld out of bounds (MutableArray length %lld)",
                    (long long)i, (long long)buf->length);
    }
    if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)i, value);
    else buf->items[i] = value;
}


/* What the interpreter's `for x in builder` needs: the two of these are the whole of its access, and they
 * take the same ownership check every other builder call takes (mutable_array_table_get). */
int ziman_array_builder_length(Value ab) {
    return (int)mutable_array_table_get(mutable_array_handle_of(ab))->length;
}

Value ziman_array_builder_at(Value ab, int i) {
    return mutable_array_element_at(mutable_array_table_get(mutable_array_handle_of(ab)), (size_t)i);
}

static Value native_mutable_array_to_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(args[0]));
    if (buf->pkind != PK_NONE) {
        size_t esz = pack_elem_size(buf->pkind);
        unsigned char *out = zalloc_atomic(esz * buf->length);
        memcpy(out, buf->pdata, esz * buf->length);
        return value_array(type_scalar_ref(TY_VOID), array_obj_new_packed(buf->pkind, out, (int)buf->length));
    }
    Value *items = buf->length > 0 ? ziman_xmalloc(buf->length * sizeof(Value)) : NULL;
    for (size_t i = 0; i < buf->length; i++) {
        items[i] = buf->items[i];
    }
    return value_array(type_scalar_ref(TY_VOID), array_obj_new(items, (int)buf->length));
}


/* Registered with gc_mark.c (see gc_mark_register_struct_content_marker's own comment) so a real
 * collection landing while a MutableArray is still being appended to doesn't sweep an
 * already-appended, gc_heap-migrated element (a string, a struct, an array -- anything but a packed
 * scalar) that no longer has any other live reference once the loop that built it has moved on.
 * `st` is the MutableArray's own wrapper StructObj, not a Value -- its `fields[0]` is the same
 * disguised handle `mutable_array_handle_of` reads off a Value, just reached a different way. */
static void mutable_array_mark_contents(const Arena *a, StructObj *st) {
    MutableArrayBuf *buf = (MutableArrayBuf *)(intptr_t)st->fields[0].as.i;
    if (buf->pkind != PK_NONE) return; /* packed elements are raw scalar bytes, nothing to mark */
    for (size_t i = 0; i < buf->length; i++) gc_mark_value(a, buf->items[i]);
}

/* --- MutableMap<K, V> and MutableSet<T> -------------------------------------------------
 * Mutable counterparts of Map and Set, for building one up entry by entry. Same
 * pointer-as-handle scheme as MutableArray; the buffer owns a MapTable (src/maptable.h) nobody
 * else can see and changes it in place. `to_map` / `to_set` build an immutable Map from it and the
 * builder stays usable, like snapshot.
 *
 * The storage used to be the same persistent trie `Map` uses, mutated in place. That cost a `Leaf`
 * allocation per entry, which made per-insert cost rise with the *live set* (135 ns/insert at 1,000
 * entries, 291 at 100,000) because a tracing collector prices a short-lived allocation by what is
 * alive -- see maptable.h. A builder needs no persistence at all: it is `#[thread_bound]`, mutated
 * in place, and exists to be the staging area. The `shared` flag went with the trie: it meant "a
 * snapshot shares these leaves, so an overwrite must replace rather than update", and a table
 * shares nothing with the Map built from it. */
typedef struct {
    pthread_t owner;
    MapTable t;
} MutableMapBuf;

static char *MAPBUILDER_FIELD_NAMES[] = {"handle"};

static MutableMapBuf *mutable_map_get_buf(Value b) {
    MutableMapBuf *buf = (MutableMapBuf *)(intptr_t)b.as.st->fields[0].as.i;
    if (!handleset_has(&g_map_builders, buf)) ziman_error("runtime", 0, 0, "not a valid MutableMap/MutableSet handle");
    builder_owner_check(buf->owner, "a MutableMap/MutableSet");
    return buf;
}

/* See mutable_array_mark_contents's own comment -- identical reasoning, for a
 * MutableMap/MutableSet's own table instead of a MutableArray's items buffer.
 *
 * This got simpler when the builder stopped being a trie, and the reason is worth keeping. The trie
 * version had to mark a *header*: `MapObj` is gc_heap-allocated (GC_MAP), so the builder's private
 * wrapper needed its own mark bit set and not merely its entries traced, which is why that version
 * fabricated a throwaway TY_MAP Value and called gc_mark_value to reuse gc_mark_map's
 * is_gc_heap_object-gated handling. Getting that wrong was found the hard way: without it the
 * wrapper looked unmarked on every real collection and was swept, and map_set_builders.zm crashed
 * inside GC_realloc on a `root` MapNode pointer read back out of a struct that had already been
 * zeroed. A MapTable has no gc_heap object anywhere in it -- both its arrays come from
 * `ziman_xmalloc`, which the ordinary allocator owns and scans itself -- so there is no header to
 * mark and that whole failure mode is gone. Only what the entries point at needs tracing. */
static void mark_table_entry(Value key, Value val, void *ctx) {
    gc_mark_value((const Arena *)ctx, key);
    gc_mark_value((const Arena *)ctx, val);
}

/* The table's own arrays need no marking -- they are `ziman_xmalloc`'d, so the ordinary allocator
 * owns them and scans them itself. What needs marking is what the entries *point at*, which can be
 * anywhere in this thread's gc_heap. This used to fabricate a TY_MAP Value over the builder's
 * private trie so that gc_mark_map would walk its leaves; a table has no leaves to mark, only
 * entries to visit. */
static void mutable_map_mark_contents(const Arena *a, StructObj *st) {
    MutableMapBuf *buf = (MutableMapBuf *)(intptr_t)st->fields[0].as.i;
    maptable_visit(&buf->t, mark_table_entry, (void *)a);
}

/* A source for the set/map operations below, which may be given either world: another builder's
 * table or an immutable Map/Set. Both are materialised into fresh arrays in insertion order, which
 * every caller wants anyway -- intersect and difference have to walk a snapshot rather than the
 * live table they are removing from. `vals` may be NULL to skip values. */
typedef struct {
    Value *keys;
    Value *vals;
    size_t n;
} EntryCollector;

static void collect_entry(Value key, Value val, void *ctx) {
    EntryCollector *c = (EntryCollector *)ctx;
    c->keys[c->n] = key;
    if (c->vals != NULL) c->vals[c->n] = val;
    c->n++;
}

static int maptable_entries(const MapTable *t, Value **keys, Value **vals) {
    size_t n = maptable_len(t);
    *keys = ziman_xmalloc((n > 0 ? n : 1) * sizeof(Value));
    if (vals != NULL) *vals = ziman_xmalloc((n > 0 ? n : 1) * sizeof(Value));
    EntryCollector c = {*keys, vals != NULL ? *vals : NULL, 0};
    maptable_visit(t, collect_entry, &c);
    return (int)n;
}

static Value mutable_map_new_value(const char *type_name) {
    MutableMapBuf *buf = ziman_xmalloc(sizeof(MutableMapBuf));
    buf->owner = pthread_self();
    handleset_add(&g_map_builders, buf);
    builder_register_drop(buf, &g_map_builders);
    maptable_init(&buf->t);
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)buf);
    return value_struct(type_ref(type_struct(type_name)), struct_obj_new(MAPBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_mutable_map_new(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return mutable_map_new_value("MutableMap");
}

static Value native_mutable_set_new(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return mutable_map_new_value("MutableSet");
}

static Value native_mutable_map_set(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    maptable_set(&buf->t, args[1], args[2]);
    return value_int(0);
}

/* `add` over a MutableSet: each of `items` put in, answering whether any of them was new, so a
 * caller can dedupe without a `contains` first. An element already present is skipped rather than
 * rewritten, which is equivalent -- a set's values are unused and overwriting a key keeps its
 * place -- and cheaper.
 *
 * **This is `add`'s builder member itself since 2026-10-22, not a primitive underneath it.** The
 * declaration `internal native fn mutable_set_add(s, item)` is gone: `add` was an ordinary Ziman
 * loop calling it once per element, which meant the prelude carried a second spelling of its own
 * vocabulary for the single purpose of being looped over. The symbol fallback reads parameter one,
 * so `add` on a `MutableSet` lands on this registration anyway -- the C name keeps exactly what
 * the Ziman name gave up, which is what that fallback is for.
 *
 * `args[1]` is one array however the call was written, `collapse_variadic_args` having run before
 * any native is entered; `#[at_least_one]` is what makes `add(s)` a compile error, and a spread of
 * an empty array is a legal call that adds nothing and answers false.
 *
 * The buffer pointer is fetched once and held across the loop, as `ziman_mutable_set_combine`
 * does: `maptable_set` grows the table's own storage, not the struct that owns it. */
static Value native_mutable_set_add(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    const ArrayObj *items = args[1].as.arr;
    int added_any = 0;
    /* One lookup per element, not two: the table reports whether the key was new, where the trie
     * needed a `get` first to answer the same question. */
    for (int i = 0; i < items->count; i++) {
        if (maptable_set(&buf->t, array_get(items, i), value_int(0))) added_any = 1;
    }
    return value_bool(added_any);
}

static Value native_mutable_map_get(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    Value found;
    if (maptable_get(&buf->t, args[1], &found)) return value_enum(type_option_ref(), enum_obj_new("Option", "Some", 1, found));
    return value_enum(type_option_ref(), enum_obj_option_none());
}

/* `mutable_map_get_or` as a native rather than Ziman sugar over `mutable_map_get`, which is what it
 * was. The sugar version switched on the `Option` the native above builds, so the one call whose
 * whole point is to avoid allocating a wrapper allocated one anyway -- and measured *slower* than
 * the `Option`-returning `get` it wraps (580-612 ns/op against 534-572 on a 100,000-entry map),
 * where the immutable `Map`'s own `map_get_or` (a builtin in interpreter.c, no wrapper) is 5-6x
 * faster than its `map_get`. Same shape as that builtin now. The idiom this exists for is the
 * word-count line in lib/prelude/map.zm's own comment, which runs it once per input word. */
static Value native_mutable_map_get_or(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    Value found;
    if (maptable_get(&buf->t, args[1], &found)) return found;
    return args[2];
}

/* A map's remove answers what was there, as `Option<V>`: the entry is otherwise gone and
 * recovering it would need a `get` first. A set's answers whether it was there at all, since its
 * values are unused -- which is why these can no longer share one native as they did when both
 * returned nothing. */

/* `remove` without the `Option`. Same lookup, same tombstone; the only difference is that it does
 * not have to box what it removed, which on a large map is most of the cost -- see map.zm's own
 * comment for the figures, and ROADMAP.md's Map/Set entry for why an allocation's price is set by
 * the live set rather than by its own size. */
static Value native_mutable_map_discard(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    return value_bool(maptable_remove(&buf->t, args[1], NULL));
}




static Value native_mutable_map_snapshot(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    /* Builds the trie rather than copying one. The trie's own snapshot copied its nodes and shared
     * its leaves, which was cheaper per snapshot than inserting every entry -- the cost moved here
     * when the builder stopped being a trie, and it is the one operation this change makes slower.
     * `shared` is gone with it: this MapObj is freshly built and nobody else holds it. */
    MapObj *m = mapobj_mut_new();
    Value *keys, *vals;
    int n = maptable_entries(&buf->t, &keys, &vals);
    for (int i = 0; i < n; i++) mapobj_set_mut(m, keys[i], vals[i], 0);
    return value_map(type_scalar_ref(TY_VOID), m);
}

/* `len` on one of the four `#[thread_bound]` handles, for the interpreter's own `len` branch --
 * the same shape as ziman_array_builder_length above, which `for x in ab` (F96) already uses, and
 * placed here rather than beside it because `mutable_map_get_buf` is defined above this line and not that
 * one. The typechecker has already established that `v` is one of the four, so this only has to
 * pick the table; each accessor takes the handle's own thread-ownership check, so a builder read
 * from the wrong thread fails there rather than here. MutableMap and MutableSet share a buffer
 * kind, which is why one branch covers both. */
int ziman_builder_length(Value v, MutableKind kind) {
    /* `kind` comes from the caller, which already had to classify the value to get here -- see
     * prelude.h's ziman_mutable_kind on why re-deriving it from the name is what this used to cost. */
    if (kind == MUTABLE_ARRAY) return (int)mutable_array_table_get(mutable_array_handle_of(v))->length;
    if (kind == MUTABLE_STRING) return (int)mutable_string_table_get(mutable_string_handle_of(v))->length;
    return (int)maptable_len(&mutable_map_get_buf(v)->t);
}

/* `contains` on one of the four handles, for the interpreter's own `contains` branch. Answers the
 * same question each immutable counterpart does: a substring for a MutableString, a key for a
 * MutableMap, an element for a MutableSet, and an O(n) scan for a MutableArray -- which is the
 * one cost difference worth knowing, the other three being O(1) or a search over bytes.
 * `value_key_equals` rather than `==`, matching call_contains's array branch exactly: `contains`
 * is one builtin and must answer "is this present" the same way whichever container it is given,
 * so a NaN appended to a builder is found just as a NaN in a Set is. */
int ziman_builder_contains(Value v, Value needle, MutableKind kind) {
    /* `kind` from the caller, same reason as ziman_builder_length above. */
    if (kind == MUTABLE_ARRAY) {
        const MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(v));
        for (size_t i = 0; i < buf->length; i++) {
            if (value_key_equals(mutable_array_element_at(buf, i), needle)) return 1;
        }
        return 0;
    }
    if (kind == MUTABLE_STRING) {
        const MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(v));
        return value_str_find(buf->data, buf->length, needle.as.str.ptr, needle.as.str.len, 0) >= 0;
    }
    return maptable_get(&mutable_map_get_buf(v)->t, needle, NULL) != 0;
}

/* One append, so that `push_all` does not repeat the adopt/grow/store dance per element. */
static void mutable_array_push_one(MutableArrayBuf *buf, Value element) {
    mutable_array_adopt_pack_kind(buf, element);
    mutable_array_ensure_capacity(buf, 1);
    if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)buf->length, element);
    else buf->items[buf->length] = element;
    buf->length++;
}

/* `push_all(xs, xs)` is **not** a no-op -- doubling an array is a perfectly reasonable thing to
 * mean -- so the source length is read once before the first append. Without that the loop would
 * chase a source growing under it and never terminate. This is the first of the three different
 * answers self-aliasing gets across the mutable types; the other two are below. */
void ziman_mutable_array_push_all(Value xs, Value source, int source_is_handle) {
    MutableArrayBuf *dst = mutable_array_table_get(mutable_array_handle_of(xs));
    if (source_is_handle) {
        const MutableArrayBuf *src = mutable_array_table_get(mutable_array_handle_of(source));
        size_t n = src->length;
        for (size_t i = 0; i < n; i++) mutable_array_push_one(dst, mutable_array_element_at(src, i));
        return;
    }
    ArrayObj *src = source.as.arr;
    for (int i = 0; i < src->count; i++) mutable_array_push_one(dst, array_get(src, i));
}

/* `set_all(m, m)` is a no-op by definition -- every key already maps to its own value -- and the
 * early return is not just an optimisation: without it this would be iterating a table it is
 * writing to. The second of the three aliasing answers. */
void ziman_mutable_map_set_all(Value m, Value source, int source_is_handle) {
    MutableMapBuf *dst = mutable_map_get_buf(m);
    const MapObj *src;
    Value *keys, *vals;
    int n;
    if (source_is_handle) {
        MutableMapBuf *from = mutable_map_get_buf(source);
        if (from == dst) return;
        n = maptable_entries(&from->t, &keys, &vals);
    } else {
        src = source.as.map;
        n = src->count;
        mapobj_entries(src, &keys, &vals);
    }
    for (int i = 0; i < n; i++) maptable_set(&dst->t, keys[i], vals[i]);
}

/* Union, intersect and difference in place. `op` is 0, 1, 2 in that order.
 *
 * The third aliasing answer, and the one that is not a no-op: `difference(s, s)` **empties** the
 * set, and doing that by walking the trie while removing from it is exactly what must not happen --
 * so it is recognised and turned into a fresh empty trie, the same thing `clear` does. Union and
 * intersect with yourself are genuine no-ops.
 *
 * Intersect and difference remove from the destination, so they walk a snapshot of its own keys
 * (materialised anyway) rather than the live table. */
void ziman_mutable_set_combine(Value s, Value source, int source_is_handle, int op) {
    MutableMapBuf *dst = mutable_map_get_buf(s);
    MutableMapBuf *from = NULL;
    if (source_is_handle) {
        from = mutable_map_get_buf(source);
        if (from == dst) {
            if (op == 2) maptable_clear(&dst->t);
            return;
        }
    }
    if (op == 0) {
        Value *keys;
        int n = from != NULL ? maptable_entries(&from->t, &keys, NULL) : source.as.map->count;
        if (from == NULL) mapobj_entries(source.as.map, &keys, NULL);
        for (int i = 0; i < n; i++) maptable_set(&dst->t, keys[i], value_int(0));
        return;
    }
    Value *mine;
    int n = maptable_entries(&dst->t, &mine, NULL);
    for (int i = 0; i < n; i++) {
        /* `mapobj_get` writes through its out-parameter unconditionally, unlike maptable_get,
         * so it gets somewhere to write even though the value is unused here. */
        Value ignored;
        int in_source = from != NULL ? maptable_get(&from->t, mine[i], NULL)
                                     : mapobj_get(source.as.map, mine[i], &ignored);
        if (op == 1 ? !in_source : in_source) maptable_remove(&dst->t, mine[i], NULL);
    }
}

/* Reads only, so no aliasing question: a set is its own subset, which the identity check answers
 * without walking anything. */
int ziman_mutable_set_is_subset(Value s, Value source, int source_is_handle) {
    MutableMapBuf *dst = mutable_map_get_buf(s);
    MutableMapBuf *from = source_is_handle ? mutable_map_get_buf(source) : NULL;
    if (from == dst) return 1;
    Value *mine;
    int n = maptable_entries(&dst->t, &mine, NULL);
    for (int i = 0; i < n; i++) {
        Value ignored; /* mapobj_get always writes through this; see ziman_mutable_set_combine. */
        int in_source = from != NULL ? maptable_get(&from->t, mine[i], NULL)
                                     : mapobj_get(source.as.map, mine[i], &ignored);
        if (!in_source) return 0;
    }
    return 1;
}

/* The `from` constructors: a handle holding what the argument holds, taking either world. The
 * typechecker has already established which, so these only have to read it.
 *
 * For the two hashed shapes this used to be where `from` earned its name over `new` plus a loop:
 * when a builder was a trie, `mapobj_snapshot` copied its nodes and shared its leaves, where
 * re-inserting every entry hashes and places each one again. A builder is a flat table now, so
 * there is no trie to copy into it and this inserts -- the same cost `to_map` moved to for the same
 * reason, and the other half of that trade. The source is untouched either way, which is what
 * `from` promises. */
Value ziman_mutable_map_from(Value source, int is_set) {
    Value out = mutable_map_new_value(is_set ? "MutableSet" : "MutableMap");
    MutableMapBuf *buf = mutable_map_get_buf(out);
    Value *keys, *vals;
    int n;
    if (source.type->kind == TY_MAP || source.type->kind == TY_SET) {
        n = source.as.map->count;
        mapobj_entries(source.as.map, &keys, &vals);
    } else {
        n = maptable_entries(&mutable_map_get_buf(source)->t, &keys, &vals);
    }
    for (int i = 0; i < n; i++) maptable_set(&buf->t, keys[i], vals[i]);
    return out;
}

/* A set from an array has to insert each element rather than copy a trie, because the array may
 * repeat one and a set may not -- the deduplication is the whole difference between this and
 * `mutable_set_from`, and the reason it is a separate name rather than another overload. */
Value ziman_mutable_set_from_array(Value source, int source_is_handle) {
    Value out = mutable_map_new_value("MutableSet");
    MutableMapBuf *buf = mutable_map_get_buf(out);
    if (source_is_handle) {
        const MutableArrayBuf *src = mutable_array_table_get(mutable_array_handle_of(source));
        for (size_t i = 0; i < src->length; i++) maptable_set(&buf->t, mutable_array_element_at(src, i), value_int(0));
    } else {
        ArrayObj *src = source.as.arr;
        for (int i = 0; i < src->count; i++) maptable_set(&buf->t, array_get(src, i), value_int(0));
    }
    return out;
}

/* The entries of a MutableMap or MutableSet in insertion order, for the interpreter's own `for`
 * branch: fresh arrays of `count` keys and, for a map, values. This is a *snapshot by
 * construction*, which is the difference from MutableArray's live-buffer walk: the body may change
 * the handle freely without affecting what the loop visits. It stays a snapshot by choice now
 * rather than by necessity -- the table's dense entries array *is* in insertion order, so a cursor
 * over it would be possible, where the trie needed a per-entry sequence number and a sort. Keeping
 * the copy keeps the existing guarantee. Pass NULL for `vals` on a set, whose values are unused. */
int ziman_mutable_map_entries(Value m, Value **keys, Value **vals) {
    return maptable_entries(&mutable_map_get_buf(m)->t, keys, vals);
}

/* A MutableArray holding a copy of what the argument holds. One allocation and one pass whichever
 * world the source is, where `new` plus a loop of pushes would regrow the buffer as it went. */
Value ziman_mutable_array_from(Value source, int source_is_handle) {
    size_t n = source_is_handle ? mutable_array_table_get(mutable_array_handle_of(source))->length : (size_t)source.as.arr->count;
    Value out = mutable_array_new_value(n > 0 ? n : 1, 2.0);
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(out));
    /* The buffer is reserved, not allocated, until the first element says which representation
     * to use -- and the loops below write straight into it rather than going through
     * mutable_array_push_one, so the one allocation this function promises has to be asked for here. */
    if (n > 0) {
        Value first = source_is_handle ? mutable_array_element_at(mutable_array_table_get(mutable_array_handle_of(source)), 0)
                                       : array_get(source.as.arr, 0);
        mutable_array_adopt_pack_kind(buf, first);
        mutable_array_ensure_capacity(buf, n);
    }
    if (source_is_handle) {
        const MutableArrayBuf *src = mutable_array_table_get(mutable_array_handle_of(source));
        for (size_t i = 0; i < src->length; i++) {
            Value element = mutable_array_element_at(src, i);
            mutable_array_adopt_pack_kind(buf, element);
            if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)i, element);
            else buf->items[i] = element;
            buf->length++;
        }
    } else {
        ArrayObj *src = source.as.arr;
        for (int i = 0; i < src->count; i++) {
            Value element = array_get(src, i);
            mutable_array_adopt_pack_kind(buf, element);
            if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, i, element);
            else buf->items[i] = element;
            buf->length++;
        }
    }
    return out;
}

/* Exchanges two elements of a builder, in place. Was three lines of Ziman over
 * `mutable_array_get`/`_set` until 2026-10-18, and moved into C for the messages rather than for
 * the speed: `swap(b, 0, 9)` reported "get index 9 out of bounds", naming a function the author
 * had not written, because the rewrite landed on sugar that called one. `swap` reports the range
 * it was given now. (F209 took that hazard out at the root -- element access is an index rather
 * than a call, so no sugar lands on a name -- but moving this into C was right anyway, for the
 * pack-awareness below.) Pack-aware, so a packed buffer exchanges by its element stride.
 *
 * Reached straight from the builtin's arm rather than through a registered native: the prefixed
 * `mutable_array_swap` was removed once `swap` covered both worlds, so there is no name for a
 * rewrite to land on and none is needed. */
Value ziman_swap_mutable_array(Value b, int64_t i, int64_t j, int line, int col) {
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(b));
    if (i < 0 || (size_t)i >= buf->length || j < 0 || (size_t)j >= buf->length) {
        ziman_error("runtime", line, col, "swap index %lld/%lld out of bounds (MutableArray length %llu)",
                    (long long)i, (long long)j, (unsigned long long)buf->length);
    }
    Value at_i = mutable_array_element_at(buf, (size_t)i);
    Value at_j = mutable_array_element_at(buf, (size_t)j);
    if (buf->pkind != PK_NONE) {
        pack_store(buf->pkind, buf->pdata, (int)i, at_j);
        pack_store(buf->pkind, buf->pdata, (int)j, at_i);
    } else {
        buf->items[i] = at_j;
        buf->items[j] = at_i;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

/* `narrow(b, start, end)`: keep only that window, in place, answering nothing. The in-place
 * counterpart of `slice`, which is a query and so copies -- see the split that keeps mutations and
 * queries apart in docs/SYNTAX.md.
 *
 * It replaces `mutable_array_truncate(b, n)`, which is `narrow(b, 0, n)`, and adds the half that
 * one could not do: dropping a *prefix*. That was `remove(b, 0)` in a loop, O(n) per
 * call and so quadratic, against one memmove here. The move is pack-aware through
 * mutable_array_move_elements, the same helper insert and remove shift with, so a packed buffer
 * moves by its element stride rather than by sizeof(Value). */
Value ziman_narrow_mutable_array(Value b, int64_t start, int64_t end, int line, int col) {
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(b));
    if (start < 0 || end < start || (size_t)end > buf->length) {
        ziman_error("runtime", line, col,
                    "narrow: range %lld..%lld is out of bounds for a MutableArray of length %llu",
                    (long long)start, (long long)end, (unsigned long long)buf->length);
    }
    mutable_array_move_elements(buf, 0, (size_t)start, (size_t)(end - start));
    buf->length = (size_t)(end - start);
    return value_int(0); /* void return -- the caller never uses this */
}

/* slice<T>(b, start, end) over a builder, which **must copy** where the array form does not: a
 * view would share a buffer that can still be pushed to, written through or truncated underneath
 * it. So the cost differs by receiver even though the answer does not -- free there, O(end - start)
 * here -- and the copy is the window only. That is the whole point of taking either world:
 * `slice(snapshot(b), s, e)` copies the *entire* builder first, since snapshot is O(n) and the
 * slice of it is then free, so a three-element window of a ten-thousand-element builder copied ten
 * thousand elements. */
/* **`len` over all nine types, one implementation, registered under the bare name.** §11 of
 * docs/design/full-overloading.md predicted exactly this: "all nine would be named `len`, so all
 * nine find the one registration: no new implementation, no new C code". It is the one place where
 * registering under the bare name is right rather than a hazard -- F213 had to *prefix* two
 * `index_of` registrations because the two members wanted different code, and here all nine want
 * the same, so `native_lookup("len")` answering for every one of them is the point.
 *
 * The body is `B_len`'s, which dispatched on the runtime type for the same reason: a length is one
 * question and the nine declarations say only which types may be asked it. */
Value ziman_len(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value v = args[0];
    /* **Text answers its code-point count since 2026-10-23**, where it answered bytes before and
     * `char_count` answered this. `byte_len` is the byte question now. The other seven members are
     * unchanged: an element count is an element count, and this is the one member where "how many"
     * had two defensible answers. */
    if (v.type->kind == TY_STRING) {
        /* The byte count *is* the character count with no high bit anywhere, so the common case is
         * the field read it always was. Without this the loop condition `i < len(s)` would rescan
         * the whole string per iteration -- the second of the two quadratic factors. */
        if (v.as.str.flags & ZSTR_ASCII) return value_int((int64_t)v.as.str.len);
        return value_int(utf8_code_points((const uint8_t *)v.as.str.ptr, v.as.str.len, "len", line, col));
    }
    if (v.type->kind == TY_MAP || v.type->kind == TY_SET) return value_int(v.as.map->count);
    /* One of the four builders, or a cell: a struct over a thread-bound handle, so the length
     * lives in the handle's table rather than in the struct.
     *
     * **A cell no longer arrives here**, its `len` having become ordinary Ziman that returns 1
     * (lib/prelude/mutable.zm, 2026-10-22), so eight of the nine declarations reach this and the
     * ninth does not. The `MUTABLE_CELL` test stays anyway: `ziman_builder_length` falls through
     * to `mutable_map_get_buf` for a kind it does not recognise, so dropping it would turn a
     * stray cell into a misread map rather than a loud failure. One comparison against a silent
     * wrong answer. */
    if (v.type->kind == TY_STRUCT) {
        MutableKind kind = ziman_mutable_kind(v.type);
        if (kind == MUTABLE_CELL) return value_int(1);
        /* A MutableString follows its immutable twin: code points, not the buffer's size -- and
         * the same fast path, from the flag the buffer maintains for want of a Value to hold it. */
        if (kind == MUTABLE_STRING) {
            MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(v));
            if (buf->ascii) return value_int((int64_t)buf->length);
            return value_int(utf8_code_points((const uint8_t *)buf->data, buf->length, "len", line, col));
        }
        return value_int(ziman_builder_length(v, kind));
    }
    return value_int(v.as.arr->count);
}

/* **One shim per declaration, for the names F215 took off the builtin ids.** Each is the body the
 * `else if (bid == B_x)` branch had, with the world chosen by *which declaration* was resolved
 * rather than by testing `args[0].type->kind` at run time -- which is the whole of what moving
 * from one id to two declarations buys at this layer. None of them sets the result's element type
 * and none has to: the `call_fn` path fixes up a generic native's return from `e->resolved_type`
 * (interpreter.c), exactly as the branches did by hand. */
static Value native_narrow_mutable_array(Value *args, int argc, int line, int col) {
    (void)argc;
    return ziman_narrow_mutable_array(args[0], args[1].as.i, args[2].as.i, line, col);
}

static Value native_array_to_mutable_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return ziman_mutable_set_from_array(args[0], 0);
}

static Value native_mutable_array_to_mutable_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return ziman_mutable_set_from_array(args[0], 1);
}

/* `to_set` over either world, in one pass. The builder is read element by element through
 * `ziman_array_builder_at` rather than snapshotted first, which would make the conversion two
 * copies instead of one -- the reason the builder member exists at all. */
static Value native_array_to_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MapObj *m = mapobj_empty();
    for (int i = 0; i < args[0].as.arr->count; i++) m = mapobj_set(m, array_get(args[0].as.arr, i), value_int(0));
    return value_map(type_scalar_ref(TY_VOID), m);
}

static Value native_mutable_array_to_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MapObj *m = mapobj_empty();
    int n = ziman_array_builder_length(args[0]);
    for (int i = 0; i < n; i++) m = mapobj_set(m, ziman_array_builder_at(args[0], i), value_int(0));
    return value_map(type_scalar_ref(TY_VOID), m);
}

/* `push`'s array member, F216. The variadic tail arrives as one array because
 * `collapse_variadic_args` runs on the `call_fn` path -- which is the whole of what F207's
 * `variadic_tail_is_array_spread` dance in `B_push` existed to work around, so converting the
 * member deletes it rather than porting it. Each fold takes the buffer's next slot in place when
 * the array it is given is the tip and there is room, which every result after the first one is,
 * so appending k values costs about k and not k copies. */
Value ziman_array_push_values(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value out = args[0];
    const ArrayObj *vals = args[1].as.arr;
    for (int i = 0; i < vals->count; i++) {
        out = value_array(type_scalar_ref(TY_VOID), array_push(out.as.arr, array_get(vals, i)));
    }
    return out;
}

/* The one-value `push(a, x)`, which is how almost every call to the array half is written, before
 * `collapse_variadic_args` allocates a one-element array to carry it. **This is the whole of what
 * F216's conversion cost and the whole of the fix**: measured without it, `a = push(a, i)` two
 * million times ran 65% slower than the builtin id had, all of it that temporary. The builder half
 * has had the same shortcut since D18 (`ziman_mutable_array_push_one`), and for the same reason;
 * this is its twin, so neither world pays for a variadic it did not use. */
Value ziman_array_push_one(Value arr, Value x) {
    return value_array(type_scalar_ref(TY_VOID), array_push(arr.as.arr, x));
}

/* `insert`'s array member, F216: the body `call_insert` had in interpreter.c, which used the
 * call's resolved type for nothing but tagging its result -- and the `call_fn` path tags a generic
 * native's result itself. `index` may equal the length, which appends. */
static Value native_array_insert(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *old = args[0].as.arr;
    int64_t i = args[1].as.i;
    if (i < 0 || i > old->count) {
        ziman_error("runtime", line, col, "insert index %lld out of bounds (array length %d)",
                    (long long)i, old->count);
    }
    if (array_pack_kind(old) != PK_NONE) {
        return value_array(type_scalar_ref(TY_VOID), array_packed_edit(old, 1, (int)i, args[2]));
    }
    int new_count = old->count + 1;
    Value *items = ziman_xmalloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->boxed_items, (size_t)i * sizeof(Value));
    items[i] = args[2];
    memcpy(items + i + 1, old->boxed_items + i, (size_t)(old->count - (int)i) * sizeof(Value));
    ArrayObj *packed = old->count == 0 ? array_try_pack(items, new_count) : NULL;
    return value_array(type_scalar_ref(TY_VOID), packed != NULL ? packed : array_obj_new(items, new_count));
}

/* `swap`'s array member answers a copy with the two exchanged, in one allocation where
 * `set(set(a, i, a[j]), j, a[i])` made two; the builder member writes in place and answers
 * nothing. The prefix on this symbol is the only `mutable_array_swap` left anywhere -- the
 * declaration of that name went when `swap` covered both worlds. */
static Value native_array_swap(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *old = args[0].as.arr;
    int64_t i = args[1].as.i, j = args[2].as.i;
    if (i < 0 || i >= old->count || j < 0 || j >= old->count) {
        ziman_error("runtime", line, col, "swap index %lld/%lld out of bounds (array length %d)",
                    (long long)i, (long long)j, old->count);
    }
    return value_array(type_scalar_ref(TY_VOID), array_swapped(old, (int)i, (int)j));
}

static Value native_mutable_array_swap(Value *args, int argc, int line, int col) {
    (void)argc;
    return ziman_swap_mutable_array(args[0], args[1].as.i, args[2].as.i, line, col);
}

/* `slice`'s two members, native since F214. The implementations below are unchanged and were
 * reached from `B_slice`'s arm until then; these are the three-argument shims the resolver's path
 * needs. Neither sets the result's element type, and neither has to: the `call_fn` path fixes up a
 * generic native's return type from `e->resolved_type` (interpreter.c), which is the same thing the
 * arm did by hand. */
static Value native_array_slice(Value *args, int argc, int line, int col) {
    (void)argc;
    return ziman_slice_array(args[0], args[1].as.i, args[2].as.i, line, col);
}

static Value native_mutable_array_slice(Value *args, int argc, int line, int col) {
    (void)argc;
    return ziman_slice_mutable_array(args[0], args[1].as.i, args[2].as.i, line, col);
}

Value ziman_slice_mutable_array(Value b, int64_t start, int64_t end, int line, int col) {
    const MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(b));
    if (start < 0 || end < start || (size_t)end > buf->length) {
        ziman_error("runtime", line, col,
                    "slice: range %lld..%lld is out of bounds for a MutableArray of length %llu",
                    (long long)start, (long long)end, (unsigned long long)buf->length);
    }
    int total = (int)(end - start);
    Value *items = total > 0 ? ziman_xmalloc((size_t)total * sizeof(Value)) : NULL;
    for (int i = 0; i < total; i++) items[i] = mutable_array_element_at(buf, (size_t)start + (size_t)i);
    ArrayObj *packed = array_try_pack(items, total);
    if (packed != NULL) return value_array(type_scalar_ref(TY_VOID), packed);
    return value_array(type_scalar_ref(TY_VOID), array_obj_new(items, total));
}

/* A MutableArray of `length` copies of one value. The gap this fills is that **capacity is not
 * length**: `mutable_array_new(capacity: 5)` reserves five slots and has `len` 0, so `b[0] = v` is
 * a runtime out-of-bounds and the only way to get addressable slots was to push them one at a time.
 * Nothing else in the language could say "n slots I can index" -- there is no `array_fill` or
 * `array_repeat` either, so the immutable side still goes through `snapshot` of one of these.
 *
 * `length` is required and so is `value`, which is the point of a second constructor rather than a
 * `fill` parameter on `mutable_array_new`: that one's parameter would mean *capacity* without a
 * fill and *length* with one, and an argument whose meaning depends on whether a later argument was
 * passed is the shape this library has been removing. Both required also means it calls
 * positionally, `mutable_array_filled(5, 0)`, where an Option parameter would have forced a named
 * call. */
static Value native_mutable_array_filled(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t length = args[0].as.i;
    if (length < 0) {
        ziman_error("runtime", line, col, "mutable_array_filled: length %lld is negative", (long long)length);
    }
    Value out = mutable_array_new_value(length > 0 ? (size_t)length : 1, 2.0);
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(out));
    if (length > 0) {
        mutable_array_adopt_pack_kind(buf, args[1]);
        mutable_array_ensure_capacity(buf, (size_t)length);
        for (int64_t i = 0; i < length; i++) {
            if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, (int)i, args[1]);
            else buf->items[i] = args[1];
        }
        buf->length = (size_t)length;
    }
    return out;
}

/* A MutableArray straight from already-evaluated elements, for `mutable([1, 2])`. The same loop as
 * `ziman_mutable_array_from`'s value branch, reading a plain `Value *` instead of an ArrayObj,
 * which is the whole point: the checker hands the literal's elements over as ordinary call
 * arguments -- the shape `$map_lit` has always used -- so the intermediate array is never built.
 * Measured against the `$mutable_array_from` path it drops two allocations and 80 bytes per
 * construction: the ArrayObj (src/value.c:147) and its packed buffer (src/value.c:245). The
 * literal's own `Value[]` is not a saving, the `args` array of any call replacing it. */
Value ziman_mutable_array_lit(const Value *items, int n) {
    Value out = mutable_array_new_value(n > 0 ? (size_t)n : 1, 2.0);
    MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(out));
    if (n > 0) {
        mutable_array_adopt_pack_kind(buf, items[0]);
        mutable_array_ensure_capacity(buf, (size_t)n);
    }
    for (int i = 0; i < n; i++) {
        mutable_array_adopt_pack_kind(buf, items[i]);
        if (buf->pkind != PK_NONE) pack_store(buf->pkind, buf->pdata, i, items[i]);
        else buf->items[i] = items[i];
        buf->length++;
    }
    return out;
}

/* `snapshot` on one of the four handles: the same O(n) copy out that `snapshot`,
 * `snapshot`, `snapshot` and `snapshot` have always done, reached by one name instead of
 * four. Nothing new happens here -- it dispatches to those very implementations -- so a builder
 * stays usable afterwards and a later change does not touch the copy, exactly as before.
 *
 * The result's own type is left for the caller to set from the call's resolved type, which is what
 * these natives already require: native_mutable_map_snapshot returns a map tagged TY_VOID and the generic
 * natives are patched the same way in the interpreter's plain-call path. */
Value ziman_builder_snapshot(Value v) {
    const char *name = v.type->struct_name;
    if (name != NULL && strcmp(name, "MutableArray") == 0) return native_mutable_array_to_array(&v, 1, 0, 0);
    if (name != NULL && strcmp(name, "MutableString") == 0) return native_mutable_string_to_string(&v, 1, 0, 0);
    return native_mutable_map_snapshot(&v, 1, 0, 0);
}

/* **`concat`'s three shims, one per world.** Each takes its variadic tail already collapsed into
 * one array, which is what the builtin branch had to assemble by hand -- and the lone-array spread
 * it had to re-derive from `e->resolved_type` is now the checker's `variadic_tail_is_array_spread`,
 * read once by `collapse_variadic_args`.
 *
 * `concat` keeps its `check_call` arm, unlike every other converted name, because its choice is not
 * one the resolver can make: `concat()` is `""` and both forms accept no arguments, and
 * `concat(someStrings)` must answer a string where the array form would answer `[string]` -- two
 * readings that differ only in what they return, which F148 ruled out as a basis for choosing. So
 * the arm chooses and records the declaration on `call_fn`, and the call runs as an ordinary
 * native from there. The builtin id is gone; what stayed is the *decision*, which is the part the
 * resolver was never able to make. */
Value ziman_string_concat(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return ziman_concat_strings(args[0].as.arr);
}

Value ziman_array_concat(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return ziman_concat_arrays(args[0].as.arr);
}

/* The builder half: every part appended to `args[0]` in place, answering nothing. **One symbol
 * serves both of its declarations and a mixed tail**, which is the third thing the resolver could
 * not have done: `concat(b, [5], mutable([6]), [7])` mixes worlds in one variadic tail, a
 * declaration's variadic parameter has one type, and a union type is refused outright. The arm
 * type-checks each part against either world, and here each part's own runtime type says whether
 * it is a handle -- so the mixing is checked where it can be and dispatched where it is cheap. */
Value ziman_mutable_array_concat(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const ArrayObj *parts = args[1].as.arr;
    for (int i = 0; i < parts->count; i++) {
        Value part = array_get(parts, i);
        ziman_mutable_array_push_all(args[0], part, part.type->kind == TY_STRUCT);
    }
    return value_int(0); /* void return */
}

/* `items` over a `Set<T>`: its elements as an array, in insertion order. A Set is a MapObj whose
 * values are placeholders, so the elements are its keys. The MutableSet member of the same name is
 * ordinary Ziman beside its declaration; this is the immutable half, which shared a builtin id
 * with `values` until the name was converted. */
/* `to_mutable_array` over either set world: the elements as a builder rather than as an immutable
 * array, which is the cross-world half of the pair `to_array` opens.
 *
 * **Native rather than `mutable(to_array(s))`**, which is what it would be in Ziman and would
 * allocate twice -- an immutable array to throw away, then the builder. `to_mutable_set` is native
 * for the same reason and this mirrors it: `array_to_mutable_set` is to `to_set` what this is to
 * `to_array`.
 *
 * One pass over the set's keys straight into a MutableArray, through the same
 * `ziman_mutable_array_lit` that builds a `MutableArray<T> { ... }` literal -- it takes a raw
 * `Value *` and a count, which is exactly the shape `mapobj_entries` and `maptable_entries` hand
 * back. The element type is patched by the caller, as it is for any generic native. */
static Value native_set_to_mutable_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value *keys;
    mapobj_entries(args[0].as.map, &keys, NULL);
    return ziman_mutable_array_lit(keys, args[0].as.map->count);
}

/* The builder-set half. A MutableSet is a MutableMapBuf, so its elements come from `maptable_entries`
 * where the immutable form reads a MapObj -- the one difference between the two, and the reason the
 * symbol fallback needs both registrations. */
static Value native_mutable_set_to_mutable_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    Value *keys;
    int n = maptable_entries(&buf->t, &keys, NULL);
    return ziman_mutable_array_lit(keys, n);
}

/* **A MapObj's keys as an array, which is two declarations' worth of question.** A Set is a MapObj
 * whose values are placeholders, so `to_array(s)` over an immutable Set and `keys_array(m)` over an
 * immutable Map are the same walk -- the elements of one are the keys of the other. One function,
 * registered under `set_to_array` and `map_keys_array`.
 *
 * It was `ziman_set_items` and then `ziman_set_to_array` earlier the same day; the neutral name is
 * the honest one once a second declaration reaches it, since neither "set" nor "to_array" is true
 * of both callers.
 *
 * `array_try_pack` is what makes this worth being native for a packable key type: the keys come
 * back as a `Value *` and a packed buffer is unscanned by the collector, so a `[int]` of keys
 * costs no pointer scanning at all. */
/* The tail all three of these share: wrap `n` already-gathered keys as an immutable array, packed
 * when the element type allows it. `array_try_pack` is the part worth being native for -- a packed
 * buffer is unscanned by the collector, so an array of `int` keys costs no pointer scanning at
 * all -- and it is the same tail whether the keys came from a MapObj or a MutableMapBuf. The
 * element type is patched by the caller, as for any generic native. */
static Value keys_as_array_tail(Value *keys, int n) {
    ArrayObj *packed = array_try_pack(keys, n);
    return value_array(type_scalar_ref(TY_VOID), packed != NULL ? packed : array_obj_new(keys, n));
}

Value ziman_keys_as_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value *keys;
    mapobj_entries(args[0].as.map, &keys, NULL);
    return keys_as_array_tail(keys, args[0].as.map->count);
}

/* The builder-set half of `to_array`, `native fn` since 2026-10-22. It was ordinary Ziman -- a
 * MutableArray built by pushing every element and then snapshotted -- which is three passes and two
 * buffers where this is one of each: `maptable_entries` already hands back the elements contiguous,
 * so there is nothing to push into.
 *
 * A MutableSet is a MutableMapBuf, which is the only difference from `ziman_keys_as_array` above:
 * the elements come from the table rather than from a MapObj, and the tail is shared. */
static Value native_mutable_set_to_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    Value *keys;
    int n = maptable_entries(&buf->t, &keys, NULL);
    return keys_as_array_tail(keys, n);
}

/* `contains` over all nine types it accepts, registered under the bare name: one question, and
 * the nine declarations say only which types may be asked it. This was `call_contains` in
 * interpreter.c until the name came off its builtin id; it sits here now, beside
 * `ziman_builder_contains` and `ziman_cell_contains`, which it calls. */
Value ziman_contains(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value first = args[0], second = args[1];
    if (first.type->kind == TY_STRING) {
        return value_bool(value_str_find(first.as.str.ptr, first.as.str.len, second.as.str.ptr, second.as.str.len, 0) >= 0);
    }
    if (first.type->kind == TY_MAP || first.type->kind == TY_SET) {
        Value ignored;
        return value_bool(mapobj_get(first.as.map, second, &ignored));
    }
    /* One of the four builders, or a `Mutable<T>` cell: the contents live behind the handle, not
     * in the struct. */
    if (first.type->kind == TY_STRUCT) {
        MutableKind kind = ziman_mutable_kind(first.type);
        if (kind == MUTABLE_CELL) return value_bool(ziman_cell_contains(first, second));
        return value_bool(ziman_builder_contains(first, second, kind));
    }
    ArrayObj *arr = first.as.arr;
    if (arr->bytes != NULL) {
        if (second.type->kind != TY_BYTE) return value_bool(0);
        return value_bool(memchr(arr->bytes, second.as.by, (size_t)arr->count) != NULL);
    }
    for (int i = 0; i < arr->count; i++) {
        /* value_key_equals, matching the Map/Set branch a dozen lines up: `contains` is one builtin
         * and must answer "is this present" the same way whichever container it is given, so a NaN
         * stored in an array is found exactly as a NaN stored in a Set is. */
        if (value_key_equals(array_get(arr, i), second)) return value_bool(1);
    }
    return value_bool(0);
}

/* `snapshot` over the four builders, registered under the bare name: every one of the four
 * declarations wants this same dispatch, as `len`'s nine do. The result's type comes from the
 * call's own `resolved_type` on the `call_fn` path -- the natives below tag theirs TY_VOID or a
 * placeholder element type -- except for the MutableString member, which has no type parameter for
 * that patch to fire on and so tags its own `string` correctly here. */
Value ziman_snapshot(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return ziman_builder_snapshot(args[0]);
}

/* `clear` over the same four, and `contains` over all nine types it accepts -- both one
 * implementation under the bare name for the same reason. */
Value ziman_clear(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ziman_builder_clear(args[0]);
    return value_int(0); /* void return, as `send` and the other effects do */
}

/* `clear` on one of the four handles: empty it and leave it usable. A MutableArray or a
 * MutableString keeps its buffer and resets the length, which is what native_mutable_array_clear already did.
 *
 * The two trie builders take a *fresh* empty trie rather than emptying the old one in place, which
 * is both simpler and what `shared` requires. A snapshot holds its own MapObj over the old nodes
 * with the leaves shared, so dropping our root leaves it untouched and the collector keeps
 * whatever the snapshot still reaches. The new trie then shares nothing with anything, so `shared`
 * goes back to 0 and a later overwrite may update a leaf in place again instead of replacing it.
 * `mapobj_mut_new` allocates each time rather than handing back a singleton, which is what makes
 * that safe. Insertion order restarts from zero, which is what an empty builder means. */
void ziman_builder_clear(Value v) {
    const char *name = v.type->struct_name;
    if (name != NULL && strcmp(name, "MutableArray") == 0) {
        mutable_array_table_get(mutable_array_handle_of(v))->length = 0;
        return;
    }
    if (name != NULL && strcmp(name, "MutableString") == 0) {
        MutableStringBuf *sbuf = mutable_string_table_get(mutable_string_handle_of(v));
        sbuf->length = 0;
        sbuf->ascii = 1; /* empty again, so the fast path is available again */
        return;
    }
    maptable_clear(&mutable_map_get_buf(v)->t);
}

/* --- Mutable<T> -------------------------------------------------------------------------
 * One cell holding one value: the root the other four specialise, and the primitive that makes the
 * mutable layer open to library code instead of needing a new native collection every time a new
 * mutable shape is wanted (docs/DESIGN.md, "`Mutable<T>`: the root of the family"). A ring buffer
 * is a struct over a MutableArray plus two `Mutable<int>`s; an LRU adds a MutableMap; a UI signal
 * is a cell plus a MutableArray of subscribers. None of those need C.
 *
 * Same pointer-as-handle scheme as the other four, and like MutableArray its payload is a `Value`
 * reachable only through an opaque int64 -- invisible to the stack scan and to a struct's own
 * field walk -- so it registers a content marker for exactly the reason mutable_array_mark_contents does. */

typedef struct {
    pthread_t owner;
    Value value;
} MutableCellBuf;

static char *MUTABLE_FIELD_NAMES[] = {"handle"};

static MutableCellBuf *cell_get_buf(Value c) {
    MutableCellBuf *buf = (MutableCellBuf *)(intptr_t)c.as.st->fields[0].as.i;
    if (!handleset_has(&g_cells, buf)) ziman_error("runtime", 0, 0, "not a valid Mutable handle");
    builder_owner_check(buf->owner, "a Mutable");
    return buf;
}

/* Always initialised: there is no empty `mutable_new<T>()`, because `T` may have no zero. A cell
 * never holds nothing, so nothing has to say what reading an unset one does. */
static Value native_mutable_new(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableCellBuf *buf = ziman_xmalloc(sizeof(MutableCellBuf));
    buf->owner = pthread_self();
    handleset_add(&g_cells, buf);
    builder_register_drop(buf, &g_cells);
    buf->value = args[0];
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)buf);
    return value_struct(type_ref(type_struct("Mutable")), struct_obj_new(MUTABLE_FIELD_NAMES, fields, 1));
}

static Value native_mutable_get(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return cell_get_buf(args[0])->value;
}

static Value native_mutable_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    cell_get_buf(args[0])->value = args[1];
    return value_int(0); /* void return -- the caller never uses this */
}

/* See mutable_array_mark_contents: the one Value a cell holds is reachable only through the handle, so a
 * collection landing while a cell is the sole reference to it would sweep it. */
static void cell_mark_contents(const Arena *a, StructObj *st) {
    MutableCellBuf *buf = (MutableCellBuf *)(intptr_t)st->fields[0].as.i;
    gc_mark_value(a, buf->value);
}

/* `len` and `contains` on a cell, for the interpreter's own branches. A cell holds exactly one
 * value, so its length is 1 and `contains` asks whether that value equals the needle -- the two
 * bare builtins it answers. It takes neither `clear` (there is no zero to reset to) nor `snapshot`
 * (crossing out is `mutable_get`, an O(1) read rather than an O(n) copy). */
int ziman_cell_contains(Value c, Value needle) {
    return value_key_equals(cell_get_buf(c)->value, needle);
}

/* --- How a handle prints -----------------------------------------------------------------
 * As the value it is building, not as the one-int struct it is: a MutableArray prints `[1, 2, 3]`,
 * a MutableString its text, a cell `Mutable(5)`. Until this was registered they printed
 * `MutableArray { handle: 139806342961664 }` -- a raw address, unreproducible across runs, which is
 * the same defect F107 removed from `==` and `hash` and which no output carrying it could ever be a
 * stable expectation for.
 *
 * `nested` means the handle is inside another value being printed, and then it renders a summary.
 * That is what keeps a cycle unfollowable: a struct holding a MutableArray of itself prints its
 * fields, and the handle field stops at `MutableArray(1 item)`. Contents go through
 * `value_display_nested`, so a handle *within* the contents summarises too. */
static char *handle_summary(const char *type_name, long long n, const char *unit) {
    char *out = zalloc_atomic(64 + strlen(type_name));
    snprintf(out, 64 + strlen(type_name), "%s(%lld %s%s)", type_name, n, unit, n == 1 ? "" : "s");
    return out;
}

static char *mutable_array_display(Value v, int nested) {
    const MutableArrayBuf *buf = mutable_array_table_get(mutable_array_handle_of(v));
    if (nested) return handle_summary("MutableArray", (long long)buf->length, "item");
    Value arr = native_mutable_array_to_array(&v, 1, 0, 0);
    arr.type = v.type->type_args.count > 0 ? type_ref(type_array(v.type->type_args.items[0])) : type_ref(type_array(type_scalar(TY_VOID)));
    return value_display_nested(arr);
}

static char *mutable_map_display(Value v, int nested) {
    int is_set = v.type->struct_name != NULL && strcmp(v.type->struct_name, "MutableSet") == 0;
    const MutableMapBuf *buf = mutable_map_get_buf(v);
    if (nested) {
        return handle_summary(is_set ? "MutableSet" : "MutableMap", (long long)maptable_len(&buf->t), is_set ? "item" : "entry");
    }
    Value snap = native_mutable_map_snapshot(&v, 1, 0, 0);
    if (v.type->type_args.count > (is_set ? 0 : 1)) {
        snap.type = is_set ? type_ref(type_set(v.type->type_args.items[0]))
                           : type_ref(type_map(v.type->type_args.items[0], v.type->type_args.items[1]));
    }
    return value_display_nested(snap);
}

static char *mutable_string_display(Value v, int nested) {
    const MutableStringBuf *buf = mutable_string_table_get(mutable_string_handle_of(v));
    if (nested) return handle_summary("MutableString", (long long)buf->length, "byte");
    return value_display_nested(native_mutable_string_to_string(&v, 1, 0, 0));
}

/* A cell follows one level into its value whether nested or not: that stays bounded precisely
 * because a handle reached from inside it summarises, so `Mutable<Node>` prints the node's fields
 * and stops at the node's own array. */
static char *cell_display(Value v, int nested) {
    (void)nested;
    char *inner = value_display_nested(cell_get_buf(v)->value);
    size_t n = strlen(inner) + 16;
    char *out = zalloc_atomic(n);
    snprintf(out, n, "Mutable(%s)", inner);
    return out;
}

/* --- Guard ------------------------------------------------------------------------------
 * guard_new(channel, message): a value whose only job is to send `message` on `channel` once
 * nothing references the guard any more (the garbage collector runs its finalizer). A database
 * connection keeps one in its Connection struct, with `message` a "close" request: a Connection
 * that is dropped without an explicit close then tells the thread that owns its socket to close it
 * and exit, instead of that thread waiting forever. It is a no-op without the collector. The
 * guard's address is the handle, as for the builders. */
enum { GUARD_MAGIC = 0x47554152 };
typedef struct {
    uint32_t magic;
    ChannelObj *ch;
    Value message;
} GuardBuf;

#ifdef ZIMAN_GC
static void guard_finalizer(void *obj, void *client_data) {
    (void)client_data;
    GuardBuf *g = obj;
    /* needs_receive 0: `message` is not a value_copy_send copy -- it is whatever guard_new was
     * handed, already safe to share as it stands (see channel_send's own comment). */
    (void)channel_send(g->ch, g->message, 0);
    ziman_guard_unregister(&g->message);
}
#endif

/* bytes_index_of(haystack, needle, from): the position of the first occurrence of `needle` in
 * `haystack` at or after `from`, or -1. An empty needle is found at `from`. */
static Value native_bytes_index_of(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int hn = 0, nn = 0;
    uint8_t *hay = array_copy_bytes(args[0].as.arr, &hn);
    uint8_t *needle = array_copy_bytes(args[1].as.arr, &nn);
    int64_t from = args[2].as.i;
    int64_t at = -1;
    if (from < 0) from = 0;
    if (from <= hn) {
        if (nn == 0) at = from;
        else if (hn >= nn) {
            for (int64_t i = from; i + nn <= hn; i++) {
                if (hay[i] == needle[0] && memcmp(hay + i, needle, (size_t)nn) == 0) { at = i; break; }
            }
        }
    }
    free(hay);
    free(needle);
    return value_int(at);
}

static Value native_guard_new(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    GuardBuf *g = ziman_xmalloc(sizeof(GuardBuf));
    g->magic = GUARD_MAGIC;
    g->ch = args[0].as.ch;
    g->message = args[1];
    /* See gc_guard_roots.h's own comment: `message` may point into this thread's own gc_heap
     * arena (any Request/EndRequest-shaped value does, through enum_obj_new/struct_obj_new), which
     * nothing else protects from this same thread's own, entirely separate Phase 3 sweep once the
     * Guard value itself stops being Ziman-visible -- exactly the window the finalizer below exists
     * to act in. */
    ziman_guard_register(ziman_thread_heap(), &g->message);
#ifdef ZIMAN_GC
    /* Ordered finalization, not NO_ORDER: the finalizer sends `message` on `ch`, both reachable only
     * from `g`, and an unordered finalizer may find them already collected (a message that turned into
     * some other value). Ordered keeps everything reachable from `g` alive until the finalizer has run. */
    GC_REGISTER_FINALIZER(g, guard_finalizer, NULL, NULL, NULL);
#endif
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)g);
    static char *names[] = {"handle"};
    return value_struct(type_ref(type_struct("Guard")), struct_obj_new(names, fields, 1));
}

/* ---- array_map / array_filter / array_flat_map / array_reverse (collections) ---------
 * One allocation for the result (packed when the elements are all byte/bool/int/float),
 * calling back into Ziman only for the user's function. */
static Value native_array_map(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    int n = a->count;
    Value *out = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(Value));
    for (int i = 0; i < n; i++) {
        Value x = array_get(a, i);
        out[i] = native_call_closure(args[1], &x, 1, line, col);
    }
    ArrayObj *packed = array_try_pack(out, n);
    if (packed != NULL) {
        free(out);
        return value_array(type_scalar_ref(TY_VOID), packed);
    }
    return value_array(type_scalar_ref(TY_VOID), array_obj_new(out, n));
}

static Value native_array_filter(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    int n = a->count, kept = 0;
    int64_t *keep = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int i = 0; i < n; i++) {
        Value x = array_get(a, i);
        if (native_call_closure(args[1], &x, 1, line, col).as.b) keep[kept++] = i;
    }
    return permute_array(args[0].type, a, keep, kept, line, col);
}

/* `set` over an immutable Map: a new map with `key` added or its value replaced, its position
 * kept. The arm still owns the name -- it carries three measured per-type refusals a single
 * `#[when_unmatched]` line cannot (a Set, a MutableArray, a MutableSet each get their own), and
 * §13 of the overloading design is why the whole name is not converted. What it no longer owns is
 * the **Map** shape: that branch only routed to the shared `check_collection_mutator`, so the
 * declaration can say it instead, and the arm declines to the resolver as it already did for a
 * MutableMap. `set` is a mixed set now, the array member still a `builtin fn` run by its id. */
static Value native_map_set(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_map(args[0].type, mapobj_set(args[0].as.map, args[1], args[2]));
}

/* ---- the immutable Map's own questions, converted off their builtin ids 2026-10-22 ----------
 *
 * Six names, eight declarations, each found through parameter one: `map_get`, `map_get_or`,
 * `map_keys`, `map_values`, `map_merge`, and one `mutable_map_merge` serving both
 * in-place forms. None had a `check_call` arm -- they were declaration-checked already, listed in
 * `BUILTINS_WITH_NAMED_ARGS` -- so the conversion is the id, the branch and the name lists, with no
 * diagnostics to replace.
 *
 * The result types are placeholders patched by the caller, as for any generic native:
 * `type_option_ref()` for an `Option<V>`, `type_scalar_ref(TY_VOID)` for the rest. */
static Value native_map_get(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value found;
    if (mapobj_get(args[0].as.map, args[1], &found)) {
        return value_enum(type_option_ref(), enum_obj_new("Option", "Some", 1, found));
    }
    return value_enum(type_option_ref(), enum_obj_option_none());
}

/* No `Option` allocated either way, which is the whole reason this is not `get` plus `unwrap_Some`
 * -- the same point native_mutable_map_get_or above makes for the builder. */
static Value native_map_get_or(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value found;
    return mapobj_get(args[0].as.map, args[1], &found) ? found : args[2];
}

/* The keys as a `Set<K>`, in the map's insertion order. A Set is a MapObj whose values are
 * placeholders, so this is the same walk with `value_int(0)` for each value. */
static Value native_map_keys(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value *keys;
    mapobj_entries(args[0].as.map, &keys, NULL);
    MapObj *ks = mapobj_empty();
    for (int i = 0; i < args[0].as.map->count; i++) ks = mapobj_set(ks, keys[i], value_int(0));
    return value_map(type_scalar_ref(TY_VOID), ks);
}

/* The values as an array, in the keys' insertion order. Packed when they all admit it, as
 * `ziman_keys_as_array` packs a set's elements. */
static Value native_map_values(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Value *vals;
    mapobj_entries(args[0].as.map, NULL, &vals);
    int n = args[0].as.map->count;
    ArrayObj *packed = array_try_pack(vals, n);
    return value_array(type_scalar_ref(TY_VOID), packed != NULL ? packed : array_obj_new(vals, n));
}

/* Every map in order onto the first, a later one winning where an earlier has the same key. One
 * `mapobj_set` per entry, which shares structure rather than copying it.
 *
 * **Variadic since 2026-10-22, so `args[0]` is one array of maps** however the call was written --
 * written out, or a lone array spread. The declaration replaced a two-parameter form; a variadic
 * `Map<K, V>` accepts exactly two maps as well, so the two could not have coexisted.
 *
 * **This said `#[at_least_one]` "guarantees the array is non-empty, which is what lets the first
 * element be the accumulator", and that was false and crashed.** The attribute guarantees at least
 * one *argument*; an empty array spread is one argument, which SYNTAX.md documents as the
 * deliberate way to spread a list that may be empty. So `merge(xs)` with `xs: [Map<K, V>]` empty
 * reached `array_get(maps, 0)` on an empty buffer and segfaulted -- found 2026-10-22 while checking
 * this same edge for `union`.
 *
 * The empty answer is the empty map rather than an error, which is the rule the language already
 * states for a variadic with an identity: `concat()` is `""`. `min`/`max` raise instead because
 * they have no identity to give. The placeholder result type is patched by the caller, as for any
 * generic native. */
static Value native_map_merge(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const ArrayObj *maps = args[0].as.arr;
    if (maps->count == 0) return value_map(type_scalar_ref(TY_VOID), mapobj_empty());
    Value first = array_get(maps, 0);
    MapObj *m = first.as.map;
    for (int i = 1; i < maps->count; i++) {
        Value next = array_get(maps, i);
        Value *keys, *vals;
        mapobj_entries(next.as.map, &keys, &vals);
        for (int j = 0; j < next.as.map->count; j++) m = mapobj_set(m, keys[j], vals[j]);
    }
    return value_map(first.type, m);
}

/* **One symbol for both in-place declarations**, because the symbol fallback reads parameter one
 * and both have a `MutableMap` there -- they differ in parameter *two*, a tail of `Map`s or a tail
 * of `MutableMap`s, which this tells apart by each element's own runtime kind. The same shape
 * `mutable_array_concat` has, and the reason a set can have more declarations than symbols.
 *
 * **Variadic on that second parameter since 2026-10-22**, so `args[1]` is one array of maps
 * however the call was written. Reading the kind per element rather than once for the array is
 * what would let a *mixed* tail work; the declarations cannot express one (a variadic parameter
 * has a single type) and there is no arm here to choose, so no call delivers one today -- but the
 * loop does not care, which is the cheaper of the two ways to be wrong. */
static Value native_mutable_map_merge(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const ArrayObj *entries = args[1].as.arr;
    for (int i = 0; i < entries->count; i++) {
        Value next = array_get(entries, i);
        ziman_mutable_map_set_all(args[0], next, next.type->kind == TY_STRUCT);
    }
    return value_int(0); /* void return */
}

/* Variadic, so the keys arrive as one collapsed array whichever way they were written -- which is
 * the half of this the builtin could not have: it saw the raw `args` and had to ask the checker's
 * own `variadic_tail_is_array_spread` which shape had been passed. */

/* `abs` over the four numeric scalars, converted off its builtin id 2026-10-22. **One
 * registration under the bare name answers for all four declarations**, as `len`'s nine do: the
 * symbol fallback derives a prefix from parameter one's type and a *scalar* has none, so there is
 * nowhere for `int_abs`/`float_abs` to come from and nothing to gain from them -- each branch is
 * two lines and the switch is what the builtin did anyway.
 *
 * `abs(INT64_MIN)` is a runtime error rather than itself, which is the one case worth stating: the
 * positive value does not exist in the range, and `int` overflow is an error here rather than a
 * wrap (see interpreter.c's `int_overflow`). The bigint form has no such limit, which is what it
 * is for.
 *
 * F191 named `abs` among the names that *can* convert -- "`len`, `abs`, `slice`, `swap`, `to_set`
 * take collections and numbers, which always describe themselves" -- against `str`, which cannot
 * for the reason recorded above its declarations. This is that sentence acted on. */
/* ---- the four set operations, and `add` over an immutable Set --------------------------
 *
 * **Thirteen declarations, nine symbols, and no type rule in C at all.** These were
 * `builtin fn` with ids until 2026-10-22 and, unusually, had no type-checking arm even then:
 * F196 gave them their bare names and the resolver had been checking them against their
 * declarations ever since, with the id doing nothing but dispatch. So this conversion is the
 * second half of a change already made -- the same shape `abs` was in after F188 -- and nothing
 * a caller sees moves, because there was no hand-written message to lose. `union(s, "x")` was
 * refused by the resolver before and after.
 *
 * **The branch they replace asked what world it was in; these do not have to.** One name
 * resolved to one id, so the old code tested `args[0].type->kind == TY_STRUCT` to find a builder
 * and fell through to the immutable walk otherwise. The symbol fallback reads parameter one, so
 * `Set` lands on `set_union` and `MutableSet` on `mutable_set_union` before any of this runs.
 * What stays is the *second* operand's test: each builder member accepts either world there, two
 * declarations to one symbol, the pattern `mutable_map_merge` and `mutable_array_concat` already
 * use.
 *
 * A Set is a MapObj whose values are placeholders, which is why every walk here stores
 * `value_int(0)` and reads keys only. No `native_gc_unsafe_enter`: these call no closure, so
 * there is no gap between two allocations for a collection to land in -- see `native_map_filter`,
 * which does guard, and `native_map_merge`, which does not and is this same shape. */
/* `union` over any number of immutable Sets, variadic since 2026-10-22 as `merge` already was:
 * `args[0]` is one array of sets however the call was written, so `union(a, b, c)` and
 * `union(sets)` are the same call and folding a list needs no loop at the call site.
 *
 * **Empty answers the empty set**, the rule the language states for a variadic with an identity --
 * `concat()` is "" -- and a case to handle rather than assume away: `#[at_least_one]` stops
 * `union()` at compile time, but an empty array spread is one argument, which is the hole that
 * crashed `merge`. `intersect` below has no identity and raises instead. */
static Value native_set_union(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const ArrayObj *sets = args[0].as.arr;
    if (sets->count == 0) return value_map(type_scalar_ref(TY_VOID), mapobj_empty());
    Value first = array_get(sets, 0);
    MapObj *m = first.as.map;
    for (int i = 1; i < sets->count; i++) {
        Value next = array_get(sets, i);
        Value *keys;
        mapobj_entries(next.as.map, &keys, NULL);
        for (int j = 0; j < next.as.map->count; j++) m = mapobj_set(m, keys[j], value_int(0));
    }
    return value_map(first.type, m);
}

/* `intersect` and `difference` are one walk and one bit: keep an element when the other set's
 * answer matches what we are looking for. */
/* One step of `intersect`/`difference`: the elements of `acc` present in (or absent from) `other`.
 * `difference` is a two-operand declaration and uses this once; `intersect` is variadic and folds
 * it. */
static MapObj *set_keep_step(MapObj *acc, MapObj *other, int keep_present) {
    Value *keys;
    Value ignored; /* mapobj_get always writes through this; see ziman_mutable_set_combine. */
    MapObj *out = mapobj_empty();
    mapobj_entries(acc, &keys, NULL);
    for (int i = 0; i < acc->count; i++) {
        if (mapobj_get(other, keys[i], &ignored) == keep_present) out = mapobj_set(out, keys[i], value_int(0));
    }
    return out;
}

/* `intersect` over any number of immutable Sets, variadic since 2026-10-22.
 *
 * **Empty raises where `union` answers the empty set**, and the asymmetry is the mathematics rather
 * than an oversight: union's identity is the empty set, which exists, and intersection's is the
 * universal set, which does not. `min`/`max` raise for the same reason.
 *
 * Folds left, so each later set only removes from the accumulator -- the work falls to the smallest
 * set seen so far rather than staying at the first's size. */
static Value native_set_intersect(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *sets = args[0].as.arr;
    if (sets->count == 0) {
        ziman_error("runtime", line, col,
                    "'intersect' of no sets -- there is no universal set to answer with, unlike "
                    "'union' of none, which is the empty set");
    }
    Value first = array_get(sets, 0);
    MapObj *acc = first.as.map;
    for (int i = 1; i < sets->count; i++) acc = set_keep_step(acc, array_get(sets, i).as.map, 1);
    return value_map(first.type, acc);
}

/* `difference` stays two-operand: it is not symmetric -- `difference(a, b)` and `difference(b, a)`
 * are different questions -- so a variadic form would have to mean "a without anything in any of
 * the rest", a fold worth having but a different request than the one made. */
static Value native_set_difference(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_map(args[0].type, set_keep_step(args[0].as.map, args[1].as.map, 0));
}

/* `is_subset` with an immutable receiver, over **either** operand world since 2026-10-22 -- the
 * one declaration that closes the asymmetry the four set operations had kept, where a builder
 * receiver took an operand from either world and an immutable receiver took only a `Set`.
 *
 * Two declarations to one symbol, as each builder member's pair already is: the fallback reads
 * parameter one, so `(Set, Set)` and `(Set, MutableSet)` both land here and argument two's own
 * runtime kind says which arrived. `ziman_mutable_set_is_subset` is the mirror of this -- the same
 * walk with the two sides swapped -- and reads its elements from a table where this reads them from
 * a MapObj.
 *
 * The receiver's elements drive the walk either way, so this is O(len(s)) lookups and an early exit
 * on the first element missing. */
static Value native_set_is_subset(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *from = args[1].type->kind == TY_STRUCT ? mutable_map_get_buf(args[1]) : NULL;
    Value *keys;
    mapobj_entries(args[0].as.map, &keys, NULL);
    int n = args[0].as.map->count;
    for (int i = 0; i < n; i++) {
        Value ignored; /* mapobj_get always writes through this; see ziman_mutable_set_combine. */
        int in_other = from != NULL ? maptable_get(&from->t, keys[i], NULL)
                                    : mapobj_get(args[1].as.map, keys[i], &ignored);
        if (!in_other) return value_bool(0);
    }
    return value_bool(1);
}

/* The three builder members, each over either world in parameter two. `op` is the same 0/1/2 the
 * removed branch computed from the id. They answer nothing -- the write is the point -- so
 * `value_int(0)` is the void stand-in every in-place native returns. */
static Value native_mutable_set_union(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    /* Variadic on parameter two since 2026-10-22: one operand or many, each told apart from the
     * other world by its own runtime kind, the shape `mutable_map_merge` already had. An empty
     * tail is a no-op, which is what naming only the receiver already means. */
    const ArrayObj *others = args[1].as.arr;
    for (int i = 0; i < others->count; i++) {
        Value next = array_get(others, i);
        ziman_mutable_set_combine(args[0], next, next.type->kind == TY_STRUCT, 0);
    }
    return value_int(0);
}

static Value native_mutable_set_intersect(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    /* Variadic on parameter two since 2026-10-22: one operand or many, each told apart from the
     * other world by its own runtime kind, the shape `mutable_map_merge` already had. An empty
     * tail is a no-op, which is what naming only the receiver already means. */
    const ArrayObj *others = args[1].as.arr;
    for (int i = 0; i < others->count; i++) {
        Value next = array_get(others, i);
        ziman_mutable_set_combine(args[0], next, next.type->kind == TY_STRUCT, 1);
    }
    return value_int(0);
}

static Value native_mutable_set_difference(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ziman_mutable_set_combine(args[0], args[1], args[1].type->kind == TY_STRUCT, 2);
    return value_int(0);
}

static Value native_mutable_set_is_subset(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_bool(ziman_mutable_set_is_subset(args[0], args[1], args[1].type->kind == TY_STRUCT));
}

/* `add` over an immutable Set: the set with the given elements in it.
 *
 * **`args[1]` is one array however the call was written**, which is the piece that got simpler.
 * The branch this replaces read `e->as.call.variadic_tail_is_array_spread` off the expression
 * node to tell `add(s, a, b)` from `add(s, items)` -- a builtin receives raw arguments, so it had
 * to ask the checker which shape arrived. `collapse_variadic_args` runs before a native is
 * entered, so both shapes are already one array here and the question does not exist. The same
 * simplification `min`/`max` got on the same day. */
static Value native_set_add(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MapObj *m = args[0].as.map;
    const ArrayObj *items = args[1].as.arr;
    for (int i = 0; i < items->count; i++) m = mapobj_set(m, array_get(items, i), value_int(0));
    return value_map(args[0].type, m);
}

/* `remove` over a collection, variadic since 2026-10-22 -- **two implementations for four
 * symbols**, which is what the merge with `remove_all` left behind.
 *
 * There were five functions here: `map_remove` and `set_remove` were byte-identical
 * `mapobj_remove` calls, `mutable_map_remove` and `mutable_set_remove` differed only in that one
 * built an `Option` out of the removed value, and `map_remove_all` was already the loop all of
 * them now are. Going variadic made each pair the same code, and dropping the `Option` made the
 * builder pair the same code too, so one function serves `map_remove` and `set_remove` and
 * another serves `mutable_map_remove` and `mutable_set_remove`. A Set is a MapObj whose values are
 * placeholders and a MutableSet is a MutableMapBuf with the same, which is why the pairing works
 * at all; it was true before and the Option was the only thing hiding it.
 *
 * `args[1]` is one array however the call was written -- `collapse_variadic_args` runs first --
 * and `#[at_least_one]` is what makes `remove(m)` a compile error. */
static Value native_map_remove(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MapObj *m = args[0].as.map;
    const ArrayObj *keys = args[1].as.arr;
    for (int i = 0; i < keys->count; i++) m = mapobj_remove(m, array_get(keys, i));
    return value_map(args[0].type, m);
}

/* The builder pair: removes in place and answers whether any of them was there.
 *
 * **This is where the `Option<V>` went.** `remove` on a MutableMap answered the value it took out,
 * which no caller in this repository ever read -- the only two readers were `remove_all` and
 * `remove_keys`, both using it as a presence test and discarding the binding. A variadic form has
 * no single value to answer, so the merge chose the `bool` its three sibling worlds already
 * answered. It is also strictly cheaper: the `Option` was an allocation per removal, which
 * `discard` existed to avoid and which `remove` no longer makes.
 *
 * The buffer pointer is held across the loop, as `ziman_mutable_set_combine` does: `maptable_remove`
 * works on the table's own storage, not the struct that owns it. */
static Value native_mutable_map_remove(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MutableMapBuf *buf = mutable_map_get_buf(args[0]);
    const ArrayObj *keys = args[1].as.arr;
    int removed_any = 0;
    for (int i = 0; i < keys->count; i++) {
        if (maptable_remove(&buf->t, array_get(keys, i), NULL)) removed_any = 1;
    }
    return value_bool(removed_any);
}

/* `remove_keys` over an immutable Map: the map without any key the given Set holds. A separate
 * name rather than a `remove_all` member because the overload could not exist -- see the
 * declaration in lib/prelude/map.zm for the refusal and why it is not a bug in the rule. */
static Value native_map_remove_keys(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    MapObj *m = args[0].as.map;
    Value *keys;
    mapobj_entries(args[1].as.map, &keys, NULL);
    for (int i = 0; i < args[1].as.map->count; i++) m = mapobj_remove(m, keys[i]);
    return value_map(args[0].type, m);
}

/* `remove` over the three immutable collections, which answer a new collection without the thing
 * removed and leave their argument alone -- the builder members of the same name write in place
 * and answer what they took out, which is the difference between the two worlds rather than
 * between two names.
 *
 * Three natives since 2026-10-22, where one builtin id served all three: its branch tested
 * `args[0].type->kind` for a Map or a Set and otherwise fell through to the array case in
 * `call_remove`. Parameter one is what the symbol fallback reads, so each has its own symbol now
 * and none of them asks what it was given. Nothing here calls back into Ziman, so unlike
 * `map_filter`/`set_filter` these need no collection guard.
 *
 * The array form is `call_remove` moved out of interpreter.c unchanged: bounds-checked against the
 * length, one `array_packed_edit` when the elements are packed, and otherwise two memcpys around
 * the hole. The result's element type is patched by the caller, as it is for any generic native. */
static Value native_array_remove(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *old = args[0].as.arr;
    int64_t i = args[1].as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "remove index %lld out of bounds (array length %d)",
                    (long long)i, old->count);
    }
    if (array_pack_kind(old) != PK_NONE) {
        return value_array(args[0].type, array_packed_edit(old, 3, (int)i, value_int(0)));
    }
    int new_count = old->count - 1;
    Value *items = ziman_xmalloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->boxed_items, (size_t)i * sizeof(Value));
    memcpy(items + i, old->boxed_items + i + 1, (size_t)(new_count - (int)i) * sizeof(Value));
    return value_array(args[0].type, array_obj_new(items, new_count));
}

/* A Map without `key`, and a Set without `item`: one `mapobj_remove` each, and absent is not an
 * error -- the answer is a collection equal to the argument. One `value_map` serves both, a Set
 * being a MapObj whose values are placeholders. */


/* `filter` over an immutable **Map** and **Set**: the entries `keep` answers true for, as a new
 * collection of the same kind, insertion order preserved. Two natives since 2026-10-22, where one
 * builtin branch in interpreter.c served both by switching on `args[0].type->kind` at run time --
 * the symbol fallback reads parameter one, so a `Map` lands on `map_filter` and a `Set` on
 * `set_filter` and neither has to ask what it was given.
 *
 * **Both guard the whole loop, not each call.** `mapobj_entries` hands back a plain `malloc`'d
 * array of keys and values, which neither an Env nor a frame holds, and `mapobj_set` allocates on
 * every kept entry -- so a collection in the gap between two predicate calls is the window.
 * `native_call_closure` guards its own call and not that gap, which is why native.h exports the
 * pair. The builtin branch guarded this same loop, and that is the reason the guard is here: **no
 * failing case was reproducible without it** -- see native.h for what was tried -- so it is
 * retained as the conservative choice rather than on a demonstrated failure. */
static Value native_map_filter(Value *args, int argc, int line, int col) {
    (void)argc;
    Value *keys, *vals;
    mapobj_entries(args[0].as.map, &keys, &vals);
    int n = args[0].as.map->count;
    MapObj *out = mapobj_empty();
    native_gc_unsafe_enter();
    for (int i = 0; i < n; i++) {
        Value pair[2] = {keys[i], vals[i]};
        if (native_call_closure(args[1], pair, 2, line, col).as.b) {
            out = mapobj_set(out, keys[i], vals[i]);
        }
    }
    native_gc_unsafe_leave();
    return value_map(args[0].type, out);
}

/* A Set is a MapObj whose values are placeholders, so the predicate takes one argument and the
 * kept entry is stored with the same placeholder the source used. */
static Value native_set_filter(Value *args, int argc, int line, int col) {
    (void)argc;
    Value *keys;
    mapobj_entries(args[0].as.map, &keys, NULL);
    int n = args[0].as.map->count;
    MapObj *out = mapobj_empty();
    native_gc_unsafe_enter();
    for (int i = 0; i < n; i++) {
        if (native_call_closure(args[1], &keys[i], 1, line, col).as.b) {
            out = mapobj_set(out, keys[i], value_int(0));
        }
    }
    native_gc_unsafe_leave();
    return value_map(args[0].type, out);
}

static Value native_array_flat_map(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    int n = a->count;
    Value *parts = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof(Value));
    for (int i = 0; i < n; i++) {
        Value x = array_get(a, i);
        parts[i] = native_call_closure(args[1], &x, 1, line, col);
    }
    return ziman_concat_arrays(array_obj_new(parts, n));
}

static Value native_array_reverse(Value *args, int argc, int line, int col) {
    (void)argc;
    const ArrayObj *a = args[0].as.arr;
    int n = a->count;
    int64_t *order = zalloc_atomic((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int i = 0; i < n; i++) order[i] = n - 1 - i;
    return permute_array(args[0].type, a, order, n, line, col);
}

void prelude_register(void) {
    native_register("parse_int", native_parse_int);
    native_register("parse_float", native_parse_float);
    native_register("parse_bigint", native_parse_bigint);
    native_register("parse_decimal", native_parse_decimal);
    native_register("substring", native_substring);
    native_register("split", native_split);
    native_register("string_index_of", native_index_of);
    /* `string_last_index_of`, not the bare name, since 2026-10-22: `last_index_of` gained a
     * `MutableString` member, and a bare registration is found by `native_lookup(fn->name)`
     * before the world-prefix fallback is ever consulted -- so every member of the set would
     * have run this string implementation, reading a handle as text and answering -1. The
     * same prefix `string_index_of` has carried since F213, whose own comment is the warning
     * this walked into: "every other string native is still registered under its bare name
     * and so never reaches this fallback at all". */
    native_register("string_last_index_of", native_last_index_of);
    native_register("is_char_at", native_is_char_at);
    native_register("equals_ignore_case", native_equals_ignore_case);
    /* `string_char_at`, not the bare name, since 2026-10-23: `char_at` gained a `MutableString`
     * member when `mutable_string_get` merged into it, and a bare registration is found by
     * `native_lookup(fn->name)` before the world-prefix fallback is consulted -- so both members
     * would have run this one, reading a handle as text. The same trap `last_index_of` fell into
     * the day before, and the reason that one is `string_last_index_of` now. */
    native_register("string_char_at", native_char_at);
    native_register("starts_with", native_starts_with);
    native_register("ends_with", native_ends_with);
    native_register("replace", native_replace);
    native_register("trim", native_trim);
    native_register("is_empty", native_is_empty);
    native_register("is_blank", native_is_blank);
    /* **Neither world gets a bare registration.** `native_lookup(fn->name)` is consulted before
     * the world-prefix fallback, so a bare `is_alpha` would be found first and run the `string`
     * implementation on a `MutableString` handle -- the trap `char_at` and `last_index_of` both
     * fell into (see lib/prelude/string.zm's own note on it). */
    native_register("string_is_alpha", native_string_is_alpha);
    native_register("string_is_numeric", native_string_is_numeric);
    native_register("string_is_alpha_numeric", native_string_is_alpha_numeric);
    native_register("string_is_digit", native_string_is_digit);
    native_register("string_is_char", native_string_is_char);
    native_register("mutable_string_is_alpha", native_mutable_string_is_alpha);
    native_register("mutable_string_is_numeric", native_mutable_string_is_numeric);
    native_register("mutable_string_is_alpha_numeric", native_mutable_string_is_alpha_numeric);
    native_register("mutable_string_is_digit", native_mutable_string_is_digit);
    native_register("mutable_string_is_char", native_mutable_string_is_char);
    native_register("to_upper", native_to_upper);
    native_register("to_lower", native_to_lower);
    native_register("join", native_join);
    native_register("is_sortable_scalars", native_is_sortable_scalars);
    native_register("sort_scalar_order", native_sort_scalar_order);
    native_register("sort_scalars", native_sort_scalars);
    native_register("permute", native_permute);
    native_register("sort_positions", native_sort_positions);
    native_register("mutable_map_new", native_mutable_map_new);
    native_register("mutable_map_set", native_mutable_map_set);
    native_register("mutable_map_get", native_mutable_map_get);
    native_register("mutable_map_get_or", native_mutable_map_get_or);
    native_register("mutable_map_remove", native_mutable_map_remove);
    native_register("mutable_map_discard", native_mutable_map_discard);
    native_register("mutable_set_new", native_mutable_set_new);
    native_register("mutable_set_add", native_mutable_set_add);
    native_register("mutable_set_remove", native_mutable_map_remove);
    native_register("guard_new", native_guard_new);
    native_register("array_index_of", native_bytes_index_of);
    native_register("array_map", native_array_map);
    native_register("array_filter", native_array_filter);
    native_register("map_set", native_map_set);
    native_register("map_get", native_map_get);
    native_register("map_get_or", native_map_get_or);
    native_register("map_keys", native_map_keys);
    native_register("map_values", native_map_values);
    native_register("map_merge", native_map_merge);
    native_register("mutable_map_merge", native_mutable_map_merge);
    native_register("map_remove_keys", native_map_remove_keys);
    native_register("array_remove", native_array_remove);
    native_register("map_remove", native_map_remove);
    native_register("set_remove", native_map_remove);
    native_register("map_filter", native_map_filter);
    native_register("set_filter", native_set_filter);
    native_register("array_flat_map", native_array_flat_map);
    native_register("array_reverse", native_array_reverse);
    native_register("len", ziman_len);
    native_register("contains", ziman_contains);
    native_register("set_to_array", ziman_keys_as_array);
    native_register("map_keys_array", ziman_keys_as_array);
    native_register("mutable_set_to_array", native_mutable_set_to_array);
    native_register("set_to_mutable_array", native_set_to_mutable_array);
    native_register("mutable_set_to_mutable_array", native_mutable_set_to_mutable_array);
    native_register("set_union", native_set_union);
    native_register("set_intersect", native_set_intersect);
    native_register("set_difference", native_set_difference);
    native_register("set_is_subset", native_set_is_subset);
    native_register("mutable_set_union", native_mutable_set_union);
    native_register("mutable_set_intersect", native_mutable_set_intersect);
    native_register("mutable_set_difference", native_mutable_set_difference);
    native_register("mutable_set_is_subset", native_mutable_set_is_subset);
    native_register("set_add", native_set_add);
    native_register("string_concat", ziman_string_concat);
    native_register("array_concat", ziman_array_concat);
    native_register("mutable_array_concat", ziman_mutable_array_concat);
    native_register("snapshot", ziman_snapshot);
    native_register("clear", ziman_clear);
    native_register("array_slice", native_array_slice);
    native_register("mutable_array_narrow", native_narrow_mutable_array);
    native_register("array_to_mutable_set", native_array_to_mutable_set);
    native_register("mutable_array_to_mutable_set", native_mutable_array_to_mutable_set);
    native_register("array_to_set", native_array_to_set);
    native_register("mutable_array_to_set", native_mutable_array_to_set);
    native_register("array_swap", native_array_swap);
    native_register("array_push", ziman_array_push_values);
    native_register("array_insert", native_array_insert);
    native_register("mutable_array_swap", native_mutable_array_swap);
    native_register("mutable_array_slice", native_mutable_array_slice);
    native_register("string_to_bytes", native_string_to_bytes);
    native_register("bytes_to_string", native_bytes_to_string);
    native_register("chars", native_chars);
    native_register("byte_len", native_byte_len);
    native_register("string_truncate", native_truncate);
    /* `string_reverse`, which is what the fallback derives for `reverse` on a `string`. Safe to add
     * because the array member is registered as `array_reverse` rather than bare -- a bare `reverse`
     * would be found by native_lookup first and run the array implementation on text, which is the
     * trap `char_at` and `last_index_of` both fell into. */
    native_register("string_reverse", native_reverse_string);
    native_register("mutable_string_char_at", native_mutable_string_get);
    native_register("mutable_string_set", native_mutable_string_set);
    native_register("mutable_string_index_of", native_mutable_string_index_of);
    native_register("mutable_string_last_index_of", native_mutable_string_last_index_of);
    native_register("mutable_string_new", native_mutable_string_new);
    native_register("mutable_string_append", native_mutable_string_append);
    native_register("mutable_string_append_byte", native_mutable_string_append_byte);
    native_register("mutable_new", native_mutable_new);
    native_register("mutable_get", native_mutable_get);
    native_register("mutable_set", native_mutable_set);
    native_register("mutable_array_filter", native_mutable_array_filter);
    native_register("mutable_array_sort_with", native_mutable_array_sort_with);
    native_register("mutable_array_insert", native_mutable_array_insert);
    native_register("mutable_array_remove", native_mutable_array_remove);
    native_register("mutable_array_new", native_mutable_array_new);
    native_register("mutable_array_filled", native_mutable_array_filled);
    native_register("mutable_array_push", ziman_mutable_array_push);

    /* See mutable_array_mark_contents/mutable_map_mark_contents's own comments and gc_mark_register_struct_content_marker's
     * (gc_mark.h) for why these are needed at all. */
    gc_mark_register_struct_content_marker("MutableArray", mutable_array_mark_contents);
    gc_mark_register_struct_content_marker("MutableMap", mutable_map_mark_contents);
    gc_mark_register_struct_content_marker("MutableSet", mutable_map_mark_contents);
    gc_mark_register_struct_content_marker("Mutable", cell_mark_contents);
    value_register_handle_display("MutableArray", mutable_array_display);
    value_register_handle_display("MutableMap", mutable_map_display);
    value_register_handle_display("MutableSet", mutable_map_display);
    value_register_handle_display("MutableString", mutable_string_display);
    value_register_handle_display("Mutable", cell_display);
}
