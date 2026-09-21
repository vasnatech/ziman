#ifndef ZIMAN_ENV_H
#define ZIMAN_ENV_H

#include "value.h"

/* A generic scope chain of (name -> slot) bindings, reused by both the
 * type checker (slot holds a Type only) and the interpreter (slot holds a
 * Value). Templated by hand since C has no generics -- see docs/DESIGN.md
 * on keeping the implementation itself boring and explicit. */

typedef struct {
    char *name;
    int is_mutable;
    Type type;
    Value value; /* unused by the type checker */
} Binding;

typedef struct Env {
    Binding *items;
    int count;
    int capacity;
    struct Env *parent;
} Env;

Env *env_new(Env *parent);
void env_free(Env *env); /* frees just this scope's binding array, not values/parent */

/* Defines a new binding in `env`'s own scope. Returns 0 and leaves env
 * unchanged if `name` is already bound in this exact scope (redeclaration). */
int env_define(Env *env, const char *name, Type type, int is_mutable, Value value);

/* Searches this scope and its parents. Returns NULL if not found. */
Binding *env_lookup(Env *env, const char *name);

#endif
