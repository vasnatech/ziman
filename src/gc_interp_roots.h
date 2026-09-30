#ifndef ZIMAN_GC_INTERP_ROOTS_H
#define ZIMAN_GC_INTERP_ROOTS_H

#include "arena.h"
#include "env.h"

/* Aggregates every root source a real collection cycle needs beyond the conservative stack scan
 * (docs/MEMORY.md, "the second root source") -- the piece `gc_heap_collect`'s `extra_roots` seam was
 * built for, still not supplied by anything until this.
 *
 * `global_env`: the interpreter's own module-level scope (`g_global_env`), walked the same way any
 * other `Env` is (`gc_mark_env_chain`).
 *
 * Every currently active call's own frame comes from `error.c`'s existing call-frame tracking
 * (`ziman_frame_count`/`ziman_frame_env_at`), extended to carry each one's `Env*` -- this file reads
 * that back directly rather than needing anything passed in for it, since `error.c` is already a
 * small, general, foundational module this one can depend on without coupling to `interpreter.c`
 * itself.
 *
 * `defer_envs`/`defer_env_count` and `env_pool`/`env_pool_count` are `interpreter.c`'s own
 * `t_defer_envs`/`t_env_pool` (see interpreter.c's own comments on why each exists) -- passed in as
 * explicit parameters rather than read from `interpreter.c`'s private `static`/`static __thread`
 * state directly. This is deliberate, not an arbitrary style choice: `interpreter.c` is far too large
 * to link into a standalone unit test the way every other module in this effort has been (it pulls in
 * every native module, the parser, the type checker, ...), so keeping the aggregation logic itself
 * parameterized is what makes it testable at all before `interpreter.c` ever calls it -- a synthetic
 * `Env**`/count pair exercises the exact same code path a real one would. `interpreter.c`'s own
 * eventual wiring is reduced to a thin `extra_roots` wrapper passing its own state through, once a
 * real per-thread `GcHeap` and a collection trigger both exist.
 *
 * `env_pool`'s own entries are expected to have no live bindings (`env_reset` already clears them
 * before a call returns an `Env` to the pool) -- walking them anyway costs nothing (each is an
 * immediate no-op) and keeps this function's own contract simple: mark everything a real collection
 * cycle might depend on, without this module needing to know or assume anything about
 * `interpreter.c`'s own pooling invariant staying true forever. */
void gc_mark_interpreter_roots(const Arena *a, const Env *global_env, Env *const *defer_envs, int defer_env_count,
                                Env *const *env_pool, int env_pool_count);

#endif
