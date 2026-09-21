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
    TY_ENUM,
    TY_CHANNEL,
    TY_TYPEVAR,
} TypeKind;

/* A Type is a small value (kind + optional payload), not a heap object in
 * its own right -- only TY_ARRAY/TY_RESULT (via `elem`/`elem_err`) and
 * TY_STRUCT/TY_ENUM (via `struct_name`/`enum_name`) allocate anything, so
 * that `[int]`, `Result<[int], string>` etc. can nest. Compare types with
 * type_equals(), never `==`/`!=` (a struct comparison would be wrong once
 * `elem`/`struct_name` are involved). Struct and enum types are nominal:
 * two are the same type only if they have the same declared name (names
 * are checked unique in typecheck(), so name equality *is* identity here
 * -- no need to carry a pointer to the declaration itself).
 *
 * `Result<T, E>` is a second hardcoded parametric type, same trick as
 * `[T]` -- not general user-definable generics (see docs/DESIGN.md).
 * `elem` doubles as "the array's element type" (TY_ARRAY) or "the
 * Result's success type" (TY_RESULT); `elem_err` is the Result's error
 * type and is unused otherwise.
 *
 * `enum` (TY_ENUM) is a *user*-declared tagged union, not hardcoded like
 * `[T]`/`Result<T, E>` -- see EnumDecl below and docs/SYNTAX.md "Enums".
 * The parser can't tell a bare TYPE_IDENT type reference apart from a
 * struct name vs. an enum name (both are just PascalCase identifiers), so
 * it always provisionally builds a TY_STRUCT; the loader reclassifies it
 * to TY_ENUM once it knows which declaration the name actually matches
 * (see loader.c's self_qualify_type).
 *
 * `Channel<T>` (TY_CHANNEL) is a third hardcoded parametric type, same
 * trick as `[T]`/`Result<T, E>` -- reuses `elem` for its one type
 * parameter, same as arrays do, since a channel is a queue of `T` the
 * same way an array is a sequence of `T`. Unlike `enum`, this one has to
 * be hardcoded: its entire value is native, thread-safe blocking
 * send/receive, which no amount of user-definable generics would ever let
 * you express in pure Ziman. See docs/SYNTAX.md "Concurrency".
 *
 * User-declared generics (`struct Box<T> { value: T }`) add two more
 * things: TY_TYPEVAR, a placeholder meaning "the formal type parameter
 * named `type_var_name`", used only *inside* a generic declaration's own
 * field/variant/param types before any concrete type is known; and
 * `type_args`, the concrete types a generic struct/enum reference is
 * instantiated with (`Box<int>`'s `type_args` is `[int]`), empty for an
 * ordinary non-generic struct/enum. There's no inference anywhere --
 * every instantiation writes its type arguments explicitly, matching
 * Ziman's "explicit over implicit" instinct elsewhere and sidestepping a
 * real unification/inference implementation entirely. Substitution
 * happens on demand (substitute_type_vars in typecheck.c) wherever a
 * generic declaration's field/variant/param type is actually looked up
 * against a concrete instantiation -- never by generating copies of the
 * declaration itself, since the interpreter's `Value` representation is
 * already uniform regardless of static type. See docs/SYNTAX.md
 * "Generics". */
typedef struct Type Type;

/* A concrete type's type arguments (`Box<int>`'s is `[int]`) -- a plain
 * growable list, same shape as every other *List in this file. Empty
 * (count 0) for a non-generic struct/enum reference. */
typedef struct {
    Type *items;
    int count;
    int capacity;
} TypeList;

struct Type {
    TypeKind kind;
    struct Type *elem;     /* TY_ARRAY: element type. TY_RESULT: success (Ok) type. TY_CHANNEL: element type. */
    struct Type *elem_err; /* only meaningful when kind == TY_RESULT: the Err type */
    char *struct_name;     /* only meaningful when kind == TY_STRUCT */
    char *enum_name;       /* only meaningful when kind == TY_ENUM */
    char *type_var_name;   /* only meaningful when kind == TY_TYPEVAR */
    TypeList type_args;    /* TY_STRUCT/TY_ENUM only, when the declaration is generic */
};

Type type_scalar(TypeKind kind); /* for anything but TY_ARRAY/TY_STRUCT/TY_RESULT/TY_ENUM/TY_CHANNEL/TY_TYPEVAR */
Type type_array(Type elem);
Type type_struct(const char *name);
Type type_result(Type ok_type, Type err_type);
Type type_enum(const char *name);
Type type_channel(Type elem);
Type type_typevar(const char *name);
int type_equals(Type a, Type b);

