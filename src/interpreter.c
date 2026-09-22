#define _POSIX_C_SOURCE 200809L
#include "interpreter.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "env.h"
#include "error.h"
#include "native.h"
#include "value.h"

typedef enum { CTRL_NONE, CTRL_RETURN, CTRL_BREAK, CTRL_CONTINUE } CtrlKind;

typedef struct {
    CtrlKind kind;
    Value value;
} ExecResult;

static FunctionDecl *find_function(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        if (strcmp(module->functions.items[i]->name, name) == 0) return module->functions.items[i];
    }
    return NULL;
}

static StructDecl *find_struct(Module *module, const char *name) {
    for (int i = 0; i < module->structs.count; i++) {
        if (strcmp(module->structs.items[i]->name, name) == 0) return module->structs.items[i];
    }
    return NULL;
}

static EnumDecl *find_enum(Module *module, const char *name) {
    for (int i = 0; i < module->enums.count; i++) {
        if (strcmp(module->enums.items[i]->name, name) == 0) return module->enums.items[i];
    }
    return NULL;
}

static Value eval_expr(Module *module, Env *env, Expr *e);
static ExecResult exec_stmt(Module *module, Env *env, Stmt *s);

static Value call_function(Module *module, FunctionDecl *fn, Value *args, int argc, int call_line, int call_col) {
    if (fn->is_native) {
        NativeFn native = native_lookup(fn->name);
        if (native == NULL) {
            ziman_error("runtime", call_line, call_col, "native function '%s' has no registered implementation", fn->name);
        }
        return native(args, argc, call_line, call_col);
    }
    Env *env = env_new(NULL);
    for (int i = 0; i < argc; i++) {
        env_define(env, fn->params.items[i].name, fn->params.items[i].type, 0, args[i]);
    }
    ExecResult result = {CTRL_NONE, value_int(0)};
    for (int i = 0; i < fn->body->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(module, env, fn->body->as.block.stmts.items[i]);
    }
    env_free(env);
    return result.kind == CTRL_RETURN ? result.value : value_int(0);
}

/* `spawn`'s payload: `args` is heap-allocated by the spawning thread (it
 * has to outlive that thread's own stack frame, unlike an ordinary call's
 * `args`, which is freed before the call even returns -- see EXPR_CALL
 * below) and is owned by the new thread from here on, freed once that
 * thread is done with it. */
typedef struct {
    Module *module;
    FunctionDecl *fn;
    Value *args;
    int argc;
    int line, col;
} SpawnArgs;

static void *spawn_thread_entry(void *raw) {
    SpawnArgs *sa = (SpawnArgs *)raw;
    call_function(sa->module, sa->fn, sa->args, sa->argc, sa->line, sa->col);
    free(sa->args);
    free(sa);
    return NULL;
}

static Value call_print(Value arg) {
    char *s = value_display(arg);
    printf("%s\n", s);
    free(s);
    return value_int(0);
}

static Value call_concat(Value a, Value b) {
    size_t la = strlen(a.as.s), lb = strlen(b.as.s);
    char *buf = malloc(la + lb + 1);
    memcpy(buf, a.as.s, la);
    memcpy(buf + la, b.as.s, lb);
    buf[la + lb] = '\0';
    return value_string(buf);
}

static Value call_str(Value arg) {
    return value_string(value_display(arg));
}

static Value call_len(Value v) {
    if (v.type.kind == TY_STRING) {
        return value_int((int64_t)strlen(v.as.s));
    }
    return value_int(v.as.arr->count);
}

