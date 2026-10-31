#ifndef ZIMAN_ENV_H
#define ZIMAN_ENV_H

#include "value.h"

struct Expr;

/* A generic scope chain of (name -> slot) bindings, reused by both the
 * type checker (slot holds a Type only) and the interpreter (slot holds a
 * Value). Templated by hand since C has no generics -- see docs/design/journal/
 * on keeping the implementation itself boring and explicit. */

typedef struct {
    /* Borrowed, never owned: every caller of env_define passes a name that belongs to the AST (a
     * declaration's name, a parameter's, a loop binder's, a defer's hidden variable), and the AST
     * outlives every environment built from it -- nothing frees it in either build. It used to be
     * strdup'd here, which measured as the single hottest allocation in the interpreter: 25.7% of all
     * allocation calls across a server workload and 99.7% of them in a call-heavy benchmark, one copy
     * per binding per call. See docs/design/journal/, "Binding names are borrowed". */
    const char *name;
    int is_mutable;
    int lint_id; /* type checker only: 1-based index into its `var` lint table, 0 = none */
    int poisoned; /* type checker only: the declaration of this name failed to check; uses of it are skipped silently */
    Type type;
    Value value; /* unused by the type checker */
} Binding;

/* A variable a lambda's body used from outside itself (see env_lookup). */
typedef struct {
    char *name;
    Type type;
    int is_mutable;
} Capture;

typedef struct Env {
    Binding *items;
    int count;
    int capacity;
    struct Env *parent;
    /* The interpreter's pending `defer`red calls for this scope (a scope
     * is exactly one block), in the order they were registered; run
     * last-in first-out when the block exits. Unused by the type checker. */
    struct Expr **defers;
    int defer_count;
    int defer_capacity;
    int defers_tracked; /* interpreter: this scope is on the thread's list of scopes with pending defers (see interpreter.c, thread errors) */
    /* Type checker only. A lambda's own root scope is a boundary: a name that
     * env_lookup finds *beyond* it, in an enclosing function's scopes, is
     * recorded in `caps` (once) -- that is how the checker learns what the body
     * captures, without a separate free-variable pass. `is_global` marks the
     * scope holding global vals, which are shared, not captured. */
    int is_lambda_boundary;
    int is_global;
    Capture *caps;
    int cap_count;
    int cap_capacity;
} Env;

Env *env_new(Env *parent);
void env_free(Env *env); /* frees just this scope's binding array, not values/parent */

/* Clears every binding out of `env` (freeing each one's own strdup'd name) without freeing
 * `env` itself or its `items` buffer -- for a loop that re-executes the same scope's worth of
 * declarations every iteration (a fresh `env_new`/`env_free` pair per iteration was most of a
 * tight loop's cost; see docs/design/journal/'s interpreter-speed entry). `items`' capacity survives
 * the reset, so after the first iteration there is no more allocation at all for a loop body
 * whose declaration count doesn't grow. Leaves `defer_count`, `defers_tracked` and `caps`
 * untouched -- `run_defers` (called once per iteration before this) already drains
 * `defer_count` to 0, and an interpreter-only scope never has anything in `caps` to begin with
 * (that list is the type checker's; see the `Env` doc comment above). */
void env_reset(Env *env);

/* What `env_define` did. The two non-failure answers are distinct because they differ in a way
 * that has segfaulted the compiler twice: `ENV_DEFINED` leaves a binding as the last element of
 * `env->items`, and `ENV_DISCARDED` leaves `env` completely untouched, so `items[count - 1]` is
 * someone else's binding (or, in a fresh scope, out of bounds) and `env_lookup` answers NULL.
 * A caller that only asks "did this work" may still test the result for truth, since both
 * answers are non-zero; a caller that then wants to reach the binding it just made must ask
 * for `ENV_DEFINED` specifically. */
typedef enum {
    ENV_REDECLARED = 0, /* already bound in this exact scope; `env` unchanged */
    ENV_DEFINED = 1,    /* bound, and it is `env->items[env->count - 1]` */
    ENV_DISCARDED = 2   /* `name` is `_`, the true discard: nothing bound, nothing to look up */
} EnvDefineResult;

