#define _POSIX_C_SOURCE 200809L
#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every constructor below starts from a fully zeroed Type (including
 * `type_args`, itself a struct) so adding a new field never means hunting
 * down every constructor again. */
Type type_scalar(TypeKind kind) {
    Type t = {0};
    t.kind = kind;
    return t;
}

Type type_array(Type elem) {
    Type t = {0};
    t.kind = TY_ARRAY;
    t.elem = malloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_struct(const char *name) {
    Type t = {0};
    t.kind = TY_STRUCT;
    t.struct_name = strdup(name);
    return t;
}

Type type_result(Type ok_type, Type err_type) {
    Type t = {0};
    t.kind = TY_RESULT;
    t.elem = malloc(sizeof(Type));
    *t.elem = ok_type;
    t.elem_err = malloc(sizeof(Type));
    *t.elem_err = err_type;
    return t;
}

Type type_enum(const char *name) {
    Type t = {0};
    t.kind = TY_ENUM;
    t.enum_name = strdup(name);
    return t;
}

Type type_channel(Type elem) {
    Type t = {0};
    t.kind = TY_CHANNEL;
    t.elem = malloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_map(Type key, Type value) {
    Type t = {0};
    t.kind = TY_MAP;
    t.elem = malloc(sizeof(Type));
    *t.elem = key;
    t.elem_err = malloc(sizeof(Type));
    *t.elem_err = value;
    return t;
}

Type type_set(Type elem) {
    Type t = {0};
    t.kind = TY_SET;
    t.elem = malloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

Type type_func(TypeList params, Type ret) {
    Type t = {0};
    t.kind = TY_FUNC;
    t.type_args = params;
    t.elem = malloc(sizeof(Type));
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
    t.type_var_name = strdup(name);
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

char *type_name(Type type) {
    switch (type.kind) {
        case TY_VOID: return strdup("void");
        case TY_INT: return strdup("int");
        case TY_FLOAT: return strdup("float");
        case TY_BOOL: return strdup("bool");
        case TY_STRING: return strdup("string");
        case TY_BIGINT: return strdup("bigint");
        case TY_DECIMAL: return strdup("decimal");
        case TY_BYTE: return strdup("byte");
        case TY_TYPEVAR: return strdup(type.type_var_name);
        case TY_STRUCT: case TY_ENUM: {
            const char *base_name = type.kind == TY_STRUCT ? type.struct_name : type.enum_name;
            if (type.type_args.count == 0) return strdup(base_name);
            /* `Box<int>`, `Pair<int, string>`, ... */
            size_t cap = 64, len = 0;
            char *out = malloc(cap);
            size_t base_len = strlen(base_name);
            while (len + base_len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + len, base_name, base_len);
            len += base_len;
            out[len++] = '<';
            for (int i = 0; i < type.type_args.count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                char *arg = type_name(type.type_args.items[i]);
                size_t arg_len = strlen(arg);
                while (len + arg_len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                memcpy(out + len, arg, arg_len);
                len += arg_len;
                free(arg);
            }
            if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[len++] = '>';
            out[len] = '\0';
            return out;
        }
        case TY_ARRAY: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 3; /* '[' + inner + ']' + '\0' */
            char *buf = malloc(len);
            snprintf(buf, len, "[%s]", inner);
            free(inner);
            return buf;
        }
        case TY_RESULT: {
            char *ok = type_name(*type.elem);
            char *err = type_name(*type.elem_err);
            size_t len = strlen(ok) + strlen(err) + 11; /* "Result<" (7) + ok + ", " (2) + err + ">" (1) + '\0' (1) */
            char *buf = malloc(len);
            snprintf(buf, len, "Result<%s, %s>", ok, err);
            free(ok);
            free(err);
            return buf;
        }
        case TY_FUNC: {
            size_t cap = 32, len = 0;
            char *out = malloc(cap);
            const char *head = "fn(";
            memcpy(out, head, 3);
            len = 3;
            for (int i = 0; i < type.type_args.count; i++) {
                char *arg = type_name(type.type_args.items[i]);
                size_t arg_len = strlen(arg);
                while (len + arg_len + 3 > cap) { cap *= 2; out = realloc(out, cap); }
                if (i > 0) { out[len++] = ','; out[len++] = ' '; }
                memcpy(out + len, arg, arg_len);
                len += arg_len;
                free(arg);
            }
            char *ret = type.elem->kind == TY_VOID ? strdup("") : type_name(*type.elem);
            size_t ret_len = strlen(ret);
            while (len + ret_len + 8 > cap) { cap *= 2; out = realloc(out, cap); }
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
            char *buf = malloc(len);
            snprintf(buf, len, "Map<%s, %s>", k, v);
            free(k);
            free(v);
            return buf;
        }
        case TY_SET: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 7; /* "Set<" + inner + ">" + '\0' */
            char *buf = malloc(len);
            snprintf(buf, len, "Set<%s>", inner);
            free(inner);
            return buf;
        }
        case TY_CHANNEL: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 10; /* "Channel<" (8) + inner + ">" (1) + '\0' (1) */
            char *buf = malloc(len);
            snprintf(buf, len, "Channel<%s>", inner);
            free(inner);
            return buf;
        }
    }
    return strdup("unknown");
}

Expr *expr_new(ExprKind kind, int line, int col) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->resolved_type = type_scalar(TY_VOID);
    return e;
}

Stmt *stmt_new(StmtKind kind, int line, int col) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

#define DEFINE_LIST_PUSH(fn_name, list_type, elem_type, field)                \
    void fn_name(list_type *list, elem_type value) {                          \
        if (list->count == list->capacity) {                                  \
            list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;    \
            list->field = realloc(list->field, (size_t)list->capacity * sizeof(elem_type)); \
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