void type_list_push(TypeList *list, Type t);

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
    EXPR_ENUM_ACCESS,
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
        /* `type_args` is only non-empty for `Name<Arg, ...> { ... }`
         * construction of a generic struct -- empty for an ordinary one. */
        struct { char *type_name; FieldInitList fields; TypeList type_args; } struct_lit;
        struct { Expr *base; char *field_name; } field_access;
        struct { Expr *base; FieldInitList overrides; } with_expr;
        /* `EnumName.Variant[(payload)]` (construction) or
         * `EnumName.function(args)` (an is_<variant>/unwrap_<variant>
         * consumption call, compiler-generated per enum) -- both share this
         * exact syntactic shape, so one node covers both; the type checker
         * tells them apart by looking `member_name` up against the named
         * enum's variants (see check_enum_access in typecheck.c).
         * `has_parens` distinguishes a bare unit-variant reference
         * (`Shape.Point`, args empty, has_parens false) from an explicit
         * `Shape.Point()` (args empty, has_parens true), which is a type
         * error -- a unit variant is written bare, never with empty
         * parens, to keep one way to write it. */
        /* `type_args` is only ever non-empty on a *construction* access
         * (`Option<int>.Some(5)`); a consumption call (`is_X`/`unwrap_X`)
         * never needs them written out -- the type checker reads the
         * concrete type straight off the argument's own already-known
         * type instead (see check_enum_access in typecheck.c). */
        struct { char *enum_name; char *member_name; ExprList args; int has_parens; TypeList type_args; } enum_access;
    } as;
};

typedef enum {
    STMT_VAL,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_SPAWN,
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
        /* `for binder1[, binder2] in <iterable | range_start..range_end> { body }`.
         * is_range: `for i in a..b`, binder2 is always NULL (no second binder
         * makes sense over a range -- there's nothing to name besides the
         * count itself). !is_range: `for x in arr` (binder2 NULL, binder1
         * gets each element) or `for i, x in arr` (binder1 gets the index,
         * binder2 gets the element). See docs/SYNTAX.md "for loops". */
        struct {
            char *binder1;
            char *binder2;
            int is_range;
            Expr *range_start;
            Expr *range_end;
            Expr *iterable;
            Stmt *body;
        } for_stmt;
        struct { Expr *value; } ret; /* value is NULL for a bare `return` */
        /* `spawn name(args)` or `spawn mod.name(args)` -- `callee` is the
         * same flat, possibly-dotted string EXPR_CALL already uses, so it
         * flows through the exact same self-qualify/cross-module
         * resolution the loader already has for calls. Statically resolved
         * against an already-declared function, never a first-class
         * function value -- see docs/SYNTAX.md "Concurrency". */
        struct { char *callee; ExprList args; } spawn_stmt;
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

/* The one hardcoded bound a type parameter can carry: "supports '=='."
 * Not a general trait/interface system (see docs/DESIGN.md's "Generics"
 * entry) -- added specifically so a generic `Map<K, V>`/`Set<T>` can
 * compare an abstract key type inside a generic function body; a
 * generic declaration still can't do anything else with an unbounded
 * type parameter beyond storing and passing it around. */
typedef enum {
    BOUND_NONE,
    BOUND_EQ,
} TypeParamBound;

typedef struct {
    char *name;
    TypeParamBound bound;
} TypeParam;

/* A generic declaration's own formal type parameters (`struct Box<T>`'s
 * is `[{"T", BOUND_NONE}]`, `Map<K: Eq, V>`'s is `[{"K", BOUND_EQ},
 * {"V", BOUND_NONE}]`). Empty for an ordinary, non-generic declaration.
 * Looked up by name, not index -- consistent with how everything else in
 * Ziman (struct fields, enum variants) is looked up by name rather than
 * position. */
typedef struct {
    TypeParam *items;
    int count;
    int capacity;
} TypeParamList;

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
 * the same shape as a function parameter. `type_params` is empty for an
 * ordinary, non-generic struct. */
typedef struct {
    char *name;
    ParamList fields;
    TypeParamList type_params;
    int is_public;
    int line, col;
} StructDecl;

typedef struct {
    StructDecl **items;
    int count;
    int capacity;
} StructList;

/* One case of an `enum` -- a name, and optionally one payload value's type.
 * `has_payload` false is a unit variant (`Point` in `enum Shape { ... Point
 * }`) -- the degenerate case of a general tagged union rather than a
 * separate mechanism, which is also what a closed set of plain named
 * constants (Java's enum shape) reduces to when nothing carries a payload.
 * There is deliberately no tuple- or struct-shaped variant payload (never
 * more than one value): Ziman has no tuple type, and anything needing more
 * than one payload value declares a separate `struct` and wraps it
 * (`Rectangle(RectDims)`), the same way `Result<T, E>`'s `Ok`/`Err` each
 * hold exactly one value. See docs/SYNTAX.md "Enums". */
typedef struct {
    char *name;
    int has_payload;
    Type payload_type; /* only meaningful when has_payload */
    int line, col;
} EnumVariant;

typedef struct {
    EnumVariant *items;
    int count;
    int capacity;
} EnumVariantList;

/* Nominal and closed, exactly like StructDecl: variants are fixed at
 * declaration, never reopened elsewhere. Access is always qualified by the
 * enum's own name (`Shape.Circle(5.0)`, `Shape.is_circle(s)`), never bare
 * -- so unlike struct fields, two different enums may freely reuse the
 * same variant name with no ambiguity. */
typedef struct {
    char *name;
    EnumVariantList variants;
    TypeParamList type_params; /* empty for an ordinary, non-generic enum */
    int is_public;
    int line, col;
} EnumDecl;

typedef struct {
    EnumDecl **items;
    int count;
    int capacity;
} EnumList;

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
    EnumList enums;
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
void enum_variant_list_push(EnumVariantList *list, EnumVariant v);
void enum_list_push(EnumList *list, EnumDecl *e);
void use_list_push(UseList *list, UseDecl u);
void type_param_list_push(TypeParamList *list, TypeParam p);

#endif
