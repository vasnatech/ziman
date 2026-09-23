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

/* Generated from corelib/prelude.zm by the same Makefile rule every
 * other bundled module's header goes through -- real .zm source, never
 * a hand-escaped C string literal (see corelib/prelude.zm's own header
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
    const char *str = args[0].as.s;
    char *endptr;
    errno = 0;
    long long v = strtoll(str, &endptr, 10);
    if (endptr == str || *endptr != '\0' || errno == ERANGE) {
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
    const char *str = args[0].as.s;
    char *endptr;
    errno = 0;
    double v = strtod(str, &endptr);
    if (endptr == str || *endptr != '\0' || errno == ERANGE) {
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
    BigNum n = bignum_from_string(args[0].as.s, &ok);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as bigint", args[0].as.s);
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
    Decimal d = decimal_from_string(args[0].as.s, &ok);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as decimal", args[0].as.s);
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

static Value native_concat(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    size_t la = strlen(args[0].as.s), lb = strlen(args[1].as.s);
    char *buf = malloc(la + lb + 1);
    memcpy(buf, args[0].as.s, la);
    memcpy(buf + la, args[1].as.s, lb);
    buf[la + lb] = '\0';
    return value_string(buf);
}

static Value native_substring(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *str = args[0].as.s;
    int64_t len = (int64_t)strlen(str);
    int64_t st = args[1].as.i, en = args[2].as.i;
    if (st < 0 || en > len || st > en) {
        ziman_error("runtime", line, col, "substring index out of bounds (start=%lld, end=%lld, length=%lld)",
                    (long long)st, (long long)en, (long long)len);
    }
    size_t n = (size_t)(en - st);
    char *buf = malloc(n + 1);
    memcpy(buf, str + st, n);
    buf[n] = '\0';
    return value_string(buf);
}

static Value native_index_of(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *str = args[0].as.s;
    const char *needle = args[1].as.s;
    int64_t len = (int64_t)strlen(str), from = args[2].as.i;
    if (from < 0 || from > len) {
        ziman_error("runtime", line, col, "index_of start out of bounds (from=%lld, length=%lld)", (long long)from, (long long)len);
    }
    const char *found = strstr(str + from, needle);
    return value_int(found ? (int64_t)(found - str) : -1);
}

static Value native_starts_with(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *str = args[0].as.s, *prefix = args[1].as.s;
    return value_bool(strncmp(str, prefix, strlen(prefix)) == 0);
}

static Value native_ends_with(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *str = args[0].as.s, *suffix = args[1].as.s;
    size_t ls = strlen(str), lx = strlen(suffix);
    return value_bool(lx <= ls && memcmp(str + ls - lx, suffix, lx) == 0);
}

static Value native_replace(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *str = args[0].as.s, *from = args[1].as.s, *to = args[2].as.s;
    size_t lf = strlen(from), lt = strlen(to);
    if (lf == 0) {
        ziman_error("runtime", line, col, "replace: the text to replace must not be empty");
    }
    size_t hits = 0;
    for (const char *p = str; (p = strstr(p, from)) != NULL; p += lf) hits++;
    char *buf = malloc(strlen(str) + hits * (lt > lf ? lt - lf : 0) + 1);
    char *out = buf;
    const char *cursor = str;
    for (const char *p; (p = strstr(cursor, from)) != NULL; cursor = p + lf) {
        memcpy(out, cursor, (size_t)(p - cursor));
        out += p - cursor;
        memcpy(out, to, lt);
        out += lt;
    }
    strcpy(out, cursor);
    return value_string(buf);
}

static Value native_split(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *str = args[0].as.s;
    const char *delim = args[1].as.s;
    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        ziman_error("runtime", line, col, "split: separator must not be empty");
    }
    Value *items = NULL;
    int count = 0, cap = 0;
    const char *cursor = str;
    for (;;) {
        const char *found = strstr(cursor, delim);
        size_t piece_len = found ? (size_t)(found - cursor) : strlen(cursor);
        char *piece = malloc(piece_len + 1);
        memcpy(piece, cursor, piece_len);
        piece[piece_len] = '\0';
        if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            items = realloc(items, (size_t)cap * sizeof(Value));
        }
        items[count++] = value_string(piece);
        if (!found) break;
        cursor = found + delim_len;
    }
    return value_array(type_array(type_scalar(TY_STRING)), array_obj_new(items, count));
}

static Value native_trim(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = args[0].as.s;
    size_t len = strlen(str);
    size_t start = 0, end = len;
    while (start < end && isspace((unsigned char)str[start])) start++;
    while (end > start && isspace((unsigned char)str[end - 1])) end--;
    size_t n = end - start;
    char *buf = malloc(n + 1);
    memcpy(buf, str + start, n);
    buf[n] = '\0';
    return value_string(buf);
}

static Value native_to_upper(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = args[0].as.s;
    size_t len = strlen(str);
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string(buf);
}

static Value native_to_lower(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *str = args[0].as.s;
    size_t len = strlen(str);
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string(buf);
}

/* An Option<string> argument's own payload, or NULL for None -- typecheck
 * already proved `opt` is genuinely an Option (see check_value_against_
 * param_type in typecheck.c), so this only ever has to look at which
 * variant it turned out to be at runtime, the same way unwrap_Some
 * itself would. */
