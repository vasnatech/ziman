#define _POSIX_C_SOURCE 200809L
#include "prelude.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "decimal.h"
#include "error.h"
#include "native.h"
#include "value.h"

/* Generated from lib/prelude.zm by the same Makefile rule every
 * other bundled module's header goes through -- real .zm source, never
 * a hand-escaped C string literal (see lib/prelude.zm's own header
 * comment for why this file, uniquely among corelib's modules, is never
 * registered in corelib.c's CORE_MODULES table). */
#include "prelude_source.h"

/* PRELUDE_SOURCE is one long string literal, now that the prelude has
 * grown enough (StringBuilder among the latest additions) to exceed the
 * C99-mandated minimum translation limit of 4095 characters -- harmless
 * on every compiler this project targets. Silenced narrowly here, right
 * where the macro is actually expanded, the same way corelib.c already
 * does for COLLECTIONS_SOURCE and friends, rather than loosening
 * -Wpedantic project-wide. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
const char *prelude_source(void) {
    return PRELUDE_SOURCE;
}
#pragma GCC diagnostic pop

static Value native_parse_int(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type result_type = type_result(type_scalar(TY_INT), type_scalar(TY_STRING));
    const char *str = value_cstr(args[0]);
    char *endptr;
    errno = 0;
    long long v = strtoll(str, &endptr, 10);
    if (endptr == str || *endptr != '\0' || errno == ERANGE || strlen(str) != args[0].as.str.len) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as int", str);
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(result_type, result_obj_new(1, value_int((int64_t)v)));
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
    if (endptr == str || *endptr != '\0' || errno == ERANGE || strlen(str) != args[0].as.str.len) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as float", str);
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(result_type, result_obj_new(1, value_float(v)));
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
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(result_type, result_obj_new(1, value_bigint(n)));
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
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(result_type, result_obj_new(1, value_decimal(d)));
}

static Value native_floor(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_int((int64_t)floor(args[0].as.f));
}

static Value native_ceil(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_int((int64_t)ceil(args[0].as.f));
}

static Value native_round(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_int((int64_t)round(args[0].as.f));
}

static Value native_decimal_round(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    return value_decimal(decimal_round(*args[0].as.dec, (int)args[1].as.i));
}

#define SPTR(v) ((v).as.str.ptr)
#define SLEN(v) ((size_t)(v).as.str.len)

/* A slice of `parent` (which must be a string value): `len` bytes starting
 * `start` bytes in. No copy -- the result points into the parent's buffer,
 * and is NUL-terminated only if it runs to the end of a terminated parent. */
static Value string_slice(Value parent, size_t start, size_t len) {
    int term = parent.as.str.term && start + len == SLEN(parent);
    return value_string_view(SPTR(parent) + start, len, term);
}

static Value native_concat(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    size_t la = SLEN(args[0]), lb = SLEN(args[1]);
    char *buf = zalloc_atomic(la + lb + 1);
    memcpy(buf, SPTR(args[0]), la);
    memcpy(buf + la, SPTR(args[1]), lb);
    buf[la + lb] = '\0';
    return value_string_view(buf, la + lb, 1);
}

static Value native_substring(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t len = (int64_t)SLEN(args[0]);
    int64_t st = args[1].as.i, en = args[2].as.i;
    if (st < 0 || en > len || st > en) {
        ziman_error("runtime", line, col, "substring index out of bounds (start=%lld, end=%lld, length=%lld)",
                    (long long)st, (long long)en, (long long)len);
    }
    return string_slice(args[0], (size_t)st, (size_t)(en - st));
}

static Value native_char_at(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t len = (int64_t)SLEN(args[0]), i = args[1].as.i;
    if (i < 0 || i >= len) {
        ziman_error("runtime", line, col, "char_at index out of bounds (index=%lld, length=%lld)", (long long)i, (long long)len);
    }
    return string_slice(args[0], (size_t)i, 1);
}

static Value native_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t len = (int64_t)SLEN(args[0]), from = args[2].as.i;
    if (from < 0 || from > len) {
        ziman_error("runtime", line, col, "index_of start out of bounds (from=%lld, length=%lld)", (long long)from, (long long)len);
    }
    return value_int(value_str_find(SPTR(args[0]), SLEN(args[0]), SPTR(args[1]), SLEN(args[1]), (size_t)from));
}

