#include "gc_interp_roots.h"

#include "error.h"
#include "gc_env_roots.h"

void gc_mark_interpreter_roots(const Arena *a, const Env *global_env, Env *const *defer_envs, int defer_env_count,
                                Env *const *env_pool, int env_pool_count) {
    gc_mark_env_chain(a, global_env);

    int frames = ziman_frame_count();
    for (int i = 0; i < frames; i++) {
        Env *env = (Env *)ziman_frame_env_at(i);
        if (env != NULL) gc_mark_env_chain(a, env);
    }

    for (int i = 0; i < defer_env_count; i++) gc_mark_env_chain(a, defer_envs[i]);
    for (int i = 0; i < env_pool_count; i++) gc_mark_env_chain(a, env_pool[i]);
}
