#define _POSIX_C_SOURCE 200809L
#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Type type_scalar(TypeKind kind) {
    Type t;
    t.kind = kind;
    t.elem = NULL;
    return t;
}

Type type_array(Type elem) {
    Type t;
    t.kind = TY_ARRAY;
    t.elem = malloc(sizeof(Type));
    *t.elem = elem;
    return t;
}

int type_equals(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == TY_ARRAY) return type_equals(*a.elem, *b.elem);
    return 1;
}

char *type_name(Type type) {
    switch (type.kind) {
        case TY_VOID: return strdup("void");
        case TY_INT: return strdup("int");
        case TY_FLOAT: return strdup("float");
        case TY_BOOL: return strdup("bool");
        case TY_STRING: return strdup("string");
        case TY_ARRAY: {
            char *inner = type_name(*type.elem);
            size_t len = strlen(inner) + 3; /* '[' + inner + ']' + '\0' */
            char *buf = malloc(len);
            snprintf(buf, len, "[%s]", inner);
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

#undef DEFINE_LIST_PUSH