static Value native_last_index_of(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t ls = SLEN(args[0]), ln = SLEN(args[1]);
    if (ln > ls) return value_int(-1);
    for (size_t i = ls - ln + 1; i-- > 0; ) {
        if (memcmp(SPTR(args[0]) + i, SPTR(args[1]), ln) == 0) return value_int((int64_t)i);
    }
    return value_int(-1);
}

static Value native_is_char_at(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int64_t len = (int64_t)SLEN(args[0]), i = args[1].as.i;
    size_t lc = SLEN(args[2]);
    if (i < 0 || i > len || (size_t)(len - i) < lc) return value_bool(0);
    return value_bool(memcmp(SPTR(args[0]) + i, SPTR(args[2]), lc) == 0);
}

static Value native_equals_ignore_case(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
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
            items = realloc(items, (size_t)cap * sizeof(Value));
        }
        items[count++] = string_slice(args[0], cursor, piece_len);
        if (found < 0) break;
        cursor = (size_t)found + ld;
    }
    return value_array(type_array(type_scalar(TY_STRING)), array_obj_new(items, count));
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

static Value native_to_upper(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = SPTR(args[0]);
    size_t len = SLEN(args[0]);
    char *buf = zalloc_atomic(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string_view(buf, len, 1);
}

static Value native_to_lower(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
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

/* --- StringBuilder -------------------------------------------------
 * A mutable string-accumulation handle, backed entirely here, native-
 * side -- Ziman code never sees anything but an opaque `handle: int`
 * (see lib/prelude.zm's StringBuilder struct). The handle IS the address of
 * an individually heap-allocated StringBuilderBuf, not an index into a
 * global table: a table would keep every builder ever made reachable
 * forever (each json_to_string node makes one), which is exactly the kind
 * of leak a collector cannot fix. As a plain int in a Value the address is
 * still visible to the conservative collector, so the buffer lives exactly
 * as long as the StringBuilder value that names it. `magic` catches a
 * forged or stale handle with a clear error instead of a wild read (not
 * airtight -- the docs already say "don't construct one by hand").
 * Appending is unsynchronized between callers, same as Java's own
 * StringBuilder: one is built up by a single thread at a time. */
enum { SB_MAGIC = 0x53424c44 };
typedef struct {
    uint32_t magic;
    char *data;
    size_t length;
    size_t capacity;
    double grow_factor;
} StringBuilderBuf;

static int64_t sb_table_register(StringBuilderBuf *buf) {
    buf->magic = SB_MAGIC;
    return (int64_t)(intptr_t)buf;
}

static StringBuilderBuf *sb_table_get(int64_t handle) {
    StringBuilderBuf *buf = (StringBuilderBuf *)(intptr_t)handle;
    if (buf == NULL || buf->magic != SB_MAGIC) ziman_error("runtime", 0, 0, "not a valid StringBuilder handle");
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

static int64_t sb_handle_of(Value sb) {
    return sb.as.st->fields[0].as.i;
}

static void sb_ensure_capacity(StringBuilderBuf *buf, size_t additional) {
    size_t needed = buf->length + additional;
    if (needed <= buf->capacity) {
        return;
    }
    size_t new_capacity = buf->capacity > 0 ? buf->capacity : 1;
    while (new_capacity < needed) {
        new_capacity = (size_t)((double)new_capacity * buf->grow_factor) + 1;
    }
    buf->data = realloc(buf->data, new_capacity);
    buf->capacity = new_capacity;
}

static Value native_sb_new(Value *args, int argc, int line, int col) {
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
         * -- sb_ensure_capacity's while loop would spin forever. Falls
         * back to the same default sb_new's own signature declares,
         * rather than erroring, since a StringBuilder is otherwise
         * perfectly usable with this one parameter simply ignored. */
        grow_factor = 2.0;
    }

    StringBuilderBuf *buf = malloc(sizeof(StringBuilderBuf));
    buf->capacity = (size_t)capacity;
    buf->length = 0;
    buf->grow_factor = grow_factor;
    buf->data = buf->capacity > 0 ? zalloc_atomic(buf->capacity) : NULL;

    int64_t handle = sb_table_register(buf);

    Value *fields = malloc(sizeof(Value));
    fields[0] = value_int(handle);
    return value_struct(type_struct("StringBuilder"), struct_obj_new(STRINGBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_sb_append(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    StringBuilderBuf *buf = sb_table_get(sb_handle_of(args[0]));
    ArrayObj *pieces = args[1].as.arr;
    for (int i = 0; i < pieces->count; i++) {
        Value piece = array_get(pieces, i);
        const char *s = SPTR(piece);
        size_t len = SLEN(piece);
        sb_ensure_capacity(buf, len);
        memcpy(buf->data + buf->length, s, len);
        buf->length += len;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

/* sb_append_byte(sb, bytes...) -> bool: raw bytes, so text can be assembled
 * from decoded bytes without a [byte] in between. A string can't hold a 0
 * byte, so if any of them is 0b nothing at all is appended and the result is
 * false. */
static Value native_sb_append_byte(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    StringBuilderBuf *buf = sb_table_get(sb_handle_of(args[0]));
    ArrayObj *bytes = args[1].as.arr;
    if (bytes->bytes != NULL) {
        if (memchr(bytes->bytes, 0, (size_t)bytes->count) != NULL) return value_bool(0);
        sb_ensure_capacity(buf, (size_t)bytes->count);
        memcpy(buf->data + buf->length, bytes->bytes, (size_t)bytes->count);
        buf->length += (size_t)bytes->count;
        return value_bool(1);
    }
    for (int i = 0; i < bytes->count; i++) {
        if (array_get(bytes, i).as.by == 0) return value_bool(0);
    }
    sb_ensure_capacity(buf, (size_t)bytes->count);
    for (int i = 0; i < bytes->count; i++) buf->data[buf->length++] = (char)(unsigned char)array_get(bytes, i).as.by;
    return value_bool(1);
}

static Value native_sb_to_string(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    StringBuilderBuf *buf = sb_table_get(sb_handle_of(args[0]));
    char *out = zalloc_atomic(buf->length + 1);
    memcpy(out, buf->data, buf->length);
    out[buf->length] = '\0';
    return value_string(out);
}

static Value native_sb_len(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    StringBuilderBuf *buf = sb_table_get(sb_handle_of(args[0]));
    return value_int((int64_t)buf->length);
}

/* --- ArrayBuilder<T> -------------------------------------------------
 * Identical shape to StringBuilder's own table/handle machinery above,
 * just storing `Value` per slot instead of `char` -- since an ordinary
 * Ziman ArrayObj is already exactly this (a generic, type-erased `Value
 * *items` buffer), the native side needs no per-T dispatch at all: a
 * `Value` copies by plain assignment regardless of what type it holds.
 * The one thing a native function genuinely can't do on its own is
 * stamp the *concrete* T onto a result's own `.type` (ArrayBuilder<T>
 * carries no argument that would reveal it, and NativeFn's signature
 * carries no type-argument information at all) -- every `.type` built
 * below is a placeholder for exactly this reason, immediately
 * overwritten with the call's own already-substituted e->resolved_type
 * right after each of these returns (see eval_expr's EXPR_CALL case). */
enum { AB_MAGIC = 0x41424c44 };
typedef struct {
    uint32_t magic;
    Value *items;
    size_t length;
    size_t capacity;
    double grow_factor;
    /* Packed mode: once the first appended element is a byte, bool, int or float,
     * elements are kept as raw values in `pdata` (with `items` unused), so an
     * array of them under construction doesn't cost a Value each and ab_to_array
     * is one memcpy. */
    PackKind pkind;
    unsigned char *pdata;
} ArrayBuilderBuf;

/* Same pointer-as-handle scheme as StringBuilder above, for the same reason. */
static int64_t ab_table_register(ArrayBuilderBuf *buf) {
    buf->magic = AB_MAGIC;
    return (int64_t)(intptr_t)buf;
}

static ArrayBuilderBuf *ab_table_get(int64_t handle) {
    ArrayBuilderBuf *buf = (ArrayBuilderBuf *)(intptr_t)handle;
    if (buf == NULL || buf->magic != AB_MAGIC) ziman_error("runtime", 0, 0, "not a valid ArrayBuilder handle");
    return buf;
}

static char *ARRAYBUILDER_FIELD_NAMES[] = {"handle"};

static int64_t ab_handle_of(Value ab) {
    return ab.as.st->fields[0].as.i;
}

static void ab_ensure_capacity(ArrayBuilderBuf *buf, size_t additional) {
    size_t needed = buf->length + additional;
    if (needed <= buf->capacity) {
        return;
    }
    size_t new_capacity = buf->capacity > 0 ? buf->capacity : 1;
    while (new_capacity < needed) {
        new_capacity = (size_t)((double)new_capacity * buf->grow_factor) + 1;
    }
    if (buf->pkind != PK_NONE) buf->pdata = realloc(buf->pdata, new_capacity * pack_elem_size(buf->pkind));
    else buf->items = realloc(buf->items, new_capacity * sizeof(Value));
    buf->capacity = new_capacity;
}

static Value native_ab_new(Value *args, int argc, int line, int col) {
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

    ArrayBuilderBuf *buf = malloc(sizeof(ArrayBuilderBuf));
    buf->capacity = (size_t)capacity;
    buf->length = 0;
    buf->grow_factor = grow_factor;
    buf->items = buf->capacity > 0 ? malloc(buf->capacity * sizeof(Value)) : NULL;
    buf->pdata = NULL;
    buf->pkind = PK_NONE;

    int64_t handle = ab_table_register(buf);

    Value *fields = malloc(sizeof(Value));
    fields[0] = value_int(handle);
    return value_struct(type_struct("ArrayBuilder"), struct_obj_new(ARRAYBUILDER_FIELD_NAMES, fields, 1));
}

static Value native_ab_append(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ArrayBuilderBuf *buf = ab_table_get(ab_handle_of(args[0]));
    ArrayObj *pieces = args[1].as.arr;
    if (pieces->count == 0) return value_int(0);
    if (buf->pkind == PK_NONE && buf->length == 0) {
        PackKind first = pack_kind_of_type(array_get(pieces, 0).type.kind);
        if (first != PK_NONE) {
            size_t cap = buf->capacity > 0 ? buf->capacity : 16;
            buf->items = NULL;
            buf->pdata = zalloc_atomic(cap * pack_elem_size(first));
            buf->capacity = cap;
            buf->pkind = first;
        }
    }
    ab_ensure_capacity(buf, (size_t)pieces->count);
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

static Value native_string_to_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    return value_byte_array((const uint8_t *)s, n);
}

static Value native_bytes_to_string(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str_type = type_scalar(TY_STRING);
    ArrayObj *arr = args[0].as.arr;
    int n = 0;
    uint8_t *tmp = array_copy_bytes(arr, &n);
    if (memchr(tmp, 0, (size_t)n) != NULL) {
        free(tmp);
        return value_result(type_result(str_type, str_type), result_obj_new(0, value_string(strdup("bytes contain a NUL byte and cannot be a string"))));
    }
    char *s = (char *)tmp; /* already NUL-terminated by array_copy_bytes */
    return value_result(type_result(str_type, str_type), result_obj_new(1, value_string(s)));
}

/* ---- sorting scalars natively (collections.sort and friends) -------------------------
 * A packed [int] / [float] / [byte], or an array of strings, is sorted by a stable merge
 * sort over positions, avoiding a per-comparison interpreter call. Anything else (a struct,
 * an enum, bigint, ...) is left to collections' generic sort. */
static int scalar_sortable(const ArrayObj *a) {
    if (array_pack_kind(a) == PK_INT || array_pack_kind(a) == PK_FLOAT || array_pack_kind(a) == PK_BYTE) return 1;
    if (array_pack_kind(a) != PK_NONE) return 0; /* [bool] has no order */
    for (int i = 0; i < a->count; i++) {
        if (a->boxed_items[i].type.kind != TY_STRING) return 0;
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

/* The positions of the array's elements in sorted order. Stable: equal elements keep their
 * input order, in descending order too (it is not the ascending result reversed). */
static Value native_sort_scalar_order(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const ArrayObj *a = args[0].as.arr;
    int desc = args[1].as.b;
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
    return value_int_array(order, (size_t)n);
}

static Value native_concat_arrays(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    ArrayObj *parts = args[0].as.arr;
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
        return value_array(type_scalar(TY_VOID), array_obj_new_packed(kind, buf, total));
    }
    Value *items = total > 0 ? malloc((size_t)total * sizeof(Value)) : NULL;
    int pos = 0;
    for (int i = 0; i < parts->count; i++) {
        ArrayObj *part = array_get(parts, i).as.arr;
        for (int j = 0; j < part->count; j++) items[pos++] = array_get(part, j);
    }
    ArrayObj *packed = array_try_pack(items, total);
    if (packed != NULL) return value_array(type_scalar(TY_VOID), packed);
    return value_array(type_scalar(TY_VOID), array_obj_new(items, total));
}

/* slice<T>(a, start, end): a zero-copy view. Arrays are immutable once built
 * (see ArrayObj), so the result can share the parent's element storage: a new
 * header pointing into `a`'s items. The same trade-off as string views: the
 * slice keeps the whole parent alive. */
static Value native_slice(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *arr = args[0].as.arr;
    int64_t start = args[1].as.i, end = args[2].as.i;
    if (start < 0 || end < start || end > arr->count) {
        ziman_error("runtime", line, col, "slice: range %lld..%lld is out of bounds for an array of length %d",
                    (long long)start, (long long)end, arr->count);
    }
    return value_array(type_scalar(TY_VOID), array_obj_view(arr, (int)start, (int)end));
}

static Value native_ab_to_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ArrayBuilderBuf *buf = ab_table_get(ab_handle_of(args[0]));
    if (buf->pkind != PK_NONE) {
        size_t esz = pack_elem_size(buf->pkind);
        unsigned char *out = zalloc_atomic(esz * buf->length);
        memcpy(out, buf->pdata, esz * buf->length);
        return value_array(type_scalar(TY_VOID), array_obj_new_packed(buf->pkind, out, (int)buf->length));
    }
    Value *items = buf->length > 0 ? malloc(buf->length * sizeof(Value)) : NULL;
    for (size_t i = 0; i < buf->length; i++) {
        items[i] = buf->items[i];
    }
    return value_array(type_scalar(TY_VOID), array_obj_new(items, (int)buf->length));
}

static Value native_ab_len(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ArrayBuilderBuf *buf = ab_table_get(ab_handle_of(args[0]));
    return value_int((int64_t)buf->length);
}

void prelude_register(void) {
    native_register("parse_int", native_parse_int);
    native_register("parse_float", native_parse_float);
    native_register("parse_bigint", native_parse_bigint);
    native_register("parse_decimal", native_parse_decimal);
    native_register("floor", native_floor);
    native_register("ceil", native_ceil);
    native_register("round", native_round);
    native_register("decimal_round", native_decimal_round);
    native_register("concat", native_concat);
    native_register("substring", native_substring);
    native_register("split", native_split);
    native_register("index_of", native_index_of);
    native_register("last_index_of", native_last_index_of);
    native_register("is_char_at", native_is_char_at);
    native_register("equals_ignore_case", native_equals_ignore_case);
    native_register("char_at", native_char_at);
    native_register("starts_with", native_starts_with);
    native_register("ends_with", native_ends_with);
    native_register("replace", native_replace);
    native_register("trim", native_trim);
    native_register("to_upper", native_to_upper);
    native_register("to_lower", native_to_lower);
    native_register("join", native_join);
    native_register("concat_arrays", native_concat_arrays);
    native_register("is_sortable_scalars", native_is_sortable_scalars);
    native_register("sort_scalar_order", native_sort_scalar_order);
    native_register("string_to_bytes", native_string_to_bytes);
    native_register("bytes_to_string", native_bytes_to_string);
    native_register("slice", native_slice);
    native_register("sb_new", native_sb_new);
    native_register("sb_append", native_sb_append);
    native_register("sb_append_byte", native_sb_append_byte);
    native_register("sb_to_string", native_sb_to_string);
    native_register("sb_len", native_sb_len);
    native_register("ab_new", native_ab_new);
    native_register("ab_append", native_ab_append);
    native_register("ab_to_array", native_ab_to_array);
    native_register("ab_len", native_ab_len);
}