static Value call_substring(Value s, Value start, Value end, int line, int col) {
    const char *str = s.as.s;
    int64_t len = (int64_t)strlen(str);
    int64_t st = start.as.i, en = end.as.i;
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

static Value call_split(Value s, Value sep, int line, int col) {
    const char *str = s.as.s;
    const char *delim = sep.as.s;
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

static Value call_trim(Value s) {
    const char *str = s.as.s;
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

static Value call_to_upper(Value s) {
    const char *str = s.as.s;
    size_t len = strlen(str);
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string(buf);
}

static Value call_to_lower(Value s) {
    const char *str = s.as.s;
    size_t len = strlen(str);
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)str[i]);
    buf[len] = '\0';
    return value_string(buf);
}

static Value call_parse_int(Value s) {
    Type result_type = type_result(type_scalar(TY_INT), type_scalar(TY_STRING));
    const char *str = s.as.s;
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

static Value call_parse_float(Value s) {
    Type result_type = type_result(type_scalar(TY_FLOAT), type_scalar(TY_STRING));
    const char *str = s.as.s;
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

static Value call_parse_bigint(Value s) {
    Type result_type = type_result(type_scalar(TY_BIGINT), type_scalar(TY_STRING));
    int ok;
    BigNum n = bignum_from_string(s.as.s, &ok);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot parse '%s' as bigint", s.as.s);
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(result_type, result_obj_new(1, value_bigint(n)));
}

static Value call_contains(Value first, Value second) {
    if (first.type.kind == TY_STRING) {
        return value_bool(strstr(first.as.s, second.as.s) != NULL);
    }
    ArrayObj *arr = first.as.arr;
    for (int i = 0; i < arr->count; i++) {
        if (value_equals(arr->items[i], second)) return value_bool(1);
    }
    return value_bool(0);
}

/* Scalars only (matching type_supports_hashing in typecheck.c) --
 * FNV-1a for strings, the raw bit pattern for a float, the value itself
 * for an int/bool. The sign bit is cleared before returning so the
 * result is always a non-negative int, safe to use directly with '%'
 * for bucket indexing (see the `collections` module's `Map<K, V>`). Not
 * a cryptographic hash, and not stable across a future language version
 * bumping this function's internals -- only ever used within a single
 * program run, never persisted. */
static Value call_hash(Value v) {
    uint64_t h;
    switch (v.type.kind) {
        case TY_INT:
            h = (uint64_t)v.as.i;
            break;
        case TY_BOOL:
            h = v.as.b ? 1 : 0;
            break;
        case TY_FLOAT:
            memcpy(&h, &v.as.f, sizeof(h));
            break;
        case TY_BIGINT: {
            /* FNV-1a folded over the sign and the normalized (trimmed)
             * digit array -- since two equal BigNums always have an
             * identical sign + digit-array representation (bignum.c
             * always trims leading zero limbs), equal values are
             * guaranteed to hash equal here too. */
            h = 1469598103934665603ULL;
            h ^= (uint64_t)(v.as.bi->sign + 1);
            h *= 1099511628211ULL;
            for (int i = 0; i < v.as.bi->count; i++) {
                h ^= v.as.bi->digits[i];
                h *= 1099511628211ULL;
            }
            break;
        }
        default: { /* TY_STRING -- the only other case type_supports_hashing allows */
            const unsigned char *s = (const unsigned char *)v.as.s;
            h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
            while (*s) {
                h ^= *s++;
                h *= 1099511628211ULL; /* FNV-1a 64-bit prime */
            }
            break;
        }
    }
    return value_int((int64_t)(h & 0x7FFFFFFFFFFFFFFFULL));
}

static Value call_abs(Value x) {
    if (x.type.kind == TY_INT) {
        return value_int(x.as.i < 0 ? -x.as.i : x.as.i);
    }
    if (x.type.kind == TY_BIGINT) {
        return value_bigint(bignum_abs(*x.as.bi));
    }
    return value_float(x.as.f < 0 ? -x.as.f : x.as.f);
}

static Value call_min(Value a, Value b) {
    if (a.type.kind == TY_INT) {
        return value_int(a.as.i < b.as.i ? a.as.i : b.as.i);
    }
    if (a.type.kind == TY_BIGINT) {
        return bignum_compare(*a.as.bi, *b.as.bi) < 0 ? a : b;
    }
    return value_float(a.as.f < b.as.f ? a.as.f : b.as.f);
}

static Value call_max(Value a, Value b) {
    if (a.type.kind == TY_INT) {
        return value_int(a.as.i > b.as.i ? a.as.i : b.as.i);
    }
    if (a.type.kind == TY_BIGINT) {
        return bignum_compare(*a.as.bi, *b.as.bi) > 0 ? a : b;
    }
    return value_float(a.as.f > b.as.f ? a.as.f : b.as.f);
}

static Value call_floor(Value x) { return value_int((int64_t)floor(x.as.f)); }
static Value call_ceil(Value x) { return value_int((int64_t)ceil(x.as.f)); }
static Value call_round(Value x) { return value_int((int64_t)round(x.as.f)); }

/* push/insert/set never mutate their `arr` argument -- they always build a
 * brand new ArrayObj and return a Value wrapping it, per docs/DESIGN.md's
 * value-semantics decision for arrays. Sharing `old->items[k]` Values by
 * plain struct copy into the new buffer is safe precisely because ArrayObj
 * is immutable once built (see value.h) -- there's nothing to alias into. */
static Value call_push(Type array_type, Value arr, Value x) {
    ArrayObj *old = arr.as.arr;
    int new_count = old->count + 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->items, (size_t)old->count * sizeof(Value));
    items[old->count] = x;
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_insert(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i > old->count) {
        ziman_error("runtime", line, col, "insert index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    int new_count = old->count + 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->items, (size_t)i * sizeof(Value));
    items[i] = x;
    memcpy(items + i + 1, old->items + i, (size_t)(old->count - (int)i) * sizeof(Value));
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_set(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "set index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    Value *items = malloc((size_t)old->count * sizeof(Value));
    memcpy(items, old->items, (size_t)old->count * sizeof(Value));
    items[i] = x;
    return value_array(array_type, array_obj_new(items, old->count));
}

static Value call_remove(Type array_type, Value arr, Value idx, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "remove index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    int new_count = old->count - 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->items, (size_t)i * sizeof(Value));
    memcpy(items + i, old->items + i + 1, (size_t)(new_count - (int)i) * sizeof(Value));
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_ok(Type result_type, Value x) {
    return value_result(result_type, result_obj_new(1, x));
}

static Value call_err(Type result_type, Value e) {
    return value_result(result_type, result_obj_new(0, e));
}

static Value call_is_ok(Value r) {
    return value_bool(r.as.res->is_ok);
}

static Value call_unwrap(Value r, int line, int col) {
    if (!r.as.res->is_ok) {
        char *msg = value_display(r.as.res->value);
        ziman_error("runtime", line, col, "unwrap called on an Err value: %s", msg);
    }
    return r.as.res->value;
}

static Value call_unwrap_err(Value r, int line, int col) {
    if (r.as.res->is_ok) {
        char *msg = value_display(r.as.res->value);
        ziman_error("runtime", line, col, "unwrap_err called on an Ok value: %s", msg);
    }
    return r.as.res->value;
}

/* args[0] is the format string, args[1..argc-1] are the interpolation
 * values. The type checker already verified `{}` placeholder count
 * matches argc-1 and that the format string is well-formed, so this
 * doesn't re-validate -- it trusts that pass. */
static Value call_format(Value *args, int argc) {
    const char *fmt = args[0].as.s;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    int arg_i = 1;

    for (const char *p = fmt; *p; p++) {
        if (p[0] == '{' && p[1] == '}') {
            char *piece = value_display(args[arg_i++]);
            size_t piece_len = strlen(piece);
            while (len + piece_len + 1 > cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + len, piece, piece_len);
            len += piece_len;
            free(piece);
            p++; /* skip the matching '}' */
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = *p;
        }
    }
    buf[len] = '\0';
    (void)argc;
    return value_string(buf);
}

static Value eval_binary(Module *module, Env *env, Expr *e) {
    Value l = eval_expr(module, env, e->as.binary.left);

    /* Short-circuit && / || before evaluating the right side. */
    if (e->as.binary.op == OP_AND) {
        if (!l.as.b) return value_bool(0);
        return eval_expr(module, env, e->as.binary.right);
    }
    if (e->as.binary.op == OP_OR) {
        if (l.as.b) return value_bool(1);
        return eval_expr(module, env, e->as.binary.right);
    }

    Value r = eval_expr(module, env, e->as.binary.right);

    switch (e->as.binary.op) {
        case OP_ADD:
            if (l.type.kind == TY_INT) return value_int(l.as.i + r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_add(*l.as.bi, *r.as.bi));
            return value_float(l.as.f + r.as.f);
        case OP_SUB:
            if (l.type.kind == TY_INT) return value_int(l.as.i - r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_sub(*l.as.bi, *r.as.bi));
            return value_float(l.as.f - r.as.f);
        case OP_MUL:
            if (l.type.kind == TY_INT) return value_int(l.as.i * r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_mul(*l.as.bi, *r.as.bi));
            return value_float(l.as.f * r.as.f);
        case OP_DIV:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_int(l.as.i / r.as.i);
            }
            if (l.type.kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(quot);
            }
            if (r.as.f == 0.0) ziman_error("runtime", e->line, e->col, "division by zero");
            return value_float(l.as.f / r.as.f);
        case OP_MOD:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_int(l.as.i % r.as.i);
            }
            if (l.type.kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(rem);
            }
            ziman_error("runtime", e->line, e->col, "internal error: '%%' on float should have been rejected by the type checker");
            return value_int(0);
        case OP_EQ: return value_bool(value_equals(l, r));
        case OP_NEQ: return value_bool(!value_equals(l, r));
        case OP_LT:
            if (l.type.kind == TY_INT) return value_bool(l.as.i < r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) < 0);
            return value_bool(l.as.f < r.as.f);
        case OP_GT:
            if (l.type.kind == TY_INT) return value_bool(l.as.i > r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) > 0);
            return value_bool(l.as.f > r.as.f);
        case OP_LE:
            if (l.type.kind == TY_INT) return value_bool(l.as.i <= r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) <= 0);
            return value_bool(l.as.f <= r.as.f);
        case OP_GE:
            if (l.type.kind == TY_INT) return value_bool(l.as.i >= r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) >= 0);
            return value_bool(l.as.f >= r.as.f);
        default:
            ziman_error("runtime", e->line, e->col, "internal error: unhandled binary operator");
            return value_int(0);
    }
}

