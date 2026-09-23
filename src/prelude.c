#define _POSIX_C_SOURCE 200809L
#include "prelude.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
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

const char *prelude_source(void) {
    return PRELUDE_SOURCE;
}

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
    native_register("trim", native_trim);
    native_register("to_upper", native_to_upper);
    native_register("to_lower", native_to_lower);
    native_register("join", native_join);
}
