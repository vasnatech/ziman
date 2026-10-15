#define _POSIX_C_SOURCE 200809L
#include "ast.h"
#include "stack.h"
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every constructor below starts from a fully zeroed Type (including
 * `type_args`, itself a struct) so adding a new field never means hunting
 * down every constructor again. */
/* One per TypeKind, built at compile time rather than on first use.
 *
 * The first version filled each slot lazily and argued the race was harmless, on the grounds
 * that every writer stores the same bytes. ThreadSanitizer disagreed, and it was right: a
 * 64-byte struct assignment is not atomic, so a concurrent reader can see a half-written slot,
 * and "benign data race" is not a thing the C memory model has -- it is undefined behaviour
 * whatever the bytes happen to be. A designated initialiser makes the whole table a constant in
 * .rodata, which removes the race by having nothing to write, and is faster besides: no branch
 * and no store on the hottest path in the interpreter. */
static const Type g_scalar_types[TY_KIND_COUNT] = {
    [TY_VOID] = {.kind = TY_VOID},       [TY_UNIT] = {.kind = TY_UNIT},
    [TY_INT] = {.kind = TY_INT},         [TY_FLOAT] = {.kind = TY_FLOAT},
    [TY_BOOL] = {.kind = TY_BOOL},       [TY_STRING] = {.kind = TY_STRING},
    [TY_BIGINT] = {.kind = TY_BIGINT},   [TY_DECIMAL] = {.kind = TY_DECIMAL},
    [TY_BYTE] = {.kind = TY_BYTE},       [TY_ARRAY] = {.kind = TY_ARRAY},
    [TY_STRUCT] = {.kind = TY_STRUCT},   [TY_RESULT] = {.kind = TY_RESULT},
    [TY_ENUM] = {.kind = TY_ENUM},       [TY_CHANNEL] = {.kind = TY_CHANNEL},
    [TY_TYPEVAR] = {.kind = TY_TYPEVAR}, [TY_FUNC] = {.kind = TY_FUNC},
    [TY_MAP] = {.kind = TY_MAP},         [TY_SET] = {.kind = TY_SET},
};

const Type *type_scalar_ref(TypeKind kind) {
    return &g_scalar_types[kind];
}

/* A compound type that has to outlive the value carrying it.
 *
 * Allocated per call, not deduplicated. An intern table was built and measured (see
 * docs/MEMORY.md): it took a million lookups down to ten allocations of ten distinct types on a
 * JSON workload and made no measurable difference to the clock, because the allocations it
 * removed were cheap short-lived garbage and a hash probe costs about what allocating 64 bytes
 * did. It is worth revisiting for a long-running process that should not churn a Type per value
 * for days, but it is not what makes this change worth having -- that is `Value` going from 80
 * bytes to 24 -- and it cost two defects for no gain when it was tried. */
const Type *type_ref(Type t) {
    Type *out = ziman_xmalloc(sizeof(Type));
    *out = t;
    return out;
}