/* `EnumName.member[(args)]` -- construction if `member` names one of the
 * enum's own variants, or an is_<variant>/unwrap_<variant> consumption
 * call otherwise. All shapes and argument counts were already validated
 * by check_enum_access in typecheck.c; this only needs to actually do it. */
static Value eval_enum_access(Module *module, Env *env, Expr *e) {
    const char *enum_name = e->as.enum_access.enum_name;
    const char *member = e->as.enum_access.member_name;
    EnumDecl *decl = find_enum(module, enum_name);

    for (int i = 0; i < decl->variants.count; i++) {
        EnumVariant *v = &decl->variants.items[i];
        if (strcmp(v->name, member) != 0) continue;
        Value payload = v->has_payload ? eval_expr(module, env, e->as.enum_access.args.items[0]) : value_int(0);
        EnumObj *obj = enum_obj_new(decl->name, v->name, v->has_payload, payload);
        return value_enum(e->resolved_type, obj);
    }

    Value arg = eval_expr(module, env, e->as.enum_access.args.items[0]);
    EnumObj *obj = arg.as.en;
    static const char *IS_PREFIX = "is_";
    static const char *UNWRAP_PREFIX = "unwrap_";
    if (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0) {
        return value_bool(strcmp(obj->variant_name, member + strlen(IS_PREFIX)) == 0);
    }
    /* unwrap_<variant> -- typecheck already proved the named variant has a
     * payload, but `obj` might currently hold a *different* variant of the
     * same enum at runtime; same fatal-by-default contract as Result's
     * unwrap/unwrap_err. */
    const char *wanted = member + strlen(UNWRAP_PREFIX);
    if (strcmp(obj->variant_name, wanted) != 0) {
        ziman_error("runtime", e->line, e->col, "called '%s.unwrap_%s' on a '%s.%s' value",
                    enum_name, wanted, enum_name, obj->variant_name);
    }
    return obj->payload;
}

