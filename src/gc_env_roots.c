#include "gc_env_roots.h"

#include "gc_mark.h"

void gc_mark_env_chain(const Arena *a, const Env *env) {
    /* Iteratively, not recursively -- a long scope chain (deeply nested blocks, a long chain of
     * function calls each contributing a scope) has no reason to also cost C stack depth here on
     * top of whatever depth it already cost to build. */
    for (; env != NULL; env = env->parent) {
        for (int i = 0; i < env->count; i++) {
            gc_mark_value(a, env->items[i].value);
        }
    }
}