Type type_array(Type elem) {
    Type t = {0};
    t.kind = TY_ARRAY;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_struct(const char *name) {
    Type t = {0};
    t.kind = TY_STRUCT;
    t.struct_name = ziman_xstrdup(name);
    return t;
}

Type type_result(Type ok_type, Type err_type) {
    Type t = {0};
    t.kind = TY_RESULT;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = ok_type;
    t.elem_err = ziman_xmalloc(sizeof(Type));
    *t.elem_err = err_type;
    return t;
}

Type type_enum(const char *name) {
    Type t = {0};
    t.kind = TY_ENUM;
    t.enum_name = ziman_xstrdup(name);
    return t;
}

Type type_channel(Type elem) {
    Type t = {0};
    t.kind = TY_CHANNEL;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_map(Type key, Type value) {
    Type t = {0};
    t.kind = TY_MAP;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = key;
    t.elem_err = ziman_xmalloc(sizeof(Type));
    *t.elem_err = value;
    return t;
}

Type type_set(Type elem) {
    Type t = {0};
    t.kind = TY_SET;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_func(TypeList params, Type ret) {
    Type t = {0};
    t.kind = TY_FUNC;
    t.type_args = params;
    t.elem = ziman_xmalloc(sizeof(Type));
    *t.elem = ret;
    return t;
}

Type function_value_type(const FunctionDecl *fn) {
    TypeList params = {0};
    for (int i = 0; i < fn->params.count; i++) type_list_push(&params, fn->params.items[i].type);
    return type_func(params, fn->return_type);
}

Type type_typevar(const char *name) {
    Type t = {0};
    t.kind = TY_TYPEVAR;
    t.type_var_name = ziman_xstrdup(name);
    return t;
}

static int type_args_equal(TypeList a, TypeList b) {
    if (a.count != b.count) return 0;
    for (int i = 0; i < a.count; i++) {
        if (!type_equals(a.items[i], b.items[i])) return 0;
    }
    return 1;
}

int type_equals(Type a, Type b) {
    ZIMAN_STACK_CHECK();
    if (a.kind != b.kind) return 0;
    if (a.kind == TY_ARRAY) return type_equals(*a.elem, *b.elem);
    if (a.kind == TY_RESULT) return type_equals(*a.elem, *b.elem) && type_equals(*a.elem_err, *b.elem_err);
    /* `Box<int>` and `Box<string>` share a struct_name but are different
     * types -- type_args has to match too, not just the declared name. */
    if (a.kind == TY_STRUCT) return strcmp(a.struct_name, b.struct_name) == 0 && type_args_equal(a.type_args, b.type_args);
    if (a.kind == TY_ENUM) return strcmp(a.enum_name, b.enum_name) == 0 && type_args_equal(a.type_args, b.type_args);
    if (a.kind == TY_CHANNEL || a.kind == TY_SET) return type_equals(*a.elem, *b.elem);
    if (a.kind == TY_MAP) return type_equals(*a.elem, *b.elem) && type_equals(*a.elem_err, *b.elem_err);
    if (a.kind == TY_TYPEVAR) return strcmp(a.type_var_name, b.type_var_name) == 0;
    if (a.kind == TY_FUNC) return type_args_equal(a.type_args, b.type_args) && type_equals(*a.elem, *b.elem);
    return 1;
}

/* A qualified struct or enum name as a *reader* must write it. Internally the loader joins a
 * module path to a name with "::" (qualify_name, chosen so it can never collide with a bare name),
 * but the syntax for one is `fs.FsError`, so printing the internal form in a diagnostic hands the
 * reader a spelling the parser then refuses. The 2026-10-09 benchmark holdout caught exactly that
 * round trip: an agent copied `Result<int, fs::FsError>` out of one message and the next message
 * blamed the lowercase `fs` and suggested a primitive type instead, costing two probe rounds
 * (docs/reviews/llm-benchmark-2026-10-09.md). Fixed where it starts, rather than by teaching the
 * downstream message about "::". */
static char *display_name(const char *qualified) {
    char *out = ziman_xstrdup(qualified);
    char *from = out, *to = out;
    while (*from != '\0') {
        if (from[0] == ':' && from[1] == ':') {
            *to++ = '.';
            from += 2;
            continue;
        }
        *to++ = *from++;
    }
    *to = '\0';
    return out;
}

char *type_name(Type type) {
    ZIMAN_STACK_CHECK();
    switch (type.kind) {
        case TY_VOID: return ziman_xstrdup("void");
        case TY_INT: return ziman_xstrdup("int");
        case TY_FLOAT: return ziman_xstrdup("float");
        case TY_BOOL: return ziman_xstrdup("bool");
        case TY_STRING: return ziman_xstrdup("string");
        case TY_BIGINT: return ziman_xstrdup("bigint");
        case TY_DECIMAL: return ziman_xstrdup("decimal");
        case TY_BYTE: return ziman_xstrdup("byte");
        case TY_UNIT: return ziman_xstrdup("unit");
        case TY_TYPEVAR: return ziman_xstrdup(type.type_var_name);
        case TY_STRUCT: case TY_ENUM: {
            char *shown = display_name(type.kind == TY_STRUCT ? type.struct_name : type.enum_name);
            const char *base_name = shown;
            if (type.type_args.count == 0) return shown;
            /* `Box<int>`, `Pair<int, string>`, ... */
            size_t cap = 64, len = 0;
            char *out = ziman_xmalloc(cap);
            size_t base_len = strlen(base_name);
            while (len + base_len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            memcpy(out + len, base_name, base_len);
            len += base_len;
            out[len++] = '<';
            for (int i = 0; i < type.type_args.count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                char *arg = type_name(type.type_args.items[i]);
                size_t arg_len = strlen(arg);
                while (len + arg_len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                memcpy(out + len, arg, arg_len);
                len += arg_len;
                free(arg);
            }
            if (len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            out[len++] = '>';
            out[len] = '\0';
            free(shown); /* only the no-type-args path hands it straight back */
            return out;
        }
        case TY_ARRAY: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 3; /* '[' + inner + ']' + '\0' */
            char *buf = ziman_xmalloc(len);
            snprintf(buf, len, "[%s]", inner);
            free(inner);
            return buf;
        }
        case TY_RESULT: {
            char *ok = type_name(*type.elem);
            char *err = type_name(*type.elem_err);
            size_t len = strlen(ok) + strlen(err) + 11; /* "Result<" (7) + ok + ", " (2) + err + ">" (1) + '\0' (1) */
            char *buf = ziman_xmalloc(len);
            snprintf(buf, len, "Result<%s, %s>", ok, err);
            free(ok);
            free(err);
            return buf;
        }
        case TY_FUNC: {
            size_t cap = 32, len = 0;
            char *out = ziman_xmalloc(cap);
            const char *head = "fn(";
            memcpy(out, head, 3);
            len = 3;
            for (int i = 0; i < type.type_args.count; i++) {
                char *arg = type_name(type.type_args.items[i]);
                size_t arg_len = strlen(arg);
                while (len + arg_len + 3 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                if (i > 0) { out[len++] = ','; out[len++] = ' '; }
                memcpy(out + len, arg, arg_len);
                len += arg_len;
                free(arg);
            }
            char *ret = type.elem->kind == TY_VOID ? ziman_xstrdup("") : type_name(*type.elem);
            size_t ret_len = strlen(ret);
            while (len + ret_len + 8 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            out[len++] = ')';
            if (type.elem->kind != TY_VOID) {
                memcpy(out + len, " -> ", 4);
                len += 4;
                memcpy(out + len, ret, ret_len);
                len += ret_len;
            }
            out[len] = '\0';
            free(ret);
            return out;
        }
        case TY_MAP: {
            char *k = type_name(*type.elem);
            char *v = type_name(*type.elem_err);
            size_t len = strlen(k) + strlen(v) + 8; /* "Map<" + k + ", " + v + ">" + '\0' */
            char *buf = ziman_xmalloc(len);
            snprintf(buf, len, "Map<%s, %s>", k, v);
            free(k);
            free(v);
            return buf;
        }
        case TY_SET: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 7; /* "Set<" + inner + ">" + '\0' */
            char *buf = ziman_xmalloc(len);
            snprintf(buf, len, "Set<%s>", inner);
            free(inner);
            return buf;
        }
        case TY_CHANNEL: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 10; /* "Channel<" (8) + inner + ">" (1) + '\0' (1) */
            char *buf = ziman_xmalloc(len);
            snprintf(buf, len, "Channel<%s>", inner);
            free(inner);
            return buf;
        }
    }
    return ziman_xstrdup("unknown");
}

Expr *expr_new(ExprKind kind, int line, int col) {
    Expr *e = ziman_xcalloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->resolved_type = type_scalar(TY_VOID);
    e->resolved_depth = -1;
    return e;
}

Stmt *stmt_new(StmtKind kind, int line, int col) {
    Stmt *s = ziman_xcalloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    s->resolved_depth = -1;
    return s;
}

#define DEFINE_LIST_PUSH(fn_name, list_type, elem_type, field)                \
    void fn_name(list_type *list, elem_type value) {                          \
        if (list->count == list->capacity) {                                  \
            list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;    \
            list->field = ziman_xrealloc(list->field, (size_t)list->capacity * sizeof(elem_type)); \
        }                                                                      \
        list->field[list->count++] = value;                                   \
    }

DEFINE_LIST_PUSH(expr_list_push, ExprList, Expr *, items)
DEFINE_LIST_PUSH(stmt_list_push, StmtList, Stmt *, items)
DEFINE_LIST_PUSH(param_list_push, ParamList, Param, items)
DEFINE_LIST_PUSH(function_list_push, FunctionList, FunctionDecl *, items)
DEFINE_LIST_PUSH(field_init_list_push, FieldInitList, FieldInit, items)
DEFINE_LIST_PUSH(struct_list_push, StructList, StructDecl *, items)
DEFINE_LIST_PUSH(enum_variant_list_push, EnumVariantList, EnumVariant, items)
DEFINE_LIST_PUSH(enum_list_push, EnumList, EnumDecl *, items)
DEFINE_LIST_PUSH(use_list_push, UseList, UseDecl, items)
DEFINE_LIST_PUSH(type_param_list_push, TypeParamList, TypeParam, items)
DEFINE_LIST_PUSH(type_list_push, TypeList, Type, items)
DEFINE_LIST_PUSH(global_val_list_push, GlobalValList, GlobalValDecl *, items)

#undef DEFINE_LIST_PUSH
