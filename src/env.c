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
    free(env->items);
    free(env->defers);
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
    env->items[env->count].type = type;
    env->items[env->count].value = value;
    env->count++;
    return 1;
}

Binding *env_lookup(Env *env, const char *name) {
    for (Env *e = env; e != NULL; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->items[i].name, name) == 0) return &e->items[i];
        }
    }
    return NULL;
}
