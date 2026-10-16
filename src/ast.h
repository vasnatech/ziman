#ifndef ZIMAN_AST_H
#define ZIMAN_AST_H

#include <stdint.h>

#include "bignum.h"
#include "decimal.h"

typedef enum {
    TY_VOID,
    TY_UNIT, /* `unit`: the type with exactly one value, also written `unit`. What a function that has nothing to give back returns when it must be a value (`Result<unit, E>`, `fn f() -> unit`, a generic `T` = unit). TY_VOID stays "no type": a function declared with no return type. */
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
    TY_FUNC,
    TY_MAP, /* Map<K, V>: `elem` is K, `elem_err` is V. A built-in persistent hash map (see docs/DESIGN.md "Map and Set"). */
    TY_SET, /* Set<T>: `elem` is T. */
} TypeKind;

/* How many kinds there are, for a per-kind table (see type_scalar_ref). A macro rather than a
 * sentinel in the enum itself: this codebase leans on exhaustive switches with no default arm,
 * and a sentinel member would make every one of them warn. */
#define TY_KIND_COUNT ((int)TY_SET + 1)

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
 * `fn(A, B) -> R` (TY_FUNC) is a function type: `type_args` holds the
 * parameter types and `elem` the return type (TY_VOID for none). A value of
 * this type is a reference to a named top-level function -- no closures, no
 * capture (see docs/DESIGN.md's function-values entry). Function types are
 * structural: two are the same when their parameters and return type are.
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
    /* TY_FUNC only: this is a lambda that captured something holding a `#[thread_bound]` value (a
     * builder or an iterator). A function type otherwise says nothing about what a closure captured,
     * which is why sending one used to be allowed and failed at run time on the receiving thread
     * instead -- the capture set is known where the lambda is checked, so typecheck.c records it here
     * and `spawn`/`send` can refuse it like any other thread-bound value. Lives in the four bytes of
     * padding that followed `kind`, so sizeof(Type) is unchanged at 64 bytes: this struct is copied
     * on every arithmetic result in a running program (see type_scalar below) and must not grow.
     * Every constructor uses `Type t = {0}`, so it is 0 unless deliberately set. */
    int func_captures_thread_bound;
    struct Type *elem;     /* TY_ARRAY: element type. TY_RESULT: success (Ok) type. TY_CHANNEL: element type. TY_FUNC: return type (TY_VOID when it returns nothing). */
    struct Type *elem_err; /* only meaningful when kind == TY_RESULT: the Err type */
    char *struct_name;     /* only meaningful when kind == TY_STRUCT */
    char *enum_name;       /* only meaningful when kind == TY_ENUM */
    char *type_var_name;   /* only meaningful when kind == TY_TYPEVAR */
    TypeList type_args;    /* TY_STRUCT/TY_ENUM only, when the declaration is generic. TY_FUNC: the parameter types, in order. */
};

/* For anything but TY_ARRAY/TY_STRUCT/TY_RESULT/TY_ENUM/TY_CHANNEL/TY_TYPEVAR.
 *
 * `static inline` in the header rather than a function in ast.c, because this is on the hottest
 * path there is: every `value_int`/`value_bool`/`value_float` calls it, which means every integer
 * literal and every arithmetic result in a running program. This build has no LTO, so out of line
 * it was a real cross-translation-unit call returning a 64-byte struct through a hidden pointer --
 * `perf` put `value_int` alone at 30% of a recursive-fib profile for exactly that reason (see
 * docs/DESIGN.md). Inline, the zeroing collapses to filling in one `kind` field in the caller's
 * own stack slot. */
static inline Type type_scalar(TypeKind kind) {
    Type t = {0};
    t.kind = kind;
    return t;
}

/* `Value.type` is a pointer (src/value.h), so a value's type must outlive it. A scalar kind
 * carries nothing per instance, so one static per kind serves every value of that kind -- and the
 * hot path stops zeroing 64 bytes per arithmetic result, becoming a constant address instead.
 * Compound types allocate per call: an intern table was measured and made no difference
 * (docs/MEMORY.md, "`Value.type` by pointer is the win; interning it is not"). */
