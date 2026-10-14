#define _POSIX_C_SOURCE 200809L
#include "env.h"
#include "xalloc.h"

#include <stdlib.h>
#include <string.h>

Env *env_new(Env *parent) {
    Env *env = ziman_xcalloc(1, sizeof(Env));
    env->parent = parent;
    return env;
}

void env_free(Env *env) {
    /* No name is freed: a binding borrows its name from the AST (see env.h's Binding). `caps` names
     * were never freed either -- the type checker keeps them. */
    free(env->items);
    free(env->defers);
    free(env->caps);
    free(env);
}

void env_reset(Env *env) {
    env->count = 0;
}

EnvDefineResult env_define(Env *env, const char *name, Type type, int is_mutable, Value value) {
    /* `_` is a true discard, not a binding: it never actually goes into `env` (so it never collides with an
     * earlier `_`, and never needs freeing), and reading it back later is the ordinary "use of undefined
     * variable '_'" every other never-declared name gets -- exactly right, since a discarded value has nothing
     * left to read. This is the one place in the whole type checker that treats the name `_` specially: no
     * parser or check_* code needs its own case for it -- a `val`/`var`, a parameter, a loop binder, and a
     * switch or `if` payload binder all go through here. */
    if (strcmp(name, "_") == 0) return ENV_DISCARDED;
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, name) == 0) return ENV_REDECLARED;
    }
    if (env->count == env->capacity) {
        env->capacity = env->capacity == 0 ? 8 : env->capacity * 2;
        env->items = ziman_xrealloc(env->items, (size_t)env->capacity * sizeof(Binding));
    }
    env->items[env->count].name = name;
    env->items[env->count].is_mutable = is_mutable;
    env->items[env->count].lint_id = 0;
    env->items[env->count].poisoned = 0;
    env->items[env->count].type = type;
    env->items[env->count].value = value;
    env->count++;
    return ENV_DEFINED;
}

static void record_capture(Env *boundary, const Binding *b) {
    for (int i = 0; i < boundary->cap_count; i++) {
        if (strcmp(boundary->caps[i].name, b->name) == 0) return;
    }
    if (boundary->cap_count == boundary->cap_capacity) {
        boundary->cap_capacity = boundary->cap_capacity == 0 ? 4 : boundary->cap_capacity * 2;
        boundary->caps = ziman_xrealloc(boundary->caps, (size_t)boundary->cap_capacity * sizeof(Capture));
    }
    boundary->caps[boundary->cap_count].name = ziman_xstrdup(b->name);
    boundary->caps[boundary->cap_count].type = b->type;
    boundary->caps[boundary->cap_count].is_mutable = b->is_mutable;
    boundary->cap_count++;
}

Binding *env_lookup(Env *env, const char *name) {
    Env *crossed[16];
    int n_crossed = 0;
    for (Env *e = env; e != NULL; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->items[i].name, name) == 0) {
                if (!e->is_global) {
                    for (int k = 0; k < n_crossed; k++) record_capture(crossed[k], &e->items[i]);
                }
                return &e->items[i];
            }
        }
        if (e->is_lambda_boundary && n_crossed < 16) crossed[n_crossed++] = e;
    }
    return NULL;
}

Binding *env_lookup_resolved(Env *env, const char *name, int *out_depth, int *out_slot) {
    Env *crossed[16];
    int n_crossed = 0;
    int depth = 0;
    for (Env *e = env; e != NULL; e = e->parent, depth++) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->items[i].name, name) == 0) {
                if (!e->is_global) {
                    for (int k = 0; k < n_crossed; k++) record_capture(crossed[k], &e->items[i]);
                }
                /* Also excludes a match in the global scope (e->is_global) or in a lambda's own
                 * root scope (e->is_lambda_boundary), even with no boundary crossed -- two more
                 * cases where this search's hop count and slot index are not guaranteed to mean
                 * the same thing at runtime:
                 *
                 *   - Global: the type checker's own g_global_env and the interpreter's are
                 *     built by two independently-ordered passes that don't actually agree --
                 *     interpreter.c's init_param_defaults defines one slot per omitted-default
                 *     parameter, anywhere in the whole program, *before* init_global_vals
                 *     defines the real module-level vals, but the type checker never calls
                 *     env_define for those particular slots at all (a reference to one is a
                 *     synthetic Expr with its type pre-filled, bypassing this search entirely;
                 *     see make_default_ref_expr in typecheck.c). A global's index at check time
                 *     and at run time are not guaranteed to be the same number.
                 *
                 *   - A lambda's own declared parameter (found directly in its root, not
                 *     something the walk had to leave root to find): check_lambda builds root's
                 *     env from the parameters exactly as declared, in order, *before* checking
                 *     the body -- but by the time the body-check finishes discovering what it
                 *     captures, those captures get prepended to the *front* of the same
                 *     FunctionDecl's param list for call_function to bind them from (see
                 *     ast.h's `lambda_ncaptured` comment). So the declared parameter that root's
                 *     own env put at index 0 is the parameter call_function will actually bind
                 *     at index `ncaptured` once any capture exists -- a shift this search has no
                 *     way to predict while still walking the body that determines it. */
                if (out_depth) *out_depth = (n_crossed == 0 && !e->is_global && !e->is_lambda_boundary) ? depth : -1;
                if (out_slot) *out_slot = i;
                return &e->items[i];
            }
        }
        if (e->is_lambda_boundary && n_crossed < 16) crossed[n_crossed++] = e;
    }
    return NULL;
}