static Value eval_expr(Module *module, Env *env, Expr *e) {
    switch (e->kind) {
        case EXPR_INT: return value_int(e->as.int_val);
        case EXPR_FLOAT: return value_float(e->as.float_val);
        case EXPR_STRING: return value_string(e->as.string_val);
        case EXPR_BOOL: return value_bool(e->as.bool_val);
        case EXPR_BIGINT: return value_bigint(e->as.bigint_val);
        case EXPR_IDENT: {
            Binding *b = env_lookup(env, e->as.ident_name);
            return b->value;
        }
        case EXPR_UNARY: {
            Value v = eval_expr(module, env, e->as.unary.operand);
            if (e->as.unary.op == OP_NEG) {
                if (v.type.kind == TY_INT) return value_int(-v.as.i);
                if (v.type.kind == TY_BIGINT) return value_bigint(bignum_negate(*v.as.bi));
                return value_float(-v.as.f);
            }
            return value_bool(!v.as.b);
        }
        case EXPR_BINARY:
            return eval_binary(module, env, e);
        case EXPR_CALL: {
            int argc = e->as.call.args.count;
            Value *args = argc > 0 ? malloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(module, env, e->as.call.args.items[i]);
            }
            Value result;
            if (strcmp(e->as.call.callee, "print") == 0) {
                result = call_print(args[0]);
            } else if (strcmp(e->as.call.callee, "concat") == 0) {
                result = call_concat(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "str") == 0) {
                result = call_str(args[0]);
            } else if (strcmp(e->as.call.callee, "format") == 0) {
                result = call_format(args, argc);
            } else if (strcmp(e->as.call.callee, "len") == 0) {
                result = call_len(args[0]);
            } else if (strcmp(e->as.call.callee, "push") == 0) {
                result = call_push(e->resolved_type, args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "insert") == 0) {
                result = call_insert(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "set") == 0) {
                result = call_set(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "remove") == 0) {
                result = call_remove(e->resolved_type, args[0], args[1], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "ok") == 0) {
                result = call_ok(e->resolved_type, args[0]);
            } else if (strcmp(e->as.call.callee, "err") == 0) {
                result = call_err(e->resolved_type, args[0]);
            } else if (strcmp(e->as.call.callee, "is_ok") == 0) {
                result = call_is_ok(args[0]);
            } else if (strcmp(e->as.call.callee, "unwrap") == 0) {
                result = call_unwrap(args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "unwrap_err") == 0) {
                result = call_unwrap_err(args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "substring") == 0) {
                result = call_substring(args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "split") == 0) {
                result = call_split(args[0], args[1], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "trim") == 0) {
                result = call_trim(args[0]);
            } else if (strcmp(e->as.call.callee, "to_upper") == 0) {
                result = call_to_upper(args[0]);
            } else if (strcmp(e->as.call.callee, "to_lower") == 0) {
                result = call_to_lower(args[0]);
            } else if (strcmp(e->as.call.callee, "parse_int") == 0) {
                result = call_parse_int(args[0]);
            } else if (strcmp(e->as.call.callee, "parse_float") == 0) {
                result = call_parse_float(args[0]);
            } else if (strcmp(e->as.call.callee, "parse_bigint") == 0) {
                result = call_parse_bigint(args[0]);
            } else if (strcmp(e->as.call.callee, "contains") == 0) {
                result = call_contains(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "hash") == 0) {
                result = call_hash(args[0]);
            } else if (strcmp(e->as.call.callee, "abs") == 0) {
                result = call_abs(args[0]);
            } else if (strcmp(e->as.call.callee, "min") == 0) {
                result = call_min(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "max") == 0) {
                result = call_max(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "floor") == 0) {
                result = call_floor(args[0]);
            } else if (strcmp(e->as.call.callee, "ceil") == 0) {
                result = call_ceil(args[0]);
            } else if (strcmp(e->as.call.callee, "round") == 0) {
                result = call_round(args[0]);
            } else if (strcmp(e->as.call.callee, "channel_new") == 0) {
                result = value_channel(e->resolved_type, channel_obj_new());
            } else if (strcmp(e->as.call.callee, "send") == 0) {
                channel_send(args[0].as.ch, args[1]);
                result = value_int(0);
            } else if (strcmp(e->as.call.callee, "receive") == 0) {
                result = channel_receive(args[0].as.ch);
            } else {
                FunctionDecl *fn = find_function(module, e->as.call.callee);
                result = call_function(module, fn, args, argc, e->line, e->col);
            }
            free(args);
            return result;
        }
        case EXPR_CAST: {
            Value v = eval_expr(module, env, e->as.cast.operand);
            TypeKind to = e->as.cast.target_type.kind;
            if (to == TY_FLOAT) {
                if (v.type.kind == TY_INT) return value_float((double)v.as.i);
                if (v.type.kind == TY_BIGINT) {
                    /* Lossy via a saturating int64 round-trip -- the same
                     * precision ceiling casting a very large int to float
                     * already has; a genuinely huge bigint can't be
                     * represented exactly by a float regardless. */
                    return value_float((double)bignum_to_i64_saturating(*v.as.bi));
                }
                return value_float(v.as.f);
            }
            if (to == TY_BIGINT) {
                if (v.type.kind == TY_INT) return value_bigint(bignum_from_i64(v.as.i));
                if (v.type.kind == TY_FLOAT) return value_bigint(bignum_from_i64((int64_t)v.as.f));
                return value_bigint(*v.as.bi); /* bigint as bigint: no-op */
            }
            /* target is TY_INT */
            if (v.type.kind == TY_FLOAT) return value_int((int64_t)v.as.f);
            if (v.type.kind == TY_BIGINT) return value_int(bignum_to_i64_saturating(*v.as.bi));
            return value_int(v.as.i);
        }
        case EXPR_ARRAY_LIT: {
            int count = e->as.array_lit.elements.count;
            Value *items = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
            for (int i = 0; i < count; i++) {
                items[i] = eval_expr(module, env, e->as.array_lit.elements.items[i]);
            }
            return value_array(e->resolved_type, array_obj_new(items, count));
        }
        case EXPR_INDEX: {
            Value arr = eval_expr(module, env, e->as.index.array);
            Value idx = eval_expr(module, env, e->as.index.index);
            int64_t i = idx.as.i;
            if (i < 0 || i >= arr.as.arr->count) {
                ziman_error("runtime", e->line, e->col, "index %lld out of bounds (array length %d)", (long long)i, arr.as.arr->count);
            }
            return arr.as.arr->items[i];
        }
        case EXPR_STRUCT_LIT: {
            StructDecl *decl = find_struct(module, e->as.struct_lit.type_name);
            int count = decl->fields.count;
            char **names = malloc((size_t)count * sizeof(char *));
            Value *fields = malloc((size_t)count * sizeof(Value));
            for (int i = 0; i < count; i++) {
                names[i] = decl->fields.items[i].name; /* borrowed from the StructDecl, never freed */
                Expr *value_expr = NULL;
                for (int j = 0; j < e->as.struct_lit.fields.count; j++) {
                    if (strcmp(e->as.struct_lit.fields.items[j].name, names[i]) == 0) {
                        value_expr = e->as.struct_lit.fields.items[j].value;
                        break;
                    }
                }
                fields[i] = eval_expr(module, env, value_expr);
            }
            return value_struct(e->resolved_type, struct_obj_new(names, fields, count));
        }
        case EXPR_FIELD_ACCESS: {
            Value base = eval_expr(module, env, e->as.field_access.base);
            StructObj *st = base.as.st;
            for (int i = 0; i < st->count; i++) {
                if (strcmp(st->field_names[i], e->as.field_access.field_name) == 0) {
                    return st->fields[i];
                }
            }
            ziman_error("runtime", e->line, e->col, "internal error: field '%s' not found at runtime", e->as.field_access.field_name);
            return value_int(0);
        }
        case EXPR_WITH: {
            Value base = eval_expr(module, env, e->as.with_expr.base);
            StructObj *old = base.as.st;
            int count = old->count;
            Value *fields = malloc((size_t)count * sizeof(Value));
            memcpy(fields, old->fields, (size_t)count * sizeof(Value));
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) {
                FieldInit *fi = &e->as.with_expr.overrides.items[i];
                for (int j = 0; j < count; j++) {
                    if (strcmp(old->field_names[j], fi->name) == 0) {
                        fields[j] = eval_expr(module, env, fi->value);
                        break;
                    }
                }
            }
            /* Reuses old->field_names directly (same struct type, same
             * field order) -- never mutates `old`, so sharing it is safe. */
            return value_struct(e->resolved_type, struct_obj_new(old->field_names, fields, count));
        }
        case EXPR_ENUM_ACCESS:
            return eval_enum_access(module, env, e);
    }
    ziman_error("runtime", e->line, e->col, "internal error: unhandled expression kind");
    return value_int(0);
}

