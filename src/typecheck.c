#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <ctype.h>
#include "typecheck.h"
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "env.h"
#include "derive.h"
#include "error.h"
#include "fmtstr.h"
#include "stack.h"
#include "token.h"
#include "xalloc.h"

/* A name index over `module->functions`, because the linear scan this replaced was the whole of a
 * measured O(n^2): checking a file of n declarations took 0.01/0.03/0.09/0.34s at 2,000/4,000/
 * 8,000/16,000, roughly quadrupling each time the count doubled (F130).
 *
 * One module is cached at a time, which is all this needs: the checker works through a module's
 * declarations together, so the hit rate is effectively 100% and switching modules is rare.
 *
 * Grown *incrementally* rather than rebuilt. `find_function` is called while the module is still
 * being filled, so rebuilding whenever the count changed would be quadratic again by another
 * route; instead the entries added since last time are inserted and nothing else moves.
 *
 * Insert keeps the FIRST declaration of a name, because that is what the scan returned and a
 * duplicate name is reported elsewhere -- changing which one wins would change which site the
 * duplicate is blamed on. */
typedef struct {
    const char *name;
    FunctionDecl *fn;
} FnIndexSlot;

static Module *g_fn_index_module = NULL;
static int g_fn_index_filled = 0;
static FnIndexSlot *g_fn_index = NULL;
static size_t g_fn_index_cap = 0;

static size_t fn_name_hash(const char *s) {
    size_t h = 1469598103934665603u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211u;
    }
    return h;
}

static void fn_index_put(FunctionDecl *fn) {
    size_t at = fn_name_hash(fn->name) & (g_fn_index_cap - 1);
    while (g_fn_index[at].name != NULL) {
        if (strcmp(g_fn_index[at].name, fn->name) == 0) return; /* first declaration wins */
        at = (at + 1) & (g_fn_index_cap - 1);
    }
    g_fn_index[at].name = fn->name;
    g_fn_index[at].fn = fn;
}

static void fn_index_rebuild(Module *module) {
    size_t want = 64;
    while (want < (size_t)module->functions.count * 2u + 2u) want *= 2;
    if (want > g_fn_index_cap) {
        g_fn_index = ziman_xrealloc(g_fn_index, want * sizeof(FnIndexSlot));
        g_fn_index_cap = want;
    }
    memset(g_fn_index, 0, g_fn_index_cap * sizeof(FnIndexSlot));
    g_fn_index_module = module;
    g_fn_index_filled = 0;
}

static FunctionDecl *find_function(Module *module, const char *name) {
    if (module == NULL) return NULL;
    /* A rebuild when the module changes or has shrunk, and when the table would pass half full --
     * open addressing degrades badly beyond that, and the load factor is what keeps the lookup O(1). */
    if (g_fn_index_module != module || g_fn_index_filled > module->functions.count ||
        (size_t)module->functions.count * 2u + 2u > g_fn_index_cap) {
        fn_index_rebuild(module);
    }
    for (; g_fn_index_filled < module->functions.count; g_fn_index_filled++) {
        fn_index_put(module->functions.items[g_fn_index_filled]);
    }
    size_t at = fn_name_hash(name) & (g_fn_index_cap - 1);
    while (g_fn_index[at].name != NULL) {
        if (strcmp(g_fn_index[at].name, name) == 0) return g_fn_index[at].fn;
        at = (at + 1) & (g_fn_index_cap - 1);
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

/* The root of every function's own scope chain (see check_function) --
 * holds every module-level `val` (see check_global_vals), so a bare
 * reference to one falls out of an ordinary env_lookup exactly the way a
 * reference to an enclosing block's local already does, with no special-
 * casing anywhere else in this file. Built once, before any function
 * body is checked; never mutated afterward. */
static Env *g_global_env = NULL;

/* Whether two declarations were written in the same source: both in one file, or both in bundled (built-in) modules. A type parameter can only be confused with a
 * struct or enum declared alongside it -- the parser reclassifies parameter names per declaration -- so a user's `struct T` does not collide with the prelude's
 * `MutableArray<T>`, which the user never sees. */
static int same_source(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

/* Whether a value of type `t` can hold a `#[thread_bound]` value (a builder): it is one, or an array, map, set, Result, Option or other generic instance with one
 * among its parts, or a struct or enum with one in a field or variant, or a lambda that captured one
 * (Type.func_captures_thread_bound, set where the lambda is checked -- a function's parameter and
 * return types say nothing about this, only what it closed over does). */
/* The declarations already on the current path, as a list built on the stack -- no allocation, and
 * depth is bounded by the number of distinct declarations rather than by a guess. */
typedef struct VisitedDecl {
    const void *decl;
    const struct VisitedDecl *prev;
} VisitedDecl;

static int visited_has(const VisitedDecl *v, const void *decl) {
    for (; v != NULL; v = v->prev) {
        if (v->decl == decl) return 1;
    }
    return 0;
}

static int type_holds_thread_bound(Module *module, Type t, const VisitedDecl *seen) {
    /* This used to bail out at `depth > 12` and answer 0. The cutoff was there to stop a recursive
     * type (`struct Node { kids: [Node] }`) recursing for ever, but it also abandoned perfectly
     * ordinary deep types and answered "holds no builder" for them -- so a MutableString fifteen
     * structs deep passed `spawn` statically, which is the one hole the #[thread_bound] story had.
     * Remembering which declarations are already on this path terminates a cycle exactly, with no
     * depth limit and no wrong answers in between; and because a cycle now terminates properly there
     * is no unbounded recursion left to turn into an error, which is the better outcome than the
     * error this task originally asked for. Revisiting is pruned per path rather than globally, so a
     * struct reached twice through different fields is still examined both times. */
    switch (t.kind) {
        case TY_ARRAY:
        case TY_SET:
        case TY_CHANNEL:
            return type_holds_thread_bound(module, *t.elem, seen);
        case TY_RESULT:
        case TY_MAP:
            return type_holds_thread_bound(module, *t.elem, seen) || type_holds_thread_bound(module, *t.elem_err, seen);
        case TY_STRUCT: {
            StructDecl *sd = find_struct(module, t.struct_name);
            if (sd != NULL && sd->thread_bound) return 1;
            if (sd != NULL && sd->thread_key) return 0; /* a key holds no per-thread state itself, whatever its type argument is */
            for (int i = 0; i < t.type_args.count; i++) {
                if (type_holds_thread_bound(module, t.type_args.items[i], seen)) return 1;
            }
            if (sd != NULL) {
                if (visited_has(seen, sd)) return 0; /* already on this path: a cycle, nothing new below */
                VisitedDecl here = {sd, seen};
                for (int i = 0; i < sd->fields.count; i++) {
                    if (type_holds_thread_bound(module, sd->fields.items[i].type, &here)) return 1;
                }
            }
            return 0;
        }
        case TY_ENUM: {
            for (int i = 0; i < t.type_args.count; i++) {
                if (type_holds_thread_bound(module, t.type_args.items[i], seen)) return 1;
            }
            EnumDecl *ed = find_enum(module, t.enum_name);
            if (ed != NULL) {
                if (visited_has(seen, ed)) return 0; /* as above */
                VisitedDecl here = {ed, seen};
                for (int v = 0; v < ed->variants.count; v++) {
                    if (ed->variants.items[v].has_payload && type_holds_thread_bound(module, ed->variants.items[v].payload_type, &here)) return 1;
                }
            }
            return 0;
        }
        case TY_FUNC:
            /* Not the signature -- a lambda's captures. See Type.func_captures_thread_bound. */
            return t.func_captures_thread_bound;
        default:
            return 0;
    }
}

/* The message for a thread-bound type where it may not go: `what` says where ("sent over a channel"). */
/* True for a name that claims membership of the `Mutable*` family: `Mutable` followed by an
 * uppercase letter, or `Mutable` on its own. Looking at the next character rather than just the
 * prefix is what keeps `MutablesRegistry` out of it. */
static int name_claims_mutable(const char *name) {
    if (strncmp(name, "Mutable", 7) != 0) return 0;
    char next = name[7];
    return next == '\0' || (next >= 'A' && next <= 'Z');
}

static void reject_thread_bound(Module *module, Type t, const char *what, int line, int col) {
    if (type_holds_thread_bound(module, t, NULL)) {
        ziman_error("type", line, col,
                    "%s cannot be %s: it belongs to the thread that made it, as a builder or an iterator does (send the finished value instead -- snapshot, snapshot, snapshot, the items as an array)",
                    type_name(t), what);
    }
}

static Type check_expr(Module *module, Env *env, Expr *e);
static Type check_lambda(Module *module, Env *env, Expr *e);
static Type check_value_block_expr(Module *module, Env *env, Expr *e, const Type *expected);
static Type check_lambda_expecting(Module *module, Env *env, Expr *e, const Type *expected);
static void check_type_well_formed(Module *module, Type t, int line, int col);
static Type check_expr_expecting(Module *module, Env *env, Expr **e_slot, Type expected);
/* The three forms that carry no type of their own and take it from the context. Declared here because
 * check_value_against_param_type needs them well before they are defined -- a call argument's parameter
 * type is a context like any other, and used not to be treated as one (see there). */
static int check_ok_err_call(Module *module, Env *env, Expr *e, Type expected);
static int check_channel_new_call(Module *module, Env *env, Expr *e, Type expected);
static int check_mutable_empty_call(Module *module, Env *env, Expr *e, Type expected);
static int check_bare_option_call(Module *module, Env *env, Expr *e, Type expected);
static int expr_needs_expected_type(const Expr *e);
static int is_option_type(Type t);

/* Rebuilds `t` with every TY_TYPEVAR named in `params` replaced by the
 * type argument at the same index in `args` -- the substitution-on-demand
 * a generic struct's own declared field types need whenever they're
 * looked at through a concrete instantiation (`Box<int>`'s `value` field
 * is `int`, not `T`). No caching/memoization: `Value` is already
 * runtime-type-erased (see ast.h's Type comment), so this is purely a
 * type-checking-time convenience, never a runtime cost. A type unrelated
 * to `params`/`args` (e.g. a plain `int` field, or a reference to some
 * other struct's own type parameter) passes through unchanged. */
static Type substitute_type(Type t, TypeParamList *params, TypeList *args) {
    ZIMAN_STACK_CHECK();
    switch (t.kind) {
        case TY_TYPEVAR:
            for (int i = 0; i < params->count; i++) {
                if (strcmp(params->items[i].name, t.type_var_name) == 0) return args->items[i];
            }
            return t;
        case TY_ARRAY:
            return type_array(substitute_type(*t.elem, params, args));
        case TY_RESULT:
            return type_result(substitute_type(*t.elem, params, args), substitute_type(*t.elem_err, params, args));
        case TY_CHANNEL:
            return type_channel(substitute_type(*t.elem, params, args));
        case TY_SET:
            return type_set(substitute_type(*t.elem, params, args));
        case TY_MAP:
            return type_map(substitute_type(*t.elem, params, args), substitute_type(*t.elem_err, params, args));
        case TY_FUNC: {
            TypeList substituted = {0};
            for (int i = 0; i < t.type_args.count; i++) type_list_push(&substituted, substitute_type(t.type_args.items[i], params, args));
            return type_func(substituted, substitute_type(*t.elem, params, args));
        }
        case TY_STRUCT:
        case TY_ENUM: {
            if (t.type_args.count == 0) return t;
            Type result = t;
            result.type_args = (TypeList){0};
            for (int i = 0; i < t.type_args.count; i++) {
                type_list_push(&result.type_args, substitute_type(t.type_args.items[i], params, args));
            }
            return result;
        }
        default:
            return t;
    }
}

/* Whether '=='/'!=' is legal for a value of type `t`: scalars always,
 * `[T]` when `T` is itself (recursively) comparable -- not scalar-only,
 * since an array of a comparable struct/enum is exactly as comparable as
 * the struct/enum itself -- a struct only when every one of its fields
 * is (recursively) comparable, a `Result<T, E>` only when both `T` and
 * `E` are (recursively) comparable -- a runtime value only ever holds one
 * of the two, but which one isn't known until the value exists, so both
 * branches have to type-check as comparable for the type itself to be --
 * and a user-declared `enum` only when every variant that carries a
 * payload has a (recursively) comparable payload type, same reasoning as
 * Result. A struct/enum that refers back to itself while this is being
 * worked out (`Json`'s own `JArray([Json])`/`JObject(Map<string, Json>)`,
 * or a user's own `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`) is
 * assumed comparable rather than re-entered -- see type_check_cycle_seen,
 * which every TY_STRUCT/TY_ENUM case here (and in type_supports_hashing)
 * consults first. This is sound: a real *value* of a recursive type is
 * always finite no matter how its type refers to itself, so the only
 * thing that would otherwise recurse forever is checking the *type's
 * own shape*, and once every payload not of the same type has already
 * checked out comparable, the recursive occurrence contributes nothing
 * further to verify. `depth` remains only as a backstop against a
 * non-cyclic but very deep chain -- see docs/ROADMAP.md's still-open
 * "self-referential struct can't be constructed" gap. */
enum { MAX_TYPE_CHECK_DEPTH = 64, MAX_TYPE_CHECK_STACK = 128 };

/* The struct/enum types currently being asked whether they support `==`/`hash`, for type_supports_equality and
 * type_supports_hashing's own cycle detection (see their shared comment above) -- two separate stacks since a
 * type may need one property checked while already mid-check for the other (`Hash` bounds are checked alongside
 * `Eq` ones in check_type_args_satisfy_bounds). Single-threaded, like current_decl_type_params. */
static Type g_eq_check_stack[MAX_TYPE_CHECK_STACK];
static int g_eq_check_depth = 0;
static Type g_hash_check_stack[MAX_TYPE_CHECK_STACK];
static int g_hash_check_depth = 0;

static int type_check_cycle_seen(Type *stack, int depth, Type t) {
    for (int i = 0; i < depth; i++) {
        if (type_equals(stack[i], t)) return 1;
    }
    return 0;
}
/* The type parameters of whichever generic declaration is currently being
 * validated or type-checked -- a struct's own fields, an enum's own
 * variant payloads, a function's own params/return type, or (separately)
 * a function's own body. Needed in two different situations: (1) a
 * bounded type parameter used *as a type argument to another bounded
 * generic type* inside the enclosing declaration's own signature (e.g.
 * `fn map_get<K: Eq, V>(m: Map<K, V>, ...)` -- checking `Map<K, V>`'s own
 * `K: Eq` bound against the *caller* `K`, which is itself just a bare,
 * unsubstituted type variable at this point, requires knowing it's
 * `map_get`'s own bound `K`, not some unrelated, unbounded one); and
 * (2) `==` on a bare `T` inside a generic function's own body (see
 * check_function's comment). Set/cleared symmetrically around each
 * declaration's own validation in typecheck() and around check_function's
 * body-checking -- a plain variable rather than a parameter threaded
 * through every mutually-recursive check_* function, sound because the
 * type checker validates exactly one declaration at a time, single-
 * threaded, with no nested declarations to re-enter this during. NULL
 * anywhere else, where a bare type variable is never comparable. */
static TypeParamList *current_decl_type_params = NULL;

static int type_supports_equality(Module *module, Type t, int depth) {
    if (depth > MAX_TYPE_CHECK_DEPTH) return 0;
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE: case TY_UNIT:
            return 1;
        case TY_ARRAY:
            return type_supports_equality(module, *t.elem, depth + 1);
        case TY_STRUCT: {
            if (type_check_cycle_seen(g_eq_check_stack, g_eq_check_depth, t)) return 1;
            StructDecl *decl = find_struct(module, t.struct_name);
            if (decl == NULL) return 0;
            /* A `#[thread_bound]` struct is a handle, so the TY_CHANNEL reasoning below applies to
             * it too: its one field is an opaque index into a native table, and comparing two of
             * them compares pointer values that mean nothing across runs. Until this was added
             * that was the *accidental* behaviour -- two empty `MutableArray`s compared unequal,
             * `hash` of one was stable only within a run, and such a struct was accepted as a
             * `Map` key -- identity equality wearing value equality's clothes, which is exactly
             * where a memoisation key or a node comparison goes wrong. Gated on the attribute
             * rather than on the four type names, so it covers a handle written in Ziman as well.
             * A struct *containing* one falls out of the field loop below. */
            if (decl->thread_bound) return 0;
            /* Bounded here as well as by `depth > MAX_TYPE_CHECK_DEPTH` at the top. The depth cap
             * does keep this index under the array today -- 64 against 128 -- but that is a bound
             * held one function away from the write, and the four pushes in this file all relied
             * on it. Answering "not comparable" at the limit is the same answer the depth cap
             * already gives (F126). */
            if (g_eq_check_depth >= MAX_TYPE_CHECK_STACK) return 0;
            g_eq_check_stack[g_eq_check_depth++] = t;
            int ok = 1;
            for (int i = 0; i < decl->fields.count; i++) {
                Type field_type = substitute_type(decl->fields.items[i].type, &decl->type_params, &t.type_args);
                if (!type_supports_equality(module, field_type, depth + 1)) { ok = 0; break; }
            }
            g_eq_check_depth--;
            return ok;
        }
        case TY_RESULT:
            return type_supports_equality(module, *t.elem, depth + 1) &&
                   type_supports_equality(module, *t.elem_err, depth + 1);
        case TY_ENUM: {
            if (type_check_cycle_seen(g_eq_check_stack, g_eq_check_depth, t)) return 1;
            EnumDecl *decl = find_enum(module, t.enum_name);
            if (decl == NULL) return 0;
            if (g_eq_check_depth >= MAX_TYPE_CHECK_STACK) return 0;
            g_eq_check_stack[g_eq_check_depth++] = t;
            int ok = 1;
            for (int i = 0; i < decl->variants.count; i++) {
                EnumVariant *v = &decl->variants.items[i];
                if (!v->has_payload) continue;
                Type payload_type = substitute_type(v->payload_type, &decl->type_params, &t.type_args);
                if (!type_supports_equality(module, payload_type, depth + 1)) { ok = 0; break; }
            }
            g_eq_check_depth--;
            return ok;
        }
        case TY_CHANNEL:
            /* A channel is a stateful, identity-based handle, not a data
             * aggregate -- "are two channels equal" doesn't have the
             * obvious structural meaning it does for an array/struct/enum,
             * so it's just not comparable at all in v0. */
            return 0;
        case TY_MAP:
            /* Order-independent: same entries, whatever order they were added in. */
            return type_supports_equality(module, *t.elem_err, depth + 1);
        case TY_SET:
            return 1; /* the element type already satisfies Hash (checked where the type is written) */
        case TY_TYPEVAR:
            /* A bare, unsubstituted type variable is comparable exactly
             * when it's the *current declaration's own* type parameter
             * and that parameter's bound bitmask includes `Eq` -- e.g.
             * `a == b` inside `fn are_equal<T: Eq>(a: T, b: T) -> bool
             * { ... }`. Anywhere else (a struct/enum's own declaration,
             * or a type variable belonging to some other, unrelated
             * declaration) this is unbounded and correctly not
             * comparable. */
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_EQ) != 0;
                    }
                }
            }
            return 0;
        case TY_VOID:
        default:
            return 0;
    }
}

/* Whether `t` supports the `hash` builtin, and so the `Hash` bound: exactly the
 * types that support `==` (see type_supports_equality) whose leaves are
 * hashable -- the scalars, arrays of scalars, structs and enums whose
 * fields/payloads are hashable, Option and Result (an enum and a builtin)
 * of hashable types, and a type parameter bounded by `Hash`. The hash is
 * structural, the same shape as `==`, so `a == b` implies `hash(a) ==
 * hash(b)` with no user function needed (unlike an order, which is not
 * structural and needs `<type>_compare`). Channels, and anything `==` rejects,
 * are not hashable. Needed for `HashMap<K, V>`'s bucket index -- see the
 * `collections` module. */
static int type_supports_hashing(Module *module, Type t, int depth) {
    if (depth > MAX_TYPE_CHECK_DEPTH) return 0;
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE: case TY_UNIT:
            return 1;
        case TY_ARRAY:
            return type_supports_hashing(module, *t.elem, depth + 1);
        case TY_STRUCT: {
            if (type_check_cycle_seen(g_hash_check_stack, g_hash_check_depth, t)) return 1;
            StructDecl *decl = find_struct(module, t.struct_name);
            if (decl == NULL) return 0;
            /* A `#[thread_bound]` struct is a handle, so the TY_CHANNEL reasoning below applies to
             * it too: its one field is an opaque index into a native table, and comparing two of
             * them compares pointer values that mean nothing across runs. Until this was added
             * that was the *accidental* behaviour -- two empty `MutableArray`s compared unequal,
             * `hash` of one was stable only within a run, and such a struct was accepted as a
             * `Map` key -- identity equality wearing value equality's clothes, which is exactly
             * where a memoisation key or a node comparison goes wrong. Gated on the attribute
             * rather than on the four type names, so it covers a handle written in Ziman as well.
             * A struct *containing* one falls out of the field loop below. */
            if (decl->thread_bound) return 0;
            if (g_hash_check_depth >= MAX_TYPE_CHECK_STACK) return 0;
            g_hash_check_stack[g_hash_check_depth++] = t;
            int ok = 1;
            for (int i = 0; i < decl->fields.count; i++) {
                Type field_type = substitute_type(decl->fields.items[i].type, &decl->type_params, &t.type_args);
                if (!type_supports_hashing(module, field_type, depth + 1)) { ok = 0; break; }
            }
            g_hash_check_depth--;
            return ok;
        }
        case TY_RESULT:
            return type_supports_hashing(module, *t.elem, depth + 1) && type_supports_hashing(module, *t.elem_err, depth + 1);
        case TY_MAP:
            return type_supports_hashing(module, *t.elem_err, depth + 1);
        case TY_SET:
            return 1;
        case TY_ENUM: {
            if (type_check_cycle_seen(g_hash_check_stack, g_hash_check_depth, t)) return 1;
            EnumDecl *decl = find_enum(module, t.enum_name);
            if (decl == NULL) return 0;
            if (g_hash_check_depth >= MAX_TYPE_CHECK_STACK) return 0;
            g_hash_check_stack[g_hash_check_depth++] = t;
            int ok = 1;
            for (int i = 0; i < decl->variants.count; i++) {
                EnumVariant *v = &decl->variants.items[i];
                if (!v->has_payload) continue;
                Type payload_type = substitute_type(v->payload_type, &decl->type_params, &t.type_args);
                if (!type_supports_hashing(module, payload_type, depth + 1)) { ok = 0; break; }
            }
            g_hash_check_depth--;
            return ok;
        }
        case TY_TYPEVAR:
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_HASH) != 0;
                    }
                }
            }
            return 0;
        default:
            return 0;
    }
}

/* The `<snake_type>_compare` function that gives a struct or enum its order
 * (written by hand, or generated by #[derive(Ord)]), or NULL. A type declared
 * in module "path::Name" looks for "path::name_compare" -- the same flat,
 * qualified naming every other module-level function has. */
/* ---- lint: a `var` that is never reassigned ---------------------------------
 * `var` and `val` differ by one letter, and a `var` that never changes is
 * mutability nobody asked for (DESIGN.md flags it as a typo hazard). While
 * lint is on, each `var` in user code gets a slot here; STMT_ASSIGN marks it;
 * check_function reports the unmarked ones when the function ends. */
typedef struct {
    const char *name;
    int line, col;
    int assigned;  /* a `var` that is never reassigned should have been a `val` */
    int used;      /* a declaration nothing ever reads is dead weight -- see lint_finish_function */
    int mutable_decl;
} LintVar;

static int g_lint_on = 0;
static int g_lint_count = 0;
static LintVar *g_lint_vars = NULL;
static int g_lint_var_count = 0, g_lint_var_cap = 0;
static int g_lint_active = 0;             /* a style lint should run here: --lint is on and this is user code */
/* The same "this is user code, not a derive expansion" test without the `--lint` half, because
 * `#[deprecated]` reports on every compile while the style lints (a `var` never reassigned, a
 * redundant `as` alias) stay behind the flag. Separating the two is the whole of F68: these used to
 * share `g_lint_active`, so a deprecation only reached anyone who ran a second command. */
static int g_user_code = 0;

/* Module-level vals carrying `#[deprecated]`. A side table rather than two more fields on
 * `Binding`, which is allocated once per binding per call and was measured as the interpreter's
 * hottest allocation (docs/DESIGN.md's "Binding names are borrowed") -- a rare attribute has no
 * business widening it.
 *
 * Keyed by name alone, which is sound here for two reasons the language guarantees rather than
 * hopes: the loader merges every module into one and self-qualifies these names, so a name is
 * unique program-wide; and a local can never shadow a global val (ast.h's GlobalValDecl comment),
 * so an identifier matching one of these names is that val and nothing else. */
typedef struct {
    const char *name;
    const char *message;
    const char *source_file;
} DeprecatedVal;
static DeprecatedVal *g_deprecated_vals = NULL;
static int g_deprecated_val_count = 0, g_deprecated_val_cap = 0;
static const char *g_lint_file = NULL;

void typecheck_enable_lint(void) { g_lint_on = 1; }

/* Whether two declarations come from the same place, for the purpose of "is this use
 * cross-module". A bundled core module and the prelude carry a NULL `source_file` (ast.h), so a
 * user file calling into one compares non-NULL against NULL and is correctly treated as crossing. */
static int lint_same_source(const char *a, const char *b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0;
}

/* `#[deprecated]`: warns when user code uses a deprecated function declared somewhere else.
 * Same-source use is silent on purpose -- whoever wrote the deprecated thing is the one person who
 * does not need telling, and a library keeping a superseded function working for its existing
 * callers (lib/http's own `open_session_with_timeout`) would otherwise warn on every internal call
 * it makes to itself. Reported on **every compile**, not only under `--lint`: a
 * deprecation that needs a second command to see is one almost nobody sees, and the point of the
 * attribute is to move callers. Warnings never change the exit code, so this cannot break a build
 * that was passing; `--lint` still counts them as findings and still exits 1, because there asking
 * for findings is the whole request. `g_user_code`, not `g_lint_active`, is the gate -- the style
 * lints stay behind the flag (docs/ROADMAP.md's `#[deprecated]` entry, F68). */
static void lint_deprecated_use(const FunctionDecl *fn, int line, int col) {
    if (!g_user_code || fn == NULL || !fn->deprecated) return;
    if (lint_same_source(fn->source_file, g_lint_file)) return;
    if (fn->deprecated_message != NULL) {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated: %s", fn->name, fn->deprecated_message);
    } else {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated", fn->name);
    }
    g_lint_count++;
}
/* The same rule as lint_deprecated_use, for a type: naming a `#[deprecated]` struct or enum from
 * another module is a warning, same-module use is silent. Only the sites a programmer actually
 * writes are linted -- a struct literal and an enum access -- and deliberately not a type
 * annotation: a function migrating away from a deprecated type still has to name it to accept one,
 * so warning there would be loudest in exactly the code doing the migration (tasks.md F58). */
static void lint_deprecated_struct(const StructDecl *sd, int line, int col) {
    if (!g_user_code || sd == NULL || !sd->deprecated) return;
    if (lint_same_source(sd->source_file, g_lint_file)) return;
    if (sd->deprecated_message != NULL) {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated: %s", sd->name, sd->deprecated_message);
    } else {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated", sd->name);
    }
    g_lint_count++;
}

/* The same rule again for a module-level `val`: reading one declared elsewhere warns, reading one
 * from its own module is silent. The read is caught at `EXPR_IDENT`, which its own comment calls
 * "the one place a name is read" -- so this covers a bare reference wherever a function body makes
 * it, which is where essentially all of them are.
 *
 * Not covered, and worth stating rather than discovering: a read inside *another* module's global
 * val initializer. `g_user_code` is set per function in check_function, and global vals are checked
 * before any function body, so during that pass this gate is off. Fixing it means tracking the
 * declaring file across check_global_vals, which is more machinery than the case is worth until
 * something actually chains deprecated vals between modules. */
static void lint_deprecated_val(const char *name, int line, int col) {
    if (!g_user_code || name == NULL) return;
    for (int i = 0; i < g_deprecated_val_count; i++) {
        if (strcmp(g_deprecated_vals[i].name, name) != 0) continue;
        if (lint_same_source(g_deprecated_vals[i].source_file, g_lint_file)) return;
        if (g_deprecated_vals[i].message != NULL) {
            ziman_warning(g_lint_file, line, col, "'%s' is deprecated: %s", name, g_deprecated_vals[i].message);
        } else {
            ziman_warning(g_lint_file, line, col, "'%s' is deprecated", name);
        }
        g_lint_count++;
        return;
    }
}

static void lint_deprecated_enum(const EnumDecl *ed, int line, int col) {
    if (!g_user_code || ed == NULL || !ed->deprecated) return;
    if (lint_same_source(ed->source_file, g_lint_file)) return;
    if (ed->deprecated_message != NULL) {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated: %s", ed->name, ed->deprecated_message);
    } else {
        ziman_warning(g_lint_file, line, col, "'%s' is deprecated", ed->name);
    }
    g_lint_count++;
}

static int g_skip_main_check = 0;
void typecheck_skip_main_check(int skip) { g_skip_main_check = skip; }
int typecheck_lint_count(void) { return g_lint_count; }

/* For a lint rule that lives outside this file. The loader has one -- a redundant `as` alias -- because
 * a module's `use` list belongs to the loader's per-module AST and is already gone from the merged
 * Module this file is handed. The count stays here so `--lint`'s exit status counts every finding. */
int typecheck_lint_enabled(void) { return g_lint_on; }

void typecheck_lint_note(void) { g_lint_count++; }

static int lint_register_var(const char *name, int line, int col, int is_mutable) {
    if (g_lint_var_count == g_lint_var_cap) {
        g_lint_var_cap = g_lint_var_cap == 0 ? 16 : g_lint_var_cap * 2;
        g_lint_vars = ziman_xrealloc(g_lint_vars, (size_t)g_lint_var_cap * sizeof(LintVar));
    }
    g_lint_vars[g_lint_var_count] = (LintVar){name, line, col, 0, 0, is_mutable};
    return ++g_lint_var_count; /* 1-based */
}

static void lint_finish_function(int first_var) {
    for (int i = first_var; i < g_lint_var_count; i++) {
        LintVar *v = &g_lint_vars[i];
        /* Unread comes first and alone: telling someone a binding should have been a `val` when the
         * answer is to delete it would be advice about the wrong line. */
        if (!v->used) {
            ziman_warning(g_lint_file, v->line, v->col,
                          "'%s %s' is never read; remove it (or name it '_' to discard on purpose)",
                          v->mutable_decl ? "var" : "val", v->name);
            g_lint_count++;
        } else if (v->mutable_decl && !v->assigned) {
            ziman_warning(g_lint_file, v->line, v->col,
                          "'var %s' is never reassigned; declare it with 'val'", v->name);
            g_lint_count++;
        }
    }
    g_lint_var_count = first_var;
}

/* `x = concat(x, ..)` or `x = set(x, i, ..)` inside a loop copies all of x on every pass:
 * quadratic. `concat` is one name over strings and arrays, so which builder to suggest is read
 * from the type.
 *
 * `set` is here on measurement, not on the look of it. Filling an array of n elements one `set` at a time
 * took 82ms at n=8,000, 225ms at 16,000 and 904ms at 32,000 -- time quadrupling as n doubles. The same
 * loop written with `push` took 3, 4 and 7ms, because appending to an array nothing else holds extends it
 * in place (see ArrayStore's `used` counter in value.c), so `x = push(x, ..)` is deliberately *not*
 * reported: it is the shape that looks worst and costs least. */
static void lint_quadratic_accumulation(const Stmt *s) {
    const Expr *v = s->as.assign.value;
    const char *name = s->as.assign.name;
    const char *fix = NULL;
    if (v->kind == EXPR_CALL && v->as.call.args.count > 0) {
        const char *callee = v->as.call.callee;
        const char *dot = strrchr(callee, '.');
        if (dot != NULL) callee = dot + 1;
        const Expr *first = v->as.call.args.items[0];
        if (first->kind != EXPR_IDENT || strcmp(first->as.ident_name, name) != 0) return;
        if (strcmp(callee, "concat") == 0) {
            /* One `concat` covers strings and arrays since 2026-10-18, so the advice has to come
             * from the *type* rather than the name. Keyed on the name, the two branches here were
             * the same test, the second one dead, and `a = concat(a, [i])` on an array was told to
             * collect into a MutableString. */
            fix = v->resolved_type.kind == TY_STRING
                      ? "a MutableString (mutable_string_new, append, snapshot)"
                      : "a MutableArray (mutable_array_new, push, snapshot), which concat appends to in place";
        } else if (strcmp(callee, "set") == 0 && v->as.call.args.count == 3) {
            /* No builder has random-access assignment, so the advice depends on what the loop is doing;
             * both halves are given rather than guessing which one applies. */
            fix = "a MutableArray (mutable_array_new, push, snapshot) if the loop fills it in order, or a "
                  "MutableMap keyed by index if it does not";
        }
    }
    if (fix == NULL) return;
    ziman_warning(g_lint_file, s->line, s->col,
                  "'%s' is rebuilt from a copy of itself on every pass of this loop (quadratic); collect the pieces in %s", name, fix);
    g_lint_count++;
}


/* --lint: reading a builder by converting it, inside a loop that keeps adding to it.
 *
 * `val seen = snapshot(counts)` in a loop whose next statement does `set(counts, ..)`
 * materialises the whole map on every pass, when `mutable_map_get`/`mutable_map_get_or`/`contains`/
 * `len` read a builder in place. That is what a builder is for. The same goes for
 * `snapshot` against `len`, and `snapshot` against `len`.
 *
 * `lint_quadratic_accumulation` above cannot see this: it matches a variable rebuilt *from
 * itself* (`x = concat(x, ..)`), and this shape has no self-reference.
 *
 * **The obvious predicate is unusable, which is why this one is narrow.** "A builder conversion
 * inside a loop" gives 14 hits across lib/, the reference backend and the benchmark, and 13 are
 * correct code: a conversion on the way out of a loop (`return ok(snapshot(out))`) runs once,
 * and a convert-then-reset (`csv.zm`'s `cells`, `migrate.zm`'s `current`) converts one batch and
 * replaces the builder, which is linear overall. A rule that noisy gets switched off.
 *
 * So all three of these must hold, and each of the 13 fails at least one:
 *   1. the conversion's result is bound by a `val`/`var` in the loop body -- not returned, not
 *      passed straight on, so the loop keeps going with it;
 *   2. a later statement in the *same* block appends to that same builder; and
 *   3. the builder is not reassigned between the two, since a reset means the next conversion is
 *      of a fresh builder.
 *
 * Found one live instance when it was written: `lib/core/fs.zm`'s `path_join` converted the whole
 * accumulated path on every piece to look at its last character. Measurably superlinear --
 * 0.02s, 0.04s, 0.09s, 0.35s at 2,000 to 16,000 pieces (docs/reviews/tasks.md F84). */

/* The lint pairs the one conversion out of a builder -- `snapshot` since F100 -- with the append
 * that makes repeating it quadratic. There used to be four conversion names, so the builder's kind
 * could be read straight off the call; with one name it comes from whichever appender the loop body
 * uses instead. That is precise rather than a guess, because only one of the four can type-check
 * against the same variable, and it is what keeps the advice per-type where a single generic
 * message would have been the easy answer and a worse one.
 *
 * The appenders are bare names now (`push` since F208, `append` since F205), which does not blur
 * the reading: it is the *variable* that picks the entry, and a MutableArray type-checks against
 * `push` and nothing else here. The array half of `push` cannot be confused for the builder half
 * either, because `push(arr, x)` in statement position is already the discarded-result error
 * below -- there is no loop body where it stands and means something.
 *
 * **The MutableSet entry was keyed on `mutable_set_add` until 2026-10-22, and that branch was
 * dead.** F199 made `add` the word in both set worlds, and `mutable_set_add` has been `internal`
 * ever since -- so any real loop writes `add(seen, x)` and the kind could not be read at all,
 * which means the one lint on this page that exists to catch a quadratic silently stopped
 * catching it for a MutableSet. It passed its own fixture only because the fixture still used the
 * obsolete name; migrating that fixture to `add` is what exposed it, three warnings firing where
 * four were expected. The third rename to break this table (F162 and F205 were the others), and
 * the reason the entries are now the names a program would actually write. */
typedef struct {
    const char *appender;
    const char *readers;
} BuilderKindAdvice;

static const BuilderKindAdvice BUILDER_KINDS[] = {
    {"push", "len, b[i] or 'for x in <the builder>'"},
    {"append", "len, or a MutableString you finish once after the loop"},
    {"set", "len, contains, get or get_or"},
    {"add", "len or contains"},
    {NULL, NULL}};


/* The bare name of a call's callee, without a module qualifier. */
static const char *callee_tail(const Expr *e) {
    if (e == NULL || e->kind != EXPR_CALL || e->as.call.callee == NULL) return NULL;
    const char *callee = e->as.call.callee;
    const char *dot = strrchr(callee, '.');
    return dot != NULL ? dot + 1 : callee;
}

/* Whether `e`, or anything inside it, is `<conversion>(<builder>)`. Set `*found` to the conversion
 * name and `*builder` to the argument's identifier. Only a direct identifier argument counts: a
 * conversion of some other expression is not a builder this block can see being appended to. */
static int finds_builder_conversion(const Expr *e, const char **found, const char **builder) {
    if (e == NULL) return 0;
    if (e->kind == EXPR_CALL) {
        const char *name = callee_tail(e);
        if (name != NULL && strcmp(name, "snapshot") == 0 && e->as.call.args.count == 1) {
            const Expr *arg = e->as.call.args.items[0];
            if (arg->kind == EXPR_IDENT) {
                *found = name;
                *builder = arg->as.ident_name;
                return 1;
            }
        }
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (finds_builder_conversion(e->as.call.args.items[i], found, builder)) return 1;
        }
    }
    return 0;
}

/* Whether `s` calls `appender(builder, ..)`, i.e. the loop keeps growing it. */
static int statement_appends_to(const Stmt *s, const char *appender, const char *builder) {
    if (s == NULL || s->kind != STMT_EXPR) return 0;
    const Expr *e = s->as.expr_stmt.expr;
    const char *name = callee_tail(e);
    if (name == NULL || strcmp(name, appender) != 0 || e->as.call.args.count < 1) return 0;
    const Expr *first = e->as.call.args.items[0];
    return first->kind == EXPR_IDENT && strcmp(first->as.ident_name, builder) == 0;
}

/* The advice for whichever kind `stmt` appends to `builder` with, or NULL when it appends nothing. */
static const char *readers_for_appending_stmt(const Stmt *stmt, const char *builder) {
    for (size_t k = 0; BUILDER_KINDS[k].appender != NULL; k++) {
        if (statement_appends_to(stmt, BUILDER_KINDS[k].appender, builder)) return BUILDER_KINDS[k].readers;
    }
    return NULL;
}

/* Whether `s` assigns to `builder`, which resets it and ends the run. */
static int statement_reassigns(const Stmt *s, const char *builder) {
    return s != NULL && s->kind == STMT_ASSIGN && s->as.assign.name != NULL
           && strcmp(s->as.assign.name, builder) == 0;
}

/* Each block *within* the loop is examined, not only the loop's own statement list: the pattern
 * usually sits inside an `if`, which is where `fs.zm`'s `path_join` had it -- the first version of
 * this check looked only at the loop body's immediate statements and missed the one live instance
 * in the library. A nested loop is descended into as well, since the shape is the same there. */
static void lint_builder_read_by_conversion(const Stmt *body);

static void lint_builder_blocks_within(const Stmt *s) {
    if (s == NULL) return;
    switch (s->kind) {
        case STMT_BLOCK: lint_builder_read_by_conversion(s); break;
        case STMT_IF:
            lint_builder_blocks_within(s->as.if_stmt.then_block);
            lint_builder_blocks_within(s->as.if_stmt.else_block);
            break;
        case STMT_WHILE: lint_builder_blocks_within(s->as.while_stmt.body); break;
        case STMT_FOR: lint_builder_blocks_within(s->as.for_stmt.body); break;
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) {
                lint_builder_blocks_within(s->as.switch_stmt.arms[i].body);
            }
            break;
        default: break;
    }
}

static void lint_builder_read_by_conversion(const Stmt *body) {
    if (body == NULL || body->kind != STMT_BLOCK) return;
    const StmtList *stmts = &body->as.block.stmts;
    for (int i = 0; i < stmts->count; i++) {
        lint_builder_blocks_within(stmts->items[i]);
    }
    for (int i = 0; i < stmts->count; i++) {
        const Stmt *s = stmts->items[i];
        /* Two shapes, not one. `val seen_so_far = snapshot(b)` is the obvious one; `for x in
         * snapshot(b)` is the one this missed for as long as the lint existed, because it only
         * ever looked at a declaration's initialiser -- and it is the *more* common way to write
         * the quadratic, since converting a handle to look through it is exactly what a `for`
         * wants. Three live instances were sitting in the reference backend unflagged, two of them
         * a linear membership scan over a full copy per pass. */
        const Expr *converted = s->kind == STMT_VAL || s->kind == STMT_VAR ? s->as.decl.init
                                : s->kind == STMT_FOR                     ? s->as.for_stmt.iterable
                                                                          : NULL;
        if (converted == NULL) continue;
        const char *conversion = NULL, *builder = NULL;
        if (!finds_builder_conversion(converted, &conversion, &builder)) continue;
        for (int j = i + 1; j < stmts->count; j++) {
            if (statement_reassigns(stmts->items[j], builder)) break;
            const char *readers = readers_for_appending_stmt(stmts->items[j], builder);
            if (readers == NULL) continue;
            if (s->kind == STMT_FOR) {
                /* All four mutable collections iterate directly as of F111, so the advice for this
                 * shape is not the reader list -- it is to drop the conversion entirely. */
                ziman_warning(g_lint_file, s->line, s->col,
                              "'%s' converts the whole of '%s' on every pass of this loop, and the loop keeps "
                              "adding to it (quadratic); iterate it directly with 'for ... in %s'",
                              conversion, builder, builder);
            } else {
                ziman_warning(g_lint_file, s->line, s->col,
                              "'%s' converts the whole of '%s' on every pass of this loop, and the loop keeps "
                              "adding to it (quadratic); read it in place with %s",
                              conversion, builder, readers);
            }
            g_lint_count++;
            break;
        }
    }
}

static int type_supports_ordering(Module *module, Type t);
static FunctionDecl *find_compare_function(Module *module, Type t) {
    const char *type_name_str = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
    if (type_name_str == NULL) return NULL;
    char *fn_name = ziman_compare_function_name(type_name_str);
    FunctionDecl *fn = find_function(module, fn_name);
    if (fn == NULL) fn = find_function(module, ziman_compare_function_name_flat(type_name_str));
    if (fn == NULL || fn->params.count != 2 || fn->return_type.kind != TY_INT) return NULL;
    if (fn->type_params.count > 0) {
        /* A generic compare function (derived for `Pair<A, B>`) orders every instantiation
         * whose type arguments satisfy the bounds it declares. */
        if (fn->type_params.count != t.type_args.count) return NULL;
        for (int i = 0; i < fn->type_params.count; i++) {
            if ((fn->type_params.items[i].bound & BOUND_ORD) && !type_supports_ordering(module, t.type_args.items[i])) return NULL;
        }
        return fn;
    }
    if (!type_equals(fn->params.items[0].type, t) || !type_equals(fn->params.items[1].type, t)) return NULL;
    return fn;
}

/* An enum's `<snake_type>_message(e: Type) -> string`, the same naming-convention dispatch
 * `Ord` uses for `<snake_type>_compare` -- what the `error_message` builtin looks for. Enums
 * only: an error type is never a struct in this codebase, and keeping it that narrow keeps the
 * lookup unambiguous with no generic-type-parameter case to handle (unlike `Ord`, no bundled or
 * derived message function is generic). */
static FunctionDecl *find_message_function(Module *module, Type t) {
    if (t.kind != TY_ENUM || t.enum_name == NULL) return NULL;
    char *fn_name = ziman_message_function_name(t.enum_name);
    FunctionDecl *fn = find_function(module, fn_name);
    if (fn == NULL || fn->params.count != 1 || fn->return_type.kind != TY_STRING) return NULL;
    if (fn->type_params.count > 0 || !type_equals(fn->params.items[0].type, t)) return NULL;
    return fn;
}

/* Whether `t` has a total order: the scalars that order naturally (int, float,
 * bigint, decimal, byte, string -- bytewise), a struct or enum with a
 * `<type>_compare(a, b) -> int` function, a type parameter bounded by `Ord`,
 * and -- recursively -- an array, an Option or a Result whose element/payload
 * types are ordered. Channels and bool don't. */
static int type_supports_ordering(Module *module, Type t) {
    ZIMAN_STACK_CHECK();
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE:
            return 1;
        case TY_ARRAY:
            return type_supports_ordering(module, *t.elem);
        case TY_RESULT:
            return type_supports_ordering(module, *t.elem) && type_supports_ordering(module, *t.elem_err);
        case TY_ENUM:
            if (t.enum_name != NULL && strcmp(t.enum_name, "Option") == 0 && t.type_args.count == 1) {
                return type_supports_ordering(module, t.type_args.items[0]);
            }
            return find_compare_function(module, t) != NULL;
        case TY_STRUCT:
            return find_compare_function(module, t) != NULL;
        case TY_TYPEVAR:
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_ORD) != 0;
                    }
                }
            }
            return 0;
        default:
            return 0;
    }
}

/* Enforces every bound a generic declaration's own type parameters carry
 * (`Eq`/`Hash`, independently) against the concrete type arguments an
 * instantiation actually supplies -- e.g. `Box<Channel<int>>` is
 * rejected here if `Box` were declared `struct Box<T: Eq>`, since
 * `Channel<int>` doesn't support `==`. This is what keeps "a bounded T
 * is always comparable/hashable" true in practice, so a generic
 * declaration's own body can rely on it (see ROADMAP.md's "Generics"
 * Stage 5). Assumes `params->count == args->count`, already checked by
 * the caller. */
static void check_type_args_satisfy_bounds(Module *module, TypeParamList *params, TypeList *args, int line, int col) {
    for (int i = 0; i < params->count; i++) {
        int bound = params->items[i].bound;
        if ((bound & BOUND_EQ) && !type_supports_equality(module, args->items[i], 0)) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Eq' required by type parameter '%s' (it must support '==')",
                        type_name(args->items[i]), params->items[i].name);
        }
        if ((bound & BOUND_ORD) && !type_supports_ordering(module, args->items[i])) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Ord' required by type parameter '%s' (it must be a number, string or byte, "
                        "or a struct/enum with a '<type>_compare(a, b) -> int' function -- '#[derive(Ord)]' writes one)",
                        type_name(args->items[i]), params->items[i].name);
        }
        if ((bound & BOUND_HASH) && !type_supports_hashing(module, args->items[i], 0)) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Hash' required by type parameter '%s' (it must support '==' and have only hashable parts)",
                        type_name(args->items[i]), params->items[i].name);
        }
    }
}

static void require_not_void(Expr *e, const char *context) {
    if (e->resolved_type.kind == TY_VOID) {
        ziman_error("type", e->line, e->col, "cannot use a void value %s", context);
    }
}

static Type check_binary(Module *module, Env *env, Expr *e) {
    /* `==`/`!=` compare two values of the same type, so each side is the other's context: that is
     * what lets `record.status != Some("ACTIVE")` stand where it needed the qualified spelling
     * before. Whichever side can be typed on its own goes first and the other is checked against
     * it; when neither can, both fall through to the ordinary error. Only the two equality
     * operators do this -- the arithmetic and ordering ones take no Option, so offering one would
     * only produce a stranger message for the same mistake. */
    int is_equality = e->as.binary.op == OP_EQ || e->as.binary.op == OP_NEQ;
    Type lt;
    Type rt;
    if (is_equality && expr_needs_expected_type(e->as.binary.left) &&
        !expr_needs_expected_type(e->as.binary.right)) {
        rt = check_expr(module, env, e->as.binary.right);
        lt = check_expr_expecting(module, env, &e->as.binary.left, rt);
    } else if (is_equality && expr_needs_expected_type(e->as.binary.right)) {
        lt = check_expr(module, env, e->as.binary.left);
        rt = check_expr_expecting(module, env, &e->as.binary.right, lt);
    } else {
        lt = check_expr(module, env, e->as.binary.left);
        rt = check_expr(module, env, e->as.binary.right);
    }
    require_not_void(e->as.binary.left, "as an operand");
    require_not_void(e->as.binary.right, "as an operand");

    switch (e->as.binary.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            if (e->as.binary.op == OP_ADD && (lt.kind == TY_STRING || rt.kind == TY_STRING)) {
                ziman_error("type", e->line, e->col,
                            "'+' doesn't join strings (found %s and %s) -- use concat(a, b), or format(\"{}{}\", a, b) for several pieces; convert a number with str(n)",
                            type_name(lt), type_name(rt));
            }
            if (e->as.binary.op == OP_ADD && (lt.kind == TY_ARRAY || rt.kind == TY_ARRAY)) {
                ziman_error("type", e->line, e->col,
                            "'+' doesn't join arrays (found %s and %s) -- use push(arr, item) to add one element or concat_arrays<T>(a, b) to join two",
                            type_name(lt), type_name(rt));
            }
            if (e->as.binary.op == OP_MOD && lt.kind == TY_FLOAT && rt.kind == TY_FLOAT) {
                ziman_error("type", e->line, e->col,
                            "'%%' is not defined for float (there is no float remainder); use it on int, bigint or decimal, or cast to int first if a whole-number remainder is what you mean");
            }
            /* An Option is the one case where the generic message's "use 'as' to cast" is advice that
             * cannot work: there is no cast from Option<int> to int, and what is needed is to get the
             * value out. Same shape as the string and array cases above, and the same class of defect
             * as C9's -- a message naming a repair that does not exist costs more than saying less. */
            if (is_option_type(lt) || is_option_type(rt)) {
                Type opt = is_option_type(lt) ? lt : rt;
                ziman_error("type", e->line, e->col,
                            "arithmetic needs the value inside the %s, not the Option itself (found %s and %s) -- unwrap it first: "
                            "'if Some(v) = o { v + 1 }', or option_unwrap_or(o, 0) for a default, or Option.unwrap_Some(o) when it cannot be None",
                            type_name(opt), type_name(lt), type_name(rt));
            }
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT && lt.kind != TY_BIGINT && lt.kind != TY_DECIMAL)) {
                ziman_error("type", e->line, e->col,
                            "arithmetic operator requires two operands of the same numeric type, found %s and %s (use 'as' to cast explicitly)",
                            type_name(lt), type_name(rt));
            }
            return lt;
        case OP_LT:
        case OP_GT:
        case OP_LE:
        case OP_GE:
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT && lt.kind != TY_BIGINT && lt.kind != TY_DECIMAL &&
                                       lt.kind != TY_STRING && lt.kind != TY_BYTE)) {
                ziman_error("type", e->line, e->col,
                            "'<' '>' '<=' '>=' need two operands of the same number, string or byte type, found %s and %s "
                            "(for a struct or enum use compare(a, b), which needs a '<type>_compare' function or '#[derive(Ord)]')",
                            type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        case OP_EQ:
        case OP_NEQ:
            if (!type_equals(lt, rt)) {
                ziman_error("type", e->line, e->col, "cannot compare %s and %s for equality", type_name(lt), type_name(rt));
            }
            if (!type_supports_equality(module, lt, 0)) {
                if (lt.kind == TY_RESULT) {
                    ziman_error("type", e->line, e->col,
                                "%s cannot be compared with '=='/'!=' because its Ok or Err type is not a comparable type",
                                type_name(lt));
                } else if (lt.kind == TY_ARRAY) {
                    ziman_error("type", e->line, e->col,
                                "array '=='/'!=' requires an array of a comparable ('==') element type, found %s", type_name(lt));
                } else if (lt.kind == TY_ENUM) {
                    ziman_error("type", e->line, e->col,
                                "enum '%s' cannot be compared with '=='/'!=' because one of its variants' payload is not a comparable type",
                                type_name(lt));
                } else if (lt.kind == TY_CHANNEL) {
                    ziman_error("type", e->line, e->col, "%s cannot be compared with '=='/'!=' in v0", type_name(lt));
                } else if (lt.kind == TY_MAP || lt.kind == TY_SET) {
                    ziman_error("type", e->line, e->col,
                                "%s cannot be compared with '=='/'!=' because its value type is not a comparable type",
                                type_name(lt));
                } else if (lt.kind == TY_FUNC) {
                    ziman_error("type", e->line, e->col,
                                "function values (%s) cannot be compared with '=='/'!=' -- compare a name or an id you keep alongside the function instead",
                                type_name(lt));
                } else if (lt.kind == TY_TYPEVAR) {
                    ziman_error("type", e->line, e->col,
                                "type parameter '%s' cannot be compared with '=='/'!=' because it has no 'Eq' bound (add '%s: Eq' to require one)",
                                type_name(lt), type_name(lt));
                } else if (lt.kind == TY_STRUCT && find_struct(module, lt.struct_name) != NULL &&
                           find_struct(module, lt.struct_name)->thread_bound) {
                    /* Without this the message blames "one of its fields", which is a lie: the
                     * field is an `int` and perfectly comparable. What is not comparable is the
                     * handle, because that int is an index into a native table and two of them
                     * say nothing about the contents. */
                    ziman_error("type", e->line, e->col,
                                "'%s' is a thread-bound handle and cannot be compared with '=='/'!=': its contents "
                                "live behind an opaque index, so two handles tell you nothing about what is in them. "
                                "Compare the snapshots instead ('snapshot(a) == snapshot(b)')",
                                type_name(lt));
                } else {
                    ziman_error("type", e->line, e->col,
                                "struct '%s' cannot be compared with '=='/'!=' because one of its fields is not a comparable type",
                                type_name(lt));
                }
            }
            return type_scalar(TY_BOOL);
        case OP_AND:
        case OP_OR:
            if (lt.kind != TY_BOOL || rt.kind != TY_BOOL) {
                ziman_error("type", e->line, e->col, "'&&'/'||' require bool operands, found %s and %s", type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        default:
            ziman_error("type", e->line, e->col, "internal error: not a binary operator");
            return type_scalar(TY_VOID);
    }
}

static Type check_unary(Module *module, Env *env, Expr *e) {
    Type t = check_expr(module, env, e->as.unary.operand);
    require_not_void(e->as.unary.operand, "as an operand");
    if (e->as.unary.op == OP_NEG) {
        if (t.kind != TY_INT && t.kind != TY_FLOAT && t.kind != TY_BIGINT && t.kind != TY_DECIMAL) {
            ziman_error("type", e->line, e->col, "unary '-' requires an int, float, bigint, or decimal operand, found %s", type_name(t));
        }
        return t;
    }
    /* OP_NOT */
    if (t.kind != TY_BOOL) {
        ziman_error("type", e->line, e->col, "unary '!' requires a bool operand, found %s", type_name(t));
    }
    return type_scalar(TY_BOOL);
}

/* Scans a format-string literal's already-unescaped content for placeholders (`{}` and `{:.N}`; `{{`/`}}` are
 * literal braces -- see fmtstr.h), exiting with a type error on a stray/unmatched brace or a bad precision. Returns
 * the count and, in `*precisions`, each placeholder's N (or -1). `fmt_expr` is only used to anchor error locations -- v0
 * doesn't track per-character positions inside a string literal, so mistakes are reported at the literal as a whole. */
static int scan_placeholders(Expr *fmt_expr, int **precisions) {
    const char *s = fmt_expr->as.string_val;
    size_t n = strlen(s), pos = 0;
    int *found = ziman_xmalloc((n + 1) * sizeof(int));
    int count = 0;
    FmtPiece piece;
    char err[256];
    for (;;) {
        int r = fmt_next(s, n, &pos, &piece, err, sizeof err);
        if (r == 0) break;
        if (r < 0) ziman_error("type", fmt_expr->line, fmt_expr->col, "%s", err);
        if (piece.placeholder) found[count++] = piece.precision;
    }
    *precisions = found;
    return count;
}

/* True for `Option<T>` specifically (the prelude's own enum), never a
 * user-declared one of some other name -- the only enum type Ziman ever
 * lets a parameter be omitted for, or a bare unwrapped value be
 * auto-wrapped into. */
static int is_option_type(Type t) {
    return t.kind == TY_ENUM && strcmp(t.enum_name, "Option") == 0;
}

/* `Option<T>`'s own `T` -- only meaningful when is_option_type(t). */
static Type option_elem_type(Type t) {
    return t.type_args.items[0];
}

/* The type `Option<elem>`, for the one place that has to build one rather than read one: inferring
 * a generic call's type argument from a bare `Some(x)` whose payload types on its own. */
static Type option_type_of(Type elem) {
    Type t = {0};
    t.kind = TY_ENUM;
    t.enum_name = "Option";
    t.type_args = (TypeList){0};
    type_list_push(&t.type_args, elem);
    return t;
}

/* Rewrites `value` (already type-checked, of type option_elem_type
 * (option_type)) into a synthetic `Option<T>.Some(value)` node, the
 * auto-wrap sugar an Option<T> parameter gives a bare T argument or
 * default value (see docs/DESIGN.md's "Option<T> parameter omission and
 * defaults" entry). Reuses ordinary enum construction (EXPR_ENUM_ACCESS)
 * wholesale -- eval_enum_access in interpreter.c needs no changes at all
 * to evaluate this exactly like a `Some(...)` a person wrote by hand. */
static Expr *wrap_bare_in_some(Expr *value, Type option_type) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, value->line, value->col);
    e->as.enum_access.enum_name = "Option";
    e->as.enum_access.member_name = "Some";
    expr_list_push(&e->as.enum_access.args, value);
    e->as.enum_access.has_parens = 1;
    e->as.enum_access.type_args = option_type.type_args;
    e->resolved_type = option_type;
    return e;
}

/* The implicit default an omitted Option<T> parameter gets when its
 * declaration gave no `= expr` of its own -- a synthetic
 * `Option<T>.None`, again reusing ordinary enum construction as-is. */
static Expr *make_none_expr(Type option_type, int line, int col) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, line, col);
    e->as.enum_access.enum_name = "Option";
    e->as.enum_access.member_name = "None";
    e->as.enum_access.type_args = option_type.type_args;
    e->resolved_type = option_type;
    return e;
}

/* Rewrites `value` (already type-checked, of type `*result_type.elem`
 * when `is_ok`, `*result_type.elem_err` otherwise) into a synthetic
 * `ok(value)`/`err(value)` call -- the auto-wrap sugar a Result<T, E>
 * parameter gives a bare T-or-E argument (see docs/DESIGN.md's
 * "Result<T, E> bare-value auto-wrap" entry). Unlike Option<T>'s own
 * wrap (an ordinary enum, reusing EXPR_ENUM_ACCESS), Result<T, E> is a
 * hardcoded primitive with no enum declaration behind it at all --
 * `ok`/`err` are themselves hardcoded core builtins (see check_call's
 * and interpreter.c's own `strcmp(callee, "ok")` branches), so this
 * reuses THAT machinery instead: a synthetic EXPR_CALL to "ok"/"err"
 * needs zero changes anywhere it's evaluated, exactly like Option<T>'s
 * own wrap needed zero changes to eval_enum_access. */
static Expr *wrap_bare_in_result(Expr *value, Type result_type, int is_ok) {
    Expr *e = expr_new(EXPR_CALL, value->line, value->col);
    e->as.call.callee = is_ok ? "ok" : "err";
    expr_list_push(&e->as.call.args, value);
    e->resolved_type = result_type;
    return e;
}

/* The implicit value an omitted Option<T> parameter gets when its
 * declaration *did* give a `= expr` default -- a synthetic reference to
 * the global-env slot init_param_defaults (interpreter.c) will have
 * already filled in, once, before `main` runs, under this exact key
 * (see ast.h's Param.default_key). Reuses ordinary bare-name lookup
 * (EXPR_IDENT) wholesale: nothing in check_call's own env can ever
 * define a same-named local (the key contains '$', unforgeable by any
 * real Ziman identifier), so it always falls through to g_global_env,
 * exactly the way a bare reference to a real global val already does. */
static Expr *make_default_ref_expr(Param *param, int line, int col) {
    Expr *e = expr_new(EXPR_IDENT, line, col);
    e->as.ident_name = param->default_key;
    e->resolved_type = param->type;
    return e;
}

/* Checks `*arg_slot` against `param_type`. Two auto-wrap exceptions to
 * check_call's usual always-exact-match rule, both narrow and both
 * driven entirely by `param_type`'s own shape:
 *   - `Option<T>`: a bare `T` value is legal too, rewritten in place
 *     into `Some(T)` (see wrap_bare_in_some) -- needed by both an
 *     explicitly supplied argument and a parameter's own default
 *     expression (two of this function's callers).
 *   - `Result<T, E>`: a bare `T` value becomes `Ok(T)`; a bare `E`
 *     value becomes `Err(E)`; when `T == E`, always `Ok` (see
 *     wrap_bare_in_result and docs/DESIGN.md's "Result<T, E> bare-value
 *     auto-wrap" entry) -- `Result<T, E>` parameters are never
 *     omittable, so this only ever applies to a supplied argument, not
 *     a default (Result<T, E> can't have one).
 * Every other parameter type keeps check_call's always-exact-match
 * behavior, unchanged. Returns 1 and leaves `*out_arg_type` untouched
 * on success; returns 0 and sets `*out_arg_type` to the value's actual
 * type so the caller (which knows whether it's reporting a bad argument
 * or a bad default) can build its own error message. */
static int check_value_against_param_type(Module *module, Env *env, Expr **arg_slot, Type param_type, Type *out_arg_type,
                                          const char *void_context) {
    Expr *arg = *arg_slot;
    Type arg_type;
    /* An empty array literal (`[]`) has no elements to infer a type
     * from, but the parameter it's passed to says exactly what it must
     * be -- the same "expected type from context" trick a `val`
     * annotation, struct field, or `return` already gets (see
     * check_expr_expecting). Without this, a call like
     * `execute(conn, sql, [])` needed a separately annotated `val` just
     * to spell "no parameters". */
    if (arg->kind == EXPR_ARRAY_LIT && arg->as.array_lit.elements.count == 0 && param_type.kind == TY_ARRAY) {
        arg->resolved_type = param_type;
        return 1;
    }
    /* A fresh (never-checked) Expr's resolved_type is always TY_VOID
     * (see expr_new) until check_expr fills it in -- the one exception
     * is a synthetic node canonicalize_named_args built for an omitted
     * Option<T> parameter (make_none_expr/make_default_ref_expr), which
     * sets resolved_type up front, to exactly this parameter's own
     * type, at construction time. Re-running check_expr on one would,
     * at best, redo work for nothing; for a default-value reference in
     * particular it would actively fail, since the global-env slot it
     * names isn't defined until interpretation starts (init_param_defaults),
     * long after type-checking is done. */
    if (arg->resolved_type.kind != TY_VOID) {
        arg_type = arg->resolved_type;
    } else if (arg->kind == EXPR_VALUE_BLOCK) {
        arg_type = check_value_block_expr(module, env, arg, &param_type);
        arg->resolved_type = arg_type;
    } else if (arg->kind == EXPR_LAMBDA && param_type.kind == TY_FUNC) {
        /* a lambda is checked against the function type the parameter wants,
         * so its own parameter and return types may be left out */
        arg_type = check_lambda_expecting(module, env, arg, &param_type);
        arg->resolved_type = arg_type;
    } else if (check_ok_err_call(module, env, arg, param_type) ||
               check_channel_new_call(module, env, arg, param_type) ||
               check_mutable_empty_call(module, env, arg, param_type) ||
               check_bare_option_call(module, env, arg, param_type)) {
        /* `ok`/`err`, `channel_new()` and a bare `Some(...)`/`None` carry no type of their own and take
         * it from the context, which is why every other context offering one -- an annotated val, a
         * struct literal field, a `with` override, a `return` -- accepts them. A call argument offers
         * one too (the parameter's declared type) and was the single place that did not use it, so
         * `f(Some(4))` was rejected where `f(4)` was accepted: the explicit spelling was the one
         * refused, and the error even listed the contexts that work without mentioning why this was not
         * among them. These are the same three helpers check_expr_expecting calls, in the same order;
         * each sets the argument's resolved_type to `param_type` when it matches, so the caller needs
         * nothing further. */
        *out_arg_type = param_type;
        return 1;
    } else if (param_type.kind == TY_RESULT && expr_needs_expected_type(arg) &&
               !type_equals(*param_type.elem, *param_type.elem_err) &&
               (check_bare_option_call(module, env, arg, *param_type.elem) ||
                check_channel_new_call(module, env, arg, *param_type.elem))) {
        /* The bare-value auto-wrap, reaching one level further in. A `Result<Option<T>, E>` already
         * accepts a bare `Option<T>` and wraps it in `ok(...)`, but only once that value had a type
         * of its own -- so `return Option<T>.Some(x)` worked and `return Some(x)` did not, which is
         * the shape every `find_by_*` in a repository has. The payload type is what the inner value
         * needed all along, so it is offered here before giving up.
         *
         * Deliberately not when the success and error types are equal: that is the one case the
         * wrap below already refuses to guess at, and guessing earlier would be worse, not better.
         * `ok`/`err` are not retried here either -- those match `param_type` itself above, and a
         * bare `ok(x)` inside an auto-wrap would mean `ok(ok(x))`, which nobody writes by accident. */
        *arg_slot = wrap_bare_in_result(arg, param_type, 1);
        *out_arg_type = param_type;
        return 1;
    } else {
        arg_type = check_expr(module, env, arg);
        require_not_void(arg, void_context);
    }
    if (type_equals(arg_type, param_type)) {
        return 1;
    }
    if (is_option_type(param_type) && type_equals(arg_type, option_elem_type(param_type))) {
        /* Option<Option<T>>: a bare Option<T> would become Some(that),
         * so a None quietly turns into Some(None) -- exactly the
         * distinction people get wrong. Make the author say which. */
        if (is_option_type(option_elem_type(param_type))) {
            ziman_error("type", arg->line, arg->col,
                        "a bare %s is ambiguous where %s is expected (it would silently become Some(...), so a None turns into Some(None)); write %s.Some(...) explicitly",
                        type_name(arg_type), type_name(param_type), type_name(param_type));
        }
        *arg_slot = wrap_bare_in_some(arg, param_type);
        return 1;
    }
    if (param_type.kind == TY_RESULT) {
        Type ok_type = *param_type.elem;
        Type err_type = *param_type.elem_err;
        if (type_equals(arg_type, ok_type)) {
            /* T == E: the sugar can't tell an intended error from a
             * success, and silently picking Ok turned `return "bad
             * input"` into a success. Require ok(...)/err(...). */
            if (type_equals(ok_type, err_type)) {
                ziman_error("type", arg->line, arg->col,
                            "a bare %s is ambiguous where %s is expected (the success and error types are the same): write ok(...) or err(...) to say which",
                            type_name(arg_type), type_name(param_type));
            }
            *arg_slot = wrap_bare_in_result(arg, param_type, 1);
            return 1;
        }
        if (!type_equals(ok_type, err_type) && type_equals(arg_type, err_type)) {
            *arg_slot = wrap_bare_in_result(arg, param_type, 0);
            return 1;
        }
    }
    *out_arg_type = arg_type;
    return 0;
}

/* Hardcoded core builtins (see README.md's "Builtins ship two ways")
 * each have exactly one fixed signature already special-cased below by
 * name, not an ordinary declared FunctionDecl -- so there's no
 * parameter list here for a named call to reorder against. Named
 * arguments are for real (user or prelude) `fn`/`native fn`
 * declarations only. */
/* Ziman has no string interpolation (docs/SYNTAX.md: "No interpolation in v0"), so `"total: ${t}"`
 * is a string containing those characters and prints as them. Round 2 of the 2026-10-12 generation
 * benchmark found that this is the costliest silence in the language: two of fifty attempts printed
 * `${total}` to stdout with --check exiting 0, and two agents reading only --check concluded that
 * interpolation *works*, one of them reporting it "stringifies int/bool". A compile-clean program
 * with silently wrong output is exactly what a model iterating against the compiler cannot find.
 *
 * The rule is deliberately narrow, and the corpus is what narrowed it. A lexical "any `${`" warning
 * fires 19 times across lib/, the tests and the reference backend, and **every one of those is
 * correct code**: `format("${}", i)` is a literal dollar followed by Ziman's own `{}` placeholder,
 * which is how the backend writes Postgres `$1` parameters, and `properties` and `regex` both take
 * `${name}` as their own template syntax in data. So this warns only when the braces hold a plain
 * identifier (or a path whose first segment is one) that is *a binding in scope right here* -- the
 * shape that can only be a mistaken
 * interpolation, since the author has that very value to hand. Both spellings are covered, `${name}`
 * and the bare `{name}` that round 3 found a third agent inventing, so the way to write those braces
 * as text is to split them from the name: concat("{", "name}") is quiet and prints `{name}`. A `\$`
 * escape was written for this and dropped instead of shipped -- the checker sees the literal after
 * escape decoding, so it could not have suppressed anything. Note `"{{name}}"` is **not** a way to
 * write it: `{{` is a format-string escape, and in a plain literal it prints both braces. */
static void warn_brace_interpolation(Env *env, Expr *e) {
    const char *s = e->as.string_val;
    if (s == NULL || env == NULL) return;
    for (const char *p = s; p[0] != '\0'; p++) {
        /* `${name}` and `{name}` are two spellings of the same mistake, and both reach here only in
         * a plain literal: `format`/`printf` never pass their format string through check_expr (it
         * is typed directly and scanned by scan_placeholders), so a real placeholder is never seen
         * by this function and a malformed one is already a hard error there. */
        int dollar = (p[0] == '$' && p[1] == '{');
        if (!dollar) {
            if (p[0] != '{') continue;
            if (p[1] == '{') { p++; continue; }       /* `{{`: two literal braces, not a name */
            if (p > s && p[-1] == '$') continue;      /* already handled as the `${` form */
        }
        const char *name = p + (dollar ? 2 : 1);
        size_t head = 0;   /* the first path segment: `${cfg.host}` is the same mistake as `${cfg}` */
        while (isalnum((unsigned char)name[head]) || name[head] == '_') head++;
        size_t n = head;
        while (name[n] != '\0' && name[n] != '}'
               && (isalnum((unsigned char)name[n]) || name[n] == '_' || name[n] == '.')) n++;
        if (head == 0 || name[n] != '}') continue; /* `${}`, `${ x }`, `${a-b}`: not a name path */
        char ident[64];
        if (head >= sizeof ident) continue;
        memcpy(ident, name, head);
        ident[head] = '\0';
        if (env_lookup(env, ident) == NULL) continue; /* a template placeholder, not a local */
        char path[128];
        if (n >= sizeof path) continue;
        memcpy(path, name, n);
        path[n] = '\0';
        ziman_warning(ziman_error_file(), e->line, e->col,
                      "'%s%s}' in this string is not interpolation -- Ziman has none, so it prints "
                      "as those characters; '%s' is in scope here, so you probably meant "
                      "format(\"...{}...\", %s). A '{}' placeholder only means anything in the format "
                      "string of 'format' or 'printf'; for the braces as text, split them from the "
                      "name, as concat(\"{\", \"%s}\")",
                      dollar ? "${" : "{", path, ident, path, path);
    }
}

/* Builtins that compute a value and do nothing else. Discarding one in statement position is
 * always a mistake, which is what makes the check in check_stmt's STMT_EXPR safe to apply without
 * an effect system: a user function may well be called for its effects and its return value
 * ignored, but none of these can be. print/printf and the channel builtins are left out on
 * purpose -- they are called precisely for their effects. So are unwrap/unwrap_err/unwrap_Some,
 * and that one is not a judgement call: adding them reported 30 errors across docs/lib/examples
 * alone, because `unwrap(fs.write_text(...))` is the corpus's own idiom for "abort if this
 * failed" -- the value is genuinely unwanted and the abort is the point. */
/* **The Map/Set pairing left this list half-renamed, and F212 is the other half.** F195-F201 took
 * the prefix off fourteen of these -- `set_union` became `union`, `map_keys` became `keys`,
 * `set_insert_all` became `add` -- and each rename deleted nothing and added nothing here, so the
 * list went on naming fourteen names that no longer resolve while thirteen that do went unlisted.
 * A stale entry costs nothing, since a call to a name that does not exist fails earlier; **the
 * missing ones were the bug**, and `union(s1, s2)`, `keys(m)`, `get(m, k)` and ten more were
 * accepted in statement position with their whole output thrown away, which is the mistake this
 * list exists to catch. `tools/check_builtins.py` now holds every entry to being a name that can
 * actually reach the gate, so half a rename cannot pass again.
 *
 * `is_Some` and `is_None` were also removed, for a different reason: `check_call` rewrites them
 * into `Option.is_Some(o)` enum accesses, so the statement is no longer an `EXPR_CALL` by the time
 * the gate looks and the entries could never match. Discarding one is still accepted -- a real if
 * narrow hole, recorded in the F212 entry rather than left as a list that looks like it covers it.
 *
 * **A name here, not an overload.** Purity is per overload, so the `t.kind != TY_VOID` test beside
 * this one is what makes a list of names safe: `filter` is here, and its three builder members
 * return nothing, so only the Map/Set/array members can trip it. Where a builder member is
 * effectful *and* answers something -- `add`, `remove_all`, `remove` -- the declaration says so
 * with `#[effectful]` and `call_is_effectful_overload` vetoes the name. Those three could not be
 * listed at all until that attribute existed (F208). */
static const char *const PURE_BUILTINS[] = {
    "set", "mutable",
    "str", "format", "hash", "compare", "error_message",
    "is_ok",
    "union", "intersect", "difference", "is_subset", "add",
    "$mutable_array_from", "$mutable_array_lit", "$mutable_map_from", "$mutable_set_from",
    NULL};

/* The callee as the author wrote it: `rewritten_from` when the checker has replaced `callee` with
 * an internal name it dispatches on, and `callee` otherwise. */
static const char *authored_callee(const Expr *e) {
    return e->as.call.rewritten_from != NULL ? e->as.call.rewritten_from : e->as.call.callee;
}

static int overload_set_size(Module *module, const char *name);

/* The `concat` declaration a call resolved to, found by what parameter one is: `string` for the
 * pieces form, `[T]` for the parts form, a struct (a MutableArray) for the in-place form, which
 * has two parameters. `concat`'s arm hands the call to it on `call_fn`, which is what makes the
 * name four `native fn` declarations with no builtin id behind them even though **the resolver
 * could never have chosen between them**: two of the four differ only in what they return, both
 * accept no arguments at all, and the in-place one takes a variadic tail that may mix worlds. The
 * arm keeps the decision and gives up the dispatch, which is the only part that had to be a
 * builtin. */
static void *concat_member(Module *module, TypeKind first_kind, int nparams) {
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *f = module->functions.items[i];
        if (strcmp(f->name, "concat") != 0 || f->params.count == 0) continue;
        if (nparams != 0 && f->params.count != nparams) continue;
        if (nparams == 0 && f->params.count != 1) continue;
        if (f->params.items[0].type.kind == first_kind) return f;
    }
    return NULL;
}

/* Purity, read off the declaration instead of a list -- stage 5 of
 * docs/design/full-overloading.md, for the part of the list that no longer needs to exist.
 *
 * A call that resolved to a member of a **resolver-owned overload set in the prelude** says
 * whether it has effects by its return type, and this is structural rather than lucky: the two
 * worlds answer the same question differently, so the `[T]` member computes a new value and the
 * `MutableArray<T>` member rearranges a buffer *and has nothing to return*. Every one of the 28
 * sets is that shape -- `sort(b)`, `reverse(b)`, `filter(b, p)` are void and discarding one is
 * the normal call; `distinct(b)`, `map(b, f)`, `count(b, p)` answer values and discarding one is
 * always a mistake.
 *
 * Before F182 those 28 names also had to appear in PURE_BUILTINS, which is a list of *names* --
 * and purity is per overload, so the list could only ever be right with `t.kind != TY_VOID`
 * standing beside it. They are gone from the list now: a 29th set needs no edit, and the list
 * cannot go stale about them.
 *
 * **What this does not cover, deliberately.** The 22 polymorphic builtins keep the list, because
 * for them the name *is* the implementation (F181) and there is no declaration to ask. And a
 * mutating overload that returns a value defeats the rule, which is what `#[effectful]` below is
 * for: `remove` on a builder is three such overloads. */
/* The case F182 said would break the return-type rule. `remove` on a MutableMap answers the
 * `Option<V>` it took out, on a MutableSet whether the item was there, and on a MutableArray the
 * element it removed -- all three **effectful and non-void**, so "returns something" stops meaning
 * "has no effect" for them. Discarding one is the ordinary call: it is how `pop` and "delete if
 * present" are spelled.
 *
 * This was a list of names and a pair of type names until F208, which is what the effort these
 * conversions belong to has been removing everywhere else. F182 called for a marker on the
 * declaration if a second case appeared, and a third if a third did. The third arrived when
 * `remove` on a MutableArray stopped being a callee rewrite to `mutable_array_remove` -- a name
 * the gate did not recognise, which is the only reason the pop form was accepted -- and became a
 * declaration like the other two. So the fact now lives on the declaration that has it. */
static int effectful_despite_answering(const FunctionDecl *fn) {
    return fn->is_effectful;
}

/* A veto, not an abstention: `remove` is in PURE_BUILTINS by name, so the builder members have
 * to override it rather than merely decline to vouch for it. */
static int call_is_effectful_overload(const Expr *e) {
    const FunctionDecl *fn = (const FunctionDecl *)e->call_fn;
    return fn != NULL && effectful_despite_answering(fn);
}

static int call_is_pure_prelude_overload(Module *module, const Expr *e) {
    const FunctionDecl *fn = (const FunctionDecl *)e->call_fn;
    if (fn == NULL || fn->is_builtin || fn->source_file != NULL) return 0;
    if (effectful_despite_answering(fn)) return 0;
    return overload_set_size(module, fn->name) > 1;
}

static int is_pure_builtin_name(const char *name) {
    for (size_t i = 0; PURE_BUILTINS[i] != NULL; i++) {
        if (strcmp(name, PURE_BUILTINS[i]) == 0) return 1;
    }
    return 0;
}

/* `sort` left this table in F178, the first pair converted to a real overload set. A name here
 * is one the interpreter dispatches by id, which is also what makes it unable to take a named
 * argument or a type argument; `sort` is now two ordinary declarations, so it takes both, and
 * `tools/check_builtins.py` is what noticed the entry had been left behind. Every later
 * conversion removes its two entries the same way -- the name appears once per world. */
static const char *const BUILTINS_NO_NAMED_ARGS[] = {
    "print", "printf", "str", "format", "hash", "compare", "error_message",
    "set", "is_ok", "unwrap", "unwrap_err", "is_Some", "is_None", "unwrap_Some", "$is_variant", "$unwrap_variant",
    "ok", "err", "channel_new", "send", "receive", "try_send", "try_receive", "channel_len", "channel_close", "receive_option", "receive_timeout", "select2", "$map_lit", "$set_lit", "union", "intersect", "difference", "is_subset", "add", "mutable", "$mutable_array_from", "$mutable_array_lit", "$mutable_map_from", "$mutable_set_from", NULL,
};

/* The names a dispatched builtin may still be called with named arguments despite appearing in
 * the registry above.
 *
 * `BUILTINS_NO_NAMED_ARGS` answers two questions, and F188 found them coming apart. It is the
 * **registry** of names the interpreter dispatches -- `ziman_builtin_count` reads it, and so does
 * the check that a `builtin fn` declares something real -- and it is also the list of names that
 * refuse a named argument. Those agreed while every builtin was handled by a hand-written arm,
 * because an arm reads its arguments positionally and would have ignored the names outright.
 *
 * A converted builtin is different: its call reaches `ordinary_call`, where
 * `canonicalize_named_args` reorders against the declaration before anything looks at an
 * argument. So it can take them, and refusing is now a restriction with no reason behind it --
 * `len(a: xs)` was rejected for no better cause than that `len` had once had an arm.
 *
 * Rather than duplicate a hundred-odd strings into a second table that would drift from the
 * first, the exception is listed: a name here is dispatched *and* declaration-checked. It grows
 * by one each time an arm is deleted, which is exactly when the restriction stops applying. A
 * name with no declaration at all (`format`, `printf`, the parser's `$` names) can never appear
 * here -- there is nothing to canonicalize against. */
static const char *const BUILTINS_WITH_NAMED_ARGS[] = {
    "set",
    "union", "intersect", "difference", "is_subset", "add", NULL,
};

static int builtin_refuses_named_args(const char *name) {
    for (int i = 0; BUILTINS_WITH_NAMED_ARGS[i] != NULL; i++) {
        if (strcmp(name, BUILTINS_WITH_NAMED_ARGS[i]) == 0) return 0;
    }
    for (int i = 0; BUILTINS_NO_NAMED_ARGS[i] != NULL; i++) {
        if (strcmp(name, BUILTINS_NO_NAMED_ARGS[i]) == 0) return 1;
    }
    return 0;
}

/* The builtins, for `ziman --api builtins`. Exposed because the 2026-10-10 benchmark wave found the
 * hole this list fills: `--api` listed the bundled *modules*, so a run looking for a string's
 * characters never saw `chars` and used `split(word, "")`, which type-checks and then fails at
 * runtime; another concluded "there is no `keys` function" when `map_keys` is a builtin. A command
 * that answers "what exists" has to include the things that need no `use`
 * (docs/reviews/llm-benchmark-2026-10-09.md, wave three). */
int ziman_builtin_count(void) {
    /* Minus one for the NULL terminator. `BUILTINS_NO_NAMED_ARGS` is NULL-terminated for
     * `is_builtin_no_named_args`'s walk, and counting the sentinel made `--api builtins` print a
     * line reading `(null)` -- harmless through `printf("%s")`, which is why it survived, and a
     * segfault the moment anything did real work with the name. */
    return (int)(sizeof(BUILTINS_NO_NAMED_ARGS) / sizeof(BUILTINS_NO_NAMED_ARGS[0])) - 1;
}

const char *ziman_builtin_name(int i) {
    if (i < 0 || i >= ziman_builtin_count()) return NULL;
    return BUILTINS_NO_NAMED_ARGS[i];
}

/* Why a builtin has no `builtin fn` declaration in lib/prelude/, for the ones that can never have
 * one. Every other dispatched name is declared there with a real signature (F148); these are the
 * exceptions, and `--api builtins` prints the reason beside the name rather than listing it bare
 * under a sentence that has to cover all of them at once.
 *
 * Two kinds. A rule that reads more than types -- `format` and `printf` check their template
 * against the argument list, `error_message` looks a `<type>_message` function up -- so no
 * signature states it. And a name the parser
 * *emits*, which a program can never type: `Map<K, V> { ... }` becomes a `$map_lit` call and
 * `if Some(x) = v` becomes `$is_variant`.
 *
 * **This table is the single source.** `tools/check_builtins.py` reads it out of this file rather
 * than keeping its own copy, the way tools/check_not_yet.py already reads the dispatch table --
 * its header puts the principle well: "a new builtin should not need an edit in this file to be
 * known about". So a new undeclarable builtin is one line here, and the gate fails until it is
 * written. */
static const struct { const char *name; const char *reason; } BUILTIN_NO_SIGNATURE[] = {
    {"printf", "checks its template against the argument list, as 'format' does"},
    {"format", "checks its template against the argument list"},
    {"error_message", "accepts any type with a '<type>_message' function, which is a lookup rather than a type"},
    {"$mutable_array_from", "not callable: the checker emits it for 'mutable' over an array"},
    {"$mutable_array_lit", "not callable: the checker emits it for 'mutable' over an array literal"},
    {"$mutable_map_from", "not callable: the checker emits it for 'mutable' over a Map"},
    {"$mutable_set_from", "not callable: the checker emits it for 'mutable' over a Set"},
    {"$map_lit", "not callable: the parser emits it for a map literal"},
    {"$set_lit", "not callable: the parser emits it for a set literal"},
    {"$is_variant", "not callable: the parser emits it for 'if V(x) = e'"},
    {"$unwrap_variant", "not callable: the parser emits it for 'if V(x) = e'"},
};

const char *ziman_builtin_no_signature_reason(const char *name) {
    for (size_t i = 0; i < sizeof BUILTIN_NO_SIGNATURE / sizeof BUILTIN_NO_SIGNATURE[0]; i++) {
        if (strcmp(name, BUILTIN_NO_SIGNATURE[i].name) == 0) return BUILTIN_NO_SIGNATURE[i].reason;
    }
    return NULL;
}

/* "did you mean" support (LLM-generation benchmark, docs/reviews/llm-benchmark-2026-09-28.md:
 * an "undefined variable"/"undefined function" with no suggestion was the one diagnostic class
 * that never let a model self-correct from zero context, where a habit with a named fix -- a
 * semicolon, a method call, `match` -- did, in 1-3 tries every time). Standard Levenshtein edit
 * distance (inserts, deletes, substitutions), same algorithm and the same "strictly less than 3"
 * cutoff as `properties.zm`'s own `check_unknown`/`suggestion`, so the two "did you mean"
 * mechanisms in the language agree. */
int ziman_name_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int *prev = ziman_xmalloc((lb + 1) * sizeof(int));
    int *cur = ziman_xmalloc((lb + 1) * sizeof(int));
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            cur[j] = m < sub ? m : sub;
        }
        int *tmp = prev;
        prev = cur;
        cur = tmp;
    }
    int result = prev[lb];
    free(prev);
    free(cur);
    return result;
}

/* The built-in type a reader probably meant when they wrote `name` and no such struct or enum
 * exists, or NULL. Added after the 2026-10-09 benchmark pilot, where this was the single largest
 * consumer of the fix budget: five of ten runs spent between two edits and fifteen throwaway
 * compiles brute-forcing the primitives, and one task failed for this reason alone, one edit short
 * of compiling (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28).
 *
 * Two mechanisms, because one does not cover it. A case-insensitive hit catches the capitalised
 * spellings every C-family and ML-family language uses (`Int`, `String`, `Bool`, `Float`), which is
 * most of what was observed. The alias table catches names too far away for edit distance to
 * reach: `Str` to `string` is three edits, `I64` to `int` is also three, and the cutoff elsewhere
 * in this file is strictly less than three. The table is foreign spellings only -- no Ziman name is
 * in its left column -- so it can never suggest something that would then fail for another
 * reason. */
static const char *const PRIMITIVE_TYPES[] = {"int", "float", "bool", "string", "byte", "bigint", "decimal", NULL};
static const struct { const char *foreign, *ziman; } TYPE_ALIASES[] = {
    {"str", "string"},    {"text", "string"},  {"char", "string"},   {"i8", "int"},
    {"i16", "int"},       {"i32", "int"},      {"i64", "int"},       {"u8", "int"},
    {"u16", "int"},       {"u32", "int"},      {"u64", "int"},       {"uint", "int"},
    {"usize", "int"},     {"isize", "int"},    {"long", "int"},      {"short", "int"},
    {"integer", "int"},   {"num", "int"},      {"number", "int"},    {"nat", "int"},
    {"double", "float"},  {"f32", "float"},    {"f64", "float"},     {"real", "float"},
    {"boolean", "bool"},  {"bigdecimal", "decimal"}, {"bignum", "bigint"}, {"biginteger", "bigint"},
    {NULL, NULL},
};

static int ascii_eq_ignore_case(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* The old name of one of the four builders. Anyone with existing code writes it, and so does any
 * model trained on the language before the rename, where the generic "unknown struct or enum type"
 * has nothing at all to offer them. Matched case-insensitively like the other aliases, so
 * `arraybuilder` lands too. The message is also the one place a reader meets the new name for the
 * first time, so it is where the thing a `Mutable*` name does not say belongs. */
/* The old name of a builder function. Half of them became a bare builtin rather than a prefixed
 * name, which no edit-distance suggester could ever find -- `ab_to_array` to `snapshot` is not a
 * near miss -- so the mapping is spelled out. Exact-match only: these are function names, where a
 * case-insensitive guess would be reaching. */
static const char *renamed_builder_fn(const char *name) {
    static const struct {
        const char *was;
        const char *now;
    } RENAMED[] = {/* The cell's two, 2026-10-22: `get` and `set` take a `Mutable<T>` now, joining
                    * the sets that already had those names for a map and an array. The prefix was
                    * the last of its kind on a *reachable* name -- `mutable_array_new` and
                    * `mutable_array_filled` keep theirs, being constructors with no bare form. */
                   {"mutable_get", "get"},
                   {"mutable_set", "set"},
                   {"ab_new", "mutable_array_new"},
                   {"ab_append", "push"},
                   {"ab_to_array", "snapshot"},
                   {"ab_len", "len"},
                   {"ab_clear", "clear"},
                   {"sb_new", "mutable_string_new"},
                   {"sb_append", "append"},
                   {"sb_append_byte", "append_byte"},
                   {"sb_to_string", "snapshot"},
                   {"sb_len", "len"},
                   {"mapb_new", "mutable_map_new"},
                   {"mapb_set", "set"},
                   {"mapb_get", "get"},
                   {"mapb_get_or", "get_or"},
                   {"mapb_remove", "remove"},
                   {"mapb_contains", "contains"},
                   {"mapb_len", "len"},
                   {"mapb_to_map", "snapshot"},
                   {"setb_new", "mutable_set_new"},
                   {"setb_add", "add"},
                   {"setb_remove", "remove"},
                   {"setb_contains", "contains"},
                   {"setb_len", "len"},
                   {"setb_to_set", "snapshot"},
                   {"mutable_string_append", "append"},
                   {"mutable_string_append_byte", "append_byte"},
                   {"bytes_concat", "concat"},
                   {"bytes_index_of", "index_of"},
                   {"mutable_map_set_all", "merge"},
                   {"mutable_set_union", "union"},
                   {"mutable_set_intersect", "intersect"},
                   {"mutable_set_difference", "difference"},
                   {"mutable_set_is_subset", "is_subset"},
                   {"mutable_array_push", "push"},
                   {"mutable_array_insert", "insert"},
                   {"mutable_array_remove", "remove"},
                   /* The `Result`/`Option` world's three, 2026-10-22, and the first entries here
                    * that are neither a collection nor a builder. Same rule as every other: the
                    * prefix said which world argument one came from and the argument says it.
                    *
                    * The two conversions were added an hour after `map_err`, overruling a comment
                    * that had just argued they should keep their prefixes because those named a
                    * conversion's *source* rather than its world. `to_set` in array.zm refutes
                    * that and predates all of this: prefix-free, named for its target, source read
                    * from argument one. */
                   {"result_map_err", "map_err"},
                   {"result_to_option", "to_option"},
                   {"option_to_result", "to_result"},
                   {NULL, NULL}};
    for (size_t k = 0; RENAMED[k].was != NULL; k++) {
        if (strcmp(name, RENAMED[k].was) == 0) return RENAMED[k].now;
    }
    return NULL;
}

/* The old names whose answer is **syntax**, so they cannot live in the table above: that one is
 * held by `tools/check_builtins.py` to naming a declaration that exists, which is the whole point
 * of it, and `b[i]` is not a declaration. Keeping them apart is what lets that guard stay strict.
 * A builder's element access was a call until F209 -- `mutable_array_get(b, i)` and
 * `mutable_array_set(b, i, v)`, which `b[i]` rewrote onto -- and `ab_get`/`ab_set` were the
 * generation of names before those. Both generations land here now. */
static int renamed_to_syntax(const char *name, const char **now, const char **why) {
    /* The element accesses share a sentence; the entries each carry their own so that a fifth
     * does not have to fit someone else's. */
    static const char *const ELEMENT =
        "element access is an index rather than a function, so there is no name left to offer";
    static const struct {
        const char *was;
        const char *now;
        const char *why;
    } RENAMED[] = {{"ab_get", "b[i]", ELEMENT},
                   {"ab_set", "b[i] = value", ELEMENT},
                   {"mutable_array_get", "b[i]", ELEMENT},
                   {"mutable_array_set", "b[i] = value", ELEMENT},
                   {"map_from_arrays", "Map<K, V> { key: value }",
                    "a map literal names each key beside its value, where this paired two positional "
                    "lists whose lengths could only be checked when the call ran; build one from two "
                    "arrays computed at run time with a MutableMap and 'set' in a loop"},
                   {NULL, NULL, NULL}};
    for (size_t k = 0; RENAMED[k].was != NULL; k++) {
        if (strcmp(name, RENAMED[k].was) == 0) {
            *now = RENAMED[k].now;
            *why = RENAMED[k].why;
            return 1;
        }
    }
    return 0;
}

static const char *renamed_builder_type(const char *name) {
    static const struct {
        const char *was;
        const char *now;
    } RENAMED[] = {{"ArrayBuilder", "MutableArray"},
                   {"StringBuilder", "MutableString"},
                   {"MapBuilder", "MutableMap"},
                   {"SetBuilder", "MutableSet"},
                   {NULL, NULL}};
    for (size_t k = 0; RENAMED[k].was != NULL; k++) {
        if (ascii_eq_ignore_case(name, RENAMED[k].was)) return RENAMED[k].now;
    }
    return NULL;
}

static const char *suggest_primitive_type(const char *name) {
    for (size_t k = 0; PRIMITIVE_TYPES[k] != NULL; k++) {
        if (ascii_eq_ignore_case(name, PRIMITIVE_TYPES[k])) return PRIMITIVE_TYPES[k];
    }
    for (size_t k = 0; TYPE_ALIASES[k].foreign != NULL; k++) {
        if (ascii_eq_ignore_case(name, TYPE_ALIASES[k].foreign)) return TYPE_ALIASES[k].ziman;
    }
    return NULL;
}

/* The closest candidate to `name` among the module's own function/struct/enum names and the
 * builtins that can be called bare, or NULL for none close enough. Deliberately does not search
 * other modules' exports (including ones not `use`d) -- suggesting a name that still needs its
 * own `use` line first would trade one error for a more confusing one. */
static const char *suggest_function_name(Module *module, const char *name) {
    const char *best = NULL;
    int best_distance = 3;
    for (size_t k = 0; BUILTINS_NO_NAMED_ARGS[k] != NULL; k++) {
        const char *cand = BUILTINS_NO_NAMED_ARGS[k];
        if (cand[0] == '$' || strcmp(cand, name) == 0) continue;
        int d = ziman_name_distance(name, cand);
        if (d < best_distance) {
            best = cand;
            best_distance = d;
        }
    }
    for (int i = 0; i < module->functions.count; i++) {
        const char *cand = module->functions.items[i]->name;
        if (strcmp(cand, name) == 0) continue;
        int d = ziman_name_distance(name, cand);
        if (d < best_distance) {
            best = cand;
            best_distance = d;
        }
    }
    for (int i = 0; i < module->structs.count; i++) {
        const char *cand = module->structs.items[i]->name;
        int d = ziman_name_distance(name, cand);
        if (d < best_distance) {
            best = cand;
            best_distance = d;
        }
    }
    for (int i = 0; i < module->enums.count; i++) {
        const char *cand = module->enums.items[i]->name;
        int d = ziman_name_distance(name, cand);
        if (d < best_distance) {
            best = cand;
            best_distance = d;
        }
    }
    return best;
}

/* Specific confusions the benchmark actually hit, named directly rather than left to a fuzzy
 * search: a foreign declaration keyword parsed as a bare identifier (task 04, and the majority
 * of round 1's failures), a hardcoded generic type called like a function because its name reads
 * like one once case is ignored (task 08's `map(...)` for `Map<K, V> { ... }`), and -- found on
 * re-validating that same task's fix -- a membership-test method name from another language
 * (`.has`/`has_key`/...), which has no Ziman equivalent at all: the real answer is `contains`. */
static const char *const HARDCODED_GENERIC_TYPES[] = {"Map", "Set", "Option", "Result", "Channel", NULL};
static const char *const MAP_MEMBERSHIP_SYNONYMS[] = {
    "has", "has_key", "hasKey", "contains_key", "containsKey", "has_value", "hasValue", "exists", NULL,
};

static const char *matching_generic_type(const char *name) {
    for (size_t k = 0; HARDCODED_GENERIC_TYPES[k] != NULL; k++) {
        const char *cand = HARDCODED_GENERIC_TYPES[k];
        if (strcmp(cand, name) != 0 && strcasecmp(cand, name) == 0) return cand;
    }
    return NULL;
}

/* How each of HARDCODED_GENERIC_TYPES is *actually* constructed. These five do not share one
 * shape, and the single sentence this used to print ("construct one with 'X<...> { ... }', e.g.
 * 'Map<string, int> { \"key\": value }'") was right for Map and Set and a dead end for the other
 * three: `Option<int> { }` is "no struct named 'Option'", `Result<int, string> { }` and
 * `Channel<int> { }` are syntax errors. A model that wrote `channel(...)`, followed this, and
 * wrote `Channel<int> { }` got a *second* failure for its trouble -- the opposite of the stated
 * goal that a habit should fail with the sentence that fixes it, since the sentence was wrong.
 * Note Result's constructors are the lowercase functions `ok`/`err`; `Ok`/`Err` are the names of
 * its variants in a `switch` arm, which is a different position. (tasks.md F47) */
static const char *generic_type_construction(const char *canonical) {
    if (strcmp(canonical, "Map") == 0) return "write one as 'Map<string, int> { \"key\": value }', or build one with 'mutable_map_new<string, int>()'";
    if (strcmp(canonical, "Set") == 0) return "write one as 'Set<int> { 1, 2 }'";
    if (strcmp(canonical, "Option") == 0) return "construct one as 'Option<int>.Some(4)' or 'Option<int>.None'";
    if (strcmp(canonical, "Result") == 0) return "construct one with the functions 'ok(value)' and 'err(problem)' ('Ok(v)'/'Err(e)' are its variant names in a 'switch' arm, not constructors)";
    if (strcmp(canonical, "Channel") == 0) return "make one with 'channel_new()', which takes its element type from the annotation, as in 'val ch: Channel<int> = channel_new()'";
    return NULL;
}

/* A named call (`f(y: 2, x: 1)`) is reordered here, once, into the same
 * positional order the rest of check_call (and, downstream, the
 * interpreter) already expects -- so nothing past this point ever needs
 * to know a call was written with names at all. Requires the given
 * names to be exactly `fn`'s declared parameter names, each appearing
 * at most once (a duplicate is always rejected, even for an Option<T>
 * parameter -- there's no reading of `f(x: 1, x: 2)` that isn't a
 * mistake). An Option<T> parameter may be left out entirely (never a
 * non-Option one, which has no "absent" state to fall back to) -- a
 * missing one is filled in with a synthetic reference to its own
 * default (make_default_ref_expr) when its declaration gave one, or a
 * synthetic `None` (make_none_expr) when it didn't, so this function's
 * one remaining job -- filling `e->as.call.args` to exactly
 * `fn->params.count`, in order -- always finishes the same way whether
 * or not any parameter was actually omitted.
 *
 * `f()` -- literally empty parentheses -- is also routed through here,
 * even though the parser calls that a positional call (`arg_names ==
 * NULL`): with zero arguments given, there's no `name:` for the parser
 * to have seen either way, so a completely empty call is genuinely
 * ambiguous between "positional, zero given" and "named, zero given,
 * everything omitted" from its syntax alone -- and unlike a *partial*
 * positional call (`f(1)` when `f` needs two, which must stay exactly
 * as strict as ever), there's nothing here that omission could get
 * wrong. Treating it as the latter is what makes `mutable_string_new()` -- a
 * function whose every parameter is Option<T> -- callable at all; it's
 * a no-op for a function with zero parameters (the fill loop below
 * never runs) and still correctly rejects any non-Option parameter
 * (the same "missing required argument" error a partially-omitted named
 * call already gives). */
static void canonicalize_named_args(Expr *e, FunctionDecl *fn) {
    char **names = e->as.call.arg_names;
    if (names == NULL && (e->as.call.args.count > 0 || fn->params.count == 0)) {
        return;
    }
    int is_variadic = fn->params.count > 0 && fn->params.items[fn->params.count - 1].is_variadic;
    int fixed_count = is_variadic ? fn->params.count - 1 : fn->params.count;
    int argc = e->as.call.args.count;
    Expr **reordered = ziman_xmalloc((size_t)fn->params.count * sizeof(Expr *));
    for (int i = 0; i < fn->params.count; i++) {
        reordered[i] = NULL;
    }
    for (int i = 0; i < argc; i++) {
        int param_index = -1;
        for (int j = 0; j < fn->params.count; j++) {
            if (strcmp(names[i], fn->params.items[j].name) == 0) {
                param_index = j;
                break;
            }
        }
        if (param_index == -1) {
            ziman_error("type", e->line, e->col,
                        "function '%s' has no parameter named '%s'",
                        e->as.call.callee, names[i]);
        }
        if (reordered[param_index] != NULL) {
            ziman_error("type", e->line, e->col,
                        "argument '%s' given more than once in this call to '%s'",
                        names[i], e->as.call.callee);
        }
        reordered[param_index] = e->as.call.args.items[i];
    }
    /* A trailing variadic parameter left unfilled (never named, and -- the `f()` case above --
     * no positional arguments at all) means zero elements, not a missing argument: unlike every
     * other parameter here, it has a perfectly good "nothing given" reading that doesn't need an
     * Option<T> to express (see check_call/collapse_variadic_args, both already fine with a
     * zero-length tail). Left NULL and excluded from the final count below, rather than filled
     * with anything, so `sum()`/`concat()`/`join()` with nothing to add up/join reach the
     * ordinary variadic-tail handling exactly as if the trailing argument had never existed at
     * all -- not a new code path, just this function no longer manufacturing a fake error for
     * one that was never required. */
    /* ... unless it is `#[at_least_one]`, where "nothing given" is exactly the reading being
     * refused, so the parameter falls through to the missing-required-argument check below. */
    int variadic_omitted = is_variadic && !fn->variadic_at_least_one && reordered[fixed_count] == NULL;
    for (int i = 0; i < fn->params.count; i++) {
        if (reordered[i] != NULL || (is_variadic && i == fixed_count && !fn->variadic_at_least_one)) {
            continue;
        }
        Param *param = &fn->params.items[i];
        if (is_variadic && i == fixed_count) {
            /* The `#[at_least_one]` parameter, reached only because it was left unfilled. Worth
             * its own message rather than "missing required argument": the rule is unusual, and a
             * caller who genuinely has nothing to pass needs to be told the empty-array spread is
             * how to say so. Same sentence as check_call's, so one rule reads one way. */
            ziman_error("type", e->line, e->col,
                        "function '%s' needs at least one value for its variadic parameter '%s', since the "
                        "call would otherwise do nothing (pass an array to spread a list that may be empty)",
                        e->as.call.callee, param->name);
        }
        if (!is_option_type(param->type)) {
            ziman_error("type", e->line, e->col,
                        "function '%s' is missing required argument '%s'",
                        e->as.call.callee, param->name);
        }
        reordered[i] = param->default_expr != NULL
                           ? make_default_ref_expr(param, e->line, e->col)
                           : make_none_expr(param->type, e->line, e->col);
    }
    e->as.call.args.items = reordered;
    e->as.call.args.count = variadic_omitted ? fixed_count : fn->params.count;
    e->as.call.args.capacity = fn->params.count;
    e->as.call.arg_names = NULL;
}

/* `Map<K, V> { k: v, ... }` and `Set<T> { a, ... }`, parsed into calls to `$map_lit`/`$set_lit`
 * with the type arguments explicit, so the literal never needs inference. */
static Type check_collection_literal(Module *module, Env *env, Expr *e, int is_map) {
    TypeList *targs = &e->as.call.type_args;
    for (int i = 0; i < targs->count; i++) check_type_well_formed(module, targs->items[i], e->line, e->col);
    Type key_type = targs->items[0];
    for (int i = 0; i < e->as.call.args.count; i++) {
        int is_value = is_map && (i % 2 == 1);
        Type want = is_value ? targs->items[1] : key_type;
        Type got = check_expr_expecting(module, env, &e->as.call.args.items[i], want);
        require_not_void(e->as.call.args.items[i], is_value ? "as a map value" : "as a collection element");
        if (!type_equals(got, want)) {
            Expr *bad = e->as.call.args.items[i];
            ziman_error("type", bad->line, bad->col, "%s of this %s literal must be %s, found %s", is_value ? "a value" : (is_map ? "a key" : "an element"),
                        is_map ? "map" : "set", type_name(want), type_name(got));
        }
    }
    Type result = is_map ? type_map(key_type, targs->items[1]) : type_set(key_type);
    check_type_well_formed(module, result, e->line, e->col);
    return result;
}

/* The Map/Set helper builtins (map_get_or, map_merge, set_union, ...): each takes its
 * types from its arguments, so none is generic-with-explicit-type-arguments. Returns 1
 * and sets *out when `name` is one of them. */
static Type helper_arg(Module *module, Env *env, Expr *e, int i, Type want, const char *name) {
    Type got = check_expr_expecting(module, env, &e->as.call.args.items[i], want);
    require_not_void(e->as.call.args.items[i], "as an argument");
    if (!type_equals(got, want)) {
        Expr *bad = e->as.call.args.items[i];
        ziman_error("type", bad->line, bad->col, "argument %d of '%s' expects %s, found %s", i + 1, name, type_name(want), type_name(got));
    }
    return got;
}

static Type helper_first(Module *module, Env *env, Expr *e, int argc, const char *name, TypeKind kind) {
    if (e->as.call.args.count != argc) {
        ziman_error("type", e->line, e->col, "'%s' takes exactly %d arguments, found %d", name, argc, e->as.call.args.count);
    }
    Type first = check_expr(module, env, e->as.call.args.items[0]);
    if (first.kind != kind) {
        ziman_error("type", e->as.call.args.items[0]->line, e->as.call.args.items[0]->col, "argument 1 of '%s' expects a %s, found %s", name,
                    kind == TY_MAP ? "Map" : "Set", type_name(first));
    }
    return first;
}

/* ------------------------------------------------------------------ overload resolution, stage 1
 *
 * `docs/design/full-overloading.md`, stage 1: the resolver, built and run *beside* the
 * hand-written arms rather than in place of them, so that the corpus says what it should do
 * before anything depends on it. It decides nothing yet -- `overload_shadow_check` compares its
 * answer with the arm's and reports a disagreement; `check_collection_helper` goes on to do
 * exactly what it did before either way.
 *
 * Silent unless ZIMAN_OVERLOAD_SHADOW is set in the environment, so an ordinary build, an
 * ordinary `make check` and the LSP are untouched by a half-built feature.
 *
 * The design's central claim is that resolution is a *filter*, not a search: it reads the
 * arguments that can be typed on their own and the syntactic *shape* of the ones that cannot,
 * and never type-checks an argument speculatively. That is what makes it safe to run here at
 * all -- a search would have to undo the AST rewrites `check_expr` performs (24 sites), and a
 * shadow pass that mutated the tree would change the behaviour it is supposed to be observing. */

#define OVL_MAX_CANDIDATES 32

typedef enum { OVL_NO_MATCH, OVL_RESOLVED, OVL_AMBIGUOUS, OVL_UNSUPPORTED } OverloadOutcome;

typedef struct {
    OverloadOutcome outcome;
    FunctionDecl *chosen;
    FunctionDecl *live[OVL_MAX_CANDIDATES];
    int live_count;
    int matched[OVL_MAX_CANDIDATES]; /* positions each candidate survived, for the ranked list in §5 */
    int eliminated_at;               /* 1-based position that emptied the set; 0 if it never filled */
    Type eliminated_actual;          /* the argument's own type there, for the message */
} OverloadResolution;

/* Whether `t` is one of `fn`'s own type parameters -- a wildcard standing for anything, rather
 * than a concrete name that happens to be capitalised. */
static int type_is_own_param(Type t, const TypeParamList *tps) {
    if (t.kind != TY_TYPEVAR) return 0;
    for (int i = 0; i < tps->count; i++) {
        if (strcmp(tps->items[i].name, t.type_var_name) == 0) return 1;
    }
    return 0;
}

/* Whether `actual` can be the argument for a parameter declared `pattern`, where a type variable
 * of the candidate's own `tps` is a wildcard that must stay consistent across its occurrences.
 *
 * Deliberately *not* `unify_type`: that one is a binder, and on a kind mismatch it returns
 * silently rather than reporting failure, which is exactly right for inference and useless as a
 * filter. It is also not `type_equals`, which has no notion of a type variable standing for
 * something. This is the one piece of genuinely new logic in stage 1, and it mirrors
 * `type_equals`'s structure case for case so that the two cannot drift apart. */
static int param_accepts(Type pattern, Type actual, const TypeParamList *tps, Type *binds, int *bound) {
    ZIMAN_STACK_CHECK();
    if (pattern.kind == TY_TYPEVAR) {
        for (int i = 0; i < tps->count; i++) {
            if (strcmp(tps->items[i].name, pattern.type_var_name) != 0) continue;
            if (bound[i]) return type_equals(binds[i], actual);
            binds[i] = actual;
            bound[i] = 1;
            return 1;
        }
        /* A type variable that is not the candidate's own is a concrete name here. */
        return actual.kind == TY_TYPEVAR && strcmp(pattern.type_var_name, actual.type_var_name) == 0;
    }
    if (pattern.kind != actual.kind) return 0;
    switch (pattern.kind) {
        case TY_ARRAY:
        case TY_CHANNEL:
        case TY_SET:
            return param_accepts(*pattern.elem, *actual.elem, tps, binds, bound);
        case TY_RESULT:
        case TY_MAP:
            return param_accepts(*pattern.elem, *actual.elem, tps, binds, bound)
                && param_accepts(*pattern.elem_err, *actual.elem_err, tps, binds, bound);
        case TY_STRUCT:
            if (strcmp(pattern.struct_name, actual.struct_name) != 0) return 0;
            if (pattern.type_args.count != actual.type_args.count) return 0;
            for (int i = 0; i < pattern.type_args.count; i++) {
                if (!param_accepts(pattern.type_args.items[i], actual.type_args.items[i], tps, binds, bound)) return 0;
            }
            return 1;
        case TY_ENUM:
            if (strcmp(pattern.enum_name, actual.enum_name) != 0) return 0;
            if (pattern.type_args.count != actual.type_args.count) return 0;
            for (int i = 0; i < pattern.type_args.count; i++) {
                if (!param_accepts(pattern.type_args.items[i], actual.type_args.items[i], tps, binds, bound)) return 0;
            }
            return 1;
        case TY_FUNC:
            if (pattern.type_args.count != actual.type_args.count) return 0;
            for (int i = 0; i < pattern.type_args.count; i++) {
                if (!param_accepts(pattern.type_args.items[i], actual.type_args.items[i], tps, binds, bound)) return 0;
            }
            return param_accepts(*pattern.elem, *actual.elem, tps, binds, bound);
        default:
            return 1;
    }
}

/* The parameter at `i`, counting a variadic tail as covering every position from its own index on. */
static int ovl_param_at(const FunctionDecl *fn, int i, Type *out) {
    if (fn->params.count == 0) return 0;
    int last = fn->params.count - 1;
    if (i < last) { *out = fn->params.items[i].type; return 1; }
    if (i == last) { *out = fn->params.items[last].type; return 1; }
    if (fn->params.items[last].is_variadic) { *out = fn->params.items[last].type; return 1; }
    return 0;
}

static int ovl_arity_fits(const FunctionDecl *fn, int argc) {
    int n = fn->params.count;
    int variadic = n > 0 && fn->params.items[n - 1].is_variadic;
    if (variadic) return argc >= n - 1;
    return argc == n;
}

/* The shape filter of §3: what a context-dependent argument says about its parameter without
 * being checked. Syntax only -- no call into check_expr, which is the whole point. */
static int ovl_shape_allows(const Expr *arg, Type pattern) {
    switch (arg->kind) {
        case EXPR_LAMBDA: {
            if (pattern.kind == TY_TYPEVAR) return 1;
            if (pattern.kind != TY_FUNC) return 0;
            const FunctionDecl *lam = (const FunctionDecl *)arg->lambda_decl;
            return lam == NULL || lam->params.count == pattern.type_args.count;
        }
        case EXPR_ARRAY_LIT:
            if (arg->as.array_lit.elements.count != 0) return 1; /* not context-dependent anyway */
            return pattern.kind == TY_ARRAY || pattern.kind == TY_TYPEVAR;
        case EXPR_CALL: {
            const char *c = arg->as.call.callee;
            if (strcmp(c, "ok") == 0 || strcmp(c, "err") == 0) {
                return pattern.kind == TY_RESULT || pattern.kind == TY_TYPEVAR;
            }
            if (strcmp(c, "Some") == 0) {
                return (pattern.kind == TY_ENUM && strcmp(pattern.enum_name, "Option") == 0)
                    || pattern.kind == TY_TYPEVAR;
            }
            return 1;
        }
        default:
            return 1;
    }
}

/* §4, steps 1-5. Never checks a context-dependent argument; checks every other one exactly once. */
static void overload_resolve(Module *module, Env *env, Expr *e, const char *name, OverloadResolution *r) {
    memset(r, 0, sizeof *r);
    r->outcome = OVL_NO_MATCH;

    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *f = module->functions.items[i];
        if (strcmp(f->name, name) != 0) continue;
        if (r->live_count >= OVL_MAX_CANDIDATES) { r->outcome = OVL_UNSUPPORTED; return; }
        r->live[r->live_count++] = f;
    }
    if (r->live_count == 0) { r->outcome = OVL_NO_MATCH; return; }

    int argc = e->as.call.args.count;

    /* step 2: arity */
    int keep = 0;
    for (int i = 0; i < r->live_count; i++) {
        if (ovl_arity_fits(r->live[i], argc)) r->live[keep++] = r->live[i];
    }
    if (keep == 0) { r->live_count = 0; r->eliminated_at = 0; r->outcome = OVL_NO_MATCH; return; }
    r->live_count = keep;

    /* steps 3 and 4, position by position */
    for (int pos = 0; pos < argc && r->live_count > 1; pos++) {
        Expr *arg = e->as.call.args.items[pos];
        int deferred = expr_needs_expected_type(arg);
        Type actual = type_scalar(TY_VOID);
        if (!deferred) actual = check_expr(module, env, arg);

        keep = 0;
        for (int i = 0; i < r->live_count; i++) {
            FunctionDecl *f = r->live[i];
            Type pattern;
            if (!ovl_param_at(f, pos, &pattern)) continue;
            int ok;
            if (deferred) {
                ok = ovl_shape_allows(arg, pattern);
            } else {
                Type binds[16]; int bound[16];
                int n = f->type_params.count > 16 ? 16 : f->type_params.count;
                memset(binds, 0, sizeof binds);
                memset(bound, 0, (size_t)n * sizeof bound[0]);
                ok = param_accepts(pattern, actual, &f->type_params, binds, bound);
                /* **The lone-array spread, which selection has to model because inference does.**
                 * One argument at the variadic position may be an array *of* the element type,
                 * standing for the whole tail -- the rule that makes `concat(parts)` and
                 * `push(b, values)` work -- and `infer_call_type_args` reads it that way. This
                 * filter did not, which went unnoticed while every variadic set had exactly one
                 * variadic member: the loop above runs only `while (live_count > 1)`, so a lone
                 * surviving candidate was never type-checked here and inference saw the argument
                 * first. Give a set two variadic members and the filter runs -- `merge(arrayOfMaps)`
                 * was refused as "expects Map<K, V> or a builder, found [Map<string, int>]" the
                 * moment `merge`'s in-place members became variadic (2026-10-22), for an argument
                 * the chosen declaration then accepted perfectly well. */
                if (!ok && pos == f->params.count - 1 && pos == argc - 1
                    && f->params.items[pos].is_variadic) {
                    memset(binds, 0, sizeof binds);
                    memset(bound, 0, (size_t)n * sizeof bound[0]);
                    ok = param_accepts(type_array(pattern), actual, &f->type_params, binds, bound);
                }
            }
            if (ok) { r->matched[keep] = r->matched[i] + 1; r->live[keep++] = f; }
        }
        if (keep == 0) {
            r->live_count = 0;
            r->eliminated_at = pos + 1;
            r->eliminated_actual = actual;
            r->outcome = OVL_NO_MATCH;
            return;
        }
        r->live_count = keep;
    }

    if (r->live_count == 1) { r->outcome = OVL_RESOLVED; r->chosen = r->live[0]; return; }
    r->outcome = OVL_AMBIGUOUS;
}

/* How many declarations share this name. One is the overwhelmingly common case and must stay
 * cheap -- this is the only cost an ordinary call to an ordinary function pays for the feature. */
/* How many declarations of `fn`'s name are visible from where `fn` itself came -- prelude
 * declarations are invisible to a program's own function of the same name, which shadows them. */
static int overload_set_size_visible(Module *module, const FunctionDecl *fn) {
    int n = 0;
    int fn_is_prelude = fn->source_file == NULL;
    for (int i = 0; i < module->functions.count; i++) {
        const FunctionDecl *other = module->functions.items[i];
        if (strcmp(other->name, fn->name) != 0) continue;
        if ((other->source_file == NULL) != fn_is_prelude) continue;
        n++;
    }
    return n;
}

static int overload_set_size(Module *module, const char *name) {
    int n = 0;
    for (int i = 0; i < module->functions.count; i++) {
        if (strcmp(module->functions.items[i]->name, name) == 0 && ++n > 1) return n;
    }
    return n;
}

/* The declaration a call resolves to, or an error that says why it could not be chosen. The
 * candidate list follows the rule set in §5: five or fewer in full, more than five ranked by how
 * far each got and truncated with the remainder counted. */
static void overload_report_candidates(const OverloadResolution *r, char *buf, size_t cap) {
    int order[OVL_MAX_CANDIDATES];
    for (int i = 0; i < r->live_count; i++) order[i] = i;
    for (int i = 1; i < r->live_count; i++) { /* stable insertion sort, most positions matched first */
        int k = order[i], j = i - 1;
        while (j >= 0 && r->matched[order[j]] < r->matched[k]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    int show = r->live_count <= 5 ? r->live_count : 3;
    size_t at = 0;
    for (int i = 0; i < show && at + 1 < cap; i++) {
        FunctionDecl *f = r->live[order[i]];
        /* The signature, not the name and arity. A reader choosing between two candidates needs
         * the parameter types -- `sort/1` twice over says nothing at all, which is what the first
         * draft of this printed and why the fixture it replaced had a better message than the
         * feature meant to improve it. */
        at += (size_t)snprintf(buf + at, cap - at, "\n  = %s(", f->name);
        for (int q = 0; q < f->params.count && at + 1 < cap; q++) {
            at += (size_t)snprintf(buf + at, cap - at, "%s%s%s: %s", q > 0 ? ", " : "",
                                   f->params.items[q].is_variadic ? "variadic " : "",
                                   f->params.items[q].name, type_name(f->params.items[q].type));
        }
        if (at + 1 < cap) at += (size_t)snprintf(buf + at, cap - at, ")");
        if (at + 1 < cap && f->return_type.kind != TY_VOID) {
            at += (size_t)snprintf(buf + at, cap - at, " -> %s", type_name(f->return_type));
        }
        if (at >= cap) return;
    }
    if (r->live_count > show && at + 1 < cap) {
        snprintf(buf + at, cap - at, "\n  = ... and %d more", r->live_count - show);
    }
}

static int is_builder_type(Type t);

/* "expects string, [T], Map<K, V>, a builder or Mutable<T>, found Point" -- built from the
 * declarations rather than written by hand.
 *
 * The hand-written arms this resolver is replacing carry sentences of exactly this shape, and
 * they were measured: `len`'s names a *category* ("an array, string, map, set, builder or
 * Mutable") where a bare list of nine signatures would show shapes. Deriving it keeps the
 * sentence and drops the hand-keeping -- and it is slightly better, since it names spellings a
 * caller can write instead of categories they have to translate.
 *
 * Only when the position is genuinely discriminating: every member must declare a *different*
 * type there, or "expects A, B or C" would list the same thing twice and say nothing. The four
 * builder types collapse to one phrase, the way the hand-written sentences already do.
 *
 * Answers 0 when it cannot build one, and the caller falls back to the signature list. */
static int overload_expected_types(const OverloadResolution *all, int pos, char *buf, size_t cap) {
    const char *seen[OVL_MAX_CANDIDATES];
    const char *builder_name = NULL;
    int n = 0, builders = 0;
    for (int i = 0; i < all->live_count; i++) {
        Type t;
        if (!ovl_param_at(all->live[i], pos - 1, &t)) return 0;
        if (is_builder_type(t)) {
            builders++;  /* one phrase for the four of them, appended last -- the hand-written
                          * sentences read "an array, string, map, set, builder or Mutable", with
                          * the category after the concrete types, and so should this one. Only
                          * when there are several, though: see the `builders > 1` below. */
            builder_name = type_name(t);
            continue;
        }
        const char *text = type_name(t);
        for (int q = 0; q < n; q++) {
            if (strcmp(seen[q], text) == 0) return 0; /* not discriminating here */
        }
        if (n >= OVL_MAX_CANDIDATES) return 0;
        seen[n++] = text;
    }
    if (builders > 0) {
        /* Collapsing is a summary, and a summary of the whole list is no help: a set whose
         * members are *only* builders would read "expects a builder", which restates the
         * question. `snapshot` and `clear` are exactly that, and their hand-written sentences
         * name all four ("a builder -- a MutableArray, MutableString, MutableMap or
         * MutableSet"). So the collapse applies only when something else is there to contrast
         * with, and otherwise the four are listed. */
        if (n == 0) {
            n = 0;
            for (int i = 0; i < all->live_count && n < OVL_MAX_CANDIDATES; i++) {
                Type t;
                if (!ovl_param_at(all->live[i], pos - 1, &t)) return 0;
                seen[n++] = type_name(t);
            }
        } else {
            if (n >= OVL_MAX_CANDIDATES) return 0;
            /* A category is a summary, and summarising one thing loses the thing. `push`,
             * `insert`, `slice`, `swap` and `to_set` each take exactly one builder world
             * alongside an array, and "expects [T] or a builder" told the reader that any of the
             * four would do -- three of which are refused. F186's own table claimed the derived
             * sentence for `slice`/`swap`/`to_set` was the hand-written "an array or a
             * MutableArray"; it was not, and naming a lone builder is what makes that true. */
            seen[n++] = builders > 1 ? "a builder" : builder_name;
        }
    }
    if (n < 2) return 0;
    size_t at = 0;
    for (int i = 0; i < n && at + 1 < cap; i++) {
        const char *sep = i == 0 ? "" : (i == n - 1 ? " or " : ", ");
        at += (size_t)snprintf(buf + at, cap - at, "%s%s", sep, seen[i]);
    }
    return at < cap;
}

static int ovl_min_arity(const FunctionDecl *f);

static FunctionDecl *overload_pick(Module *module, Env *env, Expr *e, const char *name) {
    OverloadResolution r;
    overload_resolve(module, env, e, name, &r);
    if (r.outcome == OVL_RESOLVED) return r.chosen;
    if (r.outcome == OVL_UNSUPPORTED) return find_function(module, name); /* absurdly large set: old behaviour */

    char list[1024];
    list[0] = '\0';
    if (r.outcome == OVL_AMBIGUOUS) {
        /* **A call below every candidate's `#[at_least_one]` floor is short, not ambiguous**, and
         * saying "more than one declaration accepts these arguments" of it is false -- none of
         * them does. `ovl_arity_fits` keeps a too-short variadic candidate alive deliberately, so
         * that the dedicated message below can name the floor instead of the set reporting no
         * match at all; the cost is that two candidates distinguished only *past* the floor both
         * survive and look ambiguous. `add(s)` never hit it because its two members differ at
         * position 0, so one is eliminated and the ordinary argument check speaks. `merge(mb)` did,
         * the moment `merge`'s in-place members became variadic (2026-10-22): they are a
         * `MutableMap` either way and differ only in the tail.
         *
         * Same sentence the ordinary path gives, so one rule reads one way. */
        int all_short = r.live_count > 0;
        for (int i = 0; i < r.live_count && all_short; i++) {
            if (ovl_min_arity(r.live[i]) <= e->as.call.args.count) all_short = 0;
        }
        if (all_short) {
            ziman_error("type", e->line, e->col,
                        "function '%s' expects at least %d argument(s), found %d -- its last parameter is "
                        "variadic but needs at least one value, since the call would otherwise do nothing "
                        "(pass an array to spread a list that may be empty)",
                        name, ovl_min_arity(r.live[0]), e->as.call.args.count);
        }
        overload_report_candidates(&r, list, sizeof list);
        /* An ambiguity must come with its escape, or the feature has made something unsayable
         * (the design's §5). After the self-describing filter has run, what is left undecided is
         * almost always an argument that carries no type of its own -- an empty literal, an
         * unannotated lambda, a bare `None` -- so the message names that position and asks for a
         * type there, rather than leaving the reader to guess which argument was the problem. */
        int open_at = 0;
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (expr_needs_expected_type(e->as.call.args.items[i])) { open_at = i + 1; break; }
        }
        if (open_at > 0) {
            ziman_error("type", e->line, e->col,
                        "'%s' is ambiguous here -- more than one declaration accepts these arguments, and "
                        "argument %d carries no type of its own to choose between them%s\n  = give it one: "
                        "bind it to a typed val first, or annotate it",
                        name, open_at, list);
        }
        ziman_error("type", e->line, e->col,
                    "'%s' is ambiguous here -- more than one declaration accepts these arguments%s\n"
                    "  = name the argument to choose, or give these two different names",
                    name, list);
    }
    /* No candidate. Re-resolve with the set unfiltered so the message can list what there was. */
    OverloadResolution all;
    memset(&all, 0, sizeof all);
    for (int i = 0; i < module->functions.count && all.live_count < OVL_MAX_CANDIDATES; i++) {
        if (strcmp(module->functions.items[i]->name, name) == 0) all.live[all.live_count++] = module->functions.items[i];
    }
    overload_report_candidates(&all, list, sizeof list);
    /* The set's own advice, if a member carries it -- see FunctionDecl.when_unmatched. Any one
     * member may write it and the set shares it, because it is about the *name*, not about which
     * declaration happened to be looked at first. */
    const char *advice = NULL;
    for (int i = 0; i < all.live_count && advice == NULL; i++) advice = all.live[i]->when_unmatched;
    char tail[512];
    tail[0] = '\0';
    if (advice != NULL) snprintf(tail, sizeof tail, " -- %s", advice);

    if (r.eliminated_at > 0) {
        char expected[512];
        if (overload_expected_types(&all, r.eliminated_at, expected, sizeof expected)) {
            if (r.eliminated_at == 1 && all.live_count > 1) {
                ziman_error("type", e->line, e->col, "'%s' expects %s, found %s%s",
                            name, expected, type_name(r.eliminated_actual), tail);
            }
            ziman_error("type", e->line, e->col, "argument %d of '%s' expects %s, found %s%s",
                        r.eliminated_at, name, expected, type_name(r.eliminated_actual), tail);
        }
        ziman_error("type", e->line, e->col,
                    "no declaration of '%s' accepts its argument %d%s%s",
                    name, r.eliminated_at, tail, list);
    }
    /* A call with no arguments at all is more likely a reach for the *type* than a mis-call of
     * the function, and that reading was measured, not guessed: `map()` and `set()` were what a
     * cold model wrote when it wanted `Map<K, V>` or `Set<T>`. The message lived inside the
     * WORLD_PAIRED loop until F180 deleted it along with the table -- and
     * tests/lang/diag_map_as_function is what noticed, which is the whole reason that fixture
     * carries the full text rather than a fragment. */
    if (e->as.call.args.count == 0) {
        const char *generic = matching_generic_type(name);
        if (generic != NULL) {
            ziman_error("type", e->line, e->col,
                        "'%s' takes at least 1 argument, found 0 -- and if you meant the '%s' type rather than "
                        "the function, %s", name, generic, generic_type_construction(generic));
        }
    }
    /* `tail` reaches this message as well as the per-argument ones since F207. The advice is about
     * the *name* -- `insert(aSet, x)` wants to be told about `add` -- and which check happened to
     * eliminate the last candidate is not something the author chose. It had been left off, so
     * converting `insert` turned F206's "argument 1 of 'insert' expects ... use add(set, item)"
     * into a bare "no declaration of 'insert' takes 2 argument(s)": the set's two declarations
     * both take 3, so arity eliminated them before any type was compared. */
    ziman_error("type", e->line, e->col,
                "no declaration of '%s' takes %d argument(s)%s%s", name, e->as.call.args.count, tail, list);
}

/* Stage 1's only output: whether the resolver agrees with the arm that already decided.
 * `expected` is the name the arm rewrote onto -- `mutable_array_filter` for `filter(b, p)`.
 * A disagreement is a specification, not a failure, so this prints and returns. */
/* ------------------------------------------------------- overload sets, stage 2 (declarations)
 *
 * `docs/design/full-overloading.md` §4, "Declaration-site checking", and §7 stage 2. Two members
 * of a set are *indistinguishable* when no argument list could tell them apart: their arities
 * overlap, and at every position within that overlap their parameter types accept some common
 * type. A set holding such a pair cannot be resolved, whatever a call looks like, and is an error
 * at the declaration rather than a surprise at the first ambiguous call.
 *
 * Reports rather than errors, for now. Stage 2's purpose is to find out *which* sets fail before
 * anything depends on the answer -- the design predicts exactly `mutable` and `concat`, and a
 * prediction that is checked by a gate that would break the build if it were wrong is not a
 * check, it is a dare. Run it with ZIMAN_OVERLOAD_SETS=1. */

/* Whether no type at all satisfies both parameter declarations. A type variable belonging to its
 * own function is a wildcard, so it is never disjoint from anything -- which is the whole reason
 * `mutable(item: T)` cannot coexist with its three siblings. */
static int ovl_params_disjoint(Type a, const TypeParamList *ta, Type b, const TypeParamList *tb) {
    ZIMAN_STACK_CHECK();
    if (type_is_own_param(a, ta) || type_is_own_param(b, tb)) return 0;
    if (a.kind != b.kind) return 1;
    switch (a.kind) {
        case TY_ARRAY:
        case TY_CHANNEL:
        case TY_SET:
            return ovl_params_disjoint(*a.elem, ta, *b.elem, tb);
        case TY_RESULT:
        case TY_MAP:
            return ovl_params_disjoint(*a.elem, ta, *b.elem, tb)
                || ovl_params_disjoint(*a.elem_err, ta, *b.elem_err, tb);
        case TY_STRUCT:
            if (strcmp(a.struct_name, b.struct_name) != 0) return 1;
            if (a.type_args.count != b.type_args.count) return 1;
            for (int i = 0; i < a.type_args.count; i++) {
                if (ovl_params_disjoint(a.type_args.items[i], ta, b.type_args.items[i], tb)) return 1;
            }
            return 0;
        case TY_ENUM:
            if (strcmp(a.enum_name, b.enum_name) != 0) return 1;
            if (a.type_args.count != b.type_args.count) return 1;
            for (int i = 0; i < a.type_args.count; i++) {
                if (ovl_params_disjoint(a.type_args.items[i], ta, b.type_args.items[i], tb)) return 1;
            }
            return 0;
        case TY_FUNC:
            /* Arity is the part a lambda's *shape* can see without being checked (§3), so two
             * function parameters of different arity are genuinely distinguishable. */
            if (a.type_args.count != b.type_args.count) return 1;
            for (int i = 0; i < a.type_args.count; i++) {
                if (ovl_params_disjoint(a.type_args.items[i], ta, b.type_args.items[i], tb)) return 1;
            }
            return ovl_params_disjoint(*a.elem, ta, *b.elem, tb);
        default:
            return 0; /* same scalar kind */
    }
}

static int ovl_min_arity(const FunctionDecl *f) {
    int n = f->params.count;
    /* `#[at_least_one]` is part of the arity, not a check bolted on after resolution: a call that
     * does not reach the floor must not select the declaration in the first place, or the set
     * would resolve and then be refused with a message about the wrong member. */
    if (n > 0 && f->params.items[n - 1].is_variadic) return f->variadic_at_least_one ? n : n - 1;
    return n;
}

static int ovl_is_variadic(const FunctionDecl *f) {
    return f->params.count > 0 && f->params.items[f->params.count - 1].is_variadic;
}

/* The smallest argument count at which `a` and `b` are both callable and no position tells them
 * apart, or -1 when every shared arity has a distinguishing position. */
static int ovl_indistinguishable_at(const FunctionDecl *a, const FunctionDecl *b) {
    int lo_a = ovl_min_arity(a), lo_b = ovl_min_arity(b);
    int lo = lo_a > lo_b ? lo_a : lo_b;
    int hi_a = ovl_is_variadic(a) ? INT_MAX : a->params.count;
    int hi_b = ovl_is_variadic(b) ? INT_MAX : b->params.count;
    int hi = hi_a < hi_b ? hi_a : hi_b;
    if (lo > hi) return -1; /* their arities never overlap */

    /* One past the longest fixed list is enough: beyond it both sides repeat their variadic
     * parameter, so no new position can appear. */
    int probe_max = a->params.count > b->params.count ? a->params.count : b->params.count;
    if (probe_max > hi) probe_max = hi;
    if (hi != INT_MAX && probe_max < hi) probe_max = hi;
    int cap = probe_max + 1;
    if (hi != INT_MAX && cap > hi) cap = hi;

    for (int k = lo; k <= cap; k++) {
        int distinguished = 0;
        for (int i = 0; i < k && !distinguished; i++) {
            Type pa, pb;
            if (!ovl_param_at(a, i, &pa) || !ovl_param_at(b, i, &pb)) continue;
            if (ovl_params_disjoint(pa, &a->type_params, pb, &b->type_params)) distinguished = 1;
        }
        if (!distinguished) return k;
    }
    return -1;
}

/* Two populations share the shape "one name, several declarations", and only one of them is an
 * overload set.
 *
 * A set of ordinary declarations is the resolver's: `sort` is two real functions and a call picks
 * one. A set of `builtin fn` declarations is **not** -- `len` has nine signatures over a single
 * polymorphic C function (`call_len` switches on the value's kind), and there is no `array_len`
 * or `map_len` for a call to resolve *to*. Those nine lines document one implementation's type
 * rule; they are not nine functions.
 *
 * The check measures the population it governs -- the resolver's -- and merely counts the other.
 *
 * It does **not** follow that the second population could never join the first, and F181 said so
 * before F185 corrected it: a native is found by its *declaration's* name, so nine declarations
 * all called `len` would resolve to the one existing registration rather than needing nine
 * implementations. What stands in the way is the diagnostics, not the mechanism -- `len`'s arm
 * says "expects an array, string, map, set, builder or Mutable", and a generic list of three
 * signatures out of nine is worse. See §11 of the design document. */
static int ovl_is_resolver_owned(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *f = module->functions.items[i];
        if (strcmp(f->name, name) == 0 && !f->is_builtin) return 1;
    }
    return 0;
}

/* The third population, and the smallest: `#[compiler_resolved]`, where the declarations are the
 * resolver's to *call* but not the resolver's to *choose* -- a rule in check_call picks and records
 * `call_fn`. `concat` is the one, and the check runs backwards on it. The bar for the first
 * population is that every pair can be told apart; the bar here is that some pair **cannot**,
 * because a set whose members a call can already distinguish has no business turning the first
 * check off. So the attribute cannot spread to a set that does not need it without failing here. */
static int ovl_is_compiler_resolved(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *f = module->functions.items[i];
        if (strcmp(f->name, name) == 0 && f->compiler_resolved) return 1;
    }
    return 0;
}

static void overload_sets_report(Module *module) {
    if (getenv("ZIMAN_OVERLOAD_SETS") == NULL) return;
    int sets = 0, bad = 0, polymorphic = 0, hand = 0;
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *a = module->functions.items[i];
        int first = 1;
        for (int j = 0; j < i; j++) {
            if (strcmp(module->functions.items[j]->name, a->name) == 0) { first = 0; break; }
        }
        if (!first) continue;
        int members = 0;
        for (int j = 0; j < module->functions.count; j++) {
            if (strcmp(module->functions.items[j]->name, a->name) == 0) members++;
        }
        if (members < 2) continue;
        if (!ovl_is_resolver_owned(module, a->name)) { polymorphic++; continue; }
        if (ovl_is_compiler_resolved(module, a->name)) {
            hand++;
            int indistinguishable = 0;
            for (int x = 0; x < module->functions.count && !indistinguishable; x++) {
                if (strcmp(module->functions.items[x]->name, a->name) != 0) continue;
                for (int y = x + 1; y < module->functions.count; y++) {
                    if (strcmp(module->functions.items[y]->name, a->name) != 0) continue;
                    if (ovl_indistinguishable_at(module->functions.items[x],
                                                 module->functions.items[y]) >= 0) {
                        indistinguishable = 1;
                        break;
                    }
                }
            }
            if (!indistinguishable) {
                bad++;
                fprintf(stderr, "overload-set: '%s' carries #[compiler_resolved] but every call can "
                                "tell its declarations apart -- the attribute turns off a check it "
                                "does not need and should go\n", a->name);
            }
            continue;
        }
        sets++;
        int reported = 0;
        for (int x = 0; x < module->functions.count; x++) {
            if (strcmp(module->functions.items[x]->name, a->name) != 0) continue;
            for (int y = x + 1; y < module->functions.count; y++) {
                if (strcmp(module->functions.items[y]->name, a->name) != 0) continue;
                int at = ovl_indistinguishable_at(module->functions.items[x], module->functions.items[y]);
                if (at < 0) continue;
                if (!reported) { bad++; reported = 1; }
                fprintf(stderr, "overload-set: '%s' is ambiguous for a call with %d argument(s) -- "
                                "declarations at line %d and line %d cannot be told apart\n",
                        a->name, at, module->functions.items[x]->line, module->functions.items[y]->line);
            }
        }
    }
    fprintf(stderr, "overload-set: %d resolver-owned set(s), %d unresolvable; "
                    "%d polymorphic builtin(s) not checked; %d compiler-resolved\n",
            sets, bad, polymorphic, hand);
}

static int check_collection_helper(Module *module, Env *env, Expr *e, const char *name, Type *out) {
    /* What is left of the collection-helper arm. `map_get_or`, `map_filter`, `set_filter`,
     * `set_union`, `set_intersect`, `set_difference`, `set_is_subset` and `map_remove_all` all
     * left it as the Map/Set pairing pass reached them (F192-F197) -- their declarations say
     * what this checked. The six that went in items 1, 4 and 5 had been sitting here matching
     * nothing, which is what a table of names does after the names change. */
    static const struct { const char *name; int argc; } TABLE[] = {
        {"__none__", 0},  /* every name this arm once checked now states its own type rule */
    };
    /* `to_set(xs)`: the distinct elements of a sequence, as a Set. Either world, like
     * `to_mutable_set` beside it -- the handle is read in one pass rather than snapshotted, so
     * accepting it saves a copy rather than hiding one. Named for its *result* because the
     * argument already says what the source is, and because a reader of `to_set` expects the
     * deduplication that `set_from_array` had to document. */
    /* The world-paired dispatch table stood here until F180. All 28 of its names are ordinary
     * overload sets now -- two declarations each, chosen by the resolver from the arguments --
     * so there is nothing left to rewrite a prefix onto. `concat` and `mutable` keep their own
     * arms below, for the reasons docs/design/full-overloading.md §4 records. */


    /* `concat(...)`: pieces into a string, parts into an array, or parts appended to a builder.
     * One name over three forms, chosen by the first argument, which is why it had to stop being a
     * `native` -- `concat_arrays` and `mutable_array_push_all` were the other two names and are
     * gone.
     *
     * **One argument that is an array spreads**, which is the rule that keeps `concat(parts)`
     * answering "xyz" for a `[string]`, as it has since before this merge. The cost is that
     * concatenating a single array *as one part* cannot be said -- `concat(["a", "b"])` is the
     * string "ab", never a copy of the array -- and a copy is `slice(a, 0, len(a))` anyway. Two or
     * more arguments are always parts, so `concat(["a"], ["b"])` is `["a", "b"]` and only the
     * one-argument case carries the rule. */
    if (strcmp(name, "concat") == 0) {
        if (e->as.call.args.count == 0) {
            /* `concat()` is "", as it was as a native before 2026-10-18 and is again. Both the
             * pieces and the parts form accept no arguments, so nothing in the call chooses and
             * the arm answers; the empty tail reaches `ziman_concat_strings` as an empty array. */
            e->call_fn = concat_member(module, TY_STRING, 0);
            *out = type_scalar(TY_STRING);
            return 1;
        }
        Expr *first = e->as.call.args.items[0];
        Type first_type = check_expr(module, env, first);

        /* a builder first: every part is appended to it, from either world, mixed freely */
        if (first_type.kind == TY_STRUCT && first_type.struct_name != NULL &&
            strcmp(first_type.struct_name, "MutableArray") == 0 && first_type.type_args.count == 1) {
            Type elem = first_type.type_args.items[0];
            for (int i = 1; i < e->as.call.args.count; i++) {
                Expr *part = e->as.call.args.items[i];
                Type part_type = check_expr(module, env, part);
                int ok = type_equals(part_type, type_array(elem)) ||
                         (part_type.kind == TY_STRUCT && part_type.struct_name != NULL &&
                          strcmp(part_type.struct_name, "MutableArray") == 0 &&
                          part_type.type_args.count == 1 && type_equals(part_type.type_args.items[0], elem));
                if (!ok) {
                    ziman_error("type", part->line, part->col,
                                "argument %d of 'concat' expects %s or MutableArray<%s> -- the parts appended to a "
                                "MutableArray<%s> -- found %s",
                                i + 1, type_name(type_array(elem)), type_name(elem), type_name(elem),
                                type_name(part_type));
                }
            }
            /* Either tail declaration serves: the shim reads each part's own runtime type, which
             * is what lets one call mix the two worlds. */
            e->call_fn = concat_member(module, TY_STRUCT, 2);
            *out = type_scalar(TY_VOID);
            return 1;
        }

        /* the lone-array spread */
        if (e->as.call.args.count == 1 && first_type.kind == TY_ARRAY) {
            if (first_type.elem->kind == TY_STRING) {
                e->call_fn = concat_member(module, TY_STRING, 0);
                e->as.call.variadic_tail_is_array_spread = 1;
                *out = type_scalar(TY_STRING);
                return 1;
            }
            if (first_type.elem->kind == TY_ARRAY) {
                e->call_fn = concat_member(module, TY_ARRAY, 0);
                e->as.call.variadic_tail_is_array_spread = 1;
                *out = *first_type.elem;
                check_type_well_formed(module, *out, e->line, e->col);
                return 1;
            }
            /* One part of a kind that cannot spread: a copy. Not a spread, so the collapse wraps
             * the one argument as the single part, which `ziman_concat_arrays` then copies. */
            e->call_fn = concat_member(module, TY_ARRAY, 0);
            *out = first_type;
            check_type_well_formed(module, *out, e->line, e->col);
            return 1;
        }

        /* pieces or parts, every argument the same kind as the first */
        int to_string = first_type.kind == TY_STRING;
        if (!to_string && first_type.kind != TY_ARRAY) {
            ziman_error("type", first->line, first->col,
                        "'concat' expects strings, arrays, or a MutableArray to append to, found %s",
                        type_name(first_type));
        }
        for (int i = 1; i < e->as.call.args.count; i++) {
            Expr *part = e->as.call.args.items[i];
            Type part_type = check_expr(module, env, part);
            if (!type_equals(part_type, first_type)) {
                ziman_error("type", part->line, part->col, "argument %d of 'concat' expects %s, found %s", i + 1,
                            type_name(first_type), type_name(part_type));
            }
        }
        e->call_fn = concat_member(module, to_string ? TY_STRING : TY_ARRAY, 0);
        *out = first_type;
        return 1;
    }

    /* `swap(x, i, j)`: exchange two elements. The same question of both worlds, with the answer
     * each world can give -- a new array from the array, nothing from the builder, which is written
     * in place. Both worlds are answered in one interpreter arm -- the prefixed
     * `mutable_array_swap` was removed once this covered both, so there is nothing to rewrite onto
     * and nothing needs it. The array form closed a real gap, the only previous spelling being
     * `set(set(a, i, a[j]), j, a[i])` at two whole-array copies to move two slots.
     *
     * The indices are checked by the runtime so a computed one is caught, but the *type* of each is
     * checked here so the message names `swap` rather than the element write a rewrite would
     * otherwise blame. */

    /* `slice(x, start, end)`: the elements from `start` up to `end`, as an array, from either
     * world. It was a `native` until 2026-10-18 and had to become a builtin to take both, every
     * overloaded name in the prelude being one -- a native has a single declaration and no
     * first-argument dispatch. The answer is `[T]` whichever world the source is; only the cost
     * differs, free against a copy of the window, and `ziman_slice_mutable_array` says why. */
    int found = 0;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            found = TABLE[i].argc;
            break;
        }
    }
    if (!found) return 0;
    int is_map = strncmp(name, "map_", 4) == 0;
    Type coll = helper_first(module, env, e, found, name, is_map ? TY_MAP : TY_SET);
    if (strcmp(name, "map_merge") == 0) {
        helper_arg(module, env, e, 1, coll, name);
        *out = coll;
    } else { /* unreachable while the table holds only `map_merge` */
        TypeList params = {0};
        type_list_push(&params, *coll.elem);
        if (is_map) type_list_push(&params, *coll.elem_err);
        helper_arg(module, env, e, 1, type_func(params, type_scalar(TY_BOOL)), name);
        *out = coll;
    }
    return 1;
}

static Expr *mk_enum_call_expr(const char *enum_name, const char *prefix, const char *variant, Expr *arg, int line, int col);
static Type check_enum_access(Module *module, Env *env, Expr *e);
static Expr *mk_call_expr(const char *callee, Expr *arg, int line, int col);

/* True when `t` names one of `params` anywhere in its shape -- whether a generic call's type argument for this
 * parameter can be learned from an argument checked against `t`. */
static int type_mentions_any(Type t, TypeParamList *params) {
    ZIMAN_STACK_CHECK();
    switch (t.kind) {
        case TY_TYPEVAR:
            for (int i = 0; i < params->count; i++) {
                if (strcmp(params->items[i].name, t.type_var_name) == 0) return 1;
            }
            return 0;
        case TY_ARRAY:
        case TY_CHANNEL:
        case TY_SET:
            return type_mentions_any(*t.elem, params);
        case TY_RESULT:
        case TY_MAP:
            return type_mentions_any(*t.elem, params) || type_mentions_any(*t.elem_err, params);
        case TY_FUNC: {
            if (type_mentions_any(*t.elem, params)) return 1;
            for (int i = 0; i < t.type_args.count; i++) {
                if (type_mentions_any(t.type_args.items[i], params)) return 1;
            }
            return 0;
        }
        case TY_STRUCT:
        case TY_ENUM:
            for (int i = 0; i < t.type_args.count; i++) {
                if (type_mentions_any(t.type_args.items[i], params)) return 1;
            }
            return 0;
        default:
            return 0;
    }
}

/* An argument this shape can only be checked with an expected type already in hand (an empty array literal, `ok`/
 * `err`/`channel_new`, a lambda, an `if`/`switch` used as a value) -- exactly the forms check_value_against_param_type
 * special-cases. Inference has no expected type to offer yet, so these are left for the ordinary argument-checking
 * loop to handle once every type argument is known, rather than forced here (which would just be today's "cannot
 * infer" error, for an argument that may not even matter to inference). */
/* Whether a lambda's own type is knowable without being told what it should be: every parameter has a
 * written type and the return type is written too. Such a lambda is exactly as informative to inference
 * as a named function, which already works -- `array_map(xs, double)` infers while
 * `array_map(xs, fn(n: int) -> int { return n * 2 })` did not, for no reason the writer of the
 * second could see. docs/SYNTAX.md said the exclusion applied to a lambda's *result type* not being
 * inferred, which is the unannotated case, so the behaviour was also wider than the promise. */
static int lambda_is_fully_annotated(const Expr *e) {
    const FunctionDecl *lam = (const FunctionDecl *)e->lambda_decl;
    if (lam == NULL || lam->return_omitted) return 0;
    for (int i = 0; i < lam->params.count; i++) {
        if (lam->params.items[i].type_omitted) return 0;
    }
    return 1;
}

static int expr_needs_expected_type(const Expr *e) {
    if (e->kind == EXPR_ARRAY_LIT && e->as.array_lit.elements.count == 0) return 1;
    /* An unannotated lambda still needs the expected type to fill its parameters in; an annotated one
     * does not, and checking it early is safe because check_lambda_expecting memoises on
     * `lambda_checked`, so the ordinary argument loop reuses the result rather than checking twice. */
    if (e->kind == EXPR_LAMBDA) return !lambda_is_fully_annotated(e);
    if (e->kind == EXPR_VALUE_BLOCK) return 1;
    if (e->kind == EXPR_CALL) {
        const char *callee = e->as.call.callee;
        if (strcmp(callee, "ok") == 0 || strcmp(callee, "err") == 0 || strcmp(callee, "channel_new") == 0) return 1;
        /* A bare `Some(x)` is in exactly the same position as `ok(x)`: it carries no Option<T> of
         * its own. Leaving it out meant a generic call checked it eagerly, before the type
         * arguments were bound, so `assert_eq(Some("es"), opt)` was refused even though the other
         * argument said which Option it was. A qualified `Option<T>.Some(x)` is a different node
         * (check_enum_access) and is not affected. */
        if (strcmp(callee, "Some") == 0 && e->as.call.type_args.count == 0) return 1;
    }
    /* And bare `None`, which is an identifier rather than a call until check_bare_option_call
     * rewrites it. `None` carries even less than `Some(x)` does -- there is no payload to look
     * at -- so a generic call has to bind its type parameter from the other arguments first and
     * check this one afterwards. */
    if (e->kind == EXPR_IDENT && strcmp(e->as.ident_name, "None") == 0) return 1;
    return 0;
}

/* Matches a generic function's declared parameter type (`pattern`, which may mention `params`) against one checked
 * argument's own type (`actual`), for C3 inference (see infer_call_type_args). A structural mismatch is not raised
 * here -- check_value_against_param_type gives the real, argument-specific error once every type argument is
 * known -- but two arguments disagreeing about the same type parameter is a genuine inference conflict, reported at
 * `at`. */
static void unify_type(Type pattern, Type actual, TypeParamList *params, Type *bindings, int *bound, const Expr *at,
                       const char *fn_name, int argpos) {
    ZIMAN_STACK_CHECK();
    if (pattern.kind == TY_TYPEVAR) {
        for (int i = 0; i < params->count; i++) {
            if (strcmp(params->items[i].name, pattern.type_var_name) != 0) continue;
            if (bound[i] && !type_equals(bindings[i], actual)) {
                /* Naming the call and the position, not just the type parameter. Until F195
                 * this said only "one argument requires string, another requires int", which is
                 * true and leaves the reader to find which call and which argument -- and it
                 * became the message for `get(m, 1)` on a `Map<string, int>` when that builtin's
                 * arm was replaced by its declarations. The arm had said "argument 2 of
                 * 'map_get' expects string (the map's key type), found int". Rather than give
                 * one builtin its arm back, every generic call now says as much. */
                ziman_error("type", at->line, at->col,
                            "argument %d of '%s' expects %s, which an earlier argument fixed as "
                            "type argument '%s' -- found %s",
                            argpos, fn_name != NULL ? fn_name : "this call", type_name(bindings[i]),
                            pattern.type_var_name, type_name(actual));
            }
            bindings[i] = actual;
            bound[i] = 1;
            return;
        }
        return;
    }
    if (pattern.kind != actual.kind) return;
    switch (pattern.kind) {
        case TY_ARRAY:
        case TY_CHANNEL:
        case TY_SET:
            unify_type(*pattern.elem, *actual.elem, params, bindings, bound, at, fn_name, argpos);
            return;
        case TY_RESULT:
        case TY_MAP:
            unify_type(*pattern.elem, *actual.elem, params, bindings, bound, at, fn_name, argpos);
            unify_type(*pattern.elem_err, *actual.elem_err, params, bindings, bound, at, fn_name, argpos);
            return;
        case TY_FUNC:
            if (pattern.type_args.count != actual.type_args.count) return;
            for (int i = 0; i < pattern.type_args.count; i++) {
                unify_type(pattern.type_args.items[i], actual.type_args.items[i], params, bindings, bound, at, fn_name, argpos);
            }
            unify_type(*pattern.elem, *actual.elem, params, bindings, bound, at, fn_name, argpos);
            return;
        case TY_STRUCT:
            if (strcmp(pattern.struct_name, actual.struct_name) != 0 || pattern.type_args.count != actual.type_args.count) return;
            for (int i = 0; i < pattern.type_args.count; i++) {
                unify_type(pattern.type_args.items[i], actual.type_args.items[i], params, bindings, bound, at, fn_name, argpos);
            }
            return;
        case TY_ENUM:
            if (strcmp(pattern.enum_name, actual.enum_name) != 0 || pattern.type_args.count != actual.type_args.count) return;
            for (int i = 0; i < pattern.type_args.count; i++) {
                unify_type(pattern.type_args.items[i], actual.type_args.items[i], params, bindings, bound, at, fn_name, argpos);
            }
            return;
        default:
            return;
    }
}

/* Whether every type parameter `pattern` mentions is already bound, so `pattern`'s own concrete shape can be read
 * off `bindings` right now (see infer_call_type_args's variadic-tail case, the one place a pattern is needed before
 * every argument has been visited). */
static int type_fully_bound(Type t, TypeParamList *params, const int *bound) {
    ZIMAN_STACK_CHECK();
    switch (t.kind) {
        case TY_TYPEVAR:
            for (int i = 0; i < params->count; i++) {
                if (strcmp(params->items[i].name, t.type_var_name) == 0) return bound[i];
            }
            return 1;
        case TY_ARRAY:
        case TY_CHANNEL:
        case TY_SET:
            return type_fully_bound(*t.elem, params, bound);
        case TY_RESULT:
        case TY_MAP:
            return type_fully_bound(*t.elem, params, bound) && type_fully_bound(*t.elem_err, params, bound);
        case TY_FUNC: {
            if (!type_fully_bound(*t.elem, params, bound)) return 0;
            for (int i = 0; i < t.type_args.count; i++) {
                if (!type_fully_bound(t.type_args.items[i], params, bound)) return 0;
            }
            return 1;
        }
        case TY_STRUCT:
        case TY_ENUM:
            for (int i = 0; i < t.type_args.count; i++) {
                if (!type_fully_bound(t.type_args.items[i], params, bound)) return 0;
            }
            return 1;
        default:
            return 1;
    }
}

/* C3: `fn`'s omitted type arguments, worked out from the checked types of `e`'s own arguments instead of requiring
 * them written out. Only a fixed parameter whose declared type actually mentions one of `fn`'s type parameters is
 * used (a wholly concrete parameter has nothing to contribute and is left to the ordinary checking loop, which
 * handles its own "needs an expected type" forms correctly once inference is done); of those, only an argument
 * `expr_needs_expected_type` says can be checked without one is used. The variadic tail (if any) is tried both as
 * a spread (a single argument whose own type is an array of the element type) and as individual element values,
 * since which of the two a lone trailing argument means isn't decided until the element type is known -- exactly
 * check_call's own "variadic_tail_is_array_spread" ambiguity, resolved the same way, from the argument's own static
 * type. A type parameter no argument could bind is the same "requires explicit type arguments" situation as
 * before, just with a more specific message naming which one. */
/* True when `e` is a bare `Some(x)` whose payload can be typed with no context of its own, setting
 * `*out` to that payload's type. `Some(None)` and `Some(ok(1))` are deliberately not included: the
 * payload needs a context there too, and there is nothing to read until the outer type is known. */
static int bare_some_payload_type(Module *module, Env *env, Expr *e, Type *out) {
    if (e->kind != EXPR_CALL) return 0;
    if (strcmp(e->as.call.callee, "Some") != 0) return 0;
    if (e->as.call.type_args.count > 0 || e->as.call.args.count != 1) return 0;
    Expr *payload = e->as.call.args.items[0];
    if (expr_needs_expected_type(payload)) return 0;
    Type t = check_expr(module, env, payload);
    if (t.kind == TY_VOID) return 0;
    *out = t;
    return 1;
}

/* Whether `actual` cannot match `pattern` at the top level -- the shapes differ, so `unify_type`
 * walks nothing and binds nothing. Its own contract leaves a structural mismatch to
 * `check_value_against_param_type`, which runs *after* every type argument is known; when the
 * mismatch is the reason one is not known, that check is never reached and the call used to fail
 * as "cannot infer type argument 'T' ... write it explicitly". For a parameter like `narrow`'s
 * `MutableArray<T>` given an `[int]` that is advice no `T` can satisfy, and F215 put `narrow`
 * back on a builtin id rather than ship it. This is the fix F216 made instead. */
static int shape_cannot_match(Type pattern, Type actual) {
    if (pattern.kind == TY_TYPEVAR) return 0;
    if (pattern.kind != actual.kind) return 1;
    if (pattern.kind == TY_STRUCT) {
        return pattern.struct_name == NULL || actual.struct_name == NULL
               || strcmp(pattern.struct_name, actual.struct_name) != 0;
    }
    return 0;
}

/* A lambda argument's own type, once the call can say what its *parameters* are -- the second
 * inference pass, and the one thing the first cannot do.
 *
 * The first pass skips every argument `expr_needs_expected_type` claims, and a lambda missing
 * either a parameter type or its `->` is one of them. So `map(ps, fn(p: Point) { p.x })` bound `T`
 * from `ps` and then gave up on `U` with "write it explicitly" -- advice about the one thing the
 * lambda body already states. `Point::x` desugars to exactly that lambda, which is how the
 * field-accessor shorthand inherited the gap: `map(points, Point::x)` had to be written
 * `map<Point, int>(points, Point::x)`, naming both the type it was reading the field from and the
 * field's own type.
 *
 * What makes the second pass possible is that the first one has already bound what the other
 * arguments say. Substituting those bindings into the declared parameter type turns `fn(T) -> U`
 * into `fn(Point) -> U`, and a lambda whose parameters are all concrete **types on its own**: its
 * body is checkable with no expectation at all, and a final bare expression answers the result.
 *
 * It deliberately does not hand `want`'s *return* type to the body. That is the type parameter
 * being inferred, and passing `want` whole to check_lambda_expecting is what used to type the
 * lambda `fn(Point) -> U` -- a typevar standing where the answer should be. Passing NULL is the
 * path that reads the body instead; it memoises on `lambda_checked`, so the ordinary argument loop
 * reuses this result rather than checking the body a second time.
 *
 * A lambda that writes `return` with no `->` is refused by that path ("write `-> T` after the
 * parameters"), which is the language's own rule and a better message than the one this replaces.
 * The lambda's own annotations win wherever it has them, so `fn(p: Point)`, `fn(p)` and
 * `fn(p) -> int` all work here. */
static int lambda_arg_type(Module *module, Env *env, Expr *arg, Type want, TypeParamList *params,
                           Type *out) {
    if (arg->kind != EXPR_LAMBDA) return 0;
    FunctionDecl *lam = (FunctionDecl *)arg->lambda_decl;
    if (lam == NULL) return 0;
    if (want.kind != TY_FUNC || want.type_args.count != lam->params.count) return 0;
    /* Every parameter has to be answerable: the lambda declared it, or what is left of `want`
     * after substitution no longer mentions a type parameter the first pass failed to bind. */
    for (int i = 0; i < lam->params.count; i++) {
        if (lam->params.items[i].type_omitted && type_mentions_any(want.type_args.items[i], params)) {
            return 0;
        }
    }
    for (int i = 0; i < lam->params.count; i++) {
        Param *param = &lam->params.items[i];
        if (!param->type_omitted) continue;
        param->type = want.type_args.items[i];
        param->type_omitted = 0;
    }
    *out = check_lambda_expecting(module, env, arg, NULL);
    return 1;
}

static TypeList infer_call_type_args(Module *module, Env *env, Expr *e, FunctionDecl *fn) {
    int n_params = fn->type_params.count;
    Type *bindings = ziman_xcalloc((size_t)n_params, sizeof(Type));
    int *bound = ziman_xcalloc((size_t)n_params, sizeof(int));

    int is_variadic = fn->params.count > 0 && fn->params.items[fn->params.count - 1].is_variadic;
    int fixed_count = is_variadic ? fn->params.count - 1 : fn->params.count;
    int given = e->as.call.args.count;
    int n = given < fixed_count ? given : fixed_count;

    /* The first argument whose shape the declared parameter cannot match, kept for the failure
     * below. Recorded rather than raised here, because an argument that binds nothing is only an
     * error if some type parameter then goes unbound -- `None` binds nothing either, and the other
     * arguments are expected to bind for it. */
    int blame_pos = 0;
    Type blame_pattern = (Type){0}, blame_actual = (Type){0};
    const Expr *blame_at = NULL;

    for (int i = 0; i < n; i++) {
        Type pattern = fn->params.items[i].type;
        if (!type_mentions_any(pattern, &fn->type_params)) continue;
        Expr *arg = e->as.call.args.items[i];
        if (expr_needs_expected_type(arg)) {
            /* A bare `Some(x)` does say which Option it is, as long as its payload types on its
             * own: `Some("es")` can only ever be an `Option<string>`, so a generic call need not
             * give up on it the way it must for `None`, which carries nothing to look at. Without
             * this, `same(Some("es"), None)` fails to bind `T` even though one of the two
             * arguments names it unambiguously. */
            Type payload = (Type){0};
            if (bare_some_payload_type(module, env, arg, &payload)) {
                unify_type(pattern, option_type_of(payload), &fn->type_params, bindings, bound, arg, fn->name, i + 1);
            }
            continue;
        }
        Type actual = check_expr(module, env, arg);
        if (blame_pos == 0 && shape_cannot_match(pattern, actual)) {
            blame_pos = i + 1;
            blame_pattern = pattern;
            blame_actual = actual;
            blame_at = arg;
        }
        unify_type(pattern, actual, &fn->type_params, bindings, bound, arg, fn->name, i + 1);
    }
    if (is_variadic && given > fixed_count) {
        Type pattern = fn->params.items[fixed_count].type;
        if (type_mentions_any(pattern, &fn->type_params)) {
            int tail_count = given - fixed_count;
            for (int i = fixed_count; i < given; i++) {
                Expr *arg = e->as.call.args.items[i];
                if (expr_needs_expected_type(arg)) continue;
                Type actual = check_expr(module, env, arg);
                if (tail_count != 1) {
                    /* Several trailing arguments are always individual element values (`concat_arrays(a, b)`). */
                    unify_type(pattern, actual, &fn->type_params, bindings, bound, arg, fn->name, i + 1);
                } else if (type_fully_bound(pattern, &fn->type_params, bound)) {
                    /* The element type is already known from an earlier parameter (`push<T>(b: MutableArray<T>,
                     * values: T...)` binds T from `b`) -- so, unlike the guess below, there is nothing left to infer
                     * from this argument at all. This is what makes `push(out, [x, y])` push the one `[string]`
                     * value `[x, y]` rather than spreading two `string`s into it: the ordinary argument check, run
                     * once every type argument is known, applies call_function's own real spread-vs-individual test
                     * against the now-fully-substituted element type and reports a mismatch if `actual` fits neither. */
                } else if (actual.kind == TY_ARRAY) {
                    /* Nothing bound the element type yet (`concat_arrays<T>(parts: [T]...)` has no other parameter):
                     * a lone array-typed argument is read as the spread shape, same as before. */
                    unify_type(pattern, *actual.elem, &fn->type_params, bindings, bound, arg, fn->name, i + 1);
                } else {
                    unify_type(pattern, actual, &fn->type_params, bindings, bound, arg, fn->name, i + 1);
                }
            }
        }
    }

    /* The second pass: the lambda arguments the first one had to skip, now that the others have
     * said what they can. Runs only while something is still unbound, and only over arguments a
     * lambda could answer for -- see lambda_arg_type. The partial substitution is what carries the
     * first pass's work in: an unbound parameter stays as its own typevar, so `fn(T) -> U` with
     * `T` bound becomes `fn(Point) -> U` and the helper can tell the one it can supply from the
     * one it is there to learn. */
    /* A lambda that answers nothing, where the parameter's result type is what is being inferred.
     * Recorded rather than unified: binding the type argument to "no value" is how the second pass
     * first made `map(xs, fn(x: int) { print(x) })` compile and answer `[0, 0]`, which is worse
     * than the refusal it replaced -- the old message was unhelpful, that result is wrong. */
    int void_lambda_pos = 0;
    const Expr *void_lambda_at = NULL;

    int all_bound = 1;
    for (int i = 0; i < n_params; i++) {
        if (!bound[i]) all_bound = 0;
    }
    if (!all_bound) {
        TypeList partial = {0};
        for (int i = 0; i < n_params; i++) {
            type_list_push(&partial, bound[i] ? bindings[i] : type_typevar(fn->type_params.items[i].name));
        }
        for (int i = 0; i < n; i++) {
            Type pattern = fn->params.items[i].type;
            if (!type_mentions_any(pattern, &fn->type_params)) continue;
            if (type_fully_bound(pattern, &fn->type_params, bound)) continue;
            Expr *arg = e->as.call.args.items[i];
            Type actual = (Type){0};
            if (!lambda_arg_type(module, env, arg, substitute_type(pattern, &fn->type_params, &partial),
                                 &fn->type_params, &actual)) {
                continue;
            }
            /* Only when the declared *result* is the unknown: a parameter written `fn(T) -> unit`
             * wants nothing back, so void is the right answer there and unifies against a concrete
             * `unit` without going near a type argument. */
            int result_is_unknown = pattern.kind == TY_FUNC
                                 && !type_fully_bound(*pattern.elem, &fn->type_params, bound);
            if (result_is_unknown && actual.kind == TY_FUNC && actual.elem->kind == TY_VOID) {
                if (void_lambda_pos == 0) {
                    void_lambda_pos = i + 1;
                    void_lambda_at = arg;
                }
                continue;
            }
            unify_type(pattern, actual, &fn->type_params, bindings, bound, arg, fn->name, i + 1);
        }
    }

    TypeList out = {0};
    for (int i = 0; i < n_params; i++) {
        if (!bound[i]) {
            /* An argument the parameter could not have matched is the real error, and naming it is
             * what "write it explicitly" cannot do: no type argument makes an `[int]` a
             * `MutableArray<T>`. `#[when_unmatched]` is appended here too -- a shape mismatch
             * against the only declaration is precisely the case that attribute is for, and a
             * single-declaration name never reaches the overload resolver that otherwise adds
             * it. The old message stays for a call that really is uninferable, where every
             * argument fits and none of them mentions the parameter. */
            if (blame_pos > 0) {
                ziman_error("type", blame_at->line, blame_at->col, "argument %d of '%s' expects %s, found %s%s%s",
                            blame_pos, fn->name, type_name(blame_pattern), type_name(blame_actual),
                            fn->when_unmatched != NULL ? " -- " : "",
                            fn->when_unmatched != NULL ? fn->when_unmatched : "");
            }
            /* The second pass typed a lambda argument and found it answers nothing, which names
             * both the argument and what is missing from it -- where "write it explicitly" would
             * be asking for a type argument no body can produce a value of. */
            if (void_lambda_pos > 0) {
                ziman_error("type", void_lambda_at->line, void_lambda_at->col,
                            "argument %d of '%s' is a lambda that answers nothing, so there is nothing for "
                            "type argument '%s' to be -- end its body with the value it should produce, or "
                            "write `-> T` and a 'return'",
                            void_lambda_pos, fn->name, fn->type_params.items[i].name);
            }
            ziman_error("type", e->line, e->col,
                        "cannot infer type argument '%s' of '%s' from its arguments; write it explicitly, e.g. '%s<...>(...)'",
                        fn->type_params.items[i].name, fn->name, fn->name);
        }
        type_list_push(&out, bindings[i]);
    }
    free(bindings);
    free(bound);
    return out;
}

/* True for one of the four builder handles itself. Deliberately not type_holds_thread_bound,
 * which answers a different question: that one is true of a user struct with a builder field, and
 * such a struct has no length of its own. `len` works on a builder, not on something holding one. */
/* The `Mutable<T>` cell, which is deliberately *not* one of the four above: it answers `len` and
 * `contains` but neither `snapshot` nor `clear`, so the two predicates have to be separate. */
/* The value-or-handle parameter the conversion and set-operation builtins take. There is no union
 * type in the `Type` representation and these do not need one: they are hand-checked builtins,
 * exactly as their immutable twins (`set_union`, `map_filter`, `to_set`, ...) already are
 * and have always been, so accepting two kinds is two branches here rather than a type-system
 * feature. Fills `elem`, and `value` for a map shape, and answers whether `t` is either world of
 * `shape`. */
typedef enum { SHAPE_ARRAY, SHAPE_MAP, SHAPE_SET } MutableShape;

static const char *mutable_handle_name(MutableShape shape) {
    return shape == SHAPE_ARRAY ? "MutableArray" : shape == SHAPE_MAP ? "MutableMap" : "MutableSet";
}

static int either_world(Type t, MutableShape shape, Type *elem, Type *value) {
    if (t.kind == TY_STRUCT && t.struct_name != NULL && strcmp(t.struct_name, mutable_handle_name(shape)) == 0) {
        if (t.type_args.count < (shape == SHAPE_MAP ? 2 : 1)) return 0;
        *elem = t.type_args.items[0];
        if (shape == SHAPE_MAP) *value = t.type_args.items[1];
        return 1;
    }
    if (shape == SHAPE_ARRAY && t.kind == TY_ARRAY) { *elem = *t.elem; return 1; }
    if (shape == SHAPE_SET && t.kind == TY_SET) { *elem = *t.elem; return 1; }
    if (shape == SHAPE_MAP && t.kind == TY_MAP) { *elem = *t.elem; *value = *t.elem_err; return 1; }
    return 0;
}

static Type mutable_handle_type(MutableShape shape, Type elem, const Type *value) {
    Type t = type_struct(mutable_handle_name(shape));
    type_list_push(&t.type_args, elem);
    if (value != NULL) type_list_push(&t.type_args, *value);
    return t;
}


static int is_builder_type(Type t) {
    if (t.kind != TY_STRUCT || t.struct_name == NULL) return 0;
    return strcmp(t.struct_name, "MutableArray") == 0 || strcmp(t.struct_name, "MutableString") == 0 ||
           strcmp(t.struct_name, "MutableMap") == 0 || strcmp(t.struct_name, "MutableSet") == 0;
}

/* Names a program declared that shadow a builtin of the same name. Collected once in the
 * declaration pass rather than looked up per call: `find_function` answers the *first*
 * declaration, which for a shadowed name is the builtin, so finding the program's one means
 * walking the module's functions -- and doing that at every call site is exactly the O(n^2) this
 * file already paid once and fixed (F130). Shadowing is rare, so this stays short. */
static FunctionDecl **g_builtin_shadows = NULL;
static int g_builtin_shadow_count = 0;

/* The program's declaration of a name a builtin also has, or NULL. It holds the declaration rather
 * than the name because `find_function` answers the *first* one, which for a shadowed name is the
 * builtin -- so the ordinary-call path needs this to be handed the right signature, not merely
 * told that one exists. */
/* Whether the body being checked belongs to the program rather than to a bundled module. A shadow
 * is only visible from program code: the prelude's own functions sit in this same module's list,
 * so without this a program declaring `fn len(x: string)` made every `len(...)` *inside the
 * prelude itself* resolve to it, and the prelude stopped type-checking. Shadowing a name is the
 * program's business and must not reach the library it is shadowing. */
static int g_checking_program_body = 0;

static FunctionDecl *shadowing_fn_any(const char *name) {
    for (int i = 0; i < g_builtin_shadow_count; i++) {
        if (strcmp(name, g_builtin_shadows[i]->name) == 0) return g_builtin_shadows[i];
    }
    return NULL;
}

static FunctionDecl *shadowing_fn(const char *name) {
    if (!g_checking_program_body) return NULL;
    for (int i = 0; i < g_builtin_shadow_count; i++) {
        if (strcmp(name, g_builtin_shadows[i]->name) == 0) return g_builtin_shadows[i];
    }
    return NULL;
}

static Type check_call(Module *module, Env *env, Expr *e) {
    const char *name = e->as.call.callee;

    /* A call the checker has already rewritten onto one of its own `$` names is finished: the
     * `mutable` arm replaces the callee, replaces the arguments with the literal's elements, and
     * answers the handle type directly rather than recursing -- so there is no arm to re-enter
     * and `$mutable_array_lit` reads as an undefined function the second time round.
     *
     * Nothing checked an expression twice until the resolver did. An overload set types each
     * self-describing argument to filter its candidates, and the ordinary argument loop then
     * types it again against the chosen declaration; for every other expression that is merely
     * wasted work, and for this one it was an error. `distinct(mutable([1, 2, 1]))` has been
     * broken since F178 and was found converting `slice`, whose hand-written arm had checked its
     * argument exactly once. The recorded type is the right answer and `check_expr` stores it on
     * every node, so this returns it.
     *
     * Two things the guard has to get right. It tests `rewritten_from`, not the `$` alone,
     * because the *parser* emits `$map_lit`, `$set_lit`, `$is_variant` and `$unwrap_variant` as
     * original callees and those do have arms that must still run. And it tests that a type was
     * already recorded: `mutable` over a non-literal rewrites the callee and then deliberately
     * **re-enters** `check_expr` so the `$mutable_array_from` arm runs, which is a first check
     * with nothing recorded yet -- without that half of the guard, five fixtures turned
     * `mutable(source)` into a void value. */
    if (e->as.call.rewritten_from != NULL && name[0] == '$'
        && e->resolved_type.kind != TY_VOID) {
        return e->resolved_type;
    }
    FunctionDecl *shadow_fn = shadowing_fn(name);

    /* `a.f(args)` where `a` is not a module this file imported. The loader left it alone and marked it
     * (ast.h's `unresolved_qualifier`) because only this pass knows whether `a` is a local. Two outcomes:
     * `a` is a value whose struct type has a field `f` of function type, and the call goes through that
     * field; or it is neither, and the module diagnostic the loader used to raise is raised here instead,
     * unchanged, so nothing about that message got worse to make this possible.
     *
     * Going through the field means the call's target is an expression rather than a name, which is the
     * one place that is true -- see `callee_expr`. The field access is built here rather than desugared
     * into a temporary val, so the interpreter evaluates exactly what was written. */
    if (e->as.call.unresolved_qualifier) {
        const char *dot = strchr(name, '.');
        size_t qlen = (size_t)(dot - name);
        char *qualifier = ziman_xmalloc(qlen + 1);
        memcpy(qualifier, name, qlen);
        qualifier[qlen] = '\0';
        const char *member = dot + 1;

        int depth, slot;
        Binding *base = env_lookup_resolved(env, qualifier, &depth, &slot);
        if (base != NULL && base->poisoned) ziman_abort_quietly();
        Type ft = type_scalar(TY_VOID);
        int found_field = 0;
        if (base != NULL && base->type.kind == TY_STRUCT) {
            StructDecl *sd = find_struct(module, base->type.struct_name);
            if (sd != NULL) {
                for (int i = 0; i < sd->fields.count; i++) {
                    if (strcmp(sd->fields.items[i].name, member) != 0) continue;
                    Type fieldt = substitute_type(sd->fields.items[i].type, &sd->type_params, &base->type.type_args);
                    if (fieldt.kind == TY_FUNC) {
                        ft = fieldt;
                        found_field = 1;
                    } else {
                        /* The field exists and is simply not callable. Saying so beats the module message,
                         * which would send the reader looking for an import that was never the problem. */
                        ziman_error("type", e->line, e->col,
                                    "'%s.%s' is a %s, which cannot be called -- only a field of function type can be",
                                    qualifier, member, type_name(fieldt));
                    }
                    break;
                }
            }
        }
        if (!found_field) {
            ziman_report_unknown_module_call(qualifier, member, e->line, e->col);
        }
        free(qualifier);

        if (e->as.call.arg_names != NULL) {
            ziman_error("type", e->line, e->col,
                        "named arguments aren't supported when calling through a function value ('%s')", name);
        }
        if (e->as.call.type_args.count != 0) {
            ziman_error("type", e->line, e->col,
                        "type arguments aren't supported when calling through a function value ('%s')", name);
        }
        if (e->as.call.args.count != ft.type_args.count) {
            ziman_error("type", e->line, e->col, "'%s' has type %s, which takes %d argument(s), found %d",
                        name, type_name(ft), ft.type_args.count, e->as.call.args.count);
        }
        for (int i = 0; i < e->as.call.args.count; i++) {
            Type arg_type;
            if (!check_value_against_param_type(module, env, &e->as.call.args.items[i], ft.type_args.items[i], &arg_type,
                                                 "as an argument")) {
                ziman_error("type", e->as.call.args.items[i]->line, e->as.call.args.items[i]->col,
                            "argument %d of '%s' (a %s) expects %s, found %s", i + 1, name, type_name(ft),
                            type_name(ft.type_args.items[i]), type_name(arg_type));
            }
        }
        /* The field access the call goes through, checked like any other so its own resolution happens
         * once, here. */
        Expr *base_ref = expr_new(EXPR_IDENT, e->line, e->col);
        base_ref->as.ident_name = (char *)name; /* trimmed to the qualifier just below */
        char *qual_only = ziman_xmalloc(qlen + 1);
        memcpy(qual_only, name, qlen);
        qual_only[qlen] = '\0';
        base_ref->as.ident_name = qual_only;
        Expr *access = expr_new(EXPR_FIELD_ACCESS, e->line, e->col);
        access->as.field_access.base = base_ref;
        access->as.field_access.field_name = ziman_xstrdup(member);
        access->resolved_type = ft;
        base_ref->resolved_type = base->type;
        base_ref->resolved_depth = depth;
        base_ref->resolved_slot = slot;
        e->as.call.callee_expr = access;
        return *ft.elem;
    }

    /* A call through a local of function type (a parameter or a val): a local
     * always wins over a top-level function of the same name. Named arguments
     * and type arguments don't apply -- the value carries only a parameter list. */
    if (e->as.call.local_name != NULL && e->as.call.type_args.count == 0) {
        int local_depth, local_slot;
        Binding *fb = env_lookup_resolved(env, e->as.call.local_name, &local_depth, &local_slot);
        /* Calling a function-valued binding reads it, and a call does not go through EXPR_IDENT. Without
         * this, `val to_params = crud.to_params` followed by `to_params(entity)` was reported as never
         * read -- 19 such findings in the reference backend, every one of them wrong, which is what this
         * line exists to answer. */
        if (fb != NULL && fb->lint_id > 0 && fb->lint_id - 1 < g_lint_var_count) {
            g_lint_vars[fb->lint_id - 1].used = 1;
        }
        /* A poisoned local (its own declaration already failed -- see poison_declaration) has a placeholder
         * TY_VOID type, so it never matches the `TY_FUNC` check just below and falls through to `find_function`,
         * which reports "call to undefined function" for a name that is, in fact, declared right here: an
         * accurate-sounding but nonsensical follow-on error about the real one already raised. Every *other*
         * reference to a poisoned binding already aborts quietly the same way (see check_expr's EXPR_IDENT
         * case); a call through one, going through this earlier, local-name-specific path instead, needs its
         * own check. See docs/DESIGN.md's "No cascades" entry. */
        if (fb != NULL && fb->poisoned) ziman_abort_quietly();
        if (fb != NULL && fb->type.kind == TY_FUNC) {
            Type ft = fb->type;
            if (e->as.call.arg_names != NULL) {
                ziman_error("type", e->line, e->col, "named arguments aren't supported when calling through a function value ('%s')", e->as.call.local_name);
            }
            if (e->as.call.args.count != ft.type_args.count) {
                ziman_error("type", e->line, e->col, "'%s' has type %s, which takes %d argument(s), found %d",
                            e->as.call.local_name, type_name(ft), ft.type_args.count, e->as.call.args.count);
            }
            for (int i = 0; i < e->as.call.args.count; i++) {
                Type arg_type;
                if (!check_value_against_param_type(module, env, &e->as.call.args.items[i], ft.type_args.items[i], &arg_type,
                                                 "as an argument")) {
                    ziman_error("type", e->as.call.args.items[i]->line, e->as.call.args.items[i]->col,
                                "argument %d of '%s' (a %s) expects %s, found %s", i + 1, e->as.call.local_name, type_name(ft),
                                type_name(ft.type_args.items[i]), type_name(arg_type));
                }
            }
            e->as.call.through_value = 1;
            e->resolved_depth = local_depth;
            e->resolved_slot = local_slot;
            return *ft.elem;
        }
    }

    if (e->as.call.arg_names != NULL && builtin_refuses_named_args(name)) {
        ziman_error("type", e->line, e->col, "named arguments aren't supported for '%s'", name);
    }

    /* **A program's own function shadows a builtin of the same name.** A `use`d module could
     * always do this and the main file could not, which was an asymmetry rather than a rule:
     * `typecheck` runs on the root module only, so an imported module's functions never reached
     * the reserved-name check that rejected them here. Taking `count`, `sum`, `find` and `first`
     * as builtin names made that visible -- `count` is declared by fifteen files in this tree, five
     * of them modules that were allowed it and ten of them programs that were not.
     *
     * Resolution has to agree with the declaration being legal. Every arm below runs *before*
     * `find_function`, so a declared `count` would otherwise be accepted and then be unreachable
     * by a bare call, which is worse than the error it replaces. The jump is forward, to the
     * ordinary-call path, and `source_file != NULL` is what distinguishes a program's declaration
     * from a bundled one -- the same test the `builtin fn` guard uses. */
    if (shadow_fn != NULL) goto ordinary_call;

    {
        Type helper_result;
        if (check_collection_helper(module, env, e, name, &helper_result)) return helper_result;
    }

    if (strcmp(name, "$map_lit") == 0) return check_collection_literal(module, env, e, 1);
    if (strcmp(name, "$set_lit") == 0) return check_collection_literal(module, env, e, 0);

    if (strcmp(name, "print") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'print' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        check_expr(module, env, e->as.call.args.items[0]);
        require_not_void(e->as.call.args.items[0], "as an argument");
        return type_scalar(TY_VOID);
    }

    if (strcmp(name, "str") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'str' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_INT && arg_type.kind != TY_FLOAT && arg_type.kind != TY_BOOL && arg_type.kind != TY_BIGINT && arg_type.kind != TY_DECIMAL && arg_type.kind != TY_BYTE) {
            /* The parenthetical is only true of a string, and it used to be printed
             * unconditionally: `str(P { x: 1 })` was told "found P (it's already a string)".
             * Found in F191 while measuring whether this arm could be replaced by its
             * declarations -- it cannot, but the message was wrong either way. */
            ziman_error("type", arg->line, arg->col,
                        "'str' expects an int, float, bool, bigint, decimal, or byte, found %s%s",
                        type_name(arg_type),
                        arg_type.kind == TY_STRING ? " (it's already a string)" : "");
        }
        return type_scalar(TY_STRING);
    }

    if (strcmp(name, "format") == 0 || strcmp(name, "printf") == 0) {
        if (e->as.call.args.count < 1) {
            ziman_error("type", e->line, e->col, "'%s' requires at least a format string argument", name);
        }
        Expr *fmt_expr = e->as.call.args.items[0];
        if (fmt_expr->kind != EXPR_STRING) {
            ziman_error("type", fmt_expr->line, fmt_expr->col,
                        "'%s' requires its first argument to be a string literal, not a computed value", name);
        }
        fmt_expr->resolved_type = type_scalar(TY_STRING);

        int *precisions;
        int placeholder_count = scan_placeholders(fmt_expr, &precisions);
        int provided = e->as.call.args.count - 1;
        if (placeholder_count != provided) {
            ziman_error("type", e->line, e->col,
                        "format string has %d placeholder(s) but %d argument(s) were provided",
                        placeholder_count, provided);
        }
        for (int i = 1; i < e->as.call.args.count; i++) {
            Expr *arg = e->as.call.args.items[i];
            Type arg_type = check_expr(module, env, arg);
            require_not_void(arg, "as a format argument");
            if (precisions[i - 1] >= 0 && arg_type.kind != TY_FLOAT && arg_type.kind != TY_DECIMAL) {
                ziman_error("type", arg->line, arg->col,
                            "a precision ('{:.%d}') applies to a float or a decimal, not to %s", precisions[i - 1], type_name(arg_type));
            }
        }
        return type_scalar(strcmp(name, "printf") == 0 ? TY_VOID : TY_STRING);
    }

    /* The three `$` names the parser emits for `mutable(x)`. `to_mutable_set` was here too until
     * F215 converted it to two `native fn` declarations -- and leaving it in this arm while taking
     * its builtin id away is what made `items(to_mutable_set(xs))` segfault for one build: the arm
     * answers before the resolver, so `call_fn` stayed NULL and the interpreter looked for an id
     * that no longer existed. **Converting a name means deleting its arm, not only its id.** */
    if (strcmp(name, "$mutable_array_from") == 0 || strcmp(name, "$mutable_map_from") == 0 ||
        strcmp(name, "$mutable_set_from") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        MutableShape source = strcmp(name, "$mutable_array_from") == 0 ? SHAPE_ARRAY
                              : strcmp(name, "$mutable_map_from") == 0  ? SHAPE_MAP
                                                                        : SHAPE_SET;
        Type elem = type_scalar(TY_VOID), value = type_scalar(TY_VOID);
        if (!either_world(arg_type, source, &elem, &value)) {
            ziman_error("type", arg->line, arg->col, "'%s' expects %s or %s, found %s", name,
                        source == SHAPE_ARRAY ? "an array" : source == SHAPE_MAP ? "a Map" : "a Set",
                        source == SHAPE_ARRAY ? "a MutableArray" : source == SHAPE_MAP ? "a MutableMap" : "a MutableSet",
                        type_name(arg_type));
        }
        return mutable_handle_type(source, elem, source == SHAPE_MAP ? &value : NULL);
    }

    /* `mutable(x)`: the mutable counterpart of whatever it is given. The three collections that
     * have a builder rewrite onto the `mutable_*_from` they mirror, so each inherits that
     * family's result-type derivation rather than repeating it here. Everything else -- a scalar,
     * a string, a struct, another handle -- has no builder to answer, so it rewrites onto
     * `mutable_new` and answers a `Mutable<T>` cell.
     *
     * The ordering is the dispatch: an array matches both `[T]` and the bare `T`, and the array
     * form wins, which is why `mutable([1, 2])` is never a `Mutable<[int]>`. That one is
     * `mutable_new([1, 2])`, and lib/prelude/collections.zm's note above these declarations says so. */
    if (strcmp(name, "mutable") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'mutable' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        /* Reached only when no context offered a type -- every site that can offer one tries
         * check_mutable_empty_call first. So `[]` here means there was nothing to infer from, and
         * the generic empty-literal error would send the author to `val x: [int] = []`, which is
         * not the repair for this. Shaped after `channel_new()`'s message, which names the
         * contexts for the same reason. */
        if (arg->kind == EXPR_ARRAY_LIT && arg->as.array_lit.elements.count == 0) {
            ziman_error("type", arg->line, arg->col,
                        "cannot infer the element type of 'mutable([])' here; it takes its type from the context, so "
                        "it needs one: a val/var with an explicit MutableArray<T> annotation, a struct literal "
                        "field, a 'with' override, a function's returned value, or an argument whose parameter type "
                        "says which MutableArray it is -- or write 'mutable_array_new<T>()', which names the element "
                        "type itself");
        }
        Type arg_type = check_expr(module, env, arg);
        require_not_void(arg, "as the value 'mutable' makes mutable");
        /* `mutable([1, 2])` builds the builder straight from the literal's elements, which become
         * this call's arguments -- the shape `$map_lit` has used since map literals existed. The
         * array the elements would otherwise be collected into is never allocated, and the
         * elements are already checked, `check_expr` above having checked the literal. Measured:
         * two allocations and 80 bytes less per construction. A literal with no elements cannot
         * reach here, `[]` having no element type to infer, so the guard is for the shape rather
         * than for a case. */
        if (arg_type.kind == TY_ARRAY && arg->kind == EXPR_ARRAY_LIT && arg->as.array_lit.elements.count > 0) {
            e->as.call.rewritten_from = "mutable";
            e->as.call.callee = ziman_xstrdup("$mutable_array_lit");
            e->as.call.args = arg->as.array_lit.elements;
            return mutable_handle_type(SHAPE_ARRAY, *arg_type.elem, NULL);
        }
        const char *target = arg_type.kind == TY_ARRAY ? "$mutable_array_from"
                           : arg_type.kind == TY_MAP   ? "$mutable_map_from"
                           : arg_type.kind == TY_SET   ? "$mutable_set_from"
                                                       : "mutable_new";
        e->as.call.rewritten_from = "mutable";
        e->as.call.callee = ziman_xstrdup(target);
        return check_expr(module, env, e);
    }

    if (strcmp(name, "compare") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'compare' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *first = e->as.call.args.items[0];
        Expr *second = e->as.call.args.items[1];
        Type first_type = check_expr(module, env, first);
        Type second_type = check_expr(module, env, second);
        if (!type_equals(first_type, second_type)) {
            ziman_error("type", second->line, second->col, "'compare' needs two values of the same type, found %s and %s",
                        type_name(first_type), type_name(second_type));
        }
        if (!type_supports_ordering(module, first_type)) {
            ziman_error("type", first->line, first->col,
                        "'compare' expects int, float, bigint, decimal, byte, string, an array/Option/Result of those, a struct/enum with a "
                        "'<type>_compare' function ('#[derive(Ord)]' writes one), or a type parameter bounded by 'Ord', found %s",
                        type_name(first_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "hash") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'hash' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (!type_supports_hashing(module, arg_type, 0)) {
            ziman_error("type", arg->line, arg->col,
                        "'hash' expects a type that supports '==' with hashable parts (int, float, bool, string, bigint, decimal, byte, an array/Option/Result/struct/"
                        "enum of hashable parts, however deeply, including one that refers back to itself) or a type parameter bounded by 'Hash', found %s",
                        type_name(arg_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "error_message") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'error_message' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (find_message_function(module, arg_type) == NULL) {
            ziman_error("type", arg->line, arg->col,
                        "'error_message' expects an enum with a '<type>_message(e: Type) -> string' function "
                        "(NetError/DbError/JwtError and the like already have one), found %s", type_name(arg_type));
        }
        return type_scalar(TY_STRING);
    }

    /* `push`, `insert` and `set` shared one arm until F206, which split it three ways; F207 then
     * deleted two of the three. What is left is `set`, and the reason it is left is in
     * docs/design/full-overloading.md §13: it answers three different things depending on the type
     * it rejected -- a MutableArray is told an element is written `b[i] = value`, either set world
     * is told to use `add` -- and `#[when_unmatched]` is one line for a whole set, so the
     * declarations cannot carry those. `push` and `insert` each had one message, the same sentence
     * whatever was rejected, and that sentence is now an attribute in lib/prelude/array.zm.
     *
     * Arity is read after argument one's type here, not before it. The shared arm took
     * `expected_argc` from the name in its second statement, which is how `set(mutableSet, 2)`
     * came to be told it "takes exactly 3 argument(s)" of a form that does not exist. */

    if (strcmp(name, "set") == 0) {
        if (e->as.call.args.count < 1) {
            ziman_error("type", e->line, e->col, "'set' takes exactly 3 argument(s), found %d", e->as.call.args.count);
        }
        Expr *coll_arg = e->as.call.args.items[0];
        Type coll_type = check_expr(module, env, coll_arg);

        /* A Map declines to the resolver as a MutableMap already did. That branch only routed to
         * a shared `set`/`remove` helper that checked the arity and the key's type -- both of
         * which the resolver says anyway, bar a "(the map's key type)" parenthetical -- and it was
         * that helper's last caller, so the helper is gone too. The three refusals below are what
         * keeps the arm. */
        if (coll_type.kind == TY_MAP) {
            goto ordinary_call;
        }

        /* The two refusals below are keyed on the type that was rejected rather than stating the
         * type rule, and they are the reason `set` stays arm-handled where `push` and `insert` do
         * not: `#[when_unmatched]` is one line for a whole set and cannot say a different thing per
         * argument. Both are measured messages, so they are kept and the conversion is not
         * attempted -- see docs/design/full-overloading.md §13. */
        if (coll_type.kind == TY_SET) {
            ziman_error("type", e->line, e->col, "a set has no 'set' -- use add(set, item) to add an item");
        }
        if (coll_type.kind == TY_STRUCT && coll_type.struct_name != NULL) {
            /* An element write is `b[i] = v`, which reads better than a three-argument call and is
             * the same sugar an array gets; a copying `set` would read as though it left the
             * container alone. */
            if (strcmp(coll_type.struct_name, "MutableArray") == 0) {
                ziman_error("type", e->line, e->col,
                            "a MutableArray element is written with 'b[i] = value', not with 'set' (which "
                            "copies an array, and a MutableArray is not copied)");
            }
            if (strcmp(coll_type.struct_name, "MutableSet") == 0) {
                ziman_error("type", e->line, e->col, "a set has no 'set' -- use add(set, item) to add an item");
            }
            /* F200 gave `set` a MutableMap declaration, and the shared arm claimed the name before
             * the declaration could be reached. The arm declines every shape it does not refuse:
             * a MutableMap, a Map (since the Map member became a declaration), and a `Mutable<T>`
             * cell, whose `set` was `mutable_set` until 2026-10-22. */
            if (strcmp(coll_type.struct_name, "MutableMap") == 0 ||
                strcmp(coll_type.struct_name, "Mutable") == 0) {
                goto ordinary_call;
            }
        }
        if (coll_type.kind != TY_ARRAY) {
            ziman_error("type", coll_arg->line, coll_arg->col,
                        "argument 1 of 'set' expects an array, a Map, a MutableMap or a Mutable cell, found %s",
                        type_name(coll_type));
        }

        if (e->as.call.args.count != 3) {
            ziman_error("type", e->line, e->col, "'set' takes exactly 3 argument(s), found %d", e->as.call.args.count);
        }
        Expr *idx_arg = e->as.call.args.items[1];
        Type idx_type = check_expr(module, env, idx_arg);
        if (idx_type.kind != TY_INT) {
            ziman_error("type", idx_arg->line, idx_arg->col, "argument 2 of 'set' expects int, found %s", type_name(idx_type));
        }
        Expr *value_arg = e->as.call.args.items[2];
        Type value_type = check_expr(module, env, value_arg);
        require_not_void(value_arg, "as an array element");
        if (!type_equals(value_type, *coll_type.elem)) {
            ziman_error("type", value_arg->line, value_arg->col,
                        "argument 3 of 'set' expects %s (the array's element type), found %s",
                        type_name(*coll_type.elem), type_name(value_type));
        }
        return coll_type;
    }

    /* `is_Some(o)`, `is_None(o)` and `unwrap_Some(o)`: Option's own helpers without the `Option.`, rewritten in
     * place into the enum call they abbreviate (so the interpreter never sees these names). */
    if (strcmp(name, "is_Some") == 0 || strcmp(name, "is_None") == 0 || strcmp(name, "unwrap_Some") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        int is_unwrap = name[0] == 'u';
        const char *variant = strstr(name, "None") != NULL ? "None" : "Some";
        Expr *rewritten = mk_enum_call_expr("Option", is_unwrap ? "unwrap_" : "is_", variant, e->as.call.args.items[0], e->line, e->col);
        *e = *rewritten;
        return check_enum_access(module, env, e);
    }

    /* `$is_variant(x, "V")` / `$unwrap_variant(x, "V")`, written by the parser for `if V(y) = x { ... }`: rewritten in
     * place to Result's is_ok/unwrap/unwrap_err, or to the enum's own `is_V`/`unwrap_V` helper, from the type of `x`. */
    if (strcmp(name, "$is_variant") == 0 || strcmp(name, "$unwrap_variant") == 0) {
        int is_test = strcmp(name, "$is_variant") == 0;
        Expr *arg = e->as.call.args.items[0];
        const char *variant = e->as.call.args.items[1]->as.string_val;
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind == TY_RESULT) {
            int is_ok_variant = strcmp(variant, "Ok") == 0;
            if (!is_ok_variant && strcmp(variant, "Err") != 0) {
                ziman_error("type", e->line, e->col, "a Result has variants 'Ok' and 'Err', not '%s'", variant);
            }
            Expr *inner = mk_call_expr(is_test ? "is_ok" : (is_ok_variant ? "unwrap" : "unwrap_err"), arg, e->line, e->col);
            if (is_test && !is_ok_variant) {
                Expr *neg = expr_new(EXPR_UNARY, e->line, e->col);
                neg->as.unary.op = OP_NOT;
                neg->as.unary.operand = inner;
                *e = *neg;
            } else {
                *e = *inner;
            }
            return check_expr(module, env, e);
        }
        if (arg_type.kind != TY_ENUM) {
            ziman_error("type", arg->line, arg->col, "'if %s ... = ...' needs an enum or a Result on the right, found %s", variant, type_name(arg_type));
        }
        EnumDecl *decl = find_enum(module, arg_type.enum_name);
        int known = 0;
        for (int i = 0; decl != NULL && i < decl->variants.count; i++) {
            if (strcmp(decl->variants.items[i].name, variant) == 0) known = 1;
        }
        if (!known) {
            ziman_error("type", e->line, e->col, "enum '%s' has no variant '%s'", arg_type.enum_name, variant);
        }
        Expr *rewritten = mk_enum_call_expr(arg_type.enum_name, is_test ? "is_" : "unwrap_", variant, arg, e->line, e->col);
        *e = *rewritten;
        return check_enum_access(module, env, e);
    }

    if (strcmp(name, "is_ok") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'is_ok' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_RESULT) {
            ziman_error("type", arg->line, arg->col, "'is_ok' expects a Result, found %s", type_name(arg_type));
        }
        return type_scalar(TY_BOOL);
    }

    if (strcmp(name, "unwrap") == 0 || strcmp(name, "unwrap_err") == 0) {
        int is_unwrap = strcmp(name, "unwrap") == 0;
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (is_unwrap && arg_type.kind == TY_ENUM && strcmp(arg_type.enum_name, "Option") == 0 && arg_type.type_args.count == 1) {
            /* `unwrap(option)` is `Option.unwrap_Some(option)`: rewritten in place, the argument already checked */
            Expr *rewritten = mk_enum_call_expr("Option", "unwrap_", "Some", arg, e->line, e->col);
            *e = *rewritten;
            return arg_type.type_args.items[0];
        }
        if (arg_type.kind != TY_RESULT) {
            ziman_error("type", arg->line, arg->col, "'%s' expects a Result, found %s", name, type_name(arg_type));
        }
        return is_unwrap ? *arg_type.elem : *arg_type.elem_err;
    }

    if (strcmp(name, "ok") == 0 || strcmp(name, "err") == 0) {
        /* Reachable only when nothing gave check_ok_err_call() (via
         * check_expr_expecting) an expected Result<T, E> to work with --
         * e.g. `print(ok(5))` or a bare `ok(5)` statement. There's no way
         * to infer the type this would need without that context. */
        ziman_error("type", e->line, e->col,
                    "cannot infer the type of '%s(...)' here; it takes its type from the context, so it needs one: "
                    "a val/var with an explicit Result<T, E> annotation, a struct literal field, a 'with' override, "
                    "a function's returned value, or an argument whose parameter type says which Result it is",
                    name);
    }

    if (strcmp(name, "channel_new") == 0) {
        /* Same reasoning as ok/err above, via check_channel_new_call. */
        ziman_error("type", e->line, e->col,
                    "cannot infer the type of 'channel_new()' here; it takes its type from the context, so it needs "
                    "one: a val/var with an explicit Channel<T> annotation, a struct literal field, a 'with' "
                    "override, a function's returned value, or an argument whose parameter type says which Channel "
                    "it is");
    }

    if (strcmp(name, "Some") == 0) {
        /* Reachable only when nothing gave check_bare_option_call() (via check_expr_expecting) an expected
         * Option<T> to work with -- e.g. `print(Some(5))` or a bare `Some(5)` statement. `Some(x)`'s
         * argument says what T would be, but not that an Option is wanted here at all: `print` takes
         * anything, so there is no second reading of `Some(5)` to prefer. Inside another `Some`, inside
         * `ok`/`err`, and as an argument to a generic function it now works -- each of those knows what it
         * wants and passes it inward (see check_bare_option_call, check_ok_err_call and
         * infer_call_type_args's bare_some_payload_type). */
        ziman_error("type", e->line, e->col,
                    "cannot infer the type of 'Some(...)' here; it takes its type from the context, so it needs one: "
                    "a val/var with an explicit Option<T> annotation, a struct literal field, a 'with' override, a "
                    "function's returned value, or an argument -- including a generic one, whose type argument this "
                    "can itself bind");
    }

    if (strcmp(name, "send") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'send' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "argument 1 of 'send' expects a Channel, found %s", type_name(chan_type));
        }
        /* check_expr_expecting, not check_expr: the channel's element type is exactly the context an
         * `Option`/`Result`/`channel_new()` argument needs, and without it `send(replies, Some(n))`
         * could not be written at all even with `replies` declared `Channel<Option<int>>` -- the
         * error told the caller to find "an argument whose parameter type says which Option it is",
         * which this is. Found 2026-10-16 writing tests/lang/actor_owns_mutable_state.zm, where a
         * reply channel of `Option<int>` is the natural shape and every send into it had to be
         * spelled `Option<int>.Some(...)`. The slot form, since it may rewrite the argument node. */
        Type val_type = check_expr_expecting(module, env, &e->as.call.args.items[1], *chan_type.elem);
        Expr *val_arg = e->as.call.args.items[1];
        require_not_void(val_arg, "as a channel value");
        if (!type_equals(val_type, *chan_type.elem)) {
            ziman_error("type", val_arg->line, val_arg->col,
                        "argument 2 of 'send' expects %s (the channel's element type), found %s",
                        type_name(*chan_type.elem), type_name(val_type));
        }
        /* The value's own type, as well as the channel's declared element type (checked once where
         * the Channel type is validated). They differ for exactly one thing: a lambda that captured a
         * builder has Type.func_captures_thread_bound set, which `Channel<fn() -> string>` as written
         * by the programmer cannot. Without this, `send(ch, fn() { append(sb, "x") })` type-checked
         * and failed on the receiving thread instead. */
        reject_thread_bound(module, val_type, "sent over a channel", val_arg->line, val_arg->col);
        return type_scalar(TY_VOID);
    }

    if (strcmp(name, "try_send") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'try_send' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "argument 1 of 'try_send' expects a Channel, found %s", type_name(chan_type));
        }
        /* check_expr_expecting, not check_expr: the channel's element type is exactly the context an
         * `Option`/`Result`/`channel_new()` argument needs, and without it `send(replies, Some(n))`
         * could not be written at all even with `replies` declared `Channel<Option<int>>` -- the
         * error told the caller to find "an argument whose parameter type says which Option it is",
         * which this is. Found 2026-10-16 writing tests/lang/actor_owns_mutable_state.zm, where a
         * reply channel of `Option<int>` is the natural shape and every send into it had to be
         * spelled `Option<int>.Some(...)`. The slot form, since it may rewrite the argument node. */
        Type val_type = check_expr_expecting(module, env, &e->as.call.args.items[1], *chan_type.elem);
        Expr *val_arg = e->as.call.args.items[1];
        require_not_void(val_arg, "as a channel value");
        if (!type_equals(val_type, *chan_type.elem)) {
            ziman_error("type", val_arg->line, val_arg->col,
                        "argument 2 of 'try_send' expects %s (the channel's element type), found %s",
                        type_name(*chan_type.elem), type_name(val_type));
        }
        reject_thread_bound(module, val_type, "sent over a channel", val_arg->line, val_arg->col); /* see 'send' */
        return type_scalar(TY_BOOL);
    }

    if (strcmp(name, "receive") == 0 || strcmp(name, "try_receive") == 0 || strcmp(name, "channel_len") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "'%s' expects a Channel, found %s", name, type_name(chan_type));
        }
        if (strcmp(name, "channel_len") == 0) return type_scalar(TY_INT);
        if (strcmp(name, "try_receive") == 0) {
            Type opt = type_enum("Option");
            type_list_push(&opt.type_args, *chan_type.elem);
            return opt;
        }
        return *chan_type.elem;
    }

    if (strcmp(name, "channel_close") == 0 || strcmp(name, "receive_option") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "'%s' expects a Channel, found %s", name, type_name(chan_type));
        }
        if (strcmp(name, "channel_close") == 0) return type_scalar(TY_VOID);
        Type opt = type_enum("Option");
        type_list_push(&opt.type_args, *chan_type.elem);
        return opt;
    }

    if (strcmp(name, "receive_timeout") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'receive_timeout' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "argument 1 of 'receive_timeout' expects a Channel, found %s",
                        type_name(chan_type));
        }
        Expr *millis_arg = e->as.call.args.items[1];
        Type millis_type = check_expr(module, env, millis_arg);
        if (millis_type.kind != TY_INT) {
            ziman_error("type", millis_arg->line, millis_arg->col,
                        "argument 2 of 'receive_timeout' expects an int (milliseconds), found %s", type_name(millis_type));
        }
        Type opt = type_enum("Option");
        type_list_push(&opt.type_args, *chan_type.elem);
        return opt;
    }

    if (strcmp(name, "select2") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'select2' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *a_arg = e->as.call.args.items[0];
        Type a_type = check_expr(module, env, a_arg);
        if (a_type.kind != TY_CHANNEL) {
            ziman_error("type", a_arg->line, a_arg->col, "argument 1 of 'select2' expects a Channel, found %s", type_name(a_type));
        }
        Expr *b_arg = e->as.call.args.items[1];
        Type b_type = check_expr(module, env, b_arg);
        if (b_type.kind != TY_CHANNEL) {
            ziman_error("type", b_arg->line, b_arg->col, "argument 2 of 'select2' expects a Channel, found %s", type_name(b_type));
        }
        Type selected = type_enum("Selected");
        type_list_push(&selected.type_args, *a_type.elem);
        type_list_push(&selected.type_args, *b_type.elem);
        return selected;
    }

ordinary_call:;
    /* Stage 3 of docs/design/full-overloading.md. `find_function` answers with the *first*
     * declaration of a name, which was always right while a name had one declaration and is a
     * coin toss once it has two. When the name is an overload set, the resolver chooses instead,
     * and what it chooses lands on `e->call_fn` a few lines below -- which the interpreter
     * already honours (interpreter.c's `e->call_fn != NULL` branch, built for speed and now
     * load-bearing for correctness too).
     *
     * A shadowing user function still wins outright: F164 made a program's own top-level `fn`
     * shadow a builtin, and an overload set is not a reason to take that back. */
    FunctionDecl *fn = shadow_fn != NULL ? shadow_fn : find_function(module, name);
    if (shadow_fn == NULL && fn != NULL && overload_set_size(module, name) > 1) {
        fn = overload_pick(module, env, e, name);
    }
    /* **The prelude's own visibility, enforced from outside it (2026-10-22).**
     *
     * The prelude is merged bare into every program, so a call into it resolves as an ordinary
     * same-module call and there is no import boundary for `pub` to be checked at -- which meant
     * `pub` said nothing about reachability here, only about what `--api builtins` advertises. A
     * non-`pub` prelude declaration was callable by anyone, and F172 relied on that: it left
     * `array_order_of` bare so the name would be hidden from completion *and still callable*,
     * which is the middle state this removes.
     *
     * The rule now matches what the modifier means everywhere else: `pub` is reachable, `internal`
     * reaches its own package (the bundled library, so lib/ may call one and a program may not),
     * and a bare declaration is the prelude's own. `g_checking_program_body` is the side of the
     * boundary -- it is false only while a prelude declaration's own body is being checked -- and
     * `module->is_corelib` is what makes `internal` mean package rather than nothing.
     *
     * Every overload set is uniformly visible, checked when this went in, so one test here is
     * enough and the resolver needs no filter of its own. If that ever stops being true, a set
     * with one `pub` member and one without would resolve to the hidden one and report it here,
     * which is the right failure but the wrong message. */
    if (fn != NULL && fn->source_file == NULL && g_checking_program_body
        && fn->visibility != VIS_PUBLIC
        && !(fn->visibility == VIS_INTERNAL && module->is_corelib)) {
        ziman_error("type", e->line, e->col,
                    "function '%s' is %s -- it is part of the prelude's own machinery, not its "
                    "vocabulary, and nothing outside the prelude may call it",
                    name,
                    fn->visibility == VIS_INTERNAL
                        ? "internal to the bundled library's own package"
                        : "private to the file that declares it");
    }
    lint_deprecated_use(fn, e->line, e->col);
    if (fn == NULL) {
        /* Inside a #[derive] expansion, a missing <type>_to_json / _from_json
         * / _to_sql / _from_sql means a field's type provides no conversion
         * -- the one thing a derive can't supply for it -- so say how to. */
        static const char *const suffixes[] = {"_to_json", "_from_json", "_to_sql", "_from_sql"};
        if (ziman_error_context() != NULL) {
            size_t n = strlen(name);
            for (size_t k = 0; k < sizeof suffixes / sizeof suffixes[0]; k++) {
                size_t sl = strlen(suffixes[k]);
                if (n > sl && strcmp(name + n - sl, suffixes[k]) == 0) {
                    ziman_error("type", e->line, e->col,
                                "call to undefined function '%s' -- a derived conversion needs one for every field type: "
                                "give that type its own #[derive(%s)] (or write '%s' by hand and import it with 'use')",
                                name, (k < 2) ? "Json" : "Row", name);
                }
            }
        }
        const char *generic_type = matching_generic_type(name);
        if (generic_type != NULL) {
            const char *how = generic_type_construction(generic_type);
            ziman_error("type", e->line, e->col,
                        "call to undefined function '%s' -- '%s' is a built-in generic type, not a function: %s",
                        name, generic_type, how);
        }
        for (size_t k = 0; MAP_MEMBERSHIP_SYNONYMS[k] != NULL; k++) {
            if (strcmp(name, MAP_MEMBERSHIP_SYNONYMS[k]) == 0) {
                /* Named ahead of the fuzzy suggestion below on purpose: "has" is one edit from
                 * the real builtin "hash" (an unrelated function), which the LLM-generation
                 * benchmark's own re-validation showed the fuzzy search suggesting as a
                 * misleading "fix" -- a case where a close-by name is the wrong answer, not
                 * just a missing one. */
                ziman_error("type", e->line, e->col,
                            "call to undefined function '%s' -- a Map or Set has no membership method: "
                            "use 'contains(m, key)' to test for one, or 'get(m, key)' (an Option<V>) "
                            "to look up the value at the same time", name);
            }
        }
        const char *as_syntax = NULL;
        const char *as_syntax_why = NULL;
        if (renamed_to_syntax(name, &as_syntax, &as_syntax_why)) {
            /* A shape of its own, separate from `renamed_builder_fn`'s "lost the prefix": these
             * names did not get a shorter name, they stopped being calls, so the answer is the
             * notation and the reason it is not a function any more. */
            ziman_error("type", e->line, e->col, "'%s' is gone -- write '%s' instead: %s", name, as_syntax,
                        as_syntax_why);
        }
        const char *renamed_fn = renamed_builder_fn(name);
        if (renamed_fn != NULL) {
            /* One sentence for a table that holds several kinds of rename, so it states the rule
             * they all followed rather than the details of any one: the old names said which
             * *world* the first argument came from, and the argument already says that. Naming the
             * four bare questions here was true of F104's entries and false of the ones added since
             * (`mapb_get` is `get`, `mutable_string_append` is `append`), which is exactly the
             * kind of half-updated message this project keeps paying for.
             *
             * **And it had already happened to the clause this used to open with**, "a builder is
             * a mutable collection": `bytes_concat` and `bytes_index_of` are a `[byte]`, which is
             * an array and not a builder, so the sentence had been false for two entries before
             * `result_map_err` arrived on 2026-10-22 and made it false for a third. Dropped rather
             * than extended -- the prefix rule is the whole of what these renames share, and the
             * rest was a fact about the first batch. */
            ziman_error("type", e->line, e->col,
                        "'%s' is now '%s': these names lost the prefix that said which world the first argument "
                        "came from -- the argument says it",
                        name, renamed_fn);
        }
        const char *suggestion = suggest_function_name(module, name);
        if (suggestion != NULL) {
            ziman_error("type", e->line, e->col, "call to undefined function '%s' -- did you mean '%s'?", name, suggestion);
        }
        ziman_error("type", e->line, e->col, "call to undefined function '%s'", name);
    }
    /* A `builtin fn` is bodiless, and `interpreter.c` checks `call_fn` *before* the builtin id,
     * so recording one here would send a call into `call_function` with nothing to run. Step 3 of
     * docs/design/full-overloading.md §12 makes builtins reach this point for the first time --
     * the resolver type-checks them against their declarations while the interpreter keeps
     * dispatching them by id, which is why the conversion needs no runtime change at all. */
    if (!fn->is_builtin) e->call_fn = fn;
    canonicalize_named_args(e, fn);
    TypeList *type_args = &e->as.call.type_args;
    /* A generic call with no type arguments written infers them from the checked arguments (C3); one written
     * explicitly is used as given, unchanged from before. */
    if (fn->type_params.count == 0 && type_args->count > 0) {
        ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", name, type_args->count);
    }
    if (fn->type_params.count > 0 && type_args->count == 0) {
        e->as.call.type_args = infer_call_type_args(module, env, e, fn);
        type_args = &e->as.call.type_args;
    } else if (fn->type_params.count != type_args->count) {
        ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", name, fn->type_params.count, type_args->count);
    }
    for (int i = 0; i < type_args->count; i++) {
        check_type_well_formed(module, type_args->items[i], e->line, e->col);
    }
    check_type_args_satisfy_bounds(module, &fn->type_params, type_args, e->line, e->col);

    int is_variadic = fn->params.count > 0 && fn->params.items[fn->params.count - 1].is_variadic;
    int fixed_count = is_variadic ? fn->params.count - 1 : fn->params.count;

    if (is_variadic) {
        /* `#[at_least_one]` raises the floor by one. An empty array still passes as the spread
         * form -- that is one argument, written out, and it is how a caller says "whatever is in
         * this list, which may be nothing" on purpose rather than by omission. */
        int min_args = fixed_count + (fn->variadic_at_least_one ? 1 : 0);
        if (e->as.call.args.count < min_args) {
            ziman_error("type", e->line, e->col,
                        "function '%s' expects at least %d argument(s), found %d%s", name, min_args,
                        e->as.call.args.count,
                        fn->variadic_at_least_one
                            ? " -- its last parameter is variadic but needs at least one value, since "
                              "the call would otherwise do nothing (pass an array to spread a list that "
                              "may be empty)"
                            : "");
        }
    } else if (e->as.call.args.count != fn->params.count) {
        ziman_error("type", e->line, e->col, "function '%s' expects %d argument(s), found %d", name, fn->params.count, e->as.call.args.count);
    }
    for (int i = 0; i < fixed_count; i++) {
        Type expected = substitute_type(fn->params.items[i].type, &fn->type_params, type_args);
        Type arg_type;
        if (!check_value_against_param_type(module, env, &e->as.call.args.items[i], expected, &arg_type,
                                            "as an argument")) {
            Expr *arg = e->as.call.args.items[i];
            if (is_option_type(expected)) {
                ziman_error("type", arg->line, arg->col,
                            "argument %d of '%s' expects %s or %s, found %s",
                            i + 1, name, type_name(expected), type_name(option_elem_type(expected)), type_name(arg_type));
            }
            if (expected.kind == TY_RESULT) {
                Type ok_type = *expected.elem;
                Type err_type = *expected.elem_err;
                if (type_equals(ok_type, err_type)) {
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' expects %s or %s, found %s",
                                i + 1, name, type_name(expected), type_name(ok_type), type_name(arg_type));
                }
                ziman_error("type", arg->line, arg->col,
                            "argument %d of '%s' expects %s, %s, or %s, found %s",
                            i + 1, name, type_name(expected), type_name(ok_type), type_name(err_type), type_name(arg_type));
            }
            ziman_error("type", arg->line, arg->col,
                        "argument %d of '%s' expects %s, found %s",
                        i + 1, name, type_name(expected), type_name(arg_type));
        }
    }
    /* The trailing (variadic) arguments -- either zero or more individual
     * values of the element type, collected into an array at the call
     * site (see call_function), or, when there's exactly one trailing
     * argument, an already-built array of that element type passed
     * through directly. Which of the two a given call means is decided
     * purely by that argument's own static type -- sound with no
     * ambiguity, since Ziman has no implicit coercion and no
     * overloading to confuse it with. */
    if (is_variadic) {
        Type elem_type = substitute_type(fn->params.items[fixed_count].type, &fn->type_params, type_args);
        int tail_count = e->as.call.args.count - fixed_count;
        if (tail_count == 1) {
            Expr **arg_slot = &e->as.call.args.items[fixed_count];
            Expr *arg = *arg_slot;
            /* A lone trailing argument is ambiguous -- one element, or an already-built array of
             * them -- and the choice is made from the argument's own type, which is why it cannot
             * simply be offered `elem_type` the way the several-argument case below can. Three of
             * the context-needing forms resolve the ambiguity by what they are: a bare `Some(x)`,
             * `None`, `ok(x)`, `err(e)` and `channel_new()` are never arrays, so they can only be
             * the single-element reading. A value block is left alone deliberately -- `f(if c { a }
             * else { b })` spreads an array today and must keep doing so -- and so is an empty
             * array literal, which is genuinely ambiguous (an empty spread, or one empty-array
             * element) and keeps the error it already gives.
             *
             * So a lone trailing `if`/`switch` whose arms cannot type themselves
             * (`push(out, if c { Some(1) } else { None })`) is the one position left that needs
             * the qualified spelling, and it is not a thing an extra branch here can fix: the
             * spread-or-single decision is made *from* the argument's type, and the arms error
             * while being typed, before any fallback could run. Deciding it the other way --
             * always offering the element type -- would take the array-spread reading away from a
             * form that has it today. */
            int definitely_one = check_bare_option_call(module, env, arg, elem_type) ||
                                 check_ok_err_call(module, env, arg, elem_type) ||
                                 check_channel_new_call(module, env, arg, elem_type);
            if (definitely_one) {
                (*arg_slot)->resolved_type = elem_type;
            } else {
                Type arg_type = check_expr(module, env, arg);
                if (arg_type.kind == TY_VOID && arg->kind == EXPR_VALUE_BLOCK) {
                    /* The one case above, with a message that says what to do about it rather
                     * than "cannot use a void value as an argument", which is true and useless. */
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' is an 'if'/'switch' whose arms carry no type of their own, "
                                "and it is the only trailing argument -- so it could be one %s or an array of them "
                                "to spread, and which one is read from its type. Give the arms a type "
                                "('Option<T>.Some(x)' / 'Option<T>.None' rather than bare 'Some(x)' / 'None'), or "
                                "bind it to a 'val' with an annotation first",
                                fixed_count + 1, name, type_name(elem_type));
                }
                require_not_void(arg, "as an argument");
                if (type_equals(arg_type, type_array(elem_type))) {
                    e->as.call.variadic_tail_is_array_spread = 1;
                } else if (!type_equals(arg_type, elem_type)) {
                    Type out_type;
                    /* The bare-value auto-wrap a fixed `Option<T>`/`Result<T, E>` parameter gives,
                     * which the tail never offered: `f(4)` where the element is an `Option<int>`. */
                    if (!check_value_against_param_type(module, env, arg_slot, elem_type, &out_type, "as an argument")) {
                        ziman_error("type", arg->line, arg->col,
                                    "argument %d of '%s' expects %s or %s, found %s",
                                    fixed_count + 1, name, type_name(elem_type), type_name(type_array(elem_type)), type_name(arg_type));
                    }
                }
            }
        } else {
            /* Several trailing arguments are individual element values and nothing else, so each
             * one gets exactly what a fixed parameter's argument gets -- the declared type offered
             * to a value that needs one, and the same auto-wrap. Before this they went through a
             * bare `check_expr`, so a variadic `Option<T>` parameter refused `Some(x)`, `None` and
             * a bare `T` that the same parameter would have taken had it not been variadic. */
            for (int i = fixed_count; i < e->as.call.args.count; i++) {
                Expr **arg_slot = &e->as.call.args.items[i];
                Type arg_type;
                if (!check_value_against_param_type(module, env, arg_slot, elem_type, &arg_type, "as an argument")) {
                    ziman_error("type", (*arg_slot)->line, (*arg_slot)->col,
                                "argument %d of '%s' expects %s, found %s",
                                i + 1, name, type_name(elem_type), type_name(arg_type));
                }
            }
        }
    }
    return substitute_type(fn->return_type, &fn->type_params, type_args);
}

/* `ok(x)`/`err(e)` can't fully infer their own type the way every other
 * call can -- `ok(x)`'s argument only tells us the success type, never the
 * error type (and vice versa for `err`). Legal wherever an expected type
 * is already available: a val/var's own declared type, a struct literal
 * field or `with` override (via check_expr_expecting), or a function's
 * declared return type (STMT_RETURN). Returns 1 and sets `e->resolved_type`
 * if `e` is exactly `ok(...)`/`err(...)` and `expected` is a Result<T, E>;
 * returns 0 otherwise, meaning the caller should fall back to plain
 * check_expr(). */
static int check_ok_err_call(Module *module, Env *env, Expr *e, Type expected) {
    if (e->kind != EXPR_CALL) return 0;
    int is_ok_call = strcmp(e->as.call.callee, "ok") == 0;
    int is_err_call = strcmp(e->as.call.callee, "err") == 0;
    if (!is_ok_call && !is_err_call) return 0;
    if (expected.kind != TY_RESULT) return 0;

    if (e->as.call.args.count != 1) {
        ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", e->as.call.callee, e->as.call.args.count);
    }
    Type want = is_ok_call ? *expected.elem : *expected.elem_err;
    /* Checked *expecting* the payload type, not bare: `ok(Some(x))` and `err(None)` are ordinary
     * things to write, and the inner value needs the same context this call just received. Passing
     * the slot rather than the pointer lets check_bare_option_call rewrite a bare `None` in place,
     * as it does everywhere else. */
    Expr **arg_slot = &e->as.call.args.items[0];
    Type arg_type = check_expr_expecting(module, env, arg_slot, want);
    Expr *arg = *arg_slot;
    require_not_void(arg, "as a Result value");

    if (!type_equals(arg_type, want)) {
        ziman_error("type", arg->line, arg->col, "'%s' expects %s, found %s", e->as.call.callee, type_name(want), type_name(arg_type));
    }
    e->resolved_type = expected;
    return 1;
}

/* `channel_new()` has the same "can't infer its own type" problem `ok`/
 * `err` and empty array literals have, for the same reason: the call
 * itself (zero arguments) carries no information at all about what it's a
 * channel of. Same set of legal positions as check_ok_err_call. Returns 1
 * and sets `e->resolved_type` if `e` is exactly `channel_new()` and
 * `expected` is a Channel<T>; returns 0 otherwise. */
static int check_channel_new_call(Module *module, Env *env, Expr *e, Type expected) {
    if (e->kind != EXPR_CALL) return 0;
    if (strcmp(e->as.call.callee, "channel_new") != 0) return 0;
    if (expected.kind != TY_CHANNEL) return 0;
    if (e->as.call.args.count > 1) {
        ziman_error("type", e->line, e->col, "'channel_new' takes at most 1 argument (a capacity), found %d", e->as.call.args.count);
    }
    if (e->as.call.args.count == 1) {
        Type cap = check_expr(module, env, e->as.call.args.items[0]);
        if (cap.kind != TY_INT) {
            ziman_error("type", e->as.call.args.items[0]->line, e->as.call.args.items[0]->col,
                        "the capacity of 'channel_new' must be an int, found %s", type_name(cap));
        }
    }
    e->resolved_type = expected;
    return 1;
}

/* `mutable([])` where a `MutableArray<T>` is expected. `[]` is the one literal that carries no type
 * of its own, and the annotation beside it carries exactly the missing part, so the expectation has
 * to reach the argument -- the same reason ok/err, channel_new and a bare `Some`/`None` have hooks
 * here. Without it the error told an author who had written `MutableArray<int>` one phrase to the
 * left to "add a type annotation". Only the array case needs this: `Map<K, V> {}` and `Set<T> {}`
 * write their own types, so `mutable` over either already worked.
 *
 * Rewritten onto `$mutable_array_lit` with no arguments -- the literal path added for
 * `mutable([1, 2])`, which already answers an empty builder when handed no elements -- so there is
 * no runtime for this beyond what that path already does. */
static int check_mutable_empty_call(Module *module, Env *env, Expr *e, Type expected) {
    (void)module;
    (void)env;
    if (e->kind != EXPR_CALL || e->as.call.callee == NULL) return 0;
    if (strcmp(e->as.call.callee, "mutable") != 0) return 0;
    if (expected.kind != TY_STRUCT || expected.struct_name == NULL) return 0;
    if (strcmp(expected.struct_name, "MutableArray") != 0 || expected.type_args.count != 1) return 0;
    if (e->as.call.args.count != 1) return 0;
    Expr *arg = e->as.call.args.items[0];
    if (arg->kind != EXPR_ARRAY_LIT || arg->as.array_lit.elements.count != 0) return 0;
    arg->resolved_type = type_array(expected.type_args.items[0]);
    e->as.call.rewritten_from = "mutable";
    e->as.call.callee = ziman_xstrdup("$mutable_array_lit");
    e->as.call.args.count = 0;
    e->resolved_type = expected;
    return 1;
}

/* `Some(x)`/`None` used bare -- not qualified `Option<T>.Some(x)`/`Option<T>.None` -- where an `Option<T>` is
 * already expected: the same set of legal positions ok/err/channel_new get (see check_ok_err_call), and for the
 * same reason -- `None` carries no `T` at all, and a bare `Some(x)`'s argument only pins down `T` when there is
 * nowhere else for it to come from either. `Some` and `None` can never be a real function or variable name (both
 * start uppercase, which only a struct, enum, or type-parameter name may -- see require_lower_start), so there is
 * no shadowing to consider: this always wins over check_call/plain identifier resolution once `expected` is an
 * Option<T>. Returns 1 and sets `e->resolved_type` if `e` is exactly one of the two forms and `expected` is an
 * Option<T>; returns 0 otherwise, meaning the caller should fall back to plain check_expr() (an `Option<T>.Some(x)`/
 * `Option<T>.None` already written out reaches check_enum_access unaffected, as does the bare-value auto-wrap of an
 * ordinary `T` via check_value_against_param_type). */
static int check_bare_option_call(Module *module, Env *env, Expr *e, Type expected) {
    if (!is_option_type(expected)) return 0;
    Type elem = option_elem_type(expected);
    if (e->kind == EXPR_IDENT && strcmp(e->as.ident_name, "None") == 0) {
        /* Rewritten into a zero-argument call the interpreter already knows how to evaluate (same
         * `strcmp(callee, ...)` dispatch as `ok`/`err`/`Some`) rather than special-cased by name in
         * EXPR_IDENT's own (hot -- every identifier reference goes through it) evaluation path; see
         * `unit`'s identical EXPR_IDENT -> EXPR_UNIT rewrite for the same "resolve once here, zero
         * runtime cost after" reasoning, one node kind up. */
        Expr *rewritten = expr_new(EXPR_CALL, e->line, e->col);
        rewritten->as.call.callee = "None";
        *e = *rewritten;
        e->resolved_type = expected;
        return 1;
    }
    if (e->kind != EXPR_CALL || e->as.call.type_args.count > 0 || strcmp(e->as.call.callee, "Some") != 0) return 0;
    if (e->as.call.args.count != 1) {
        ziman_error("type", e->line, e->col, "'Some' takes exactly 1 argument, found %d", e->as.call.args.count);
    }
    /* Same as ok/err above: the element type goes inward, so `Some(Some(4))` and `Some(ok(x))`
     * work rather than needing the qualified spelling the old error message told you to write. */
    Expr **arg_slot = &e->as.call.args.items[0];
    Type arg_type = check_expr_expecting(module, env, arg_slot, elem);
    Expr *arg = *arg_slot;
    require_not_void(arg, "as an Option value");
    if (!type_equals(arg_type, elem)) {
        ziman_error("type", arg->line, arg->col, "'Some' expects %s, found %s", type_name(elem), type_name(arg_type));
    }
    e->resolved_type = expected;
    return 1;
}

static Type check_cast(Module *module, Env *env, Expr *e) {
    Type from = check_expr(module, env, e->as.cast.operand);
    require_not_void(e->as.cast.operand, "as a cast operand");
    Type to = e->as.cast.target_type;
    int numeric_from = from.kind == TY_INT || from.kind == TY_FLOAT || from.kind == TY_BIGINT || from.kind == TY_DECIMAL;
    int numeric_to = to.kind == TY_INT || to.kind == TY_FLOAT || to.kind == TY_BIGINT || to.kind == TY_DECIMAL;
    /* byte only casts to/from int, kept separate from the numeric web
     * above rather than folded into it -- byte isn't a number to compute
     * with (see ast.h's TY_BYTE comment), so byte<->float/bigint/decimal
     * were never asked for and would just be unused interpreter.c
     * branches. int as byte saturates to [0, 255] rather than wrapping,
     * matching bignum_to_i64_saturating's "clamp, don't silently wrap"
     * precedent for narrowing a wider numeric type. */
    int byte_int_cast = (from.kind == TY_BYTE && to.kind == TY_INT) || (from.kind == TY_INT && to.kind == TY_BYTE);
    if (!(numeric_from && numeric_to) && !byte_int_cast) {
        ziman_error("type", e->line, e->col, "cannot cast %s to %s (only casts between int/float/bigint/decimal, or int/byte, are supported)", type_name(from), type_name(to));
    }
    return to;
}

static Type check_array_literal(Module *module, Env *env, Expr *e) {
    ExprList *elements = &e->as.array_lit.elements;
    if (elements->count == 0) {
        ziman_error("type", e->line, e->col,
                    "cannot infer the element type of an empty array literal; add a type annotation (e.g. val x: [int] = [])");
    }
    Type elem_type = check_expr(module, env, elements->items[0]);
    require_not_void(elements->items[0], "as an array element");
    for (int i = 1; i < elements->count; i++) {
        Expr *el = elements->items[i];
        Type t = check_expr(module, env, el);
        if (!type_equals(t, elem_type)) {
            ziman_error("type", el->line, el->col,
                        "array elements must all have the same type: element 0 is %s, element %d is %s",
                        type_name(elem_type), i, type_name(t));
        }
    }
    return type_array(elem_type);
}

/* The loop binders currently in scope, innermost last, so an index
 * expression can tell whether its index is the ELEMENT binder of a
 * `for x in arr` over that very array. `array` is the iterated
 * identifier for a single-binder array loop, NULL for every other kind
 * of binder (a range index, or a two-binder loop's variables) -- those
 * still get an entry so they shadow an outer element binder of the same
 * name. */
typedef struct {
    const char *binder;
    const char *array;
} LoopBinder;
enum { MAX_LOOP_BINDERS = 64 };
static LoopBinder g_loop_binders[MAX_LOOP_BINDERS];
static int g_loop_binder_count = 0;

static void push_loop_binder(const char *binder, const char *array) {
    if (g_loop_binder_count < MAX_LOOP_BINDERS) {
        g_loop_binders[g_loop_binder_count].binder = binder;
        g_loop_binders[g_loop_binder_count].array = array;
    }
    g_loop_binder_count++;
}

/* `for i in a { ... a[i] ... }`: in Ziman `i` is the ELEMENT here, but
 * models (and people) coming from Go/JS/Python read it as the index and
 * silently compute the wrong thing. Reject exactly this shape and say how
 * to write each intent. */
static void reject_element_used_as_index(Expr *e) {
    Expr *arr = e->as.index.array;
    Expr *idx = e->as.index.index;
    if (arr->kind != EXPR_IDENT || idx->kind != EXPR_IDENT) return;
    int n = g_loop_binder_count < MAX_LOOP_BINDERS ? g_loop_binder_count : MAX_LOOP_BINDERS;
    for (int i = n - 1; i >= 0; i--) {
        if (strcmp(g_loop_binders[i].binder, idx->as.ident_name) != 0) continue;
        if (g_loop_binders[i].array != NULL && strcmp(g_loop_binders[i].array, arr->as.ident_name) == 0) {
            ziman_error("type", idx->line, idx->col,
                        "'%s' is an element of '%s' here ('for %s in %s' binds each element, not its position), so '%s[%s]' indexes with a value; "
                        "to loop over positions write 'for %s in 0..len(%s)', or use 'for %s, item in %s' for both",
                        idx->as.ident_name, arr->as.ident_name, idx->as.ident_name, arr->as.ident_name,
                        arr->as.ident_name, idx->as.ident_name, idx->as.ident_name, arr->as.ident_name,
                        idx->as.ident_name, arr->as.ident_name);
        }
        return;
    }
}

static Type check_index(Module *module, Env *env, Expr *e) {
    reject_element_used_as_index(e);
    Type arr_type = check_expr(module, env, e->as.index.array);
    /* A MutableArray answers `b[i]` for the same reason it answers `len(b)`: reading element i is
     * one of the questions every sequence answers, and needing a different word for it -- here
     * `mutable_array_get` -- was a per-type name for a question that has nothing per-type about
     * it.
     *
     * **It is checked here and evaluated as an index, with no call anywhere (F209).** F149 made
     * this a rewrite onto `mutable_array_get`, which meant that name had to be *declared* for the
     * sugar to resolve and *advertised* so the frame a traceback printed named something a reader
     * could look up -- a function nobody had written appearing in the stack for `b[5]`. The
     * interpreter reads the handle directly now, the frame is gone, and the out-of-range message
     * is the array's own with the length named for the type it came from. */
    if (arr_type.kind == TY_STRUCT && arr_type.struct_name != NULL &&
        strcmp(arr_type.struct_name, "MutableArray") == 0) {
        Type idx_type = check_expr(module, env, e->as.index.index);
        if (idx_type.kind != TY_INT) {
            ziman_error("type", e->as.index.index->line, e->as.index.index->col,
                        "array index must be int, found %s", type_name(idx_type));
        }
        if (arr_type.type_args.count != 1) {
            ziman_error("type", e->as.index.array->line, e->as.index.array->col,
                        "cannot index into %s -- its element type is not known here", type_name(arr_type));
        }
        return arr_type.type_args.items[0];
    }
    if (arr_type.kind != TY_ARRAY) {
        /* A map is the common case here, and `m[k]` is how most languages read one. Naming the
         * alternative matters because the getter's name is not guessable: in the F28 validation run
         * one agent brute-forced six candidates (`lookup`, `at`, `value_at`, `fetch`, `find`,
         * `map_get`) after `get(m, k)` had suggested `set` -- the mutator, which is the wrong
         * direction entirely (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28). */
        if (arr_type.kind == TY_MAP) {
            ziman_error("type", e->as.index.array->line, e->as.index.array->col,
                        "a map is not indexed with '[...]' -- read one with 'get(m, key)', which answers "
                        "'Option<V>' ('Some(v)' or 'None'), and write one with 'm = set(m, key, value)'");
        }
        if (arr_type.kind == TY_SET) {
            ziman_error("type", e->as.index.array->line, e->as.index.array->col,
                        "a set is not indexed with '[...]' -- test membership with 'contains(s, value)', and add "
                        "with 's = add(s, value)'");
        }
        ziman_error("type", e->as.index.array->line, e->as.index.array->col, "cannot index into %s (not an array)", type_name(arr_type));
    }
    Type idx_type = check_expr(module, env, e->as.index.index);
    if (idx_type.kind != TY_INT) {
        ziman_error("type", e->as.index.index->line, e->as.index.index->col, "array index must be int, found %s", type_name(idx_type));
    }
    return *arr_type.elem;
}

/* Shared by struct-literal construction and `with`: does `field_name`
 * (from a FieldInit at line:col) name a real field on `decl`, and is this
 * the first time it's been named among `checked_so_far` entries? Returns
 * the matching field on success; exits on an unknown or duplicate name. */
static Param *resolve_field_init(StructDecl *decl, FieldInitList *inits, int index) {
    FieldInit *fi = &inits->items[index];
    Param *field = NULL;
    for (int j = 0; j < decl->fields.count; j++) {
        if (strcmp(decl->fields.items[j].name, fi->name) == 0) { field = &decl->fields.items[j]; break; }
    }
    if (field == NULL) {
        ziman_error("type", fi->line, fi->col, "no field '%s' on struct '%s'", fi->name, decl->name);
    }
    for (int k = 0; k < index; k++) {
        if (strcmp(inits->items[k].name, fi->name) == 0) {
            ziman_error("type", fi->line, fi->col, "field '%s' specified more than once", fi->name);
        }
    }
    return field;
}

/* Like check_expr, but tries every "can't infer its own type" case Ziman
 * has -- an empty array literal (`[]`), `ok(...)`/`err(...)`,
 * `channel_new()`, and the Option<T>/Result<T, E> bare-value auto-wrap
 * -- against `expected` first, before falling back to plain check_expr
 * for everything else (including any of those where `expected` doesn't
 * match what they need; those keep getting whatever error they'd
 * otherwise get, unchanged -- the auto-wrap case is the one exception,
 * see below). This is the same "expected type from context" idea
 * val/var declarations already use (see check_stmt's STMT_VAL/STMT_VAR
 * case) extended to the other positions where the expected type is just
 * as statically known: a struct literal field, a `with` override, and a
 * `return` value.
 *
 * Takes `Expr **e_slot` rather than `Expr *e`, unlike every other
 * check_* function, specifically so the auto-wrap case can replace the
 * expression in place (the same reason check_value_against_param_type
 * itself takes one) -- every other branch here only ever reads through
 * it, never reassigns it. On an auto-wrap mismatch, this deliberately
 * does NOT raise its own error the way check_value_against_param_type's
 * other caller (check_call) does: it returns the value's own actual
 * type instead, so each of *this* function's four callers keeps raising
 * its own already-existing, position-specific message ("field 'x' of
 * 'Y' expects...", "returned X but function is declared to return
 * Y", ...) exactly as it did before auto-wrap existed, just without the
 * richer "or T"/"or T, E" wording check_call's own version gives --
 * simpler than teaching four different messages that wording, and no
 * worse than what the mismatch case already said. */
static Type check_expr_expecting(Module *module, Env *env, Expr **e_slot, Type expected) {
    ZIMAN_STACK_CHECK();
    Expr *e = *e_slot;
    if (e->kind == EXPR_ARRAY_LIT && e->as.array_lit.elements.count == 0 && expected.kind == TY_ARRAY) {
        e->resolved_type = expected;
        return expected;
    }
    if (e->kind == EXPR_VALUE_BLOCK) {
        Type t = check_value_block_expr(module, env, e, &expected);
        e->resolved_type = t;
        return t;
    }
    if (e->kind == EXPR_LAMBDA && expected.kind == TY_FUNC) {
        Type t = check_lambda_expecting(module, env, e, &expected);
        e->resolved_type = t;
        return t;
    }
    if (check_ok_err_call(module, env, e, expected)) {
        return expected;
    }
    if (check_channel_new_call(module, env, e, expected)) {
        return expected;
    }
    if (check_mutable_empty_call(module, env, e, expected)) {
        return expected;
    }
    if (check_bare_option_call(module, env, e, expected)) {
        return expected;
    }
    if (is_option_type(expected) || expected.kind == TY_RESULT) {
        Type arg_type;
        if (check_value_against_param_type(module, env, e_slot, expected, &arg_type, "as a value")) {
            return expected;
        }
        return arg_type;
    }
    return check_expr(module, env, *e_slot);
}

static Type check_struct_literal(Module *module, Env *env, Expr *e) {
    const char *type_name_str = e->as.struct_lit.type_name;
    StructDecl *decl = find_struct(module, type_name_str);
    if (decl == NULL) {
        ziman_error("type", e->line, e->col, "no struct named '%s'", type_name_str);
    }
    lint_deprecated_struct(decl, e->line, e->col);
    TypeList *type_args = &e->as.struct_lit.type_args;
    /* Explicit type arguments everywhere, no inference: a generic struct's
     * construction must always spell out `Name<Arg, ...>`, and a
     * non-generic one must never spell any out. */
    if (decl->type_params.count == 0 && type_args->count > 0) {
        ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", type_name_str, type_args->count);
    }
    if (decl->type_params.count > 0 && type_args->count == 0) {
        ziman_error("type", e->line, e->col, "'%s' is generic and requires explicit type arguments, e.g. '%s<...> { ... }'", type_name_str, type_name_str);
    }
    if (decl->type_params.count != type_args->count) {
        ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", type_name_str, decl->type_params.count, type_args->count);
    }
    for (int i = 0; i < type_args->count; i++) {
        check_type_well_formed(module, type_args->items[i], e->line, e->col);
    }
    check_type_args_satisfy_bounds(module, &decl->type_params, type_args, e->line, e->col);

    FieldInitList *inits = &e->as.struct_lit.fields;

    for (int i = 0; i < inits->count; i++) {
        Param *field = resolve_field_init(decl, inits, i);
        Type field_type = substitute_type(field->type, &decl->type_params, type_args);
        Type value_type = check_expr_expecting(module, env, &inits->items[i].value, field_type);
        require_not_void(inits->items[i].value, "as a field value");
        if (!type_equals(value_type, field_type)) {
            ziman_error("type", inits->items[i].value->line, inits->items[i].value->col,
                        "field '%s' of '%s' expects %s, found %s",
                        field->name, type_name_str, type_name(field_type), type_name(value_type));
        }
    }
    /* Construction requires every field, unlike `with` -- there's no base
     * value to fall back to for anything left unmentioned. */
    for (int j = 0; j < decl->fields.count; j++) {
        const char *field_name = decl->fields.items[j].name;
        int provided = 0;
        for (int i = 0; i < inits->count; i++) {
            if (strcmp(inits->items[i].name, field_name) == 0) { provided = 1; break; }
        }
        if (!provided) {
            ziman_error("type", e->line, e->col, "missing field '%s' in construction of '%s'", field_name, type_name_str);
        }
    }
    Type result = type_struct(type_name_str);
    result.type_args = *type_args;
    return result;
}

/* A top-level function used as a value (`apply(double, 3)`). Records the
 * FunctionDecl on the expression (`fn_ref`) for the interpreter and returns the
 * `fn(...) -> ...` type. Only reached when no local variable of that name
 * exists -- a local always wins. Not every function qualifies: a generic one
 * has no single type to give it (an explicit-type-argument form of reference
 * doesn't exist yet), and one with a variadic parameter can't be called with an
 * argument list of unknown length through a value. */
static int resolve_function_reference(Module *module, Expr *e, Type *out) {
    if (e->fn_ref_name == NULL) return 0;
    if (e->fn_ref_name[0] == '!') {
        ziman_error("type", e->line, e->col, "%s", e->fn_ref_name + 1);
    }
    /* A program's own function of this name shadows the prelude's (F164), and `find_function`
     * answers with whichever the index reached first -- which is the prelude's for a name the
     * prelude also declares. Asking the shadow table first is what makes `fold(xs, 0, add)`
     * still mean the file's own two-argument `add` now that the prelude has an overload set of
     * that name. */
    FunctionDecl *shadow = shadowing_fn_any(e->fn_ref_name);
    FunctionDecl *fn = shadow != NULL ? shadow : find_function(module, e->fn_ref_name);
    if (fn == NULL) return 0;
    /* **A builtin is not a function value**, and this is the one place a `builtin fn` declaration
     * is *not* inert. A call never reaches `find_function` -- every builtin arm returns first --
     * but a bare `len` in value position does, and before these declarations existed it fell
     * through to the "is a builtin, not a function value" message further down. Resolving it to a
     * declaration silently made `val f = len` legal, with nothing behind it: the interpreter
     * dispatches a builtin by id at a call site and there is no closure to hand out. Caught by
     * tests/lang/function_values_builtin_rejected.zm, which is what that fixture is for. */
    if (fn->is_builtin) {
        ziman_error("type", e->line, e->col,
                    "'%s' is a builtin, not a function value -- wrap it in a function of your own to pass it around",
                    fn->name);
    }
    /* An overload set has nothing to resolve from in value position: there is no argument list,
     * and the expected type is not consulted (no return-type dispatch -- see the design's
     * non-goals). Before this, `find_function` handed back whichever declaration it reached
     * first, so `val f = sort<int>` silently became the MutableArray one and the mistake only
     * surfaced as a confusing error at the *call* to `f`. A silently chosen overload is the
     * failure mode this language refuses, so it is named here instead. */
    /* Counting only the declarations that could actually be chosen. A program's own function
     * **shadows** the prelude's of the same name (F164), so when `fn` came from a program the
     * prelude's declarations are not alternatives and must not make the name look ambiguous.
     * Without this, a file declaring `fn add(a: int, b: int)` could no longer pass its own `add`
     * as a function value the moment the prelude gained an overload set called `add` --
     * two fixtures caught exactly that. */
    if (overload_set_size_visible(module, fn) > 1) {
        ziman_error("type", e->line, e->col,
                    "'%s' names more than one function, so it cannot be used as a value -- there are no "
                    "arguments here to choose between them; wrap the one you mean in a function of your own "
                    "('fn(a: [T]) -> [T] { return %s(a) }')",
                    fn->name, fn->name);
    }
    lint_deprecated_use(fn, e->line, e->col);
    for (int i = 0; i < fn->params.count; i++) {
        if (fn->params.items[i].is_variadic) {
            ziman_error("type", e->line, e->col,
                        "'%s' takes a variable number of arguments, so it can't be used as a value -- wrap it in a function with a fixed parameter list",
                        fn->name);
        }
    }
    TypeList *targs = &e->fn_ref_type_args;
    if (fn->type_params.count == 0) {
        if (targs->count > 0) {
            ziman_error("type", e->line, e->col, "'%s' is not generic, so it takes no type arguments", fn->name);
        }
        e->fn_ref = fn;
        *out = function_value_type(fn);
        return 1;
    }
    /* A generic function as a value: the type arguments are written out (there is
     * no inference), which fixes its parameter and return types. */
    if (fn->is_native) {
        ziman_error("type", e->line, e->col,
                    "'%s' is a generic native function, so it can't be used as a value -- wrap it in a lambda: fn(...) { %s<...>(...) }",
                    fn->name, fn->name);
    }
    if (targs->count == 0) {
        ziman_error("type", e->line, e->col,
                    "'%s' is generic: write its type arguments where you use it as a value, e.g. %s<int>", fn->name, fn->name);
    }
    if (targs->count != fn->type_params.count) {
        ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", fn->name, fn->type_params.count, targs->count);
    }
    for (int i = 0; i < targs->count; i++) check_type_well_formed(module, targs->items[i], e->line, e->col);
    check_type_args_satisfy_bounds(module, &fn->type_params, targs, e->line, e->col);
    TypeList params = {0};
    for (int i = 0; i < fn->params.count; i++) {
        type_list_push(&params, substitute_type(fn->params.items[i].type, &fn->type_params, targs));
    }
    e->fn_ref = fn;
    *out = type_func(params, substitute_type(fn->return_type, &fn->type_params, targs));
    return 1;
}

static Type check_field_access(Module *module, Env *env, Expr *e) {
    if (e->as.field_access.base->kind == EXPR_IDENT && env_lookup(env, e->as.field_access.base->as.ident_name) == NULL) {
        Type ft;
        if (resolve_function_reference(module, e, &ft)) return ft;
    }
    Type base_type = check_expr(module, env, e->as.field_access.base);
    if (base_type.kind != TY_STRUCT) {
        const char *fname = e->as.field_access.field_name;
        if ((base_type.kind == TY_STRING || base_type.kind == TY_ARRAY) &&
            (strcmp(fname, "len") == 0 || strcmp(fname, "length") == 0 || strcmp(fname, "size") == 0 || strcmp(fname, "count") == 0)) {
            ziman_error("type", e->as.field_access.base->line, e->as.field_access.base->col,
                        "%s has no field '%s' -- use len(x) for its length", type_name(base_type), fname);
        }
        ziman_error("type", e->as.field_access.base->line, e->as.field_access.base->col,
                    "cannot access field '%s' on %s (not a struct)", fname, type_name(base_type));
    }
    StructDecl *decl = find_struct(module, base_type.struct_name);
    for (int i = 0; i < decl->fields.count; i++) {
        if (strcmp(decl->fields.items[i].name, e->as.field_access.field_name) == 0) {
            return substitute_type(decl->fields.items[i].type, &decl->type_params, &base_type.type_args);
        }
    }
    ziman_error("type", e->line, e->col, "no field '%s' on struct '%s'", e->as.field_access.field_name, base_type.struct_name);
    return type_scalar(TY_VOID);
}

static Type check_with_expr(Module *module, Env *env, Expr *e) {
    Type base_type = check_expr(module, env, e->as.with_expr.base);
    if (base_type.kind != TY_STRUCT) {
        ziman_error("type", e->as.with_expr.base->line, e->as.with_expr.base->col,
                    "'with' requires a struct value, found %s", type_name(base_type));
    }
    StructDecl *decl = find_struct(module, base_type.struct_name);
    FieldInitList *overrides = &e->as.with_expr.overrides;

    for (int i = 0; i < overrides->count; i++) {
        Param *field = resolve_field_init(decl, overrides, i);
        Type field_type = substitute_type(field->type, &decl->type_params, &base_type.type_args);
        Type value_type = check_expr_expecting(module, env, &overrides->items[i].value, field_type);
        require_not_void(overrides->items[i].value, "as a field value");
        if (!type_equals(value_type, field_type)) {
            ziman_error("type", overrides->items[i].value->line, overrides->items[i].value->col,
                        "field '%s' of '%s' expects %s, found %s",
                        field->name, base_type.struct_name, type_name(field_type), type_name(value_type));
        }
    }
    /* Unlike a struct literal, fields left unmentioned aren't an error --
     * they're kept from `base`. */
    return base_type;
}

/* Whether `t` is *some* instantiation of the enum `decl` names, ignoring
 * type arguments -- used on a consumption call (`is_X`/`unwrap_X`),
 * where the concrete instantiation is read off the argument's own
 * already-known type rather than written at the call site (see ast.h's
 * enum_access comment), so a full type_equals (which would also compare
 * type_args) is the wrong check here. */
static int is_enum_instance(Type t, EnumDecl *decl) {
    return t.kind == TY_ENUM && strcmp(t.enum_name, decl->name) == 0;
}

/* `EnumName[<Args>].member[(args)]` -- construction if `member` names one
 * of the enum's own variants, or a consumption call if it matches the
 * compiler-generated `is_<variant>`/`unwrap_<variant>` naming convention
 * for one of them (mirroring Result's is_ok/unwrap/unwrap_err exactly,
 * just per-enum instead of hardcoded once). A variant name always starts
 * uppercase and these two prefixes are always lowercase, so there's no way
 * for a real variant to collide with the generated-function convention. */
static Type check_enum_access(Module *module, Env *env, Expr *e) {
    const char *enum_name = e->as.enum_access.enum_name;
    const char *member = e->as.enum_access.member_name;
    EnumDecl *decl = find_enum(module, enum_name);
    if (decl == NULL) {
        ziman_error("type", e->line, e->col, "no enum named '%s'", enum_name);
    }
    lint_deprecated_enum(decl, e->line, e->col);
    TypeList *type_args = &e->as.enum_access.type_args;

    for (int i = 0; i < decl->variants.count; i++) {
        EnumVariant *v = &decl->variants.items[i];
        if (strcmp(v->name, member) != 0) continue;
        /* Construction: explicit type arguments everywhere, no
         * inference -- same rules as a generic struct literal. */
        if (decl->type_params.count == 0 && type_args->count > 0) {
            ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", enum_name, type_args->count);
        }
        if (decl->type_params.count > 0 && type_args->count == 0) {
            ziman_error("type", e->line, e->col, "'%s' is generic and requires explicit type arguments, e.g. '%s<...>.%s'", enum_name, enum_name, member);
        }
        if (decl->type_params.count != type_args->count) {
            ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", enum_name, decl->type_params.count, type_args->count);
        }
        for (int ai = 0; ai < type_args->count; ai++) {
            check_type_well_formed(module, type_args->items[ai], e->line, e->col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, type_args, e->line, e->col);

        if (v->has_payload) {
            if (!e->as.enum_access.has_parens || e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col,
                            "variant '%s.%s' takes exactly one payload value, e.g. %s.%s(...)",
                            enum_name, member, enum_name, member);
            }
            /* The payload type first, then the same check a call argument gets. A variant has exactly
             * one declared payload type, which says as precisely as a parameter does what the value
             * must be -- so the forms that can only be checked with an expected type in hand (an empty
             * `[]`, an unannotated lambda, `ok`/`err`, `channel_new()`, an `if`/`switch` used as a
             * value, a bare value auto-wrapped to Option/Result) work here for the same reason they
             * already work in a struct literal field and a call argument.
             *
             * Before this, `Json.JArray([])` was the one place that asymmetry showed: `Holder { items:
             * [] }` worked because a struct literal pushes its field type down, while a variant whose
             * payload type is just as statically fixed did not, and the writer had to bind the empty
             * array to a separately annotated `val` first (ROADMAP.md, the `Result<T, E>` entry's last
             * bullet, which found it while simplifying `[Json]`-returning code).
             *
             * The helper raises the void-value error itself, which is why it takes the context word:
             * a `require_not_void` call out here would be dead, and leaving the helper's own default
             * would have quietly changed this message from "as a variant payload" to "as an
             * argument". The slot is re-read rather than held, since an auto-wrap replaces the
             * expression in place. */
            Type payload_type = substitute_type(v->payload_type, &decl->type_params, type_args);
            Type arg_type;
            int payload_ok = check_value_against_param_type(module, env, &e->as.enum_access.args.items[0],
                                                            payload_type, &arg_type, "as a variant payload");
            Expr *arg = e->as.enum_access.args.items[0];
            if (!payload_ok) {
                ziman_error("type", arg->line, arg->col, "variant '%s.%s' expects %s, found %s",
                            enum_name, member, type_name(payload_type), type_name(arg_type));
            }
        } else if (e->as.enum_access.has_parens) {
            ziman_error("type", e->line, e->col,
                        "variant '%s.%s' takes no payload -- write '%s.%s' without parentheses",
                        enum_name, member, enum_name, member);
        }
        Type result = type_enum(decl->name);
        result.type_args = *type_args;
        return result;
    }

    static const char *IS_PREFIX = "is_";
    static const char *UNWRAP_PREFIX = "unwrap_";
    /* Consumption (is_X/unwrap_X): no type arguments at the call site --
     * the concrete instantiation comes from the argument's own already-
     * known type instead. Checked only once `member` actually looks like
     * a consumption call, so a genuinely unknown member (`Option.Nope`)
     * still falls through to the clearer "no variant or function" error
     * below instead of this one. */
    if (type_args->count > 0 &&
        (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0 || strncmp(member, UNWRAP_PREFIX, strlen(UNWRAP_PREFIX)) == 0)) {
        ziman_error("type", e->line, e->col,
                    "'%s.%s' takes no type arguments -- they're inferred from the argument's own type", enum_name, member);
    }

    if (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0) {
        const char *variant_part = member + strlen(IS_PREFIX);
        for (int i = 0; i < decl->variants.count; i++) {
            if (strcmp(decl->variants.items[i].name, variant_part) != 0) continue;
            if (e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col, "'%s.%s' takes exactly 1 argument, found %d",
                            enum_name, member, e->as.enum_access.args.count);
            }
            Expr *arg = e->as.enum_access.args.items[0];
            Type arg_type = check_expr(module, env, arg);
            if (!is_enum_instance(arg_type, decl)) {
                ziman_error("type", arg->line, arg->col, "'%s.%s' expects a %s, found %s",
                            enum_name, member, decl->name, type_name(arg_type));
            }
            return type_scalar(TY_BOOL);
        }
    }
    if (strncmp(member, UNWRAP_PREFIX, strlen(UNWRAP_PREFIX)) == 0) {
        const char *variant_part = member + strlen(UNWRAP_PREFIX);
        for (int i = 0; i < decl->variants.count; i++) {
            EnumVariant *v = &decl->variants.items[i];
            if (strcmp(v->name, variant_part) != 0) continue;
            if (!v->has_payload) {
                ziman_error("type", e->line, e->col, "variant '%s.%s' has no payload to unwrap", enum_name, v->name);
            }
            if (e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col, "'%s.%s' takes exactly 1 argument, found %d",
                            enum_name, member, e->as.enum_access.args.count);
            }
            Expr *arg = e->as.enum_access.args.items[0];
            Type arg_type = check_expr(module, env, arg);
            if (!is_enum_instance(arg_type, decl)) {
                ziman_error("type", arg->line, arg->col, "'%s.%s' expects a %s, found %s",
                            enum_name, member, decl->name, type_name(arg_type));
            }
            return substitute_type(v->payload_type, &decl->type_params, &arg_type.type_args);
        }
    }

    ziman_error("type", e->line, e->col, "enum '%s' has no variant or function '%s'", enum_name, member);
    return type_scalar(TY_VOID);
}

static Type check_expr(Module *module, Env *env, Expr *e) {
    ZIMAN_STACK_CHECK();
    Type t;
    switch (e->kind) {
        case EXPR_INT:
            if (e->int_min_literal) {
                ziman_error("type", e->line, e->col,
                            "integer literal 9223372036854775808 is out of range for int (the largest is 9223372036854775807; write 9223372036854775808n for a bigint)");
            }
            t = type_scalar(TY_INT);
            break;
        case EXPR_FLOAT: t = type_scalar(TY_FLOAT); break;
        case EXPR_STRING:
            warn_brace_interpolation(env, e);
            t = type_scalar(TY_STRING);
            break;
        case EXPR_BOOL: t = type_scalar(TY_BOOL); break;
        case EXPR_UNIT: t = type_scalar(TY_UNIT); break;
        case EXPR_BIGINT: t = type_scalar(TY_BIGINT); break;
        case EXPR_DECIMAL: t = type_scalar(TY_DECIMAL); break;
        case EXPR_BYTE: t = type_scalar(TY_BYTE); break;
        case EXPR_IDENT: {
            int depth, slot;
            Binding *b = env_lookup_resolved(env, e->as.ident_name, &depth, &slot);
            /* The one place a name is read, so the one place the unread-declaration lint needs. An
             * assignment's target does not come through here (STMT_ASSIGN resolves its own binding),
             * which is what makes `x = 1` with no reader still report as unread. */
            if (b != NULL && b->lint_id > 0 && b->lint_id - 1 < g_lint_var_count) {
                g_lint_vars[b->lint_id - 1].used = 1;
            }
            lint_deprecated_val(e->as.ident_name, e->line, e->col);
            if (b == NULL) {
                Type ft;
                if (resolve_function_reference(module, e, &ft)) {
                    t = ft;
                    break;
                }
                if (strcmp(e->as.ident_name, "unit") == 0) {
                    /* the value of the type `unit`, where no variable of that name is in scope */
                    e->kind = EXPR_UNIT;
                    t = type_scalar(TY_UNIT);
                    break;
                }
                if (strcmp(e->as.ident_name, "None") == 0) {
                    /* Reachable only when nothing gave check_bare_option_call() (via check_expr_expecting) an
                     * expected Option<T> to work with -- `None` alone carries no `T` at all to infer. */
                    ziman_error("type", e->line, e->col,
                                "cannot infer the type of 'None' here; it takes its type from the context, so it "
                                "needs one: a val/var with an explicit Option<T> annotation, a struct literal field, "
                                "a 'with' override, a function's returned value, or an argument whose type is known. "
                                "A generic argument counts when something else binds the type parameter -- another "
                                "argument, or a 'Some(x)' whose payload names it -- since 'None' has no payload of "
                                "its own to read");
                }
                for (size_t k = 0; k < sizeof BUILTINS_NO_NAMED_ARGS / sizeof BUILTINS_NO_NAMED_ARGS[0] && BUILTINS_NO_NAMED_ARGS[k] != NULL; k++) {
                    if (strcmp(e->as.ident_name, BUILTINS_NO_NAMED_ARGS[k]) == 0) {
                        ziman_error("type", e->line, e->col,
                                    "'%s' is a builtin, not a function value -- wrap it in a function of your own to pass it around", e->as.ident_name);
                    }
                }
                /* A foreign binding keyword that reached an expression -- so not `let x = 1`, which
                 * the parser rewrites and check_stmt reports (see Stmt.foreign_decl_keyword), but a
                 * bare `let` somewhere no name follows it. */
                if (ziman_foreign_decl_keyword(e->as.ident_name, (int)strlen(e->as.ident_name)) != NULL) {
                    ziman_error("type", e->line, e->col,
                                "Ziman has no '%s' -- declare with 'val' (never reassigned) or 'var' (may be), "
                                "e.g. 'val x = 1' or 'var x = 1'", e->as.ident_name);
                }
                ziman_error("type", e->line, e->col, "use of undefined variable '%s'", e->as.ident_name);
            }
            if (b->poisoned) ziman_abort_quietly();
            if (e->fn_ref_type_args.count > 0) {
                ziman_error("type", e->line, e->col, "'%s' is a variable, not a generic function, so it takes no type arguments", e->as.ident_name);
            }
            e->resolved_depth = depth;
            e->resolved_slot = slot;
            t = b->type;
            break;
        }
        case EXPR_LAMBDA: t = check_lambda(module, env, e); break;
        case EXPR_VALUE_BLOCK: t = check_value_block_expr(module, env, e, NULL); break;
        case EXPR_UNARY: t = check_unary(module, env, e); break;
        case EXPR_BINARY: t = check_binary(module, env, e); break;
        case EXPR_CALL: t = check_call(module, env, e); break;
        case EXPR_CAST: t = check_cast(module, env, e); break;
        case EXPR_ARRAY_LIT: t = check_array_literal(module, env, e); break;
        case EXPR_INDEX: t = check_index(module, env, e); break;
        case EXPR_STRUCT_LIT: t = check_struct_literal(module, env, e); break;
        case EXPR_FIELD_ACCESS: t = check_field_access(module, env, e); break;
        case EXPR_WITH: t = check_with_expr(module, env, e); break;
        case EXPR_ENUM_ACCESS: t = check_enum_access(module, env, e); break;
        case EXPR_TRY_UNWRAP:
            /* Reachable only when a `?` wasn't already intercepted by
             * one of the two statement forms it's legal in (a val/var's
             * entire initializer, or its own bare statement) -- see
             * check_try_unwrap and its two callers. */
            ziman_error("type", e->line, e->col,
                        "'try' cannot be used here: it must run exactly once and unconditionally (not in an 'else if' or 'while' condition, on the right of '&&' or '||', in a 'defer' or 'spawn' argument, or inside an 'if' or 'switch' used as a value); put it in its own 'val' first");
            t = type_scalar(TY_VOID);
            break;
        default:
            ziman_error("type", e->line, e->col, "internal error: unhandled expression kind");
            t = type_scalar(TY_VOID);
    }
    e->resolved_type = t;
    return t;
}

/* Returns true if executing `s` is statically guaranteed to hit a
 * `return` on every path -- used to require that non-void functions
 * can't fall off the end. Conservative: a `while` loop is never assumed
 * to return, since v0 does no loop-bound analysis. */
static int stmt_always_returns(Stmt *s) {
    switch (s->kind) {
        case STMT_RETURN:
            return 1;
        case STMT_BLOCK: {
            for (int i = 0; i < s->as.block.stmts.count; i++) {
                if (stmt_always_returns(s->as.block.stmts.items[i])) return 1;
            }
            return 0;
        }
        case STMT_VALUE_BLOCK:
            return s->as.value_block.target == STMT_RETURN;
        case STMT_IF:
            return s->as.if_stmt.else_block != NULL &&
                   stmt_always_returns(s->as.if_stmt.then_block) &&
                   stmt_always_returns(s->as.if_stmt.else_block);
        default:
            return 0;
    }
}

/* `defer f(args)`: type-checks the call exactly like an expression
 * statement (so named args, Option/Result auto-wrap, generics and
 * variadics all work), discards its result -- any type, a Result
 * included, which is the documented cost of a deferred cleanup call
 * having nowhere to report failure -- and builds the "replay" call the
 * interpreter runs at block exit.
 *
 * check_call has by now finalised the argument list (reordered named
 * arguments, inserted auto-wraps and defaults for omitted parameters),
 * so `call`'s argument nodes are the real ones. The interpreter
 * evaluates each into a hidden `$defer<N>$<i>` variable when the `defer`
 * statement runs; the replay call is a copy of `call` whose arguments
 * are just references to those variables -- which is why arguments
 * carry their `defer`-time values rather than whatever a `var` holds
 * later.
 *
 * A few hardcoded builtins can't be replayed this way because they look
 * at their argument *expressions* rather than just values: `format`
 * needs a string-literal template, `ok`/`err`/`channel_new` take their
 * type from context. They make no sense as a deferred cleanup anyway. */
static int g_defer_counter = 0;

static void check_defer(Module *module, Env *env, Stmt *s) {
    Expr *call = s->as.defer_stmt.call;
    static const char *const UNDEFERRABLE[] = {"format", "ok", "err", "channel_new"};
    for (size_t i = 0; i < sizeof(UNDEFERRABLE) / sizeof(UNDEFERRABLE[0]); i++) {
        if (strcmp(call->as.call.callee, UNDEFERRABLE[i]) == 0) {
            ziman_error("type", s->line, s->col, "'%s' cannot be deferred", UNDEFERRABLE[i]);
        }
    }
    check_expr(module, env, call);

    int n = call->as.call.args.count;
    int id = g_defer_counter++;
    Expr *replay = expr_new(EXPR_CALL, call->line, call->col);
    *replay = *call;
    replay->as.call.args.items = NULL;
    replay->as.call.args.count = 0;
    replay->as.call.args.capacity = 0;
    char **hidden = ziman_xmalloc(sizeof(char *) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        char name[48];
        snprintf(name, sizeof(name), "$defer%d$%d", id, i);
        hidden[i] = ziman_xstrdup(name);
        Expr *ref = expr_new(EXPR_IDENT, call->line, call->col);
        ref->as.ident_name = hidden[i];
        ref->resolved_type = call->as.call.args.items[i]->resolved_type;
        expr_list_push(&replay->as.call.args, ref);
        /* Reserves this hidden variable's own slot in `env` now, at type-check time, exactly
         * where interpreter.c's own STMT_DEFER handling will env_define it at run time (see
         * that code and ast.h's `defer_stmt` comment) -- otherwise every ordinary declaration
         * later in this same scope would be type-checked one slot short of where it actually
         * ends up once the interpreter's own env_define for this hidden variable takes a slot
         * this pass never accounted for. `ref` itself is never run through check_expr (its
         * resolved_type is set directly, above), so this is the only place that registers it. */
        env_define(env, hidden[i], ref->resolved_type, 0, value_int(0));
    }
    s->as.defer_stmt.replay = replay;
    s->as.defer_stmt.hidden = hidden;
    s->as.defer_stmt.hidden_count = n;
}

static void check_stmt(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop);

/* Type-checks `e` (always exactly an EXPR_TRY_UNWRAP node -- `inner?`)
 * against the enclosing function's own `fn_return_type`. The only two
 * legal call sites are STMT_VAL/STMT_VAR (binding the unwrapped value
 * to a name) and STMT_EXPR (a bare `expr?`, discarding it); any other
 * position instead reaches check_expr's own EXPR_TRY_UNWRAP case,
 * which always errors -- this function is never reached from there.
 *
 * `inner`'s type must be exactly `Option<T>` or `Result<T, E>`.
 *   - Option<T>: `fn_return_type` must be `Option<T2>` for *any* T2 --
 *     `None` carries no value to propagate, so T and T2 need no
 *     relationship at all.
 *   - Result<T, E>: `fn_return_type` must be `Result<T2, E>` with the
 *     *same* E -- `Err(e)` carries a real value that has to match.
 *
 * Returns the unwrapped type (T) -- what a val/var binds to, or the
 * (discarded) type of a bare `expr?` statement. Also fills in
 * `e->as.try_unwrap.propagated_type` with `fn_return_type` itself,
 * cached here because the interpreter needs the full, concrete
 * propagated type to build the runtime None/Err value and has no other
 * way to learn it (NativeFn-style context-passing doesn't apply to an
 * ordinary AST node) -- see ast.h's own comment on that field. */
static Type check_try_unwrap(Module *module, Env *env, Expr *e, Type fn_return_type) {
    Expr *inner = e->as.try_unwrap.inner;
    Type inner_type = check_expr(module, env, inner);
    require_not_void(inner, "with 'try'");

    if (is_option_type(inner_type)) {
        if (!is_option_type(fn_return_type)) {
            ziman_error("type", e->line, e->col,
                        "'try' on an Option<T> value requires the enclosing function or lambda to return Option<...>, "
                        "found %s -- give it that return type (a lambda needs it written out: "
                        "'fn(...) -> Option<T> { ... }')",
                        type_name(fn_return_type));
        }
        e->as.try_unwrap.propagated_type = fn_return_type;
        Type t = option_elem_type(inner_type);
        e->resolved_type = t;
        return t;
    }
    if (inner_type.kind == TY_RESULT) {
        e->as.try_unwrap.convert_variant = NULL;
        if (fn_return_type.kind == TY_RESULT && !type_equals(*inner_type.elem_err, *fn_return_type.elem_err) && fn_return_type.elem_err->kind == TY_ENUM) {
            EnumDecl *target = find_enum(module, fn_return_type.elem_err->enum_name);
            for (int v = 0; target != NULL && v < target->variants.count; v++) {
                EnumVariant *cand = &target->variants.items[v];
                if (cand->is_from && type_equals(cand->payload_type, *inner_type.elem_err)) {
                    e->as.try_unwrap.convert_variant = cand->name;
                    break;
                }
            }
        }
        if (fn_return_type.kind != TY_RESULT ||
            (e->as.try_unwrap.convert_variant == NULL && !type_equals(*inner_type.elem_err, *fn_return_type.elem_err))) {
            ziman_error("type", e->line, e->col,
                        "'try' on a Result<T, E> value requires the enclosing function or lambda to return "
                        "Result<..., %s>, found %s -- give it that return type (a lambda needs it written out: "
                        "'fn(...) -> Result<T, %s> { ... }')%s",
                        type_name(*inner_type.elem_err), type_name(fn_return_type), type_name(*inner_type.elem_err),
                        fn_return_type.kind == TY_RESULT && fn_return_type.elem_err->kind == TY_ENUM
                            ? " (or mark a variant of that enum `#[from]` to convert the error)" : "");
        }
        e->as.try_unwrap.propagated_type = fn_return_type;
        Type t = *inner_type.elem;
        e->resolved_type = t;
        return t;
    }
    ziman_error("type", inner->line, inner->col,
                "'try' can only be used on an Option<T> or Result<T, E> value, found %s", type_name(inner_type));
}

/* Runs `block`'s statements directly in `scope` (no further scope nesting) --
 * shared by check_block (which makes its own fresh scope first) and
 * STMT_FOR (which needs its loop binder(s) defined in that same scope,
 * ahead of the body's own statements). `in_loop` is true whenever these
 * statements execute somewhere inside a while/for body -- threaded through
 * so a `break`/`continue` deeper inside (e.g. under an `if`) can still be
 * validated without re-deriving it from scratch. */
typedef struct ValueCtx {
    int has_expected;
    Type expected;
    int count;
    Type first;
} ValueCtx;

static ValueCtx *g_value_ctx = NULL;

/* ---- reporting several errors: recovery per statement ------------------------
 * An error inside a statement is reported and the checker moves on to the next
 * one (see error.h). The globals the checker keeps while it walks are restored,
 * since the jump left them pointing into frames that no longer exist. A `val`/
 * `var` whose statement failed is defined anyway, "poisoned": a later use of it
 * is skipped without a message, so one bad declaration does not report an error
 * at every place that uses the name. */
typedef struct {
    ValueCtx *value_ctx;
    TypeParamList *decl_params;
    int lint_active;
} CheckState;

static CheckState check_state_save(void) {
    CheckState s = {g_value_ctx, current_decl_type_params, g_lint_active};
    return s;
}

static void check_state_restore(const CheckState *s) {
    g_value_ctx = s->value_ctx;
    current_decl_type_params = s->decl_params;
    g_lint_active = s->lint_active;
}

static void poison_declaration(Env *scope, const Stmt *s) {
    const char *name = NULL;
    if (s->kind == STMT_VAL || s->kind == STMT_VAR) name = s->as.decl.name;
    else if (s->kind == STMT_VALUE_BLOCK && (s->as.value_block.target == STMT_VAL || s->as.value_block.target == STMT_VAR)) name = s->as.value_block.name;
    if (name == NULL) return;
    for (int i = 0; i < scope->count; i++) {
        if (strcmp(scope->items[i].name, name) == 0) return; /* it got defined before the error */
    }
    /* `ENV_DEFINED`, not merely non-zero: `val _ = <error>` discards, binding nothing, and this
     * used to write `items[-1]` and segfault `--check` and `--lsp` on a two-line program. There is
     * also nothing a poisoned `_` could do for us -- the marker exists to suppress the follow-on
     * "undefined variable" errors from later uses of a name whose declaration failed, and `_` has
     * no later uses by construction. */
    if (env_define(scope, name, type_scalar(TY_VOID), 0, value_int(0)) == ENV_DEFINED) {
        scope->items[scope->count - 1].poisoned = 1;
    }
}

/* Checks one statement, recovering from an error in it. */
static void check_stmt_recovering(Module *module, Env *scope, Stmt *s, Type fn_return_type, int in_loop) {
    jmp_buf jb;
    CheckState saved = check_state_save();
    ziman_recovery_push(&jb);
    if (setjmp(jb) == 0) {
        check_stmt(module, scope, s, fn_return_type, in_loop);
        ziman_recovery_pop();
    } else {
        check_state_restore(&saved);
        poison_declaration(scope, s);
    }
}

static void check_stmts_in_scope(Module *module, Env *scope, Stmt *block, Type fn_return_type, int in_loop) {
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        check_stmt_recovering(module, scope, block->as.block.stmts.items[i], fn_return_type, in_loop);
    }
}

static void check_block(Module *module, Env *parent, Stmt *block, Type fn_return_type, int in_loop) {
    Env *scope = env_new(parent);
    check_stmts_in_scope(module, scope, block, fn_return_type, in_loop);
    env_free(scope);
}

/* ---- switch: exhaustive consumption of an enum or a Result ----
 *
 * `switch e { A(x) { ... } B { ... } }` is checked and then rewritten in
 * place into `{ val $swN = e; if Enum.is_A($swN) { val x = Enum.unwrap_A($swN); ... }
 * else { ... } }` -- the same is_X/unwrap_X calls a person could write by
 * hand, so generics, payload typing and the "does every path return" check
 * all come from machinery that already exists. What `switch` adds is the
 * exhaustiveness rule: every variant exactly once, none forgotten, so adding
 * a variant to an enum is a compile error at every switch over it. There is
 * deliberately no default arm -- that would reintroduce the silent case. */
static Expr *mk_ident_expr(const char *name, int line, int col) {
    Expr *e = expr_new(EXPR_IDENT, line, col);
    e->as.ident_name = ziman_xstrdup(name);
    return e;
}

static Expr *mk_enum_call_expr(const char *enum_name, const char *prefix, const char *variant, Expr *arg, int line, int col) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, line, col);
    e->as.enum_access.enum_name = ziman_xstrdup(enum_name);
    size_t n = strlen(prefix) + strlen(variant) + 1;
    e->as.enum_access.member_name = ziman_xmalloc(n);
    snprintf(e->as.enum_access.member_name, n, "%s%s", prefix, variant);
    expr_list_push(&e->as.enum_access.args, arg);
    e->as.enum_access.has_parens = 1;
    return e;
}

static Expr *mk_call_expr(const char *callee, Expr *arg, int line, int col) {
    Expr *e = expr_new(EXPR_CALL, line, col);
    e->as.call.callee = ziman_xstrdup(callee);
    expr_list_push(&e->as.call.args, arg);
    return e;
}

/* The same, with the three arguments every element write takes: the collection, the index and the
 * value. `set(a, i, v)` and a builder's `b[i] = v` are both this shape. */
static Expr *mk_call3_expr(const char *callee, Expr *a, Expr *b, Expr *c, int line, int col) {
    Expr *e = mk_call_expr(callee, a, line, col);
    expr_list_push(&e->as.call.args, b);
    expr_list_push(&e->as.call.args, c);
    return e;
}

/* An arm's block with `val <binder> = <payload_expr>` put first. */
static void prepend_binder(Stmt *body, const char *binder, Expr *payload_expr, int line, int col) {
    Stmt *decl = stmt_new(STMT_VAL, line, col);
    decl->as.decl.name = ziman_xstrdup(binder);
    decl->as.decl.init = payload_expr;
    StmtList old = body->as.block.stmts;
    memset(&body->as.block.stmts, 0, sizeof body->as.block.stmts);
    stmt_list_push(&body->as.block.stmts, decl);
    for (int i = 0; i < old.count; i++) stmt_list_push(&body->as.block.stmts, old.items[i]);
}

static void check_switch(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop) {
    static int switch_counter = 0;
    Expr *scrutinee = s->as.switch_stmt.scrutinee;
    SwitchArm *arms = s->as.switch_stmt.arms;
    int arm_count = s->as.switch_stmt.arm_count;

    char *hidden = ziman_xmalloc(32);
    snprintf(hidden, 32, "$sw%d", switch_counter++);
    Env *scope = env_new(env);
    Stmt *decl = stmt_new(STMT_VAL, s->line, s->col);
    decl->as.decl.name = hidden;
    decl->as.decl.init = scrutinee;
    check_stmt(module, scope, decl, fn_return_type, in_loop);
    Type t = env_lookup(scope, hidden)->type;

    EnumDecl *enum_decl = NULL;
    const char *const result_variants[2] = {"Ok", "Err"};
    int variant_count;
    if (t.kind == TY_ENUM) {
        enum_decl = find_enum(module, t.enum_name);
        if (enum_decl == NULL) ziman_error("type", scrutinee->line, scrutinee->col, "no enum named '%s'", t.enum_name);
        variant_count = enum_decl->variants.count;
    } else if (t.kind == TY_RESULT) {
        variant_count = 2;
    } else {
        ziman_error("type", scrutinee->line, scrutinee->col, "'switch' works on an enum or a Result, found %s", type_name(t));
        return;
    }

    int *seen = ziman_xcalloc((size_t)variant_count, sizeof(int));
    int *arm_variant = ziman_xmalloc(sizeof(int) * (size_t)arm_count);
    for (int a = 0; a < arm_count; a++) {
        SwitchArm *arm = &arms[a];
        int vi = -1;
        int has_payload = 1;
        for (int v = 0; v < variant_count; v++) {
            const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
            if (strcmp(name, arm->variant) == 0) {
                vi = v;
                has_payload = enum_decl != NULL ? enum_decl->variants.items[v].has_payload : 1;
                break;
            }
        }
        if (vi < 0) {
            char list[512] = "";
            for (int v = 0; v < variant_count; v++) {
                const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
                if (strlen(list) + strlen(name) + 3 < sizeof list) {
                    if (v > 0) strcat(list, ", ");
                    strcat(list, name);
                }
            }
            ziman_error("type", arm->line, arm->col, "%s has no variant '%s' (its variants: %s)", type_name(t), arm->variant, list);
        }
        if (seen[vi]) {
            ziman_error("type", arm->line, arm->col, "this 'switch' already has an arm for '%s'", arm->variant);
        }
        seen[vi] = 1;
        arm_variant[a] = vi;
        if (has_payload && arm->binder == NULL) {
            ziman_error("type", arm->line, arm->col, "variant '%s' carries a value: write '%s(name) { ... }' to bind it", arm->variant, arm->variant);
        }
        if (!has_payload && arm->binder != NULL) {
            ziman_error("type", arm->line, arm->col, "variant '%s' carries no value: write '%s { ... }' without a name", arm->variant, arm->variant);
        }
    }
    char missing[512] = "";
    for (int v = 0; v < variant_count; v++) {
        if (seen[v]) continue;
        const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
        if (strlen(missing) + strlen(name) + 3 < sizeof missing) {
            if (missing[0] != '\0') strcat(missing, ", ");
            strcat(missing, name);
        }
    }
    if (missing[0] != '\0') {
        ziman_error("type", s->line, s->col,
                    "this 'switch' over %s is not exhaustive: missing %s (add an arm for each variant; there is no default arm on purpose, so a new variant is a compile error here)",
                    type_name(t), missing);
    }

    /* Build: if <arm0> { ... } else if <arm1> { ... } else { <last arm> } */
    Stmt *chain = NULL;
    for (int a = arm_count - 1; a >= 0; a--) {
        SwitchArm *arm = &arms[a];
        int is_result = enum_decl == NULL;
        if (arm->binder != NULL) {
            Expr *payload;
            if (is_result) {
                payload = mk_call_expr(strcmp(arm->variant, "Ok") == 0 ? "unwrap" : "unwrap_err", mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            } else {
                payload = mk_enum_call_expr(t.enum_name, "unwrap_", arm->variant, mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            }
            prepend_binder(arm->body, arm->binder, payload, arm->line, arm->col);
        }
        if (a == arm_count - 1) {
            chain = arm->body;
            continue;
        }
        Stmt *ifs = stmt_new(STMT_IF, arm->line, arm->col);
        Expr *cond;
        if (is_result) {
            cond = mk_call_expr("is_ok", mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            if (strcmp(arm->variant, "Err") == 0) {
                Expr *neg = expr_new(EXPR_UNARY, arm->line, arm->col);
                neg->as.unary.op = OP_NOT;
                neg->as.unary.operand = cond;
                cond = neg;
            }
        } else {
            cond = mk_enum_call_expr(t.enum_name, "is_", arm->variant, mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
        }
        ifs->as.if_stmt.cond = cond;
        ifs->as.if_stmt.then_block = arm->body;
        ifs->as.if_stmt.else_block = chain;
        chain = ifs;
    }
    check_stmt(module, scope, chain, fn_return_type, in_loop);
    env_free(scope);

    free(seen);
    free(arm_variant);
    memset(&s->as, 0, sizeof s->as);
    s->kind = STMT_BLOCK;
    stmt_list_push(&s->as.block.stmts, decl);
    stmt_list_push(&s->as.block.stmts, chain);
}

/* A wrong-typed initializer. `name` is NULL for an arm's yielded value. */
/* `_Noreturn`: every path out of this ends in `ziman_error`. Saying so is what lets the compiler
 * see that `check_initializer`'s `declared` is set on every reachable path -- clang reported it as
 * "used uninitialized whenever 'if' condition is false" precisely because it could not tell that
 * the `else` branch never comes back (F124). gcc never warned, so this was clang-only. */
static _Noreturn void init_mismatch(const char *name, Type declared, Type actual, int line, int col) {
    if (name == NULL) {
        ziman_error("type", line, col, "an arm of this 'if'/'switch' has type %s but %s is expected here", type_name(actual), type_name(declared));
    }
    /* An `int` where a wider scalar is declared is almost always a missing literal suffix rather
     * than a real type confusion: bare digits mean `int`, and the wider types are written `1n`,
     * `1d`, `1b`. Found by the F28 validation run -- the one step in it the compiler did not
     * direct was guessing `1n`, after `bigint(1)` was rejected too (there is no conversion-call
     * form). Worth saying here, because this is the message a reader actually meets. */
    /* The same courtesy for a `float` where `decimal` or `bigint` is declared, which the int branch
     * below had and this did not. Round 4 of the 2026-10-12 benchmark found the asymmetry twice, in
     * two different runs of 18-money-decimal: `val x: decimal = 19.99` is the *natural* wrong
     * attempt for that task, and it got a bare mismatch, while `= 1` got the suffix spelled out. One
     * agent said so outright -- it found `19.99d` only by carrying the suffix over from the int
     * path. Forms named here are the ones verified to work: `19.99d`, `5n`, and `as` for a float
     * already in hand (`5.0n` is rejected, so it is deliberately not suggested). */
    if (actual.kind == TY_FLOAT && (declared.kind == TY_DECIMAL || declared.kind == TY_BIGINT)) {
        if (declared.kind == TY_DECIMAL) {
            ziman_error("type", line, col,
                        "cannot initialize '%s' of declared type decimal with a value of type float -- a decimal "
                        "literal is written with a 'd' suffix, as '19.99d'; to convert a float you already have, "
                        "write 'as decimal'", name);
        }
        /* Guarded rather than left to fall through: ziman_error does not return, so the branch
         * above already ends the function, and an explicit condition keeps that from being
         * something the next reader has to work out. */
        if (declared.kind == TY_BIGINT) {
            ziman_error("type", line, col,
                        "cannot initialize '%s' of declared type bigint with a value of type float -- a bigint has no "
                        "fractional part, so its literal is a whole number with an 'n' suffix, as '5n'; to convert a "
                        "float you already have, write 'as bigint'", name);
        }
    }
    if (actual.kind == TY_INT) {
        const char *suffix = declared.kind == TY_BIGINT ? "n" : (declared.kind == TY_DECIMAL ? "d" : (declared.kind == TY_BYTE ? "b" : NULL));
        if (suffix != NULL) {
            ziman_error("type", line, col,
                        "cannot initialize '%s' of declared type %s with a value of type int -- bare digits mean "
                        "'int', so write the literal with its suffix, as '1%s'", name, type_name(declared), suffix);
        }
    }
    ziman_error("type", line, col, "cannot initialize '%s' of declared type %s with a value of type %s", name, type_name(declared), type_name(actual));
}

/* Checks the initializer of a `val`/`var` (or a value arm's yielded value)
 * against an optional declared type, applying the special paths that need the
 * expected type -- `x?`, an empty `[]`, `ok(...)`/`err(...)`, `channel_new()`,
 * a bare value auto-wrapped to Option/Result -- and returns the type the
 * binding gets. `*init_slot` is updated in place when a wrap is inserted. */
static Type check_initializer(Module *module, Env *env, Expr **init_slot, int has_declared, Type declared_type,
                              const char *name_for_msg, Type fn_return_type) {
    Expr *init = *init_slot;
    Type declared;
    /* `val|var NAME = inner?` -- checked directly against
     * `fn_return_type`, not the declared type (there usually
     * isn't one; the unwrapped type is already fully determined
     * by `inner`'s own Option<T>/Result<T, E> type, with
     * nothing left to infer or disambiguate). A declared type
     * annotation is still allowed, and still checked against
     * the unwrapped type, exactly like the plain fallback below
     * already does for an ordinary initializer. */
    if (init->kind == EXPR_TRY_UNWRAP) {
        Type unwrapped = check_try_unwrap(module, env, init, fn_return_type);
        declared = unwrapped;
        if (has_declared) {
            declared = declared_type;
            if (!type_equals(declared, unwrapped)) {
                init_mismatch(name_for_msg, declared, unwrapped, init->line, init->col);
            }
        }
    } else if (init->kind == EXPR_ARRAY_LIT && init->as.array_lit.elements.count == 0) {
        /* An empty array literal (`[]`) has no elements to infer
         * a type from -- it's only legal when a declared type
         * gives its element type explicitly, checked here
         * rather than inside the generic check_expr, which has
         * no such context to work from. */
        if (!has_declared) {
            ziman_error("type", init->line, init->col,
                        "cannot infer the element type of an empty array literal; add a type annotation (e.g. val x: [int] = [])");
        }
        if (declared_type.kind != TY_ARRAY) {
            ziman_error("type", init->line, init->col,
                        "cannot initialize '%s' of declared type %s with an empty array literal",
                        name_for_msg, type_name(declared_type));
        }
        init->resolved_type = declared_type;
        declared = declared_type;
    } else if (has_declared && check_ok_err_call(module, env, init, declared_type)) {
        /* ok(...)/err(...) resolved against the declared type -- see
         * check_ok_err_call's own comment for why this needs its own
         * path instead of going through plain check_expr. */
        declared = declared_type;
    } else if (has_declared && check_channel_new_call(module, env, init, declared_type)) {
        /* channel_new() resolved against the declared type -- same
         * reasoning as ok(...)/err(...) above. */
        declared = declared_type;
    } else if (has_declared && check_mutable_empty_call(module, env, init, declared_type)) {
        /* `mutable([])` resolved against the declared type -- same reasoning as ok(...)/err(...)
         * above, and it has to be repeated here because this chain and check_expr_expecting's are
         * two lists of the same hooks: a `val` initializer comes through here and never through
         * that one. Adding the hook to only one of them is why this looked done and was not. */
        declared = declared_type;
    } else if (has_declared && check_bare_option_call(module, env, init, declared_type)) {
        /* bare Some(x)/None resolved against the declared type -- same reasoning as ok(...)/err(...) above. */
        declared = declared_type;
    } else if (has_declared && (is_option_type(declared_type) || declared_type.kind == TY_RESULT)) {
        /* A bare Option<T>/Result<T, E> auto-wrap against the
         * declared type -- same reasoning as ok(...)/err(...)
         * above, via check_value_against_param_type directly
         * (passing init_slot, not the local `init` copy,
         * so a successful wrap persists into the AST the
         * interpreter later evaluates). */
        Type arg_type;
        if (check_value_against_param_type(module, env, init_slot, declared_type, &arg_type,
                                           "as an initializer")) {
            declared = declared_type;
        } else {
            init_mismatch(name_for_msg, declared_type, arg_type, init->line, init->col);
        }
    } else {
        Type init_type;
        if (init->kind == EXPR_VALUE_BLOCK && has_declared) {
            init_type = check_value_block_expr(module, env, init, &declared_type);
            init->resolved_type = init_type;
        } else if (init->kind == EXPR_LAMBDA && has_declared && declared_type.kind == TY_FUNC) {
            init_type = check_lambda_expecting(module, env, init, &declared_type);
            init->resolved_type = init_type;
        } else {
            init_type = check_expr(module, env, init);
        }
        require_not_void(init, "to initialize a variable");
        declared = init_type;
        if (has_declared) {
            declared = declared_type;
            if (!type_equals(declared, init_type)) {
                init_mismatch(name_for_msg, declared, init_type, init->line, init->col);
            }
        }
    }
    return declared;
}

/* ---- `if`/`switch` used as a value (see docs/DESIGN.md) ------------------
 * A STMT_VALUE_BLOCK wraps an ordinary if/switch whose arms end in STMT_YIELD.
 * The enclosing statement's expected type (a declared type, the assigned
 * variable's type, the function's return type) is pushed here so each yielded
 * value is checked against it exactly as an initializer would be; with no
 * expected type the first arm decides and the rest must match. */

/* Whether every path through `s` (an arm block, or an if/switch ending one)
 * ends by yielding a value or by leaving (return/break/continue). */
static int arm_yields_or_leaves(Stmt *s) {
    switch (s->kind) {
        case STMT_BLOCK:
            if (s->as.block.stmts.count == 0) return 0;
            return arm_yields_or_leaves(s->as.block.stmts.items[s->as.block.stmts.count - 1]);
        case STMT_YIELD:
        case STMT_RETURN:
        case STMT_BREAK:
        case STMT_CONTINUE:
            return 1;
        case STMT_IF:
            return s->as.if_stmt.else_block != NULL && arm_yields_or_leaves(s->as.if_stmt.then_block) &&
                   arm_yields_or_leaves(s->as.if_stmt.else_block);
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) {
                if (!arm_yields_or_leaves(s->as.switch_stmt.arms[i].body)) return 0;
            }
            return 1;
        default:
            return 0;
    }
}

/* The type `fn(P...) -> R` of a lambda: its declared parameters only (the
 * captured ones, in front, are not part of it). */
static Type lambda_type(FunctionDecl *lam, int ncaptured) {
    TypeList params = {0};
    for (int i = ncaptured; i < lam->params.count; i++) type_list_push(&params, lam->params.items[i].type);
    return type_func(params, lam->return_type);
}

/* Whether a lambda body contains an explicit `return` (not counting nested
 * lambdas, which are expressions and never walked here). */
static int has_return_stmt(const Stmt *s) {
    switch (s->kind) {
        case STMT_RETURN: return 1;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) if (has_return_stmt(s->as.block.stmts.items[i])) return 1;
            return 0;
        case STMT_IF:
            return has_return_stmt(s->as.if_stmt.then_block) || (s->as.if_stmt.else_block != NULL && has_return_stmt(s->as.if_stmt.else_block));
        case STMT_WHILE: return has_return_stmt(s->as.while_stmt.body);
        case STMT_FOR: return has_return_stmt(s->as.for_stmt.body);
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) if (has_return_stmt(s->as.switch_stmt.arms[i].body)) return 1;
            return 0;
        case STMT_VALUE_BLOCK: return has_return_stmt(s->as.value_block.inner);
        default: return 0;
    }
}

/* `fn(x: int) -> int { ... }`. The body is checked in a scope whose parent is
 * the enclosing scope but which is a *capture boundary*: any local of the
 * enclosing function that the body uses is recorded by env_lookup. Those are
 * captured by value, so they must be immutable (`val`, a parameter, a loop or
 * switch binder); a `var` would let a lambda observe or change state the
 * enclosing function also touches, which is what the actor model avoids. The
 * captures become the lambda's leading parameters.
 *
 * `expected` is the function type the lambda is being used as, when the context
 * says (an argument, a declared `val`, a return value, a struct field): omitted
 * parameter types and an omitted `->` are taken from it, so
 * `map<int, int>(xs, fn(n) { n * 2 })` needs no annotations. With no `->` and no
 * expected type, a body ending in an expression returns it (a call that returns
 * nothing leaves the lambda void), and a body without one is void. A non-void
 * lambda's final bare expression is its result; `return` remains for early exits. */
static Type check_lambda_expecting(Module *module, Env *env, Expr *e, const Type *expected) {
    FunctionDecl *lam = e->lambda_decl;
    if (e->lambda_checked) return lambda_type(lam, e->lambda_ncaptured);
    const Type *want = expected != NULL && expected->kind == TY_FUNC ? expected : NULL;

    if (want != NULL && want->type_args.count != lam->params.count) {
        ziman_error("type", e->line, e->col, "this lambda takes %d parameter(s) but a function taking %d is expected here (%s)",
                    lam->params.count, want->type_args.count, type_name(*want));
    }
    for (int i = 0; i < lam->params.count; i++) {
        Param *param = &lam->params.items[i];
        if (!param->type_omitted) continue;
        if (want == NULL) {
            ziman_error("type", param->line, param->col,
                        "cannot infer the type of parameter '%s' -- write it (`%s: T`), or use the lambda where a function type is expected "
                        "(an argument, a `val` with a declared type, a return value)",
                        param->name, param->name);
        }
        param->type = want->type_args.items[i];
        param->type_omitted = 0;
    }
    if (lam->return_omitted && want != NULL) {
        lam->return_type = *want->elem;
        lam->return_omitted = 0;
    }

    Env *root = env_new(env);
    root->is_lambda_boundary = 1;
    for (int i = 0; i < lam->params.count; i++) {
        Param *param = &lam->params.items[i];
        if (param->is_variadic || param->default_expr != NULL) {
            ziman_error("type", param->line, param->col, "a lambda's parameters can't be variadic or have defaults");
        }
        check_type_well_formed(module, param->type, param->line, param->col);
        if (!env_define(root, param->name, param->type, 0, value_int(0))) {
            ziman_error("type", param->line, param->col, "duplicate parameter name '%s'", param->name);
        }
    }
    if (!lam->return_omitted) check_type_well_formed(module, lam->return_type, lam->line, lam->col);

    if (lam->self_name != NULL) {
        /* Only ever set (STMT_VAL/STMT_VAR's own case above) when `!lam->return_omitted` --
         * self-reference needs the full signature known before checking a body that may call it,
         * which an inferred-from-the-body return type can't give. `lambda_type(lam, 0)` below
         * would read the wrong (not-yet-inferred) return type otherwise. */
        /* Defined directly in `root`, the same scope a declared parameter lives in -- not via an
         * ordinary env_lookup from inside the body crossing back out to the enclosing scope --
         * so referencing `self_name` is never recorded as a capture. `lambda_type(lam, 0)` reads
         * `lam->params` before this function's own capture-prepending step below runs, so it
         * already excludes captures from the signature `self_name` is bound at, the same as a
         * caller from outside sees. */
        if (!env_define(root, lam->self_name, lambda_type(lam, 0), 0, value_int(0))) {
            ziman_error("type", lam->line, lam->col, "recursive lambda '%s' has a parameter also named '%s'", lam->self_name, lam->self_name);
        }
    }

    StmtList *stmts = &lam->body->as.block.stmts;
    int n = stmts->count;
    Stmt *last = n > 0 ? stmts->items[n - 1] : NULL;
    int last_is_expr = last != NULL && last->kind == STMT_EXPR;

    ValueCtx *saved_value_ctx = g_value_ctx;
    g_value_ctx = NULL;
    if (!lam->return_omitted) {
        /* the return type is known: a final bare expression is the result */
        if (lam->return_type.kind != TY_VOID && last_is_expr) {
            Expr *value = last->as.expr_stmt.expr;
            last->kind = STMT_RETURN;
            memset(&last->as, 0, sizeof last->as);
            last->as.ret.value = value;
        }
        for (int i = 0; i < n; i++) check_stmt(module, root, stmts->items[i], lam->return_type, 0);
    } else {
        /* no `->` and no expected type: infer from a final expression */
        if (has_return_stmt(lam->body)) {
            ziman_error("type", lam->line, lam->col,
                        "cannot infer the return type of a lambda that uses 'return' -- write `-> T` after the parameters");
        }
        Type void_type = type_scalar(TY_VOID);
        lam->return_type = void_type;
        int checked = last_is_expr ? n - 1 : n;
        for (int i = 0; i < checked; i++) check_stmt(module, root, stmts->items[i], void_type, 0);
        if (last_is_expr) {
            Expr *value = last->as.expr_stmt.expr;
            if (value->kind == EXPR_TRY_UNWRAP) {
                check_stmt(module, root, last, void_type, 0);
            } else {
                Type t = check_expr(module, root, value);
                if (t.kind != TY_VOID) {
                    last->kind = STMT_RETURN;
                    memset(&last->as, 0, sizeof last->as);
                    last->as.ret.value = value;
                    lam->return_type = t;
                }
            }
        }
        lam->return_omitted = 0;
    }
    g_value_ctx = saved_value_ctx;
    if (lam->return_type.kind != TY_VOID && lam->return_type.kind != TY_UNIT && !stmt_always_returns(lam->body)) {
        ziman_error("type", lam->line, lam->col, "this lambda does not return a value on every path");
    }

    int ncaptured = root->cap_count;
    for (int i = 0; i < ncaptured; i++) {
        if (root->caps[i].is_mutable) {
            ziman_error("type", e->line, e->col,
                        "a lambda cannot capture '%s' because it is a 'var' -- copy it into a 'val' first and use that "
                        "(captures are by value, so the lambda and the enclosing function never share mutable state)",
                        root->caps[i].name);
        }
    }
    ParamList all = {0};
    for (int i = 0; i < ncaptured; i++) {
        Param p;
        memset(&p, 0, sizeof p);
        p.name = root->caps[i].name;
        p.type = root->caps[i].type;
        p.line = e->line;
        p.col = e->col;
        param_list_push(&all, p);
    }
    for (int i = 0; i < lam->params.count; i++) param_list_push(&all, lam->params.items[i]);
    lam->params = all;
    e->lambda_ncaptured = ncaptured;
    lam->self_ncaptured = ncaptured;
    e->lambda_checked = 1;
    /* Recorded on the type, not just checked here, because the lambda and the `spawn`/`send` that
     * rejects it are usually far apart: `val f = fn() { mutable_string_append(sb, "x") }` then `send(ch, f)`
     * three lines later. The type is what travels between the two. Captures are the only source --
     * a named function's value type carries 0, as does a lambda that captured nothing thread-bound.
     * A capture that is itself such a lambda is covered by its own flag. */
    Type lt = lambda_type(lam, ncaptured);
    for (int i = 0; i < ncaptured; i++) {
        if (type_holds_thread_bound(module, root->caps[i].type, NULL)) {
            lt.func_captures_thread_bound = 1;
            break;
        }
    }
    env_free(root);
    return lt;
}

static Type check_lambda(Module *module, Env *env, Expr *e) {
    return check_lambda_expecting(module, env, e, NULL);
}

/* Whether running `s` (an arm of an `if`/`switch` used inside an expression)
 * could leave the arm other than by yielding: a `return`, a `break`/`continue`
 * that is not inside a loop of the arm itself, or a `?` (which returns early).
 * Such an arm needs statement context (the interpreter can leave a statement,
 * not the middle of an expression). Nested lambdas are expressions and are
 * never walked: their `return` is their own. */
static int arm_may_leave(const Stmt *s, int in_loop) {
    switch (s->kind) {
        case STMT_RETURN: return 1;
        case STMT_BREAK:
        case STMT_CONTINUE: return !in_loop;
        case STMT_VAL:
        case STMT_VAR: return s->as.decl.init->kind == EXPR_TRY_UNWRAP;
        case STMT_EXPR: return s->as.expr_stmt.expr->kind == EXPR_TRY_UNWRAP;
        case STMT_YIELD: return s->as.yield_stmt.value->kind == EXPR_TRY_UNWRAP;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) if (arm_may_leave(s->as.block.stmts.items[i], in_loop)) return 1;
            return 0;
        case STMT_IF:
            return arm_may_leave(s->as.if_stmt.then_block, in_loop) || (s->as.if_stmt.else_block != NULL && arm_may_leave(s->as.if_stmt.else_block, in_loop));
        case STMT_WHILE: return arm_may_leave(s->as.while_stmt.body, 1);
        case STMT_FOR: return arm_may_leave(s->as.for_stmt.body, 1);
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) if (arm_may_leave(s->as.switch_stmt.arms[i].body, in_loop)) return 1;
            return 0;
        case STMT_VALUE_BLOCK: return s->as.value_block.target == STMT_RETURN || arm_may_leave(s->as.value_block.inner, in_loop);
        default: return 0;
    }
}

/* An `if`/`switch` inside an expression. With an expected type (an argument, a
 * declared `val`, a struct field) each arm is checked against it like an
 * initializer; without one the first arm decides. */
static Type check_value_block_expr(Module *module, Env *env, Expr *e, const Type *expected) {
    if (e->resolved_type.kind != TY_VOID) return e->resolved_type; /* already checked */
    Stmt *inner = e->as.value_block.inner;
    if (!arm_yields_or_leaves(inner)) {
        ziman_error("type", inner->line, inner->col,
                    "every arm of an 'if'/'switch' used as a value must end with a value expression");
    }
    if (arm_may_leave(inner, 0)) {
        ziman_error("type", inner->line, inner->col,
                    "an arm of an 'if'/'switch' used inside an expression cannot 'return', 'break', 'continue' or use 'try' -- "
                    "bind it to a 'val' (or return it) first, where an arm may leave");
    }
    ValueCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (expected != NULL) {
        ctx.has_expected = 1;
        ctx.expected = *expected;
    }
    ValueCtx *saved = g_value_ctx;
    g_value_ctx = &ctx;
    check_stmt(module, env, inner, type_scalar(TY_VOID), 0);
    g_value_ctx = saved;
    return ctx.has_expected ? ctx.expected : ctx.first;
}

/* The index and the value of an `a[i] = v`, checked here rather than left to whatever the
 * statement becomes. For an array that is still a rewrite into `set`; for a MutableArray there is
 * nothing left to rewrite into since F209, the write going through the handle. Doing it here is
 * what keeps the message about the array the author wrote: when both halves were rewrites, a bad
 * index reported "argument 2 of 'mutable_array_set'" and a bad value reported a type-argument
 * inference failure, both naming a call that is not in the source. `what` is the receiver as the
 * reader wrote it. */
static const Type *arr_type_elem(Type t) { return t.elem; }

static void check_index_assign_parts(Module *module, Env *env, Expr *index, Expr *value,
                                     const Type *elem, const char *what) {
    Type idx_type = check_expr(module, env, index);
    if (idx_type.kind != TY_INT) {
        ziman_error("type", index->line, index->col, "array index must be int, found %s", type_name(idx_type));
    }
    Type value_type = check_expr(module, env, value);
    require_not_void(value, "as an array element");
    if (elem != NULL && !type_equals(value_type, *elem)) {
        ziman_error("type", value->line, value->col, "cannot assign %s into %s of %s",
                    type_name(value_type), what, type_name(*elem));
    }
}

static void check_stmt(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop) {
    ZIMAN_STACK_CHECK();
    /* The one statement the parser deliberately left for here: `let x = 1` and its relatives,
     * rewritten into the declaration they meant so that this reports the word once and the
     * recovery around this call poisons the name, rather than the program reporting it undefined
     * at every mention. See Stmt.foreign_decl_keyword. */
    if (s->foreign_decl_keyword != NULL) {
        ziman_error("syntax", s->line, s->col,
                    "Ziman has no '%s' -- declare with 'val' (never reassigned) or 'var' (may be), and the type, "
                    "when written, comes after the name: 'val x = 1', 'var total: int = 0'",
                    s->foreign_decl_keyword);
    }
    switch (s->kind) {
        case STMT_SWITCH:
            check_switch(module, env, s, fn_return_type, in_loop);
            break;
        case STMT_VAL:
        case STMT_VAR: {
            if (s->as.decl.has_declared_type) {
                check_type_well_formed(module, s->as.decl.declared_type, s->line, s->col);
            }
            /* `val NAME = fn(...) -> T { ... }`: let NAME's own body call itself, recursively,
             * by this name -- never for `var` (see FunctionDecl.self_name's own doc comment for
             * why a reassignable binding would make "self" ambiguous), and only when the return
             * type is already written explicitly. Self-reference needs the signature known
             * *before* checking the body (see check_lambda_expecting), so an omitted return type
             * (inferred *from* that same body) can never support it -- rather than reject every
             * omitted-return-type `val`-bound lambda that doesn't even use self-reference (the
             * overwhelmingly common case, `val greet = fn(name: string) { print(...) }`), simply
             * don't offer it there: `self_name` stays NULL, exactly as before this feature
             * existed, and a lambda that genuinely needs to recurse without a declared type
             * fails the ordinary "undefined variable" way, discoverable by adding `-> T`. Set
             * before check_initializer reaches check_lambda_expecting, which defines
             * `self_name` directly in the lambda's own root scope so a reference to it from
             * inside the body is never treated as a capture. */
            if (s->kind == STMT_VAL && s->as.decl.init->kind == EXPR_LAMBDA) {
                FunctionDecl *lam = (FunctionDecl *)s->as.decl.init->lambda_decl;
                if (!lam->return_omitted) lam->self_name = s->as.decl.name;
            }
            Type declared = check_initializer(module, env, &s->as.decl.init, s->as.decl.has_declared_type,
                                              s->as.decl.declared_type, s->as.decl.name, fn_return_type);
            int is_mutable = s->kind == STMT_VAR;
            EnvDefineResult declared_as = env_define(env, s->as.decl.name, declared, is_mutable, value_int(0));
            if (declared_as == ENV_REDECLARED) {
                ziman_error("type", s->line, s->col, "'%s' is already declared in this scope", s->as.decl.name);
            }
            /* Every declaration, not only a `var`: an unread one is dead weight whichever keyword
             * declared it, and the two findings share this table.
             *
             * `ENV_DEFINED` excludes `_`, which binds nothing -- it is a true discard, and
             * `val _ = f()` is already the way to say "deliberately unread". This used to ask
             * `strcmp(name, "_")` here instead, which is the same question asked in the wrong
             * place: the answer belongs to `env_define`, and asking it locally left the
             * identical NULL dereference standing in the value-block case below. */
            if (g_lint_active && declared_as == ENV_DEFINED) {
                env_lookup(env, s->as.decl.name)->lint_id =
                    lint_register_var(s->as.decl.name, s->line, s->col, is_mutable);
            }
            break;
        }
        case STMT_YIELD: {
            ValueCtx *ctx = g_value_ctx;
            Type t = check_initializer(module, env, &s->as.yield_stmt.value, ctx->has_expected, ctx->expected, NULL, fn_return_type);
            if (ctx->count == 0) {
                ctx->first = t;
            } else if (!type_equals(t, ctx->first)) {
                ziman_error("type", s->line, s->col,
                            "the arms of an 'if'/'switch' used as a value must all have the same type: found %s and %s",
                            type_name(ctx->first), type_name(t));
            }
            ctx->count++;
            break;
        }
        case STMT_VALUE_BLOCK: {
            Stmt *inner = s->as.value_block.inner;
            if (!arm_yields_or_leaves(inner)) {
                ziman_error("type", inner->line, inner->col,
                            "every arm of an 'if'/'switch' used as a value must end with a value expression (or return/break/continue)");
            }
            ValueCtx ctx;
            memset(&ctx, 0, sizeof ctx);
            StmtKind target = s->as.value_block.target;
            Binding *assigned = NULL;
            if (target == STMT_VAL || target == STMT_VAR) {
                if (s->as.value_block.has_declared_type) {
                    check_type_well_formed(module, s->as.value_block.declared_type, s->line, s->col);
                    ctx.has_expected = 1;
                    ctx.expected = s->as.value_block.declared_type;
                }
            } else if (target == STMT_ASSIGN) {
                int depth, slot;
                assigned = env_lookup_resolved(env, s->as.value_block.name, &depth, &slot);
                if (assigned == NULL) {
                    ziman_error("type", s->line, s->col, "assignment to undefined variable '%s'", s->as.value_block.name);
                }
                s->resolved_depth = depth;
                s->resolved_slot = slot;
                if (assigned->lint_id > 0) g_lint_vars[assigned->lint_id - 1].assigned = 1;
                if (!assigned->is_mutable) {
                    ziman_error("type", s->line, s->col, "cannot assign to '%s' because it was declared with 'val' (use 'var' to make it mutable)", s->as.value_block.name);
                }
                ctx.has_expected = 1;
                ctx.expected = assigned->type;
            } else {
                if (fn_return_type.kind == TY_VOID) {
                    ziman_error("type", s->line, s->col, "cannot return a value from a function that returns nothing");
                }
                ctx.has_expected = 1;
                ctx.expected = fn_return_type;
            }
            ValueCtx *saved = g_value_ctx;
            g_value_ctx = &ctx;
            check_stmt(module, env, inner, fn_return_type, in_loop);
            g_value_ctx = saved;
            if (ctx.count == 0 && target != STMT_RETURN) {
                ziman_error("type", s->line, s->col, "no arm of this 'if'/'switch' produces a value (every arm leaves)");
            }
            if (target == STMT_VAL || target == STMT_VAR) {
                Type result = ctx.has_expected ? ctx.expected : ctx.first;
                EnvDefineResult bound_as =
                    env_define(env, s->as.value_block.name, result, target == STMT_VAR, value_int(0));
                if (bound_as == ENV_REDECLARED) {
                    ziman_error("type", s->line, s->col, "'%s' is already declared in this scope", s->as.value_block.name);
                }
                if (target == STMT_VAR && g_lint_active && bound_as == ENV_DEFINED) {
                    env_lookup(env, s->as.value_block.name)->lint_id =
                        lint_register_var(s->as.value_block.name, s->line, s->col, 1);
                }
            }
            break;
        }
        /* `a[i] = v`, parsed as itself because only the receiver's type says which statement it
         * is. An array is a value, so the element write is a rebinding of the name; a MutableArray
         * is a container, so it is a write through the handle and the name is untouched. Both are
         * statements the checker already knows how to check, so this rewrites into one of them and
         * re-checks, which is also what gives the rebinding the ordinary `val`/`var` diagnostic
         * for free. */
        case STMT_INDEX_ASSIGN: {
            Expr *target = s->as.index_assign.target;
            Expr *base = target->as.index.array;
            Expr *index = target->as.index.index;
            Expr *value = s->as.index_assign.value;
            Type base_type = check_expr(module, env, base);

            if (base_type.kind == TY_ARRAY) {
                /* Only a name can be rebound. `p.xs[0] = v` and `rows[0][1] = v` would each need a
                 * copy of something larger than the array -- `p = with p { xs: set(p.xs, 0, v) }`
                 * -- and writing that out is the author's decision, not a rewrite's. */
                if (base->kind != EXPR_IDENT) {
                    ziman_error("type", base->line, base->col,
                                "only a variable's array can be assigned through '[...]' -- this one is an "
                                "expression, so write the copy out: 'x = set(x, i, value)' for the array, or "
                                "'p = with p { field: set(p.field, i, value) }' for a field");
                }
                check_index_assign_parts(module, env, index, value, arr_type_elem(base_type), "an array");
                Stmt *rebind = stmt_new(STMT_ASSIGN, s->line, s->col);
                rebind->as.assign.name = base->as.ident_name;
                rebind->as.assign.value = mk_call3_expr("set", base, index, value, s->line, s->col);
                *s = *rebind;
                check_stmt(module, env, s, fn_return_type, in_loop);
                break;
            }
            if (base_type.kind == TY_STRUCT && base_type.struct_name != NULL &&
                strcmp(base_type.struct_name, "MutableArray") == 0) {
                /* **The one statement kind that reaches the interpreter as itself.** It was
                 * rewritten onto a `mutable_array_set` call until F209; the write goes through the
                 * handle now, so the statement needs no name to stand for and `mutable_array_set`
                 * needs no declaration. `target`'s type is set for the benefit of anything that
                 * walks a checked tree expecting every expression to carry one. */
                check_index_assign_parts(module, env, index, value,
                                         base_type.type_args.count == 1 ? &base_type.type_args.items[0] : NULL,
                                         "a MutableArray");
                if (base_type.type_args.count == 1) target->resolved_type = base_type.type_args.items[0];
                break;
            }
            /* Everything else, named so the reader is pointed at the write that does exist. The
             * parser used to answer all of these with "arrays are values", which was wrong for a
             * MutableMap and misleading for a map. */
            if (base_type.kind == TY_MAP) {
                ziman_error("type", base->line, base->col,
                            "a map is not written with '[...]' -- write 'm = set(m, key, value)' (and see "
                            "MutableMap for filling one in a loop)");
            }
            if (base_type.kind == TY_SET) {
                ziman_error("type", base->line, base->col,
                            "a set is not written with '[...]' -- add with 's = add(s, value)'");
            }
            if (base_type.kind == TY_STRUCT && base_type.struct_name != NULL &&
                strcmp(base_type.struct_name, "MutableMap") == 0) {
                ziman_error("type", base->line, base->col,
                            "a MutableMap is not written with '[...]' -- write 'set(m, key, value)'");
            }
            if (base_type.kind == TY_STRUCT && base_type.struct_name != NULL &&
                strcmp(base_type.struct_name, "MutableString") == 0) {
                ziman_error("type", base->line, base->col,
                            "a MutableString is not written with '[...]' -- write 'mutable_string_set(s, i, piece)'");
            }
            ziman_error("type", base->line, base->col, "cannot assign into %s with '[...]' (not an array)",
                        type_name(base_type));
            break;
        }
        case STMT_ASSIGN: {
            int depth, slot;
            Binding *b = env_lookup_resolved(env, s->as.assign.name, &depth, &slot);
            if (b == NULL) {
                ziman_error("type", s->line, s->col, "assignment to undefined variable '%s'", s->as.assign.name);
            }
            s->resolved_depth = depth;
            s->resolved_slot = slot;
            if (b->poisoned) ziman_abort_quietly();
            if (b->lint_id > 0) g_lint_vars[b->lint_id - 1].assigned = 1;
            if (!b->is_mutable) {
                ziman_error("type", s->line, s->col, "cannot assign to '%s' because it was declared with 'val' (use 'var' to make it mutable)", s->as.assign.name);
            }
            /* The variable's declared type is the context the value needs, exactly as it is in the
             * `val`/`var` declaration one case up -- which already accepts `val a: Option<int> = 5`
             * and a bare `Some(x)`. Assignment was checking its value with no context at all, so
             * `found = Some(x)` was refused where `var found: Option<string> = None` beside it was
             * fine. check_expr_expecting subsumes the lambda case this used to handle by hand. */
            Type value_type = check_expr_expecting(module, env, &s->as.assign.value, b->type);
            require_not_void(s->as.assign.value, "in an assignment");
            if (!type_equals(value_type, b->type)) {
                ziman_error("type", s->as.assign.value->line, s->as.assign.value->col,
                            "cannot assign a value of type %s to '%s' (declared as %s)",
                            type_name(value_type), s->as.assign.name, type_name(b->type));
            }
            if (in_loop && g_lint_active) lint_quadratic_accumulation(s);
            break;
        }
        case STMT_IF: {
            Type cond_type = check_expr(module, env, s->as.if_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.if_stmt.cond->line, s->as.if_stmt.cond->col, "'if' condition must be bool, found %s", type_name(cond_type));
            }
            check_block(module, env, s->as.if_stmt.then_block, fn_return_type, in_loop);
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    check_stmt(module, env, s->as.if_stmt.else_block, fn_return_type, in_loop);
                } else {
                    check_block(module, env, s->as.if_stmt.else_block, fn_return_type, in_loop);
                }
            }
            break;
        }
        case STMT_WHILE: {
            Type cond_type = check_expr(module, env, s->as.while_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.while_stmt.cond->line, s->as.while_stmt.cond->col, "'while' condition must be bool, found %s", type_name(cond_type));
            }
            if (g_lint_active) lint_builder_read_by_conversion(s->as.while_stmt.body);
            check_block(module, env, s->as.while_stmt.body, fn_return_type, 1);
            break;
        }
        case STMT_FOR: {
            Env *scope = env_new(env);
            if (s->as.for_stmt.is_range) {
                Type start_type = check_expr(module, env, s->as.for_stmt.range_start);
                if (start_type.kind != TY_INT) {
                    ziman_error("type", s->as.for_stmt.range_start->line, s->as.for_stmt.range_start->col,
                                "'for' range start must be int, found %s", type_name(start_type));
                }
                Type end_type = check_expr(module, env, s->as.for_stmt.range_end);
                if (end_type.kind != TY_INT) {
                    ziman_error("type", s->as.for_stmt.range_end->line, s->as.for_stmt.range_end->col,
                                "'for' range end must be int, found %s", type_name(end_type));
                }
                env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(0));
            } else {
                Type iter_type = check_expr(module, env, s->as.for_stmt.iterable);
                int is_iter = iter_type.kind == TY_STRUCT && iter_type.struct_name != NULL &&
                              strcmp(iter_type.struct_name, "Iter") == 0 && iter_type.type_args.count == 1;
                /* A MutableArray iterates like the array it is becoming, which is the point: reading one
                 * back used to mean snapshot, a full copy per call (`--lint` flags that in a loop). The
                 * loop reads the live buffer by index and stops at whatever the length is then, so a body
                 * that appends sees its own additions and one that clears ends the loop -- neither can read
                 * past the end. */
                int is_array_builder = iter_type.kind == TY_STRUCT && iter_type.struct_name != NULL &&
                                       strcmp(iter_type.struct_name, "MutableArray") == 0 &&
                                       iter_type.type_args.count == 1;
                /* A MutableMap and MutableSet iterate like the Map and Set they are building, for the
                 * same reason a MutableArray does: reading one back otherwise means `snapshot`, a
                 * full copy per call. Unlike the array they walk a snapshot of the entries (see the
                 * interpreter's own branch), so the body may change the handle freely. */
                int is_mutable_map = iter_type.kind == TY_STRUCT && iter_type.struct_name != NULL &&
                                     strcmp(iter_type.struct_name, "MutableMap") == 0 && iter_type.type_args.count == 2;
                int is_mutable_set = iter_type.kind == TY_STRUCT && iter_type.struct_name != NULL &&
                                     strcmp(iter_type.struct_name, "MutableSet") == 0 && iter_type.type_args.count == 1;
                if (iter_type.kind != TY_ARRAY && iter_type.kind != TY_MAP && iter_type.kind != TY_SET &&
                    !is_iter && !is_array_builder && !is_mutable_map && !is_mutable_set) {
                    ziman_error("type", s->as.for_stmt.iterable->line, s->as.for_stmt.iterable->col,
                                "'for' can only iterate an array, a map, a set, any of the four mutable collections, an Iter or a range ('a..b', which stops before b), found %s",
                                type_name(iter_type));
                }
                if (is_mutable_set && s->as.for_stmt.binder2 != NULL) {
                    ziman_error("type", s->as.for_stmt.iterable->line, s->as.for_stmt.iterable->col,
                                "a MutableSet has no index or value to bind: write 'for item in s'");
                }
                if (is_iter && s->as.for_stmt.binder2 != NULL) {
                    ziman_error("type", s->as.for_stmt.iterable->line, s->as.for_stmt.iterable->col,
                                "an Iter has no index to bind: write 'for item in iter'");
                }
                if (iter_type.kind == TY_SET && s->as.for_stmt.binder2 != NULL) {
                    ziman_error("type", s->as.for_stmt.iterable->line, s->as.for_stmt.iterable->col,
                                "a set has no index or value to bind: write 'for item in set'");
                }
                if (iter_type.kind == TY_MAP) {
                    /* `for k in m` binds each key; `for k, v in m` each key and its value */
                    env_define(scope, s->as.for_stmt.binder1, *iter_type.elem, 0, value_int(0));
                    if (s->as.for_stmt.binder2 != NULL) env_define(scope, s->as.for_stmt.binder2, *iter_type.elem_err, 0, value_int(0));
                } else if (is_iter) {
                    env_define(scope, s->as.for_stmt.binder1, iter_type.type_args.items[0], 0, value_int(0));
                } else if (is_mutable_map) {
                    /* `for k in m` binds each key; `for k, v in m` each key and its value -- the two
                     * shapes an immutable Map takes. */
                    env_define(scope, s->as.for_stmt.binder1, iter_type.type_args.items[0], 0, value_int(0));
                    if (s->as.for_stmt.binder2 != NULL) {
                        env_define(scope, s->as.for_stmt.binder2, iter_type.type_args.items[1], 0, value_int(0));
                    }
                } else if (is_mutable_set) {
                    env_define(scope, s->as.for_stmt.binder1, iter_type.type_args.items[0], 0, value_int(0));
                } else if (is_array_builder) {
                    /* `for x in ab` and `for i, x in ab`, the same two shapes an array takes. */
                    if (s->as.for_stmt.binder2 == NULL) {
                        env_define(scope, s->as.for_stmt.binder1, iter_type.type_args.items[0], 0, value_int(0));
                    } else {
                        env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(0));
                        env_define(scope, s->as.for_stmt.binder2, iter_type.type_args.items[0], 0, value_int(0));
                    }
                } else if (s->as.for_stmt.binder2 == NULL) {
                    env_define(scope, s->as.for_stmt.binder1, *iter_type.elem, 0, value_int(0));
                } else {
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(0));
                    env_define(scope, s->as.for_stmt.binder2, *iter_type.elem, 0, value_int(0));
                }
            }
            int binders_pushed_from = g_loop_binder_count;
            if (s->as.for_stmt.is_range || s->as.for_stmt.binder2 != NULL) {
                push_loop_binder(s->as.for_stmt.binder1, NULL);
                if (s->as.for_stmt.binder2 != NULL) push_loop_binder(s->as.for_stmt.binder2, NULL);
            } else {
                Expr *iterable = s->as.for_stmt.iterable;
                push_loop_binder(s->as.for_stmt.binder1, iterable->kind == EXPR_IDENT && iterable->resolved_type.kind == TY_ARRAY ? iterable->as.ident_name : NULL);
            }
            if (g_lint_active) lint_builder_read_by_conversion(s->as.for_stmt.body);
            check_stmts_in_scope(module, scope, s->as.for_stmt.body, fn_return_type, 1);
            g_loop_binder_count = binders_pushed_from;
            env_free(scope);
            break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (!in_loop) {
                ziman_error("type", s->line, s->col, "'%s' outside of a loop", s->kind == STMT_BREAK ? "break" : "continue");
            }
            break;
        case STMT_SPAWN: {
            const char *callee = s->as.spawn_stmt.callee;
            /* `spawn` resolves its callee here rather than through check_call, so it needs the
             * shadow too: `spawn count(path, out)` otherwise found the builtin's declaration and
             * reported the program's void function as "returning int". */
            FunctionDecl *shadow = shadowing_fn(callee);
            FunctionDecl *fn = shadow != NULL ? shadow : find_function(module, callee);
            s->as.spawn_stmt.spawn_fn = fn;
            if (fn == NULL) {
                ziman_error("type", s->line, s->col, "call to undefined function '%s'", callee);
            }
            if (fn->return_type.kind != TY_VOID) {
                ziman_error("type", s->line, s->col,
                            "'spawn' requires a function that returns nothing, found %s returning %s "
                            "(send a result back through a Channel instead)",
                            callee, type_name(fn->return_type));
            }
            TypeList *spawn_type_args = &s->as.spawn_stmt.type_args;
            if (fn->type_params.count == 0 && spawn_type_args->count > 0) {
                ziman_error("type", s->line, s->col, "'%s' is not generic, but was given %d type argument(s)", callee, spawn_type_args->count);
            }
            if (fn->type_params.count > 0) {
                if (spawn_type_args->count != fn->type_params.count) {
                    ziman_error("type", s->line, s->col, "'%s' is generic and takes %d type argument(s), written out: spawn %s<...>(...); found %d",
                                callee, fn->type_params.count, callee, spawn_type_args->count);
                }
                for (int ti = 0; ti < spawn_type_args->count; ti++) check_type_well_formed(module, spawn_type_args->items[ti], s->line, s->col);
                check_type_args_satisfy_bounds(module, &fn->type_params, spawn_type_args, s->line, s->col);
            }
            if (s->as.spawn_stmt.args.count != fn->params.count) {
                ziman_error("type", s->line, s->col, "function '%s' expects %d argument(s), found %d",
                            callee, fn->params.count, s->as.spawn_stmt.args.count);
            }
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) {
                Expr *arg = s->as.spawn_stmt.args.items[i];
                Type arg_type = check_expr(module, env, arg);
                require_not_void(arg, "as an argument");
                reject_thread_bound(module, arg_type, "passed to another thread with 'spawn'", arg->line, arg->col);
                Type expected = fn->params.items[i].type;
                if (fn->type_params.count > 0) expected = substitute_type(expected, &fn->type_params, spawn_type_args);
                if (!type_equals(arg_type, expected)) {
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' expects %s, found %s",
                                i + 1, callee, type_name(expected), type_name(arg_type));
                }
            }
            /* --lint: a thread that can die silently, holding a channel the parent is waiting on. Without
             * `on_error`, a runtime error in a spawned function ends that thread and nothing else; if the
             * parent is blocked in `receive` on a channel only that thread was ever going to send to, the
             * program hangs for good. A12 added deadlock detection for the whole-program case, which
             * reports rather than hangs; this is the cheaper half, catching it before it runs.
             *
             * Only a spawn that is handed a Channel is reported. A fire-and-forget thread that shares no
             * channel cannot strand anybody, so warning about it would be noise. The shape was chosen
             * against the two real cases in this library -- `pg.zm` and `mysql.zm` each spawn the loop
             * their own `requests` channel feeds, with no handler -- which `--lint` itself will never
             * report, since it only ever reports the code the person running it wrote. */
            if (g_lint_active && s->as.spawn_stmt.on_error == NULL) {
                for (int i = 0; i < s->as.spawn_stmt.args.count; i++) {
                    Expr *arg = s->as.spawn_stmt.args.items[i];
                    if (arg->resolved_type.kind == TY_CHANNEL) {
                        ziman_warning(g_lint_file, s->line, s->col,
                                      "'spawn %s' has no 'on_error' and is given a Channel: if it fails, "
                                      "whoever is waiting on that channel waits for ever",
                                      callee);
                        g_lint_count++;
                        break;
                    }
                }
            }
            if (s->as.spawn_stmt.on_error != NULL) {
                /* the handler: a function of one string (the error message), returning nothing */
                TypeList handler_params = {0};
                type_list_push(&handler_params, type_scalar(TY_STRING));
                Type handler_type = type_func(handler_params, type_scalar(TY_VOID));
                Type given = check_expr_expecting(module, env, &s->as.spawn_stmt.on_error, handler_type);
                if (!type_equals(given, handler_type)) {
                    ziman_error("type", s->as.spawn_stmt.on_error->line, s->as.spawn_stmt.on_error->col,
                                "the 'on_error' handler of spawn must be fn(string) (it is called with the message of the error that ended the thread), found %s",
                                type_name(given));
                }
            }
            break;
        }
        case STMT_RETURN: {
            if (s->as.ret.value == NULL && (fn_return_type.kind == TY_UNIT ||
                                            (fn_return_type.kind == TY_RESULT && fn_return_type.elem->kind == TY_UNIT))) {
                /* `return` in a function that gives back `unit` (or `Result<unit, E>`) is `return unit` */
                s->as.ret.value = expr_new(EXPR_UNIT, s->line, s->col);
            }
            if (s->as.ret.value == NULL) {
                if (fn_return_type.kind != TY_VOID) {
                    ziman_error("type", s->line, s->col, "bare 'return' in a function declared to return %s", type_name(fn_return_type));
                }
            } else {
                /* check_expr_expecting resolves ok(...)/err(...)/
                 * channel_new()/an empty array literal/a bare Option<T>
                 * or Result<T, E> value against the function's own
                 * declared return type when the value is exactly one of
                 * those; falls back to plain check_expr otherwise. */
                Type value_type = check_expr_expecting(module, env, &s->as.ret.value, fn_return_type);
                if (fn_return_type.kind == TY_VOID) {
                    ziman_error("type", s->as.ret.value->line, s->as.ret.value->col, "cannot return a value from a function declared with no return type");
                }
                if (!type_equals(value_type, fn_return_type)) {
                    ziman_error("type", s->as.ret.value->line, s->as.ret.value->col,
                                "returned %s but function is declared to return %s", type_name(value_type), type_name(fn_return_type));
                }
            }
            break;
        }
        case STMT_EXPR: {
            /* A bare `inner?` statement -- checked the same way as
             * val/var's own `= inner?` form, just with the unwrapped
             * value discarded rather than bound to a name. */
            if (s->as.expr_stmt.expr->kind == EXPR_TRY_UNWRAP) {
                check_try_unwrap(module, env, s->as.expr_stmt.expr, fn_return_type);
            } else {
                Type t = check_expr(module, env, s->as.expr_stmt.expr);
                /* A Result discarded as a bare statement throws away a real Err along with an Ok, silently --
                 * `defer` is the one place that is still allowed (a deliberate, documented choice, checked by
                 * check_defer instead of here, since a defer'd call has nowhere left to report failure to).
                 * `val _ = ...`/`try ...` say the same discard on purpose; see env_define's own comment on `_`. */
                if (t.kind == TY_RESULT) {
                    ziman_error("type", s->as.expr_stmt.expr->line, s->as.expr_stmt.expr->col,
                                "this %s is discarded, silently dropping a possible error -- bind it ('val _ = ...' "
                                "to discard on purpose), or use 'try' to propagate the error", type_name(t));
                }
                /* The same discard, one layer out: a pure builtin called in statement position. Its
                 * whole output is the value it returns, so throwing that away cannot have been meant
                 * -- and the 2026-10-12 benchmark round 2 shows it is a habit, not a typo. `push(arr, x)`
                 * compiled and did nothing in four of fifty attempts, because arrays are values and the
                 * new array went nowhere; two of those printed an empty result. The names below are
                 * builtins with no effects, so unlike a user function none of them can be called for
                 * one; channel and print builtins are deliberately absent. `val _ = ...` still discards
                 * on purpose, exactly as it does for a Result above. */
                /* Both the gate and the message read the name the *author* wrote, not the one a
                 * rewrite dispatches on -- see Expr's `rewritten_from`. Reading the rewritten name
                 * got this wrong twice over: discarding `mutable([1, 2])` reported
                 * `$mutable_array_lit`, which does not lex as an identifier, while discarding
                 * `mutable(42)` was accepted in silence, `mutable_new` not being on the list even
                 * though `mutable` is. A rewrite target that returns void, such as a `push` onto a
                 * builder, is excluded by the `t.kind != TY_VOID` test above rather than by its
                 * name. */
                else if (s->as.expr_stmt.expr->kind == EXPR_CALL && t.kind != TY_VOID
                         && authored_callee(s->as.expr_stmt.expr) != NULL
                         && shadowing_fn(authored_callee(s->as.expr_stmt.expr)) == NULL
                         && !call_is_effectful_overload(s->as.expr_stmt.expr)
                         && (is_pure_builtin_name(authored_callee(s->as.expr_stmt.expr))
                             || call_is_pure_prelude_overload(module, s->as.expr_stmt.expr))) {
                    const char *name = authored_callee(s->as.expr_stmt.expr);
                    if (strcmp(name, "push") == 0 || strcmp(name, "insert") == 0 || strcmp(name, "set") == 0
                        || strcmp(name, "remove") == 0 || strcmp(name, "swap") == 0) {
                        ziman_error("type", s->as.expr_stmt.expr->line, s->as.expr_stmt.expr->col,
                                    "the %s returned by '%s' is discarded, and '%s' does not modify its argument -- "
                                    "arrays, maps and sets are values, so assign the result back "
                                    "('arr = %s(arr, ...)'), or build one up with MutableArray/MutableMap "
                                    "('val _ = ...' discards on purpose)",
                                    type_name(t), name, name, name);
                    } else {
                        ziman_error("type", s->as.expr_stmt.expr->line, s->as.expr_stmt.expr->col,
                                    "the %s returned by '%s' is discarded, and '%s' has no other effect -- "
                                    "bind the result ('val x = %s(...)', or 'val _ = ...' to discard on purpose)",
                                    type_name(t), name, name, name);
                    }
                }
            }
            break;
        }
        case STMT_DEFER:
            check_defer(module, env, s);
            break;
        case STMT_BLOCK:
            check_block(module, env, s, fn_return_type, in_loop);
            break;
    }
}

/* Recursively verifies that every struct/enum name mentioned by `t`
 * (including inside array element types) actually refers to a declared
 * one. Run over every struct field type, enum variant payload type, and
 * function param/return type before any function body is checked, so a
 * typo in a type reference is caught at one precise location instead of
 * cascading into confusing mismatches later (see typecheck()). A TY_STRUCT
 * here that doesn't resolve to a real struct might have actually meant an
 * enum -- the loader already reclassifies same-module references it can
 * tell apart (self_qualify_type), but this is still the first point that
 * can report a *genuinely* undefined name either way. TY_ENUM itself is
 * never produced by parse_type() directly (see ast.h's Type comment), so
 * reaching this function with one already means the loader resolved it. */
static void check_type_well_formed(Module *module, Type t, int line, int col) {
    ZIMAN_STACK_CHECK();
    if (t.kind == TY_ARRAY) {
        check_type_well_formed(module, *t.elem, line, col);
    } else if (t.kind == TY_RESULT) {
        check_type_well_formed(module, *t.elem, line, col);
        check_type_well_formed(module, *t.elem_err, line, col);
    } else if (t.kind == TY_STRUCT) {
        StructDecl *decl = find_struct(module, t.struct_name);
        if (decl == NULL) {
            /* `Unit`/`Void` are not a misspelling of anything: Ziman has no unit type, and the fix
             * is to delete the clause rather than to name it. Seven of ten cold attempts in the
             * 2026-10-09 pilot wrote one, making it the most frequent habit in that set, and the
             * old message framed a deletion as a missing type -- so a reader went looking for the
             * right name, or defined one. */
            if (ascii_eq_ignore_case(t.struct_name, "unit") || ascii_eq_ignore_case(t.struct_name, "void") ||
                ascii_eq_ignore_case(t.struct_name, "nil") || ascii_eq_ignore_case(t.struct_name, "none")) {
                ziman_error("type", line, col,
                            "Ziman has no '%s' type -- a function that returns no value leaves the '->' clause off "
                            "altogether, as 'fn main() { ... }'", t.struct_name);
            }
            const char *renamed = renamed_builder_type(t.struct_name);
            if (renamed != NULL) {
                ziman_error("type", line, col,
                            "'%s' is now '%s': the four builders are the mutable collections. Every 'Mutable*' type "
                            "belongs to the thread that made it, which the name does not say -- it cannot be sent over "
                            "a Channel, passed to 'spawn', or held in a module-level 'val'",
                            t.struct_name, renamed);
            }
            const char *primitive = suggest_primitive_type(t.struct_name);
            if (primitive != NULL) {
                ziman_error("type", line, col,
                            "unknown type '%s' -- did you mean '%s'? (the built-in types are lowercase: int, float, "
                            "bool, string, byte, bigint, decimal)", t.struct_name, primitive);
            }
            const char *near = suggest_function_name(module, t.struct_name);
            if (near != NULL) {
                ziman_error("type", line, col, "unknown struct or enum type '%s' -- did you mean '%s'?", t.struct_name, near);
            }
            ziman_error("type", line, col,
                        "unknown struct or enum type '%s' (the built-in types are lowercase: int, float, bool, "
                        "string, byte, bigint, decimal)", t.struct_name);
        }
        if (t.type_args.count != decl->type_params.count) {
            ziman_error("type", line, col, "'%s' takes %d type argument(s), found %d", t.struct_name, decl->type_params.count, t.type_args.count);
        }
        for (int i = 0; i < t.type_args.count; i++) {
            check_type_well_formed(module, t.type_args.items[i], line, col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, &t.type_args, line, col);
    } else if (t.kind == TY_ENUM) {
        EnumDecl *decl = find_enum(module, t.enum_name);
        if (decl == NULL) {
            ziman_error("type", line, col, "unknown enum type '%s'", t.enum_name);
        }
        if (t.type_args.count != decl->type_params.count) {
            ziman_error("type", line, col, "'%s' takes %d type argument(s), found %d", t.enum_name, decl->type_params.count, t.type_args.count);
        }
        for (int i = 0; i < t.type_args.count; i++) {
            check_type_well_formed(module, t.type_args.items[i], line, col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, &t.type_args, line, col);
    } else if (t.kind == TY_CHANNEL) {
        check_type_well_formed(module, *t.elem, line, col);
        reject_thread_bound(module, *t.elem, "sent over a channel", line, col);
    } else if (t.kind == TY_MAP || t.kind == TY_SET) {
        check_type_well_formed(module, *t.elem, line, col);
        if (t.kind == TY_MAP) check_type_well_formed(module, *t.elem_err, line, col);
        if (!type_supports_hashing(module, *t.elem, 0)) {
            ziman_error("type", line, col,
                        "%s keys must support '==' and hashing (int, float, bool, string, bigint, decimal, byte, arrays of scalars, "
                        "structs/enums made of those, or a type parameter bounded by 'Hash'), found %s",
                        t.kind == TY_MAP ? "map" : "set", type_name(*t.elem));
        }
    } else if (t.kind == TY_FUNC) {
        for (int i = 0; i < t.type_args.count; i++) check_type_well_formed(module, t.type_args.items[i], line, col);
        check_type_well_formed(module, *t.elem, line, col);
    }
}

/* Type-checks every module-level `val` in file order (a later one may
 * reference an earlier one, same-module or already-`use`d, the same
 * "must already be declared" rule an ordinary local `val` enforces
 * within one function body -- see docs/DESIGN.md's "Global val" entry
 * for why this is a deliberate scope cut, not an oversight), building
 * `g_global_env` as it goes by reusing check_stmt's own STMT_VAL case
 * wholesale (via a synthetic Stmt) rather than re-deriving its declared-
 * vs-inferred type logic here. Must run before any check_function call,
 * since g_global_env is what every function's own scope is rooted in. */
static void check_global_vals(Module *module) {
    g_global_env = env_new(NULL);
    g_global_env->is_global = 1;
    for (int i = 0; i < module->vals.count; i++) {
        GlobalValDecl *v = module->vals.items[i];
        Stmt synthetic = {0};
        synthetic.kind = STMT_VAL;
        synthetic.line = v->line;
        synthetic.col = v->col;
        synthetic.as.decl.name = v->name;
        synthetic.as.decl.declared_type = v->type;
        synthetic.as.decl.has_declared_type = v->has_declared_type;
        synthetic.as.decl.init = v->init;
        check_stmt(module, g_global_env, &synthetic, type_scalar(TY_VOID), 0);
        if (v->deprecated) {
            if (g_deprecated_val_count == g_deprecated_val_cap) {
                g_deprecated_val_cap = g_deprecated_val_cap == 0 ? 8 : g_deprecated_val_cap * 2;
                g_deprecated_vals = ziman_xrealloc(g_deprecated_vals,
                                                   (size_t)g_deprecated_val_cap * sizeof *g_deprecated_vals);
            }
            g_deprecated_vals[g_deprecated_val_count].name = v->name;
            g_deprecated_vals[g_deprecated_val_count].message = v->deprecated_message;
            g_deprecated_vals[g_deprecated_val_count].source_file = v->source_file;
            g_deprecated_val_count++;
        }
        Binding *made = env_lookup(g_global_env, v->name);
        if (made != NULL) reject_thread_bound(module, made->type, "the type of a module-level val (every thread would share it)", v->line, v->col);
    }
}

static void check_function(Module *module, FunctionDecl *fn) {
    Env *env = env_new(g_global_env);
    for (int i = 0; i < fn->params.count; i++) {
        Param *param = &fn->params.items[i];
        /* A variadic parameter's own declared type is the *element*
         * type (see ast.h's Param comment) -- inside the function's own
         * body it behaves as an ordinary array of that type, the same
         * value a caller's collected trailing arguments become at the
         * call site (see check_call/call_function). */
        Type bound_type = param->is_variadic ? type_array(param->type) : param->type;
        if (!env_define(env, param->name, bound_type, 0 /* params are immutable */, value_int(0))) {
            ziman_error("type", param->line, param->col, "duplicate parameter name '%s'", param->name);
        }
        /* A default value is only legal on an Option<T> parameter -- a
         * plain required parameter has no "absent" state a default
         * could ever stand in for, since it can never be omitted from a
         * call at all. Checked against g_global_env, not this
         * function's own `env` above: same restriction a global val's
         * own initializer already has (see docs/DESIGN.md's "Global
         * val" entry), since a default is sugar for one, evaluated once
         * before `main` runs -- long before any particular call, or any
         * of this function's *other* parameters, exist to reference. */
        if (param->default_expr != NULL) {
            if (!is_option_type(param->type)) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "a default value is only legal for an Option<T> parameter (parameter '%s' is %s)",
                            param->name, type_name(param->type));
            }
            if (option_elem_type(param->type).kind == TY_TYPEVAR) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "parameter '%s' can't have a default value -- its Option<T> isn't a concrete type yet here (T is this function's own generic type parameter, not known until a call instantiates it)",
                            param->name);
            }
            Type arg_type;
            if (!check_value_against_param_type(module, g_global_env, &param->default_expr, param->type, &arg_type,
                                                "as a default value")) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "default value for parameter '%s' expects %s or %s, found %s",
                            param->name, type_name(param->type), type_name(option_elem_type(param->type)), type_name(arg_type));
            }
        }
    }
    /* Lets type_supports_equality's TY_TYPEVAR case (above) know whether a
     * bare, unsubstituted `T` referenced inside *this* function's own body
     * is bounded by `Eq` -- the one place a generic declaration's bound
     * actually gets exercised, rather than just enforced at instantiation
     * (see ROADMAP.md's "Generics" Stage 2/5). Set/cleared symmetrically
     * around this one call: sound as a plain variable rather than a
     * parameter threaded through every check_* function because the type
     * checker processes exactly one function body at a time, single-
     * threaded, with no nested function declarations to re-enter this
     * during. */
    current_decl_type_params = &fn->type_params;
    /* A `native fn` has no body to check or prove returns-on-every-path for -- its behavior is a
     * contract with whatever C implementation gets registered for it, not something this pass can
     * see into. A `builtin fn` has none either, and for a nearer reason: its rule is one of the
     * arms *in this file*, which every call reaches before `find_function` is consulted. */
    if (fn_has_body(fn)) {
        int lint_first_var = g_lint_var_count;
        g_user_code = fn->source_file != NULL && fn->derived_from == NULL;
        g_lint_active = g_lint_on && g_user_code;
        g_lint_file = fn->source_file;
        int errors_before_body = ziman_error_count();
        for (int i = 0; i < fn->body->as.block.stmts.count; i++) {
            check_stmt_recovering(module, env, fn->body->as.block.stmts.items[i], fn->return_type, 0);
        }
        if (g_lint_active) lint_finish_function(lint_first_var);
        g_lint_active = 0;
        /* Skipped once any statement in the body already errored (a non-exhaustive `switch` is the common
         * case): that statement's own AST is left exactly as written, unrewritten, so a purely structural
         * "does every path return" walk over it is often truthfully "no" -- an accurate but redundant second
         * finding about the very statement whose real problem was already reported (see docs/DESIGN.md's
         * "No cascades" entry). */
        if (fn->return_type.kind != TY_VOID && fn->return_type.kind != TY_UNIT && !stmt_always_returns(fn->body) &&
            ziman_error_count() == errors_before_body) {
            ziman_error("type", fn->line, fn->col, "function '%s' does not return a value on every path", fn->name);
        }
    }
    current_decl_type_params = NULL;
    env_free(env);
}

void typecheck(Module *module) {
    overload_sets_report(module);
    g_builtin_shadows = NULL;
    g_builtin_shadow_count = 0;
    g_checking_program_body = 0;

    /* Struct names and field names within a struct must be unique --
     * checked before any field TYPE is validated, so every struct name a
     * field could legally reference is already known. */
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        for (int j = i + 1; j < module->structs.count; j++) {
            if (strcmp(sd->name, module->structs.items[j]->name) == 0) {
                ziman_error("type", module->structs.items[j]->line, module->structs.items[j]->col,
                            "struct '%s' is already defined", sd->name);
            }
        }
        for (int f = 0; f < sd->fields.count; f++) {
            for (int g = f + 1; g < sd->fields.count; g++) {
                if (strcmp(sd->fields.items[f].name, sd->fields.items[g].name) == 0) {
                    ziman_error("type", sd->fields.items[g].line, sd->fields.items[g].col,
                                "field '%s' is already defined in struct '%s'", sd->fields.items[g].name, sd->name);
                }
            }
        }
        /* A type parameter name colliding with a real struct/enum name
         * would make parse_struct_decl's reclassify_type_params silently
         * shadow every legitimate reference to that real type inside this
         * struct's own fields -- caught here instead of failing subtly
         * later. Duplicate parameter names have no per-name position (see
         * ast.h's TypeParamList), so both point at the struct's own line. */
        for (int tp = 0; tp < sd->type_params.count; tp++) {
            const char *param_name = sd->type_params.items[tp].name;
            for (int tq = tp + 1; tq < sd->type_params.count; tq++) {
                if (strcmp(param_name, sd->type_params.items[tq].name) == 0) {
                    ziman_error("type", sd->line, sd->col, "type parameter '%s' is already defined in struct '%s'", param_name, sd->name);
                }
            }
            StructDecl *clash_struct = find_struct(module, param_name);
            if (clash_struct != NULL && same_source(clash_struct->source_file, sd->source_file)) {
                ziman_error("type", sd->line, sd->col, "type parameter '%s' in struct '%s' collides with an existing struct name", param_name, sd->name);
            }
            EnumDecl *clash_enum = find_enum(module, param_name);
            if (clash_enum != NULL && same_source(clash_enum->source_file, sd->source_file)) {
                ziman_error("type", sd->line, sd->col, "type parameter '%s' in struct '%s' collides with an existing enum name", param_name, sd->name);
            }
        }
    }
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        current_decl_type_params = &sd->type_params;
        for (int f = 0; f < sd->fields.count; f++) {
            Param *field = &sd->fields.items[f];
            check_type_well_formed(module, field->type, field->line, field->col);
        }
        /* A type named `Mutable*` has to actually be thread-bound. "Every `Mutable*` type belongs
         * to the thread that made it" (docs/DESIGN.md, "The builders become mutable collections")
         * is worth stating as a rule only if a plain value cannot wear the name and falsify it --
         * which is not a style matter but a documented guarantee going untrue, and is the line
         * between this and R16's naming convention.
         *
         * Satisfied by the attribute *or* by holding something that has it, which is why it costs
         * legitimate uses nothing: `struct MutableRingBuffer { buf: MutableArray<int>, head: ... }`
         * is already thread-bound transitively and needs no annotation. The error only fires on a
         * `Mutable*` name with nothing mutable inside. Checked here rather than with the
         * name-uniqueness pass above, because the field types have to be well-formed first. */
        if (!sd->thread_bound && name_claims_mutable(sd->name)) {
            int holds = 0;
            for (int f = 0; f < sd->fields.count && !holds; f++) {
                if (type_holds_thread_bound(module, sd->fields.items[f].type, NULL)) holds = 1;
            }
            if (!holds) {
                ziman_error("type", sd->line, sd->col,
                            "a type named '%s' must be thread-bound: add #[thread_bound], or rename it. Every "
                            "'Mutable*' type belongs to the thread that made it, and a plain value wearing the name "
                            "would make that untrue",
                            sd->name);
            }
        }
        current_decl_type_params = NULL;
    }

    /* Enum names must be unique among themselves and distinct from struct
     * names (both live in the same "type name" namespace conceptually,
     * even though tracked as two separate lists) -- checked before any
     * variant payload type is validated. Variant names only need to be
     * unique *within* their own enum, never globally: access is always
     * enum-qualified (`Shape.Circle`, never a bare `Circle`), so two
     * different enums may freely reuse the same variant name. */
    for (int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        for (int j = i + 1; j < module->enums.count; j++) {
            if (strcmp(ed->name, module->enums.items[j]->name) == 0) {
                ziman_error("type", module->enums.items[j]->line, module->enums.items[j]->col,
                            "enum '%s' is already defined", ed->name);
            }
        }
        for (int s = 0; s < module->structs.count; s++) {
            if (strcmp(ed->name, module->structs.items[s]->name) == 0) {
                ziman_error("type", ed->line, ed->col, "'%s' is already defined as a struct", ed->name);
            }
        }
        for (int v = 0; v < ed->variants.count; v++) {
            for (int w = v + 1; w < ed->variants.count; w++) {
                if (strcmp(ed->variants.items[v].name, ed->variants.items[w].name) == 0) {
                    ziman_error("type", ed->variants.items[w].line, ed->variants.items[w].col,
                                "variant '%s' is already defined in enum '%s'", ed->variants.items[w].name, ed->name);
                }
            }
        }
        /* Same reasoning as the struct type-parameter checks above. */
        for (int tp = 0; tp < ed->type_params.count; tp++) {
            const char *param_name = ed->type_params.items[tp].name;
            for (int tq = tp + 1; tq < ed->type_params.count; tq++) {
                if (strcmp(param_name, ed->type_params.items[tq].name) == 0) {
                    ziman_error("type", ed->line, ed->col, "type parameter '%s' is already defined in enum '%s'", param_name, ed->name);
                }
            }
            StructDecl *clash_struct = find_struct(module, param_name);
            if (clash_struct != NULL && same_source(clash_struct->source_file, ed->source_file)) {
                ziman_error("type", ed->line, ed->col, "type parameter '%s' in enum '%s' collides with an existing struct name", param_name, ed->name);
            }
            EnumDecl *clash_enum = find_enum(module, param_name);
            if (clash_enum != NULL && same_source(clash_enum->source_file, ed->source_file)) {
                ziman_error("type", ed->line, ed->col, "type parameter '%s' in enum '%s' collides with an existing enum name", param_name, ed->name);
            }
        }
    }
    for (int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        current_decl_type_params = &ed->type_params;
        for (int v = 0; v < ed->variants.count; v++) {
            if (ed->variants.items[v].has_payload) {
                check_type_well_formed(module, ed->variants.items[v].payload_type,
                                        ed->variants.items[v].line, ed->variants.items[v].col);
            }
            if (ed->variants.items[v].is_from) {
                EnumVariant *fv = &ed->variants.items[v];
                if (!fv->has_payload) {
                    ziman_error("type", fv->line, fv->col, "#[from] variant '%s' has no payload: it converts an error of its payload type", fv->name);
                }
                if (ed->type_params.count > 0) {
                    ziman_error("type", fv->line, fv->col, "#[from] is not supported on a generic enum ('%s')", ed->name);
                }
                for (int w = 0; w < v; w++) {
                    EnumVariant *other = &ed->variants.items[w];
                    if (other->is_from && type_equals(other->payload_type, fv->payload_type)) {
                        ziman_error("type", fv->line, fv->col, "#[from] on '%s' and '%s' both convert %s: at most one variant of an enum may be #[from] for a payload type",
                                    other->name, fv->name, type_name(fv->payload_type));
                    }
                }
            }
        }
        /* The same rule the structs take, and an enum needs it too or the guarantee has a hole an
         * enum can be driven through. An `EnumDecl` carries no `#[thread_bound]` of its own, so the
         * only way one satisfies the rule is by holding something that is -- a variant payload --
         * which is exactly how the existing refusals already reach through an `Option<T>`. */
        if (name_claims_mutable(ed->name)) {
            int holds = 0;
            for (int v = 0; v < ed->variants.count && !holds; v++) {
                if (ed->variants.items[v].has_payload &&
                    type_holds_thread_bound(module, ed->variants.items[v].payload_type, NULL)) {
                    holds = 1;
                }
            }
            if (!holds) {
                ziman_error("type", ed->line, ed->col,
                            "a type named '%s' must be thread-bound: hold a thread-bound payload, or rename it. "
                            "Every 'Mutable*' type belongs to the thread that made it, and an enum cannot carry "
                            "#[thread_bound] of its own",
                            ed->name);
            }
        }
        current_decl_type_params = NULL;
    }

    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        ziman_set_error_context(fn->derived_from);
        /* A `builtin fn` is exempt from the redefinition guard -- it is the *declaration of* the
         * builtin, not a redefinition of it -- and is held to the opposite rule instead: the name
         * has to be one the interpreter actually dispatches. Without this a typo declares a
         * signature for nothing, which is worse than no declaration, since `--api builtins` would
         * print it and an editor would complete it. */
        if (fn->is_builtin) {
            int dispatched = 0;
            for (int b = 0; b < ziman_builtin_count(); b++) {
                if (strcmp(fn->name, ziman_builtin_name(b)) == 0) { dispatched = 1; break; }
            }
            if (!dispatched) {
                ziman_error("type", fn->line, fn->col,
                            "'%s' is declared 'builtin fn' but the interpreter dispatches no builtin of that name "
                            "-- 'ziman --api builtins' lists the ones it does", fn->name);
            }
            /* **Only a bundled module may declare one**, which in practice means lib/prelude/,
             * the file `--api builtins` reads. A declaration stamped with a source file came from
             * a program, and a program declaring these would be adding overloads to the language's
             * own vocabulary -- the one thing F148's design says `builtin fn` must not become,
             * since it would make this C++ overloading and half-undo "no methods, a function takes
             * its receiver as an argument". Stage 1 documented this restriction in SYNTAX.md and
             * did not implement it; declaring `builtin fn len<T>(a: [T]) -> int` in a program was
             * accepted. It is an error now. */
            if (fn->source_file != NULL) {
                ziman_error("type", fn->line, fn->col,
                            "'builtin fn' declares one of the language's own builtins and only a bundled module may "
                            "do it -- '%s' cannot be declared here", fn->name);
            }
        }
        /* The duplicate check, through the name index rather than as an inner loop over every
         * later declaration. That inner loop was the whole of the measured O(n^2): n^2/2 strcmps,
         * which is about two billion of them at 64,000 declarations and the 9.36s this took.
         * `find_function` returns the *first* declaration of a name, so a function that is not the
         * one the index gives back is a later duplicate -- reported at its own position, which is
         * where the inner loop reported it too (F130). */
        /* A `builtin fn` is exempt, because several of them under one name is how an **overload
         * set** is written: `len` has nine shapes and `contains` nine, and no single signature
         * states either. This is not general overloading -- the interpreter dispatches these on
         * `args[0]` and the type rule stays an arm in this file, so the declarations describe a
         * dispatch that already exists rather than introducing one. The check above confines them
         * to a bundled module, so a program still cannot declare a second anything. */
        /* A program's function may carry a builtin's name and shadows it -- see the jump to
         * `ordinary_call` in check_call for why resolution agrees. So "already defined" has to
         * ignore the case where the earlier declaration is the builtin itself: `fn count(conn,
         * table)` beside `builtin fn count<T>(a: [T], test)` is a shadow, not a duplicate. Two
         * *program* declarations of one name are still a duplicate, which is what this guards. */
        /* A program's function may carry a builtin's name and shadows it -- see the jump to
         * `ordinary_call` in check_call, and `g_checking_program_body` for why the library it
         * shadows cannot see it. Two consequences here.
         *
         * It is registered against the names the *interpreter dispatches*, not against the
         * `builtin fn` declarations: eleven dispatched builtins have no declaration at all (they
         * carry a recorded reason instead -- `format`, `error_message`, the parser's `$` names), so
         * keying on a prior declaration missed exactly those. `fn error_message(x: int)` was
         * accepted and then still routed to the builtin arm.
         *
         * And "already defined" has to ignore the case where the earlier declaration is the
         * builtin's own: `fn count(conn, table)` beside `builtin fn count<T>(a: [T], test)` is a
         * shadow. Two *program* declarations of one name are still a duplicate. */
        /* Since F179, "a name the language already owns" is no longer the same as "a name the
         * interpreter dispatches". Converting a paired builtin into two ordinary declarations
         * (stage 3 of docs/design/full-overloading.md) takes it off the dispatch list, and
         * keying only on that list silently withdrew F164's shadowing from all 22 converted
         * names at once: a program's own `fn sum(...)` or `fn first(...)` stopped shadowing and
         * started colliding. Both tests/lang/variadic_zero_args and option_bare_helpers caught
         * it. So a prelude declaration counts too, which is the property F164 was always about
         * -- the prelude is merged into every program, so any name in it is one a program might
         * reasonably want back. */
        if (!fn->is_builtin && fn->source_file != NULL && shadowing_fn_any(fn->name) == NULL) {
            int dispatched = 0;
            for (int b = 0; b < ziman_builtin_count(); b++) {
                if (strcmp(fn->name, ziman_builtin_name(b)) == 0) { dispatched = 1; break; }
            }
            if (!dispatched) {
                for (int q = 0; q < module->functions.count; q++) {
                    FunctionDecl *other = module->functions.items[q];
                    if (other != fn && other->source_file == NULL && strcmp(other->name, fn->name) == 0) {
                        dispatched = 1;
                        break;
                    }
                }
            }
            if (dispatched) {
                g_builtin_shadows = ziman_xrealloc(g_builtin_shadows,
                                                   (size_t)(g_builtin_shadow_count + 1) * sizeof(FunctionDecl *));
                g_builtin_shadows[g_builtin_shadow_count++] = fn;
            }
        }
        /* `#[compiler_resolved]` is refused where it would mean nothing, as `#[at_least_one]` and
         * `#[effectful]` are: it exists to turn off the distinguishability check below, so a
         * declaration no pair of which that check would have rejected is carrying an attribute
         * whose only effect is to disable a live guard. Checked here rather than in the parser
         * because the claim is about the *set*, which only the module knows.
         *
         * `tools/overload_sets.sh` reports the same thing per set; this is the version that cannot
         * be skipped. Every declaration is examined, not only the second and later ones, so a lone
         * function wearing it is refused too -- with no sibling there is nothing to be told apart
         * from. */
        if (fn->compiler_resolved) {
            if (fn->is_builtin) {
                ziman_error("type", fn->line, fn->col,
                            "#[compiler_resolved] on '%s' means nothing on a 'builtin fn' -- a builtin is "
                            "dispatched by name and the resolver never chooses among its signatures, so the "
                            "check this attribute turns off was already skipped",
                            fn->name);
            } else {
                int needed = 0;
                int fn_is_prelude = fn->source_file == NULL;
                for (int q = 0; q < module->functions.count && !needed; q++) {
                    FunctionDecl *other = module->functions.items[q];
                    if (other == fn || other->is_builtin || strcmp(other->name, fn->name) != 0) continue;
                    if ((other->source_file == NULL) != fn_is_prelude) continue;
                    if (ovl_indistinguishable_at(fn, other) >= 0) needed = 1;
                }
                if (!needed) {
                    ziman_error("type", fn->line, fn->col,
                                "#[compiler_resolved] on '%s' turns off a check it does not need -- every call "
                                "can already tell this declaration from the others of that name, so remove the "
                                "attribute; it is for a set no argument list can resolve, where a rule in the "
                                "compiler chooses instead",
                                fn->name);
                }
            }
        }
        if (!fn->is_builtin && find_function(module, fn->name) != fn) {
            const FunctionDecl *first = find_function(module, fn->name);
            /* A program declaration shadows a prelude one whatever kind it is -- see the
             * registration above for why `is_builtin` stopped being the right test. */
            int shadows_a_builtin = first != NULL && fn->source_file != NULL
                                 && (first->is_builtin || first->source_file == NULL);
            if (!shadows_a_builtin) {
                /* Stage 3 of docs/design/full-overloading.md: a repeated name is an overload set
                 * when every pair of its members can be told apart, and a redefinition when any
                 * pair cannot -- which is stage 2's `ovl_indistinguishable_at`, promoted from a
                 * report into the rule it was measuring.
                 *
                 * Restricted to the prelude on purpose. Library and program code keep today's
                 * flat "already defined" until stage 7, which is the stage that deliberately
                 * opens this up; letting it in here as a side effect would deliver the feature
                 * without the error messages, the documentation or the tests that stage owes. */
                /* Stage 7: the rule is the same wherever the declarations live. Two of them
                 * may share a name when every call can tell them apart, and a pair that no
                 * argument list separates is a redefinition. Until F184 this was confined to the
                 * prelude, so that stage 3 could convert the paired builtins without handing the
                 * feature to programs before its errors and its documentation existed.
                 *
                 * Declarations are compared only against others from the same place -- a
                 * program's `fn first(...)` beside the prelude's is a *shadow* (F164), not a
                 * conflict, and the check is reached from either side because `find_function`
                 * answers with the program's first. */
                FunctionDecl *clash = NULL;
                int fn_is_prelude = fn->source_file == NULL;
                for (int q = 0; q < module->functions.count && clash == NULL; q++) {
                    FunctionDecl *other = module->functions.items[q];
                    if (other == fn || strcmp(other->name, fn->name) != 0) continue;
                    if ((other->source_file == NULL) != fn_is_prelude) continue;
                    /* `#[compiler_resolved]` on either side: a rule in check_call chooses between
                     * them, so there is nothing for this check to protect -- see the attribute's
                     * comment in ast.h for the one name that carries it and why. */
                    if (fn->compiler_resolved || other->compiler_resolved) continue;
                    if (ovl_indistinguishable_at(fn, other) >= 0) clash = other;
                }
                if (clash != NULL) {
                    ziman_error("type", fn->line, fn->col,
                                "function '%s' is already defined (line %d) -- two declarations may share a "
                                "name only when every call can tell them apart",
                                fn->name, clash->line);
                }
            }
        }
        /* Same reasoning as the struct/enum type-parameter checks above. */
        for (int tp = 0; tp < fn->type_params.count; tp++) {
            const char *param_name = fn->type_params.items[tp].name;
            for (int tq = tp + 1; tq < fn->type_params.count; tq++) {
                if (strcmp(param_name, fn->type_params.items[tq].name) == 0) {
                    ziman_error("type", fn->line, fn->col, "type parameter '%s' is already defined in function '%s'", param_name, fn->name);
                }
            }
            StructDecl *clash_struct = find_struct(module, param_name);
            if (clash_struct != NULL && same_source(clash_struct->source_file, fn->source_file)) {
                ziman_error("type", fn->line, fn->col, "type parameter '%s' in function '%s' collides with an existing struct name", param_name, fn->name);
            }
            EnumDecl *clash_enum = find_enum(module, param_name);
            if (clash_enum != NULL && same_source(clash_enum->source_file, fn->source_file)) {
                ziman_error("type", fn->line, fn->col, "type parameter '%s' in function '%s' collides with an existing enum name", param_name, fn->name);
            }
        }
        current_decl_type_params = &fn->type_params;
        for (int pi = 0; pi < fn->params.count; pi++) {
            check_type_well_formed(module, fn->params.items[pi].type, fn->params.items[pi].line, fn->params.items[pi].col);
        }
        check_type_well_formed(module, fn->return_type, fn->line, fn->col);
        current_decl_type_params = NULL;
        ziman_set_error_context(NULL);
    }

    check_global_vals(module);
    /* Each function is checked on its own, so an error in one does not hide the
     * errors in the others (and within a function, in the other statements). */
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        ziman_set_error_context(fn->derived_from);
        /* `origin_file` only when there is no `source_file`: a prelude declaration has no source
         * file by design (see ast.h), and without this a type error inside the prelude reports a
         * line number without saying which of its parts it is in. */
        ziman_set_error_file(fn->source_file != NULL ? fn->source_file : fn->origin_file);
        g_checking_program_body = fn->source_file != NULL;
        jmp_buf jb;
        CheckState saved = check_state_save();
        ziman_recovery_push(&jb);
        if (setjmp(jb) == 0) {
            check_function(module, fn);
            ziman_recovery_pop();
        } else {
            check_state_restore(&saved);
        }
        ziman_set_error_context(NULL);
    }
    ziman_set_error_file(NULL);

    FunctionDecl *main_fn = find_function(module, "main");
    if (main_fn == NULL && !g_skip_main_check) {
        ziman_error("type", 0, 0, "no 'main' function found");
    }
    if (main_fn != NULL && (main_fn->params.count != 0 || main_fn->return_type.kind != TY_VOID)) {
        ziman_error("type", main_fn->line, main_fn->col, "'main' must take no parameters and return nothing");
    }

    /* `#[test]` (see ast.h's FunctionDecl.is_test/test_cases and docs/SYNTAX.md
     * "Testing"): a plain one takes no parameters; a `#[test(cases: NAME)]`
     * one takes exactly the element type of NAME's array, NAME being a val or
     * zero-parameter function declared in this same file (env_lookup on
     * g_global_env only ever matches this file's own, unqualified globals --
     * see its own comment above -- so there is no accidental cross-module
     * hit). Either way the return type is nothing or `Result<unit, E>`, so a
     * body can freely `try` a fallible call and still be run by `ziman test`. */
    for (volatile int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        if (!fn->is_test) continue;
        ziman_set_error_file(fn->source_file != NULL ? fn->source_file : fn->origin_file);
        jmp_buf jb;
        ziman_recovery_push(&jb);
        if (setjmp(jb) != 0) {
            ziman_set_error_file(NULL);
            continue;
        }
        int return_ok = fn->return_type.kind == TY_VOID ||
                        (fn->return_type.kind == TY_RESULT && fn->return_type.elem->kind == TY_UNIT);
        if (!return_ok) {
            ziman_error("type", fn->line, fn->col,
                        "#[test] function '%s' must return nothing or 'Result<unit, E>', not '%s'",
                        fn->name, type_name(fn->return_type));
        }
        if (fn->test_cases == NULL) {
            if (fn->params.count != 0) {
                ziman_error("type", fn->line, fn->col,
                            "#[test] function '%s' must take no parameters (use #[test(cases: ...)] for one)", fn->name);
            }
            ziman_recovery_pop();
            ziman_set_error_file(NULL);
            continue;
        }
        if (fn->params.count != 1) {
            ziman_error("type", fn->line, fn->col, "#[test(cases: ...)] function '%s' must take exactly one parameter", fn->name);
        } else {
            Type want = fn->params.items[0].type;
            Binding *cases_val = env_lookup(g_global_env, fn->test_cases);
            if (cases_val != NULL) {
                if (cases_val->type.kind != TY_ARRAY || !type_equals(*cases_val->type.elem, want)) {
                    ziman_error("type", fn->line, fn->col, "#[test(cases: %s)]: '%s' is '%s', not '[%s]'",
                                fn->test_cases, fn->test_cases, type_name(cases_val->type), type_name(want));
                }
            } else {
                FunctionDecl *cases_fn = find_function(module, fn->test_cases);
                if (cases_fn == NULL) {
                    ziman_error("type", fn->line, fn->col,
                                "#[test(cases: %s)]: no val or zero-parameter function named '%s' in this file",
                                fn->test_cases, fn->test_cases);
                } else if (cases_fn->params.count != 0 || cases_fn->return_type.kind != TY_ARRAY ||
                           !type_equals(*cases_fn->return_type.elem, want)) {
                    ziman_error("type", fn->line, fn->col, "#[test(cases: %s)]: '%s' must take no parameters and return '[%s]'",
                                fn->test_cases, fn->test_cases, type_name(want));
                }
            }
        }
        ziman_recovery_pop();
        ziman_set_error_file(NULL);
    }

    ziman_finish_errors();
}