static const char *option_string_or_null(Value opt) {
    EnumObj *obj = opt.as.en;
    if (strcmp(obj->variant_name, "Some") == 0) {
        return obj->payload.as.s;
    }
    return NULL;
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
    const char *prefix = option_string_or_null(args[0]);
    const char *suffix = option_string_or_null(args[1]);
    const char *separator = option_string_or_null(args[2]);
    ArrayObj *pieces = args[3].as.arr;

    size_t prefix_len = prefix != NULL ? strlen(prefix) : 0;
    size_t suffix_len = suffix != NULL ? strlen(suffix) : 0;
    size_t separator_len = separator != NULL ? strlen(separator) : 0;

    size_t total = prefix_len + suffix_len;
    for (int i = 0; i < pieces->count; i++) {
        total += strlen(pieces->items[i].as.s);
    }
    if (pieces->count > 1) {
        total += separator_len * (size_t)(pieces->count - 1);
    }

    char *buf = malloc(total + 1);
    size_t pos = 0;
    if (prefix != NULL) {
        memcpy(buf + pos, prefix, prefix_len);
        pos += prefix_len;
    }
    for (int i = 0; i < pieces->count; i++) {
        if (i > 0 && separator != NULL) {
            memcpy(buf + pos, separator, separator_len);
            pos += separator_len;
        }
        size_t piece_len = strlen(pieces->items[i].as.s);
        memcpy(buf + pos, pieces->items[i].as.s, piece_len);
        pos += piece_len;
    }
    if (suffix != NULL) {
        memcpy(buf + pos, suffix, suffix_len);
        pos += suffix_len;
    }
    buf[total] = '\0';
    return value_string(buf);
}

/* --- StringBuilder -------------------------------------------------
 * A mutable string-accumulation handle, backed entirely here, native-
 * side -- Ziman code never sees anything but an opaque `handle: int`
 * (see corelib/prelude.zm's StringBuilder struct). Each handle indexes
 * into `g_sb_table`, an array of *pointers* to individually heap-
 * allocated StringBuilderBuf objects: growing the table (realloc'ing
 * the array of pointers) never invalidates a StringBuilderBuf a caller
 * already holds a pointer to, since only the pointers move, never the
 * buffers themselves. `g_sb_table_mutex` guards every read or write of
 * the table itself (registering a new handle, or looking one up) --
 * necessary because reading a pointer variable that another thread
 * might concurrently be reallocating is undefined behavior in C even if
 * the bytes involved would turn out identical. Appending to one
 * already-fetched StringBuilderBuf happens outside that lock, entirely
 * unsynchronized between callers -- same as Java's own (non-thread-
 * safe) StringBuilder: one is meant to be built up by a single thread
 * at a time, not shared. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    double grow_factor;
} StringBuilderBuf;

static StringBuilderBuf **g_sb_table = NULL;
static int g_sb_count = 0;
static int g_sb_capacity = 0;
static pthread_mutex_t g_sb_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static int sb_table_register(StringBuilderBuf *buf) {
    pthread_mutex_lock(&g_sb_table_mutex);
    if (g_sb_count == g_sb_capacity) {
        g_sb_capacity = g_sb_capacity == 0 ? 16 : g_sb_capacity * 2;
        g_sb_table = realloc(g_sb_table, (size_t)g_sb_capacity * sizeof(StringBuilderBuf *));
    }
    int handle = g_sb_count++;
    g_sb_table[handle] = buf;
    pthread_mutex_unlock(&g_sb_table_mutex);
    return handle;
}

static StringBuilderBuf *sb_table_get(int handle) {
    pthread_mutex_lock(&g_sb_table_mutex);
    StringBuilderBuf *buf = g_sb_table[handle];
    pthread_mutex_unlock(&g_sb_table_mutex);
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

/* Every StringBuilder value shares this same one-field shape, so unlike
 * a struct built from real .zm source (whose field_names array is
 * borrowed from its StructDecl -- see value.h's StructObj comment),
 * this is just a small static array: a native function has no Module*
 * to look a StructDecl up in at all, and the content is identical for
 * every instance regardless. */
