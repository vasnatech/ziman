#define _POSIX_C_SOURCE 200809L
#include "env.h"

#include <stdlib.h>
#include <string.h>

Env *env_new(Env *parent) {
    Env *env = calloc(1, sizeof(Env));
    env->parent = parent;
    return env;
}

void env_free(Env *env) {
    /* the names env_define copied; `caps` names are not freed -- the type checker keeps them */
    for (int i = 0; i < env->count; i++) free(env->items[i].name);
    free(env->items);
    free(env->defers);
    free(env->caps);
    free(env);
}

int env_define(Env *env, const char *name, Type type, int is_mutable, Value value) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, name) == 0) return 0;
    }
    if (env->count == env->capacity) {
        env->capacity = env->capacity == 0 ? 8 : env->capacity * 2;
        env->items = realloc(env->items, (size_t)env->capacity * sizeof(Binding));
    }
    env->items[env->count].name = strdup(name);
    env->items[env->count].is_mutable = is_mutable;
    env->items[env->count].lint_id = 0;
    env->items[env->count].poisoned = 0;
    env->items[env->count].type = type;
    env->items[env->count].value = value;
    env->count++;
    return 1;
}

static void record_capture(Env *boundary, const Binding *b) {
    for (int i = 0; i < boundary->cap_count; i++) {
        if (strcmp(boundary->caps[i].name, b->name) == 0) return;
    }
    if (boundary->cap_count == boundary->cap_capacity) {
        boundary->cap_capacity = boundary->cap_capacity == 0 ? 4 : boundary->cap_capacity * 2;
        boundary->caps = realloc(boundary->caps, (size_t)boundary->cap_capacity * sizeof(Capture));
    }
    boundary->caps[boundary->cap_count].name = strdup(b->name);
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
