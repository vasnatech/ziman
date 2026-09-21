#include "ast.h"

#include <stdlib.h>

const char *type_name(Type type) {
    switch (type) {
        case TY_VOID: return "void";
        case TY_INT: return "int";
        case TY_FLOAT: return "float";
        case TY_BOOL: return "bool";
        case TY_STRING: return "string";
    }
    return "unknown";
}

Expr *expr_new(ExprKind kind, int line, int col) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->resolved_type = TY_VOID;
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