static char *STRINGBUILDER_FIELD_NAMES[] = {"handle"};

static int sb_handle_of(Value sb) {
    return (int)sb.as.st->fields[0].as.i;
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
    buf->data = buf->capacity > 0 ? malloc(buf->capacity) : NULL;

    int handle = sb_table_register(buf);

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
        const char *s = pieces->items[i].as.s;
        size_t len = strlen(s);
        sb_ensure_capacity(buf, len);
        memcpy(buf->data + buf->length, s, len);
        buf->length += len;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

static Value native_sb_to_string(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    StringBuilderBuf *buf = sb_table_get(sb_handle_of(args[0]));
    char *out = malloc(buf->length + 1);
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
typedef struct {
    Value *items;
    size_t length;
    size_t capacity;
    double grow_factor;
} ArrayBuilderBuf;

static ArrayBuilderBuf **g_ab_table = NULL;
static int g_ab_count = 0;
static int g_ab_capacity = 0;
static pthread_mutex_t g_ab_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ab_table_register(ArrayBuilderBuf *buf) {
    pthread_mutex_lock(&g_ab_table_mutex);
    if (g_ab_count == g_ab_capacity) {
        g_ab_capacity = g_ab_capacity == 0 ? 16 : g_ab_capacity * 2;
        g_ab_table = realloc(g_ab_table, (size_t)g_ab_capacity * sizeof(ArrayBuilderBuf *));
    }
    int handle = g_ab_count++;
    g_ab_table[handle] = buf;
    pthread_mutex_unlock(&g_ab_table_mutex);
    return handle;
}

static ArrayBuilderBuf *ab_table_get(int handle) {
    pthread_mutex_lock(&g_ab_table_mutex);
    ArrayBuilderBuf *buf = g_ab_table[handle];
    pthread_mutex_unlock(&g_ab_table_mutex);
    return buf;
}

static char *ARRAYBUILDER_FIELD_NAMES[] = {"handle"};

static int ab_handle_of(Value ab) {
    return (int)ab.as.st->fields[0].as.i;
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
    buf->items = realloc(buf->items, new_capacity * sizeof(Value));
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

    int handle = ab_table_register(buf);

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
    ab_ensure_capacity(buf, (size_t)pieces->count);
    for (int i = 0; i < pieces->count; i++) {
        buf->items[buf->length] = pieces->items[i];
        buf->length++;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

static Value native_ab_to_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ArrayBuilderBuf *buf = ab_table_get(ab_handle_of(args[0]));
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
    native_register("starts_with", native_starts_with);
    native_register("ends_with", native_ends_with);
    native_register("replace", native_replace);
    native_register("trim", native_trim);
    native_register("to_upper", native_to_upper);
    native_register("to_lower", native_to_lower);
    native_register("join", native_join);
    native_register("sb_new", native_sb_new);
    native_register("sb_append", native_sb_append);
    native_register("sb_to_string", native_sb_to_string);
    native_register("sb_len", native_sb_len);
    native_register("ab_new", native_ab_new);
    native_register("ab_append", native_ab_append);
    native_register("ab_to_array", native_ab_to_array);
    native_register("ab_len", native_ab_len);
}