/* Defines a new binding in `env`'s own scope. See `EnvDefineResult`: the answer is not a
 * boolean, because succeeding without binding anything is one of the things this can do. */
EnvDefineResult env_define(Env *env, const char *name, Type type, int is_mutable, Value value);

/* Searches this scope and its parents. Returns NULL if not found. */
Binding *env_lookup(Env *env, const char *name);

/* Same search as env_lookup, but also reports how many `->parent` hops (`*out_depth`) and which
 * index into that scope's `items` (`*out_slot`) the match was found at -- either out-param may
 * be NULL when the caller doesn't need it. The type checker's own resolution pass calls this
 * once per identifier so the interpreter can look the same name up again, every time the
 * expression actually runs, by direct indexing (env_at_slot) instead of by `strcmp`.
 *
 * `*out_depth` is set to -1 -- "not safe to reuse, fall back to env_lookup by name" -- in three
 * cases, all about the match landing somewhere this search's own hop count or slot index can't
 * be trusted to still mean the same thing at runtime:
 *
 *   1. Finding the match crossed at least one lambda boundary (is_lambda_boundary) -- whether
 *      what was found is a captured local or a global reached past one. A lambda's *runtime*
 *      call frame is rooted directly at the global env (a capture is a value copied into one of
 *      its own leading parameters at call time, never found by walking up to the scope the
 *      lambda was defined in -- see call_function/the closure-calling code in interpreter.c),
 *      while this search walks the type checker's *compile-time* nested scope chain, where the
 *      lambda's root is nested inside whatever scope contains the lambda literal. Those two
 *      chains agree on hop counts only for a name that resolves without ever leaving the
 *      current function or lambda activation's own scopes.
 *
 *   2. The match itself is in the global scope (`is_global`), lambda boundary or not. The type
 *      checker's own g_global_env and the interpreter's g_global_env are built by two
 *      independently-ordered passes that don't actually agree: interpreter.c's
 *      init_module_globals defines one slot per omitted-default parameter, anywhere in the
 *      whole program, interleaved with the real module-level vals in dependency order -- but the
 *      type checker never calls env_define for those particular slots at all (a reference to
 *      one is a synthetic Expr with its type pre-filled, bypassing this search entirely; see
 *      make_default_ref_expr in typecheck.c). So a global's index at check time and at run time
 *      are not guaranteed to be the same number, and trusting the check-time one caused a real,
 *      silent read of a wrong slot the first time this was tried.
 *
 *   3. The match is a lambda's own declared parameter (found directly in its root scope, not
 *      something the walk had to leave root to find -- so n_crossed is 0). check_lambda builds
 *      root's env from the parameters exactly as declared, in order, *before* checking the
 *      body -- but by the time the body-check finishes discovering what the lambda captures,
 *      those captures get prepended to the *front* of the same FunctionDecl's param list for
 *      call_function to bind them from at call time. So the declared parameter root's own env
 *      put at index 0 is the parameter call_function will actually bind at index `ncaptured`
 *      once any capture exists at all -- a shift this search has no way to predict while it's
 *      still walking the very body that determines whether there will be one.
 *
 * Global access, captures, and a capturing lambda's own declared parameters are looked up again
 * by name every time they're used, same as before this function existed -- nothing about them
 * regresses, they just don't gain the new fast path (which was never their bottleneck: the
 * benchmarks that motivated this only ever reference locals and parameters of ordinary
 * functions, not globals, captures, or a capturing lambda's own parameters). */
Binding *env_lookup_resolved(Env *env, const char *name, int *out_depth, int *out_slot);

/* `depth` `->parent` hops up from `env`, then `items[slot]` directly -- no name comparison at
 * all. The interpreter's fast path once the type checker has already resolved a name via
 * env_lookup_resolved for this exact occurrence; `depth`/`slot` from one occurrence are
 * meaningless for any other, since two different scopes' `items` arrays have unrelated
 * indexing. `static inline` so it can be a single array-index instruction at the call site
 * rather than a real function call, in the hottest path this codebase has. */
static inline Binding *env_at_slot(Env *env, int depth, int slot) {
    for (int i = 0; i < depth; i++) env = env->parent;
    return &env->items[slot];
}

#endif
