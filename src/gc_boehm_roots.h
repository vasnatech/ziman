#ifndef ZIMAN_GC_BOEHM_ROOTS_H
#define ZIMAN_GC_BOEHM_ROOTS_H

/* Closes a gap this session's own investigation found and confirmed directly (docs/design/journal/,
 * "confirm the root cause directly -- main()'s own Env drops off the stack"): Boehm's own
 * conservative collector (`GC_gcollect`, automatic or explicit) has no way to find an `Env` once the
 * one C-level copy of its pointer the optimizer kept on the stack is gone -- `Env` lives on the
 * ordinary allocator's heap, not a thread's `GcHeap` arena, and its *only* other root source
 * (`gc_mark_env_chain`/`error.c`'s own frame tracking, `gc_interp_roots.c`) is Ziman's own, separate
 * Phase 3 mark pass, walked by `gc_heap_collect`, never consulted by Boehm's own `GC_stopped_mark`.
 * Two independent collectors ended up with two different, disjoint notions of what is reachable.
 *
 * The fix: Boehm exposes exactly the seam this needs, `GC_set_push_other_roots` (vendored
 * `gc/gc_mark.h`, unused anywhere in this codebase before now) -- a callback its own mark phase
 * calls on every cycle to push additional roots beyond its own conservative stack/register scan.
 * The hook also carries one root source that is not an `Env` thing at all: every live
 * `arena_create_scanned` arena (`ziman_arena_push_scanned_roots`, arena.c). Those blocks have to be
 * conservatively scanned for the reasons arena.h's own comment on `arena_create_scanned` gives, and
 * they were registered one-by-one with `GC_add_roots` originally -- correct, but capped at
 * `MAX_ROOT_SETS` (2048) ranges process-wide, which `value_copy_send`'s own arena-per-queued-
 * `Channel`-message turned into a hard concurrency ceiling on correct programs. This hook has no
 * such capacity, which is the whole reason that registration moved here.
 *
 * This module installs one that walks the *same* root set `gc_mark_interpreter_roots` already does
 * for Ziman's own collector (`error.c`'s per-thread call frames, `interpreter.c`'s per-thread
 * `t_defer_envs`/`t_env_pool`/`t_active_iters`/`t_pending_handler`, and the shared `g_global_env`),
 * unifying what the two collectors consider reachable instead of leaving Boehm to guess.
 *
 * The one real complication: `GC_push_other_roots_proc` takes no arguments and runs on whichever
 * thread is doing the collecting, with every other thread already stopped -- it must visit *every*
 * thread's own frame/env state, not just the calling thread's, which the existing `t_frames`/
 * `t_defer_envs`/`t_env_pool` (all `__thread`-scoped) have no way to do on their own. `native_thread.c`
 * already solved the identical problem for `thread.local<T>`'s own slots, the same way: an ordinary
 * (non-`__thread`) global list, Boehm-visible because it is *not* thread-local storage, that each
 * thread adds itself to on entry and removes itself from on exit, protected by a mutex only the
 * (rare, never-during-a-collection-in-progress) add/remove needs. This module mirrors that shape for
 * the env-root sources specifically, rather than extending `native_thread.c`'s own registry, which
 * serves an unrelated feature. */

/* Installs the `GC_push_other_roots_proc` with Boehm. Call exactly once, during startup, before any
 * thread (including the one that calls this) registers itself below. A no-op without the collector
 * (`ZIMAN_GC` undefined, a `nogc` build) -- Boehm has nothing to hook into there. */
void ziman_gc_install_boehm_root_hook(void);

/* Registers the calling thread's own root-source addresses into the global list the hook above
 * walks. Call once, as early as possible, on every thread Ziman creates: `main`'s own, and every
 * `spawn`. Idempotent is not assumed -- call exactly once per thread, mirroring `ziman_thread_begin`'s
 * own contract (`native_thread.c`). A no-op without the collector. */
void ziman_gc_register_thread_roots(void);

/* Removes the calling thread's own record. Call once, when a thread is done needing any of this --
 * mirrors `ziman_thread_end`'s own shape and placement (`native_thread.c`) for the identical reason:
 * a thread about to exit must not leave a stale record pointing at state that is about to be freed
 * for a *collector* (Boehm's own) to stumble on later. A no-op without the collector. */
void ziman_gc_unregister_thread_roots(void);

#endif
