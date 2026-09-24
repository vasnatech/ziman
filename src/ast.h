#ifndef ZIMAN_AST_H
#define ZIMAN_AST_H

#include <stdint.h>

#include "bignum.h"
#include "decimal.h"

typedef enum {
    TY_VOID,
    TY_INT,
    TY_FLOAT,
    TY_BOOL,
    TY_STRING,
    TY_BIGINT, /* arbitrary-precision integer -- see src/bignum.h */
    TY_DECIMAL, /* arbitrary-precision decimal -- see src/decimal.h */
    TY_BYTE, /* unsigned, 0-255 -- deliberately not a number to compute
              * with: no arithmetic operators, no abs/min/max, just
              * construction, ==/!=, hash, str/print/format, and a cast
              * to/from int. See docs/DESIGN.md's "byte" entry. */
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
    EXPR_BIGINT,
    EXPR_DECIMAL,
    EXPR_BYTE,
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
    EXPR_TRY_UNWRAP,
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
        BigNum bigint_val; /* computed once, directly from the literal's raw digits, at parse time */
        Decimal decimal_val; /* likewise, computed once at parse time */
        int byte_val; /* 0-255, range-checked at parse time */
        char *ident_name;
        struct { Op op; Expr *left; Expr *right; } binary;
        struct { Op op; Expr *operand; } unary;
        /* `type_args` is only non-empty for `name<Args>(...)` -- a call
         * to a generic function. Empty for an ordinary one.
         * `arg_names` is NULL for an ordinary positional call
         * (`f(1, 2)`); for a named call (`f(x: 1, y: 2)`) it holds one
         * name per entry of `args`, same order as written. A call is
         * always entirely one or the other -- the parser rejects mixing
         * them, so `arg_names` is either NULL or fully populated, never
         * partial. check_call reorders `args` into the callee's declared
         * parameter order and clears `arg_names` back to NULL once it
         * has, so nothing past type-checking (the interpreter included)
         * ever needs to know a call was written with names at all.
         * `variadic_tail_is_array_spread` is only meaningful when the
         * callee's last parameter is variadic and exactly one trailing
         * argument was written -- the "did the caller pass one element,
         * or a whole already-built array" ambiguity check_call alone can
         * resolve, decisively, from that argument's own static type
         * (see check_call's variadic-tail comment). Recorded here rather
         * than re-derived at runtime because runtime has no reliable way
         * to: comparing a value's own `.type.kind == TY_ARRAY` silently
         * gives the wrong answer whenever the variadic element type is
         * itself an array (`ArrayBuilder<[T]>`'s `ab_append<[T]>`, for
         * instance) -- a single element of type `[T]` also happens to
         * satisfy "is an array", the exact case that exposed this. */
        struct { char *callee; ExprList args; TypeList type_args; char **arg_names; int variadic_tail_is_array_spread; } call;
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
        /* `inner?` -- postfix Option<T>/Result<T, E> unwrap-or-propagate
         * (see docs/DESIGN.md's "? operator" entry). `inner`'s own type
         * decides which of the two this is at type-check time; nothing
         * here records that choice, since the interpreter re-derives it
         * cheaply from the runtime value itself (an Option's EnumObj vs.
         * a Result's ResultObj, exactly like is_ok/Option.is_Some
         * already do). `propagated_type` is check_try_unwrap's one real
         * output beyond `inner`'s own checking: the *enclosing
         * function's* full, concrete Option<T2>/Result<T2, E> return
         * type, filled in once at type-check time and reused verbatim
         * to build the propagated None/Err value at runtime -- since
         * NativeFn-style plumbing to hand the interpreter that context
         * some other way doesn't exist, and doesn't need to: it's
         * already known, and already static, right here. Only ever
         * legal as the entire initializer of a `val`/`var` or as its
         * own bare statement, never nested inside another expression --
         * enforced by check_expr's own EXPR_TRY_UNWRAP case, which is
         * only ever reached when it wasn't already intercepted by one
         * of those two statement forms. */
        struct { Expr *inner; Type propagated_type; } try_unwrap;
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
    STMT_DEFER,
    STMT_SWITCH,
    STMT_RETURN,
    STMT_EXPR,
    STMT_BLOCK,
} StmtKind;

typedef struct Stmt Stmt;

/* One arm of a `switch`: a bare variant name, an optional payload binder
 * (`Circle(r)`), and its block. */
