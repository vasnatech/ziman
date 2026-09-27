#ifndef ZIMAN_ENV_H
#define ZIMAN_ENV_H

#include "value.h"

struct Expr;

/* A generic scope chain of (name -> slot) bindings, reused by both the
 * type checker (slot holds a Type only) and the interpreter (slot holds a
 * Value). Templated by hand since C has no generics -- see docs/DESIGN.md
 * on keeping the implementation itself boring and explicit. */

typedef struct {
    char *name;
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

/* Defines a new binding in `env`'s own scope. Returns 0 and leaves env
 * unchanged if `name` is already bound in this exact scope (redeclaration). */
int env_define(Env *env, const char *name, Type type, int is_mutable, Value value);

/* Searches this scope and its parents. Returns NULL if not found. */
Binding *env_lookup(Env *env, const char *name);

#endif
