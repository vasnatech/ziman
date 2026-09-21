#ifndef ZIMAN_AST_H
#define ZIMAN_AST_H

#include <stdint.h>

typedef enum {
    TY_VOID,
    TY_INT,
    TY_FLOAT,
    TY_BOOL,
    TY_STRING,
    TY_ARRAY,
} TypeKind;

/* A Type is a small value (kind + an optional pointer), not a heap object in
 * its own right -- only TY_ARRAY allocates, for its element type, so that
 * `[int]`, `[[int]]` etc. can nest. Compare types with type_equals(), never
 * `==`/`!=` (a struct comparison would be wrong once `elem` is involved). */
typedef struct Type {
    TypeKind kind;
    struct Type *elem; /* only meaningful when kind == TY_ARRAY */
} Type;

Type type_scalar(TypeKind kind); /* for anything but TY_ARRAY */
Type type_array(Type elem);
int type_equals(Type a, Type b);

/* Heap-allocated, human-readable name ("int", "[int]", "[[string]]", ...).
 * Never freed -- see the no-free policy note on the AST constructors below;
 * every call site uses this immediately to build an error message. */
char *type_name(Type type);

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR,
    OP_NEG, OP_NOT,
} Op;

typedef enum {
    EXPR_INT,
    EXPR_FLOAT,
    EXPR_STRING,
    EXPR_BOOL,
    EXPR_IDENT,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_CAST,
    EXPR_ARRAY_LIT,
    EXPR_INDEX,
} ExprKind;

typedef struct Expr Expr;

typedef struct {
    Expr **items;
    int count;
    int capacity;
} ExprList;

struct Expr {
    ExprKind kind;
    int line, col;
    Type resolved_type; /* filled in by the type checker */

    union {
        int64_t int_val;
        double float_val;
        char *string_val; /* heap, NUL-terminated */
        int bool_val;
        char *ident_name;
        struct { Op op; Expr *left; Expr *right; } binary;
        struct { Op op; Expr *operand; } unary;
        struct { char *callee; ExprList args; } call;
        struct { Expr *operand; Type target_type; } cast;
        struct { ExprList elements; } array_lit;
        struct { Expr *array; Expr *index; } index;
    } as;
};

typedef enum {
    STMT_VAL,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_IF,
    STMT_WHILE,
    STMT_RETURN,
    STMT_EXPR,
    STMT_BLOCK,
} StmtKind;

typedef struct Stmt Stmt;

typedef struct {
    Stmt **items;
    int count;
    int capacity;
} StmtList;

struct Stmt {
    StmtKind kind;
    int line, col;

    union {
        struct { char *name; Type declared_type; int has_declared_type; Expr *init; } decl;
        struct { char *name; Expr *value; } assign;
        struct { Expr *cond; Stmt *then_block; Stmt *else_block; } if_stmt;
        struct { Expr *cond; Stmt *body; } while_stmt;
        struct { Expr *value; } ret; /* value is NULL for a bare `return` */
        struct { Expr *expr; } expr_stmt;
        struct { StmtList stmts; } block;
    } as;
};

typedef struct {
    char *name;
    Type type;
    int line, col;
} Param;

typedef struct {
    Param *items;
    int count;
    int capacity;
} ParamList;

typedef struct {
    char *name;
    ParamList params;
    Type return_type;
    Stmt *body; /* STMT_BLOCK */
    int line, col;
} FunctionDecl;

typedef struct {
    FunctionDecl **items;
    int count;
    int capacity;
} FunctionList;

typedef struct {
    FunctionList functions;
} Program;

/* Constructors. All AST nodes are heap-allocated and never freed --
 * the interpreter is a short-lived process (see docs/ROADMAP.md). */
Expr *expr_new(ExprKind kind, int line, int col);
Stmt *stmt_new(StmtKind kind, int line, int col);

void expr_list_push(ExprList *list, Expr *e);
void stmt_list_push(StmtList *list, Stmt *s);
void param_list_push(ParamList *list, Param p);
void function_list_push(FunctionList *list, FunctionDecl *f);

#endif