typedef struct {
    char *variant;
    char *binder; /* NULL when the arm has no `(name)` */
    Stmt *body;
    int line, col;
} SwitchArm;

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
        /* `defer f(args)` -- runs the call when the enclosing *block* exits,
         * by any path (falling off the end, return, `?`, break, continue),
         * last-in first-out; see docs/DESIGN.md's "defer" entry. `call` is
         * the call as written. Its arguments are evaluated when the
         * `defer` statement itself runs, into the hidden variables named
         * `hidden` (one per argument, `$`-containing so no source
         * identifier can collide), and `replay` -- built once by
         * type-checking, after check_call has finalised the argument
         * list -- is the same call with each argument replaced by a
         * reference to its hidden variable. That is what actually runs
         * at block exit, so arguments read the values they had at
         * `defer` time, not whatever a `var` holds by then. */
        struct { Expr *call; Expr *replay; char **hidden; int hidden_count; } defer_stmt;
        struct { StmtList stmts; } block;
        /* `switch expr { Variant(binder) { ... } ... }` over an enum or a
         * Result, one arm per variant, none omitted. Type-checking rewrites
         * it in place into a block holding a hidden `val` for the scrutinee
         * and an if/else chain over the existing is_X/unwrap_X consumption
         * calls, so the interpreter never sees this node. */
        struct { Expr *scrutinee; SwitchArm *arms; int arm_count; } switch_stmt;
    } as;
};

typedef struct {
    char *name;
    Type type; /* the element type, e.g. `string` for `pieces: string...` --
                * NOT the array type; see is_variadic below for why. */
    /* True only for the last Param in a ParamList (enforced at parse
     * time -- see parser.c's parse_params). `type` above stays the
     * *element* type (`string`, not `[string]`) so ordinary code that
     * reads a Param's type (substitute_type, reclassify_type_params,
     * self_qualify_type, ...) needs no special case for a variadic
     * parameter at all -- the one place that does care (binding the
     * parameter into a call's own scope, in check_function/
     * call_function) wraps it in `[T]` right there, and the one place
     * that needs to know a call is *to* a variadic function
     * (check_call/call_function) reads this flag directly off the
     * callee's last declared Param. */
    int is_variadic;
    /* `= expr` after the type -- NULL for every parameter without one.
     * Only legal on an Option<T> parameter (check_function enforces
     * this; a struct's own fields, which also use Param, never carry
     * one -- parse_struct_decl always leaves this NULL). Omitting an
     * Option<T> parameter is only ever legal in a *named* call (see
     * check_call's canonicalize_named_args), where it's filled in with
     * either this default (auto-wrapped to Some(T) once, up front, if
     * it was written as a bare T rather than an Option<T> already) or,
     * when no default was written at all, `None`. */
    Expr *default_expr;
    /* Set at parse time whenever `default_expr` is (NULL otherwise) --
     * a synthetic, unforgeable (contains '$', which no real Ziman
     * identifier can) global-env key. init_param_defaults evaluates
     * `default_expr` exactly once, before `main` runs (the same timing
     * a global val's own initializer gets, and for the same reason:
     * Ziman values are immutable, so evaluating once up front and
     * evaluating fresh on every omitted call are behaviorally
     * identical, and once is strictly cheaper), storing the result in
     * `g_global_env` under this key; canonicalize_named_args, when it
     * fills in an omitted argument that has a default, builds a
     * synthetic EXPR_IDENT referencing this same key -- reusing
     * ordinary bare-name lookup instead of inventing a way to embed an
     * already-evaluated Value straight into the AST. */
    char *default_key;
    int line, col;
} Param;

typedef struct {
    Param *items;
    int count;
    int capacity;
} ParamList;

/* The two hardcoded bounds a type parameter can carry: "supports '=='"
 * and "supports 'hash'." Not a general trait/interface system (see
 * docs/DESIGN.md's "Generics" entry) -- added specifically so a generic
 * `Map<K, V>`/`Set<T>` can compare and hash an abstract key type inside a
 * generic function body; a generic declaration still can't do anything
 * else with an unbounded type parameter beyond storing and passing it
 * around. A bitmask (`<K: Eq + Hash>` combines both) rather than a single
 * value, since `Map`/`Set` need both bounds on the same parameter at once. */
typedef enum {
    BOUND_NONE = 0,
    BOUND_EQ = 1 << 0,
    BOUND_HASH = 1 << 1,
    BOUND_ORD = 1 << 2,
} TypeParamBound;