const Type *type_scalar_ref(TypeKind kind);

/* The `Option` enum type as one shared constant, for a native returning `Option` on a hot path --
 * see ast.c for why building it per call cost two of the three allocations such a native makes,
 * and why one shared value is sound. */
const Type *type_option_ref(void);
const Type *type_ref(Type t);
Type type_array(Type elem);
Type type_struct(const char *name);
Type type_result(Type ok_type, Type err_type);
Type type_enum(const char *name);
Type type_channel(Type elem);
Type type_map(Type key, Type value);
Type type_set(Type elem);
Type type_typevar(const char *name);
/* `fn(A, B) -> R`: the parameter types and the return type (TY_VOID for none). */
Type type_func(TypeList params, Type ret);
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
    EXPR_UNIT,
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
    EXPR_LAMBDA,
    EXPR_VALUE_BLOCK,
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
    /* For EXPR_IDENT (an ordinary variable, not a function reference -- see fn_ref below) and
     * for `call.local_name` (calling through a local of function type): how many `Env->parent`
     * hops from this expression's own runtime `env`, and which index into that scope's
     * `items`, the type checker's `env_lookup_resolved` found this exact name at. The
     * interpreter uses `env_at_slot` to jump straight there instead of re-searching by name
     * every time the expression runs -- see docs/DESIGN.md's interpreter-speed entry.
     * -1 (the default; see expr_new) means "not resolved, use env_lookup by name instead" --
     * the case for any reference that crossed a lambda boundary to be found (a capture, or
     * anything beyond it, including a global): a lambda's runtime call frame is rooted
     * directly at the global env (captures are copied by value into its own leading
     * parameters, not found by walking up to the scope the lambda was defined in), so a depth
     * counted through the *compile-time* nested scope chain would count the wrong number of
     * hops at runtime. `env_lookup_resolved`'s own doc comment has the full reasoning. */
    int resolved_depth;
    int resolved_slot;
    /* EXPR_INT only: the literal 9223372036854775808, which is one more than INT64_MAX and only legal directly under a unary minus (that is `-9223372036854775808`, the
     * smallest int). parse_unary clears the flag when it folds the minus in; a literal that still has it reaches the type checker as an out-of-range error. */
    unsigned char int_min_literal;
    /* A bare identifier (EXPR_IDENT) or `module.name` (EXPR_FIELD_ACCESS) that
     * names a top-level function, used as a value. The loader records the
     * fully-qualified candidate in `fn_ref_name` without deciding (it cannot see
     * local variables, and a local always wins); the type checker resolves it
     * to `fn_ref` (a FunctionDecl *) only when no local of that name exists. */
    char *fn_ref_name;
    void *fn_ref;
    /* `identity<int>` / `mod.identity<int>` used as a value: the explicit type
     * arguments of the generic function being referred to (empty otherwise). */
    TypeList fn_ref_type_args;
    /* EXPR_CALL only, and only for the "ordinary named function" case (not a builtin, not
     * `call.through_value`, not a generic native whose own type-argument dispatch needs
     * find_function's linear scan for other reasons -- see call_function's caller in
     * interpreter.c): the FunctionDecl* check_call's own find_function(module, name) already
     * found during type-checking, cached here so the interpreter doesn't re-run that same
     * linear scan over every loaded function (including all of prelude/corelib) on every single
     * call. Always non-NULL by the time interpretation reaches this branch, since type-checking
     * already required it to resolve for the program to compile at all -- unlike
     * resolved_depth/resolved_slot, this has no "not resolved" fallback state to check for.
     * Distinct from fn_ref (a function referenced *as a value*, never a call) even though both
     * hold a FunctionDecl* -- a node is exactly one ExprKind, so there is no ambiguity in
     * practice, but they answer different questions and are kept as different fields for that
     * reason. See docs/DESIGN.md's interpreter-speed entry. */
    void *call_fn;
    /* EXPR_LAMBDA: `fn(x: int) -> int { ... }`. `lambda_decl` (a FunctionDecl *)
     * holds the parameters, return type and body as written. The type checker
     * finds which enclosing `val`s the body uses (captures, by value) and puts
     * them in front of the declared parameters; the first `lambda_ncaptured`
     * parameters of the decl are those captures, filled in when the closure is
     * created. See docs/DESIGN.md "Closures". */
    void *lambda_decl;
    int lambda_ncaptured;
    int lambda_checked;

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
         * itself an array (`MutableArray<[T]>`'s `mutable_array_push<[T]>`, for
         * instance) -- a single element of type `[T]` also happens to
         * satisfy "is an array", the exact case that exposed this. */
        /* `local_name` is the callee exactly as written when it had no dot -- the
         * name of a local variable of function type that a call may go through
         * (a local wins over a same-named top-level function). `through_value`
         * is set by the type checker when the call resolved that way. */
        /* `callee_expr` is set only for a call through a *field* holding a function value
         * (`handler.run(21)`): the loader cannot tell `handler` from a module name, so it leaves the
         * dotted callee alone with `unresolved_qualifier` set, and the type checker either resolves it
         * to this field access or reports the unknown module. A call's target is otherwise always a
         * name, which is why this one case needs an expression. */
        struct { char *callee; ExprList args; TypeList type_args; char **arg_names; int variadic_tail_is_array_spread; char *local_name; int through_value; Expr *callee_expr; int unresolved_qualifier; } call;
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
        /* `try inner` -- Option<T>/Result<T, E> unwrap-or-propagate (see docs/DESIGN.md's "try" entries). `inner`'s own type decides which of the two it is at
         * type-check time; the interpreter re-derives that from the runtime value (an Option's EnumObj vs. a Result's ResultObj). `propagated_type` is
         * check_try_unwrap's one real output beyond `inner`'s own checking: the enclosing function's full, concrete Option<T2>/Result<T2, E> return type, filled in
         * once at type-check time and reused to build the propagated None/Err value at runtime. The interpreter and the checker only accept it as the entire
         * initializer of a val/var or as its own statement; the parser hoists a `try` written anywhere else in an expression into such a `val $tN = try inner`
         * in front of the statement (hoist_stmt in parser.c), and a `try` it could not hoist reaches check_expr's EXPR_TRY_UNWRAP case and is refused. */
        struct { Expr *inner; Type propagated_type; char *convert_variant; } try_unwrap;
        /* `convert_variant` is set by check_try_unwrap when the error of `inner` is not the enclosing function's error type but converts into the
         * `#[from]` variant of that type (an enum) of this name: the propagated `Err` then holds `Enum.Variant(error)`. */
        /* `if`/`switch` used as an operand or argument: `f(if c { 1 } else { 2 })`,
         * `1 + (switch e { ... })`. `inner` is the same STMT_IF/STMT_SWITCH with
         * yielding arms that STMT_VALUE_BLOCK wraps; here no arm may leave
         * (return/break/continue/`?`), so the interpreter can evaluate it in place. */
        struct { struct Stmt *inner; } value_block;
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
    STMT_VALUE_BLOCK,
    STMT_YIELD,
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
    /* `--coverage` only: how many times the interpreter has executed this statement. Counted in
     * exec_stmt, behind a flag so an ordinary run pays nothing but a well-predicted branch, and
     * read back by src/coverage.c, which walks the AST afterwards to turn it into per-file line
     * coverage. Living on the Stmt rather than in a side table is what makes the denominator
     * exact: the same walk that finds a statement's line is the one that reports whether it ran,
     * so "executable line" means precisely "line some statement starts on" with nothing to keep
     * in sync. Zero for every statement in a `calloc`'d Stmt (parser.c), so an un-run program
     * reports honestly rather than emptily. */
    long exec_count;
    /* Same resolution as Expr's own `resolved_depth`/`resolved_slot` (see there for the full
     * reasoning, including why a reference crossing a lambda boundary is left at -1) -- for
     * STMT_ASSIGN's `assign.name` and STMT_VALUE_BLOCK's `value_block.name` when its target is
     * STMT_ASSIGN, the two places a Stmt itself looks an existing name up rather than
     * declaring one. */
    int resolved_depth;
    int resolved_slot;
    /* `"let"`, `"const"`, `"mut"` or `"auto"` when this statement was written with another
     * language's binding keyword, NULL (the ordinary case) otherwise. The parser rewrites
     * `let x = 1` into the `val` it meant and records the word here instead of reporting it,
     * because a parser error skips the rest of the declaration: the type checker reports it
     * once per statement, from check_stmt, and the recovery that follows poisons the name the
     * rewrite bound -- so one wrong word costs one diagnostic instead of an error at every
     * mention of the name (5 to 9 of them in the 2026-10-12 benchmark; tasks.md F95). On the
     * Stmt rather than inside `decl`, since `let x = if c { 1 } else { 2 }` parses to a
     * STMT_VALUE_BLOCK and the mistake cannot be allowed to go unreported in that one shape. */
    const char *foreign_decl_keyword;

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
        /* `type_args` is non-empty for `spawn f<T>(args)` -- spawning a generic
         * function, with its type arguments written out like any generic call. */
        /* `on_error` is the optional `on_error <handler>` clause: a `fn(string)` called, in the thread that failed, with the message of the runtime error that
         * ended it (NULL: the error is written to stderr and only that thread ends). */
        struct { char *callee; ExprList args; TypeList type_args; Expr *on_error; } spawn_stmt;
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
        /* `if`/`switch` used as a value: `val x = if c { 1 } else { 2 }`,
         * `x = switch e { ... }`, `return if ...`. `target` says which of
         * STMT_VAL / STMT_VAR / STMT_ASSIGN / STMT_RETURN this stands in for
         * (`name`/`declared_type` are the val/var's or the assignment's
         * variable). `inner` is an ordinary STMT_IF or STMT_SWITCH whose arms
         * each end in a STMT_YIELD (produces the value) or in a statement that
         * leaves (return/break/continue). Everything else about it -- scoping,
         * control flow, the switch rewrite -- is the existing statement
         * machinery; see docs/DESIGN.md's "if and switch as expressions". */
        struct { StmtKind target; char *name; Type declared_type; int has_declared_type; Stmt *inner; } value_block;
        /* The trailing expression of a value arm. The interpreter evaluates it
         * into a per-thread register the enclosing STMT_VALUE_BLOCK reads. */
        struct { Expr *value; } yield_stmt;
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
    /* A lambda parameter written without a type (`fn(n) { ... }`); the type
     * checker fills `type` in from the function type the lambda is used as. */
    int type_omitted;
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

