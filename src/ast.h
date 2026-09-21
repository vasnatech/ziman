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
    TY_STRUCT,
    TY_RESULT,
} TypeKind;

/* A Type is a small value (kind + optional payload), not a heap object in
 * its own right -- only TY_ARRAY/TY_RESULT (via `elem`/`elem_err`) and
 * TY_STRUCT (via `struct_name`) allocate anything, so that `[int]`,
 * `Result<[int], string>` etc. can nest. Compare types with type_equals(),
 * never `==`/`!=` (a struct comparison would be wrong once
 * `elem`/`struct_name` are involved). Struct types are nominal: two
 * structs are the same type only if they have the same declared name
 * (struct names are checked unique in typecheck(), so name equality *is*
 * identity here -- no need to carry a pointer to the declaration itself).
 *
 * `Result<T, E>` is a second hardcoded parametric type, same trick as
 * `[T]` -- not general user-definable generics (see docs/DESIGN.md).
 * `elem` doubles as "the array's element type" (TY_ARRAY) or "the
 * Result's success type" (TY_RESULT); `elem_err` is the Result's error
 * type and is unused otherwise. */
typedef struct Type {
    TypeKind kind;
    struct Type *elem;     /* TY_ARRAY: element type. TY_RESULT: success (Ok) type. */
    struct Type *elem_err; /* only meaningful when kind == TY_RESULT: the Err type */
    char *struct_name;     /* only meaningful when kind == TY_STRUCT */
} Type;

Type type_scalar(TypeKind kind); /* for anything but TY_ARRAY/TY_STRUCT/TY_RESULT */
Type type_array(Type elem);
Type type_struct(const char *name);
Type type_result(Type ok_type, Type err_type);
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
    EXPR_STRUCT_LIT,
    EXPR_FIELD_ACCESS,
    EXPR_WITH,
} ExprKind;

typedef struct Expr Expr;

typedef struct {
    Expr **items;
    int count;
    int capacity;
} ExprList;

/* `name: value` inside a struct literal (`Point { x: 1, y: 2 }`) or a
 * `with` expression's override list (`with p { x: 99 }`). */
typedef struct {
    char *name;
    Expr *value;
    int line, col;
} FieldInit;

typedef struct {
    FieldInit *items;
    int count;
    int capacity;
} FieldInitList;

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
        struct { char *type_name; FieldInitList fields; } struct_lit;
        struct { Expr *base; char *field_name; } field_access;
        struct { Expr *base; FieldInitList overrides; } with_expr;
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
    Stmt *body; /* STMT_BLOCK; NULL when is_native (a `native fn` has no body --
                 * its implementation is a registered C function, looked up by
                 * module path + name at call time) */
    int is_public;
    int is_native;
    int line, col;
} FunctionDecl;

typedef struct {
    FunctionDecl **items;
    int count;
    int capacity;
} FunctionList;

/* A struct's fields reuse Param -- a field is exactly "a typed name",
 * the same shape as a function parameter. */
typedef struct {
    char *name;
    ParamList fields;
    int is_public;
    int line, col;
} StructDecl;

typedef struct {
    StructDecl **items;
    int count;
    int capacity;
} StructList;

/* `use path/to/mod [as alias]`. `path` is the slash-joined module path
 * exactly as written (e.g. "models/user"); `qualifier` is the local name
 * this module is referenced by in code (the path's last segment, or the
 * `as` alias if one was given). Not resolved to anything yet at parse
 * time -- that's the loader's job, still ahead. */
typedef struct {
    char *path;
    char *qualifier;
    int line, col;
} UseDecl;

typedef struct {
    UseDecl *items;
    int count;
    int capacity;
} UseList;

typedef struct {
    FunctionList functions;
    StructList structs;
    UseList uses;
} Module;

/* Constructors. All AST nodes are heap-allocated and never freed --
 * the interpreter is a short-lived process (see docs/ROADMAP.md). */
Expr *expr_new(ExprKind kind, int line, int col);
Stmt *stmt_new(StmtKind kind, int line, int col);

void expr_list_push(ExprList *list, Expr *e);
void stmt_list_push(StmtList *list, Stmt *s);
void param_list_push(ParamList *list, Param p);
void function_list_push(FunctionList *list, FunctionDecl *f);
void field_init_list_push(FieldInitList *list, FieldInit fi);
void struct_list_push(StructList *list, StructDecl *s);
void use_list_push(UseList *list, UseDecl u);

#endif