typedef struct {
    char *name;
    int bound; /* a bitmask of TypeParamBound values, e.g. BOUND_EQ | BOUND_HASH */
} TypeParam;

/* A generic declaration's own formal type parameters (`struct Box<T>`'s
 * is `[{"T", BOUND_NONE}]`, `Map<K: Eq + Hash, V>`'s is
 * `[{"K", BOUND_EQ | BOUND_HASH}, {"V", BOUND_NONE}]`). Empty for an
 * ordinary, non-generic declaration. Looked up by name, not index --
 * consistent with how everything else in Ziman (struct fields, enum
 * variants) is looked up by name rather than position. */
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
    TypeParamList type_params; /* empty for an ordinary, non-generic function */
    int is_public;
    int is_native;
    int line, col;
    /* Non-NULL for a function a #[derive] generated, e.g. "derive(Json) for
     * 'Point'": every position inside it is stamped with the declaration's
     * own line/column, and errors in it say where it came from. */
    const char *derived_from;
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
    /* `#[derive(Json, ...)]` written directly above the declaration --
     * each entry is one derive target name (e.g. "Json"). Empty for a
     * struct with no attribute at all. See src/derive.c. */
    char **derives;
    int derive_count;
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
    /* Same `#[derive(...)]` mechanism as StructDecl -- see src/derive.c. */
    char **derives;
    int derive_count;
    int line, col;
} EnumDecl;

typedef struct {
    EnumDecl **items;
    int count;
    int capacity;
} EnumList;

/* Two forms. `use path/to/mod [as alias]` imports a whole module under a
 * local qualifier -- `path` is the slash-joined module path exactly as
 * written (e.g. "models/user"), `qualifier` is the local name this
 * module is referenced by (the path's last segment, or the `as` alias),
 * and every reference into it stays dotted (`qualifier.Name`).
 * `use path/to/mod.Name [as alias]` instead imports one specific
 * function/struct/enum (`imported_name`) and binds it to a bare local
 * name (`local_name` -- `imported_name` itself, or the alias) with no
 * dot required at the call site. `qualifier` is NULL for a per-name
 * import; `imported_name`/`local_name` are NULL for a whole-module one.
 * Not resolved to anything yet at parse time -- that's the loader's job,
 * still ahead. */
typedef struct {
    char *path;
    char *qualifier;
    char *imported_name;
    char *local_name;
    int line, col;
} UseDecl;

typedef struct {
    UseDecl *items;
    int count;
    int capacity;
} UseList;

/* `[pub] val NAME[: Type] = <expr>` at module top level -- a module-wide
 * constant, evaluated once before `main` runs (see interpreter.c's
 * init_global_vals), not per-call the way a same-shaped local `val`
 * inside a function body is. Otherwise the exact same shape as a local
 * `val` (reused via a synthetic STMT_VAL in both typecheck.c and
 * interpreter.c -- see check_global_vals/init_global_vals), so it gets
 * the same declared-vs-inferred type checking for free. Renamed to its
 * fully-qualified form by the loader exactly like a function/struct/enum
 * (see loader.c's self_qualify_module), and every bare reference -- a
 * same-module one or one reached through a per-name import (`use
 * mod.NAME`) -- is rewritten to that same qualified form too, so every
 * global `val`, regardless of which module declared it, lives in the one
 * flat global Env every function's own scope chains to (see
 * check_function/call_function). That rewrite has no scope-tracking:
 * loader.c's check_no_val_shadowing forbids reusing a global val's name
 * (this module's own, or an imported one's local name) as any local
 * val/var/parameter/loop-binder anywhere in the module, so it's always
 * unambiguous -- unlike an ordinary local `val`, a global one can't be
 * shadowed. There is deliberately no `mod.NAME` *dotted* bare access
 * (unlike a qualified function call/struct literal) -- see
 * docs/DESIGN.md's "Global val" entry for why. */
typedef struct {
    char *name;
    Type type; /* only meaningful when has_declared_type */
    int has_declared_type;
    Expr *init;
    int is_public;
    int line, col;
} GlobalValDecl;

typedef struct {
    GlobalValDecl **items;
    int count;
    int capacity;
} GlobalValList;

typedef struct {
    FunctionList functions;
    StructList structs;
    EnumList enums;
    UseList uses;
    GlobalValList vals;
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
void global_val_list_push(GlobalValList *list, GlobalValDecl *v);

#endif
