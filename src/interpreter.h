#ifndef ZIMAN_INTERPRETER_H
#define ZIMAN_INTERPRETER_H

#include "ast.h"
#include "error.h"
#include "value.h"

/* Runs `main()`. Assumes `module` already passed typecheck(). Exits the
 * process with a runtime error (line:col + message) on things the type
 * checker can't catch statically (division by zero, etc). */
void interpret(Module *module);

/* Initialises `module`'s parameter defaults and global vals into a fresh environment, the way
 * `interpret` does before calling `main` -- for a program that is loaded to be *called into*
 * rather than run. See the comment at the definition for why one `g_global_env` is enough.
 *
 * Used by the generator bridge: a generator program is initialised before the user program's
 * load begins, and `interpret` replaces its environment when the user program runs. */
void ziman_init_module_globals(Module *module);

/* Keeps `env` reachable for the collector after `g_global_env` has moved to another program's.
 * Called for a program whose globals are cached across runs -- the generator program. */
void ziman_keep_globals(struct Env *env);

/* Calls `name` in `module`, which must already have been through `ziman_init_module_globals`.
 * Answers 1 and writes `*out`, or 0 and `*out_error` when the function is missing or the call
 * fails at run time -- caught rather than fatal, because a generator's bug must be reported
 * against the declaration it was generating for. */
int ziman_call_in_module(Module *module, const char *name, Value *args, int argc, Value *out,
                         const char **out_error);

/* Runs every `#[test]` function in `module` (see ast.h's
 * FunctionDecl.is_test/test_cases); see interpreter.c for the full
 * contract. Assumes `module` already passed typecheck(). Adds to
 * out_run and out_failed rather than resetting them, so `ziman test`
 * can tally one summary across several files. */
void interpret_tests(Module *module, const char *file_label, int *out_run, int *out_failed);

/* Forces a real Phase 3 collection on the calling thread's own GcHeap right now, ignoring the
 * usual growth threshold -- for `os.collect_garbage()` (native_os.c), so that an explicit request
 * to collect garbage also cleans up this thread's own gc_heap-allocated content, not just Boehm's.
 * Skipped (a no-op) if the calling thread currently has a temporary buffer in progress (see
 * interpreter.c's own gc_unsafe_enter/leave) -- an explicit request is not an exception to the
 * same safety rule the automatic trigger already follows; `os.collect_garbage()` itself is never
 * called from within such a region (it is always its own complete call, by the time it runs), but
 * a future caller of this function directly might not be, so the check stays here rather than
 * being assumed away. */
void ziman_force_collect(void);

/* `--gc-stress`: collect at every safe point. See interpreter.c. */
void ziman_set_gc_stress(int on);

/* This thread's own per-thread root-source addresses (t_defer_envs, t_env_pool, t_active_iters,
 * t_pending_handler/t_has_pending_handler) -- addresses, not values, for the same reason
 * ZimanFramesRootAddrs (error.h) is: each array is reallocated as it grows, so
 * ziman_interp_root_ranges must re-read these pointers at collection time. Captured once at
 * registration time (ziman_gc_register_thread_roots, gc_boehm_roots.c). */
typedef struct {
    void **defer_envs_ptr_addr;
    int *defer_env_count_addr;
    void **env_pool_ptr_addr;
    int *env_pool_count_addr;
    void **active_iters_ptr_addr;
    int *active_iter_count_addr;
    void *pending_handler_addr;
    int *has_pending_handler_addr;
} ZimanInterpRootAddrs;
ZimanInterpRootAddrs ziman_interp_root_addrs(void);

/* Writes this thread's own current defer_envs/env_pool/active_iters ranges, plus
 * pending_handler's own single-slot range if one is live, into out_ranges (up to max_ranges) and
 * returns how many it wrote. Reads addrs' pointers now, not whatever they pointed to when
 * captured -- see ZimanInterpRootAddrs's own comment. */
int ziman_interp_root_ranges(ZimanInterpRootAddrs addrs, ZimanMemRange *out_ranges, int max_ranges);

/* The address of the one, shared (not per-thread) g_global_env slot -- `void **` rather than
 * `Env **` purely so this header does not need env.h's full definition just for this. */
void **ziman_global_env_addr(void);

#endif