/* A top-level declaration's visibility, in ascending order of reach. `VIS_PRIVATE` (no
 * modifier, the default) is visible only within its own declaring file, same as always.
 * `VIS_PUBLIC` (`pub`) is visible everywhere, same as always. `VIS_INTERNAL` (`internal`) is
 * new: visible to any module in the same *package* as the declaring one, but not beyond it --
 * see loader.c's `same_package`/`visibility_allows` for what "package" means today (the bundled
 * standard library is one package, a project is another) and docs/DESIGN.md's visibility entry
 * for why that definition is deliberately narrow but meant to grow into a real package/crate
 * system later without this type or its enforcement changing. `internal` is a soft keyword
 * (checked by identifier text at the one declaration-prefix position `pub` already occupies,
 * not a reserved word everywhere -- see parser.c), specifically so a program that already has a
 * function, variable or import named `internal` keeps compiling unchanged. */
typedef enum { VIS_PRIVATE, VIS_INTERNAL, VIS_PUBLIC } Visibility;

typedef struct {
    char *name;
    ParamList params;
    Type return_type;
    Stmt *body; /* STMT_BLOCK; NULL when is_native (a `native fn` has no body --
                 * its implementation is a registered C function, looked up by
                 * module path + name at call time) */
    TypeParamList type_params; /* empty for an ordinary, non-generic function */
    Visibility visibility;
    int is_native;
    /* `is_native` only: the registered C implementation (a `NativeFn`, src/native.h), resolved from
     * `name` on this declaration's first call and kept. Held as a bare `void (*)(void)` rather than
     * a `NativeFn` to keep this header free of the dependency -- native.h needs value.h, which
     * needs this -- and cast back at the one place that uses it (interpreter.c's call_function).
     * A *function* pointer, not `void *`: converting between function pointer types and back is
     * well defined, where round-tripping one through an object pointer is not (and -Wpedantic says
     * so).
     *
     * Memoised because `native_lookup` is a linear scan with a `strcmp` per entry over every
     * registered native, and there are 247 of them: resolving on every call cost an average of 205
     * string comparisons per native call, measured at 1,964,293 of them in a single run of
     * tests/lang/yaml_basic.zm, which `perf` put at a fifth of all CPU time (`__strcmp_avx2`
     * plus `native_lookup`, docs/DESIGN.md). A `FunctionDecl` lives as long as the program and its
     * name never changes, so one resolution per declaration is all that was ever needed. */
    void (*native_impl)(void);
    int line, col;
    /* Non-NULL for a function a #[derive] generated, e.g. "derive(Json) for
     * 'Point'": every position inside it is stamped with the declaration's
     * own line/column, and errors in it say where it came from. */
    const char *derived_from;
    /* The file this function was loaded from, or NULL for a bundled core
     * module and the prelude. What `--lint` uses to report only code the user
     * owns. */
    const char *source_file;
    /* A lambda written without `->`: its return type comes from the function
     * type it is used as, or else from its final expression. */
    int return_omitted;
    /* `#[deprecated]` / `#[deprecated("use X instead")]`: calling or referring to this function
     * from *another* module is a `--lint` warning, carrying `deprecated_message` when one was
     * given. Same-module use is deliberately silent -- the author of the thing already knows.
     * `deprecated_message` is NULL for the bare form. */
    int deprecated;
    const char *deprecated_message;
    /* `#[test]`: `ziman test` runs this function (see docs/SYNTAX.md
     * "Testing"). Must take no parameters (or, with `test_cases` set,
     * exactly one) and return nothing or `Result<unit, E>`. */
    int is_test;
    /* `#[test(cases: NAME)]`'s `NAME`: a `[T]` val or a zero-parameter
     * function producing one, declared in the same file (no cross-module
     * lookup, same restriction a global val's own bare reference has).
     * `ziman test` runs this function once per element, labelled
     * "name[i]". NULL for a plain `#[test]`, run once with no arguments. */
    const char *test_cases;
    /* Non-NULL for a lambda declared as `val NAME = fn(...) -> T { ... }` (never `var` -- self-
     * reference binds to *this* function permanently, which a reassignable `var` would make
     * ambiguous): the name its own body may call itself by, recursively. NULL for every other
     * function -- an ordinary lambda with no self-reference, and every top-level function, which
     * already supports recursion the ordinary way through `find_function`/`call_fn` and needs
     * none of this. See docs/DESIGN.md's recursive-lambdas entry. */
    char *self_name;
    /* Meaningful only when `self_name` is non-NULL: how many of this function's own leading
     * `params` are real captures (computed once, for the same lambda, alongside
     * `lambda_ncaptured` on the EXPR_LAMBDA node -- see check_lambda_expecting). `call_function`
     * (interpreter.c) slices exactly this many entries off the front of its own `args` on every
     * call to rebuild a fresh closure value under `self_name`, so a recursive call threads the
     * same captured context through correctly -- constructed fresh per call, never stored, so
     * there is no self-referential value to bootstrap before it exists. */
    int self_ncaptured;
} FunctionDecl;