static ExecResult no_ctrl(void) {
    ExecResult r = {CTRL_NONE, value_int(0)};
    return r;
}

/* Runs `block`'s statements directly in `scope` (no further scope nesting) --
 * shared by exec_block (which makes its own fresh scope first) and STMT_FOR
 * (which needs a fresh scope per iteration holding that iteration's loop
 * binder(s), ahead of the body's own statements). */
static ExecResult exec_stmts_in_scope(Module *module, Env *scope, Stmt *block) {
    ExecResult result = no_ctrl();
    for (int i = 0; i < block->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(module, scope, block->as.block.stmts.items[i]);
    }
    return result;
}

static ExecResult exec_block(Module *module, Env *parent, Stmt *block) {
    Env *scope = env_new(parent);
    ExecResult result = exec_stmts_in_scope(module, scope, block);
    env_free(scope);
    return result;
}

static ExecResult exec_stmt(Module *module, Env *env, Stmt *s) {
    switch (s->kind) {
        case STMT_VAL:
        case STMT_VAR: {
            Value v = eval_expr(module, env, s->as.decl.init);
            Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : v.type;
            env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, v);
            return no_ctrl();
        }
        case STMT_ASSIGN: {
            Binding *b = env_lookup(env, s->as.assign.name);
            b->value = eval_expr(module, env, s->as.assign.value);
            return no_ctrl();
        }
        case STMT_IF: {
            Value cond = eval_expr(module, env, s->as.if_stmt.cond);
            if (cond.as.b) {
                return exec_block(module, env, s->as.if_stmt.then_block);
            }
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    return exec_stmt(module, env, s->as.if_stmt.else_block);
                }
                return exec_block(module, env, s->as.if_stmt.else_block);
            }
            return no_ctrl();
        }
        case STMT_WHILE: {
            for (;;) {
                Value cond = eval_expr(module, env, s->as.while_stmt.cond);
                if (!cond.as.b) break;
                ExecResult result = exec_block(module, env, s->as.while_stmt.body);
                if (result.kind == CTRL_RETURN) return result;
                if (result.kind == CTRL_BREAK) break;
                /* CTRL_CONTINUE and CTRL_NONE both just fall through to the
                 * next iteration's condition check. */
            }
            return no_ctrl();
        }
        case STMT_FOR: {
            if (s->as.for_stmt.is_range) {
                Value start = eval_expr(module, env, s->as.for_stmt.range_start);
                Value end = eval_expr(module, env, s->as.for_stmt.range_end);
                for (int64_t i = start.as.i; i < end.as.i; i++) {
                    Env *scope = env_new(env);
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    env_free(scope);
                    if (result.kind == CTRL_RETURN) return result;
                    if (result.kind == CTRL_BREAK) break;
                }
                return no_ctrl();
            }

            Value iterable = eval_expr(module, env, s->as.for_stmt.iterable);
            ArrayObj *arr = iterable.as.arr;
            Type elem_type = *iterable.type.elem;
            for (int i = 0; i < arr->count; i++) {
                Env *scope = env_new(env);
                if (s->as.for_stmt.binder2 == NULL) {
                    env_define(scope, s->as.for_stmt.binder1, elem_type, 0, arr->items[i]);
                } else {
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    env_define(scope, s->as.for_stmt.binder2, elem_type, 0, arr->items[i]);
                }
                ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                env_free(scope);
                if (result.kind == CTRL_RETURN) return result;
                if (result.kind == CTRL_BREAK) break;
            }
            return no_ctrl();
        }
        case STMT_BREAK: {
            ExecResult r = {CTRL_BREAK, value_int(0)};
            return r;
        }
        case STMT_CONTINUE: {
            ExecResult r = {CTRL_CONTINUE, value_int(0)};
            return r;
        }
        case STMT_SPAWN: {
            FunctionDecl *fn = find_function(module, s->as.spawn_stmt.callee);
            int argc = s->as.spawn_stmt.args.count;
            Value *args = argc > 0 ? malloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(module, env, s->as.spawn_stmt.args.items[i]);
            }
            SpawnArgs *sa = malloc(sizeof(SpawnArgs));
            sa->module = module;
            sa->fn = fn;
            sa->args = args;
            sa->argc = argc;
            sa->line = s->line;
            sa->col = s->col;
            pthread_t thread;
            pthread_create(&thread, NULL, spawn_thread_entry, sa);
            pthread_detach(thread);
            return no_ctrl();
        }
        case STMT_RETURN: {
            ExecResult r;
            r.kind = CTRL_RETURN;
            r.value = s->as.ret.value != NULL ? eval_expr(module, env, s->as.ret.value) : value_int(0);
            return r;
        }
        case STMT_EXPR: {
            eval_expr(module, env, s->as.expr_stmt.expr);
            return no_ctrl();
        }
        case STMT_BLOCK:
            return exec_block(module, env, s);
    }
    return no_ctrl();
}

void interpret(Module *module) {
    FunctionDecl *main_fn = find_function(module, "main");
    call_function(module, main_fn, NULL, 0, main_fn->line, main_fn->col);
}