/* The `fn(P1, P2) -> R` type of a (non-generic, non-variadic) function used as
 * a value -- see type_func. */
Type function_value_type(const FunctionDecl *fn);

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
    Visibility visibility;
    /* `#[derive(Json, ...)]` written directly above the declaration --
     * each entry is one derive target name (e.g. "Json"). Empty for a
     * struct with no attribute at all. See src/derive.c. */
    char **derives;
    int derive_count;
    /* `#[thread_bound]`: values of this type belong to the thread that made them (a builder is a mutable handle). Sharing one is refused: at compile time when
     * a type containing it is a Channel's element, a `spawn` argument or a module-level `val`, and at run time (the native that owns the handle checks
     * the calling thread) for every other way it could get to another thread. */
    int thread_bound;
    /* `#[thread_key]`: a value of this type is an immutable key naming a slot that has a different value on every thread (`thread.ThreadLocal<T>`), so it holds nothing
     * that belongs to a thread even when its type argument is a `#[thread_bound]` type -- a `ThreadLocal<MutableMap<..>>` may be a module-level `val`. */
    int thread_key;
    /* `#[deprecated]` / `#[deprecated("use X instead")]`: naming this type from *another* module --
     * a struct literal, an enum access -- is a `--lint` warning, carrying `deprecated_message`
     * when one was given. Same-module use is silent, for the reason the function case is: the
     * author already knows. Deliberately *not* warned on a type annotation, because a function
     * migrating away from a deprecated type still has to name it to accept one, so warning there
     * would be loudest in exactly the code doing the migration (tasks.md F58). */
    int deprecated;
    const char *deprecated_message;
    int line, col;
    const char *source_file; /* the file it was loaded from; NULL for a bundled module */
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
    int is_from;       /* `#[from]`: a `try` on a Result whose error is `payload_type` converts it into this variant */
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
    Visibility visibility;
    /* Same `#[derive(...)]` mechanism as StructDecl -- see src/derive.c. */
    char **derives;
    int derive_count;
    /* Same `#[deprecated]` as StructDecl -- an enum access from another module is a `--lint`
     * warning. See StructDecl's own comment for why a type annotation is not one. */
    int deprecated;
    const char *deprecated_message;
    int line, col;
    const char *source_file;
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
    /* Whether an `as` was actually written, and where. Needed because `qualifier` is the alias *or*
     * the path's last segment, so the two are indistinguishable afterwards -- and `use a/b as b` is a
     * line that does nothing, which `--lint` reports (there were 16 of them in the reference backend).
     * `alias_line`/`alias_col` point at the alias rather than at the `use`, so the warning points at
     * the word to delete. Zero when no alias was written. */
    int has_alias;
    int alias_line, alias_col;
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
    Visibility visibility;
    int line, col;
    const char *source_file;
    int deprecated;                  /* `#[deprecated]` or `#[deprecated("...")]` was written on it */
    const char *deprecated_message;  /* the `("...")` text, or NULL for the bare form */
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
    /* Set once in loader.c's load_module, from the same `from_disk` check that already decides
     * whether to stamp this module's own declarations with a real `source_file` or leave it
     * NULL for a bundled one: 1 for a module resolved from the bundled standard library
     * (corelib.c/prelude.c), 0 for one resolved from the project's own filesystem tree. This is
     * today's whole definition of a "package" for `VIS_INTERNAL` visibility -- see
     * `same_package` in loader.c and Visibility's own doc comment above. */
    int is_corelib;
    /* `#![experimental]` at the top of this module's own file (before any declaration), the
     * module-level form of an attribute -- `#[...]` applies to the declaration after it, `#![...]`
     * to the file it is in, the same inner/outer split Rust draws. Carries no compile-time or
     * runtime behaviour on purpose: its job is to be the *one* place a module is called
     * experimental, which the documentation generators read (tools/gen_corelib_reference.sh,
     * tools/gen_lib_manual.py) instead of each hand-repeating the claim. */
    int experimental;
    /* `#![deprecated]` or `#![deprecated("...")]` at the top of this module's own file: the whole
     * module should not be used in new code. Reported at each `use` of it from a file that is not
     * itself a bundled module -- the `use` line being the one place the dependency is written, and
     * so the one line changing it means editing. Warned about in loader.c's resolve_named_imports,
     * not in typecheck.c, because a module's `use` list belongs to the loader and is already gone
     * from the merged Module the type checker is handed (the same reason the redundant-`as` lint
     * lives there). See docs/ROADMAP.md, F70. */
    int deprecated;
    const char *deprecated_message;
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
