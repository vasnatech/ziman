#define _POSIX_C_SOURCE 200809L
#include "interpreter.h"
#include "iter.h"
#include "prelude.h"
#include "native_thread.h"
#include "stack.h"

#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "decimal.h"
#include "derive.h"
#include "env.h"
#include "coverage.h"
#include "error.h"
#include "fmtstr.h"
#include "gc_boehm_roots.h"
#include "gc_guard_roots.h"
#include "gc_heap.h"
#include "gc_interp_roots.h"
#include "gc_mark.h"
#include "gc_thread_heap.h"
#include "mapobj.h"
#include "native.h"
#include "native_locale.h"
#include "value_copy.h"
#include "value.h"
#include "xalloc.h"

/* CTRL_YIELD carries the value of an `if`/`switch` arm used as a value up to the
 * enclosing STMT_VALUE_BLOCK, the way CTRL_RETURN carries a return value. */
typedef enum { CTRL_NONE, CTRL_RETURN, CTRL_BREAK, CTRL_CONTINUE, CTRL_YIELD } CtrlKind;

typedef struct {
    CtrlKind kind;
    Value value;
} ExecResult;

/* `float as int`: truncates toward zero and saturates to INT64_MIN / INT64_MAX (the same "clamp, don't wrap" rule as `bigint as int`); NaN is 0. */
static int64_t float_to_int_saturating(double f) {
    if (isnan(f)) return 0;
    if (f >= 9223372036854775808.0) return INT64_MAX;
    if (f <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)f;
}

static FunctionDecl *find_function(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        if (strcmp(module->functions.items[i]->name, name) == 0) return module->functions.items[i];
    }
    return NULL;
}

static StructDecl *find_struct(Module *module, const char *name) {
    for (int i = 0; i < module->structs.count; i++) {
        if (strcmp(module->structs.items[i]->name, name) == 0) return module->structs.items[i];
    }
    return NULL;
}

static EnumDecl *find_enum(Module *module, const char *name) {
    for (int i = 0; i < module->enums.count; i++) {
        if (strcmp(module->enums.items[i]->name, name) == 0) return module->enums.items[i];
    }
    return NULL;
}

/* How many call arguments go in the evaluating frame instead of the heap. Four, because the average
 * call passes 1.6 of them -- 314MB over 2.44M argument arrays in a measured run, against an 80-byte
 * Value -- so four covers nearly every call while a larger buffer would only cost recursion depth.
 *
 * Measured at c=16 on tests/bench/server_concurrency.sh, interleaved rounds, against the maximum
 * recursion depth found by bisection:
 *
 *     heap array (before)   919 req/s   82,148 deep
 *     ARGS_INLINE 2        1,136 req/s  79,062 deep
 *     ARGS_INLINE 4        1,205 req/s  74,433 deep
 *
 * Four it is: the depth it costs is 9%, and `--stack-mb` takes it back (600 gives 84,355, past where
 * this started), while throughput under concurrency is not something a flag can recover. */
#define ARGS_INLINE 4

static Value eval_expr(Module *module, Env *env, Expr *e);

/* Phase 3's collection-safety guard (docs/MEMORY.md, "one heap per thread") -- found necessary,
 * not anticipated, while wiring the first real allocation site: a temporary `Value[]` buffer built
 * by *repeated* calls that can each recurse back into Ziman code (eval_expr's own EXPR_CALL/
 * EXPR_ARRAY_LIT/EXPR_STRUCT_LIT/EXPR_WITH, and every native function that calls back into a
 * closure via call_closure_value/native_call_closure) is invisible to both root sources for as
 * long as it's only partially built: earlier elements sit in a plain `malloc`'d block (not the C
 * stack, not any Env) while a later element's own evaluation is still running -- and if *that*
 * evaluation recurses into a function call, its own body's statements can trigger a real
 * collection while the earlier elements are completely unprotected. `maybe_collect` (below) skips
 * entirely whenever this depth is nonzero, so a collection can never run partway through building
 * such a buffer. Every site that builds one wraps it with `gc_unsafe_enter`/`gc_unsafe_leave`; see
 * docs/DESIGN.md's own entry on the bug this closes (found in `struct_obj_new`'s own first real
 * migration attempt) for the full incident and the audit that produced this list. */
static __thread int t_gc_unsafe_depth = 0;
static void gc_unsafe_enter(void) { t_gc_unsafe_depth++; }
static void gc_unsafe_leave(void) { t_gc_unsafe_depth--; }

/* The same pair for native functions, which live in prelude.c and cannot see the statics. Exported
 * when `filter` over a Map and a Set stopped being a builtin (2026-10-22): that branch guarded its
 * whole loop here, and a native had no way to. See native.h for when a native needs it. */
void native_gc_unsafe_enter(void) { gc_unsafe_enter(); }
void native_gc_unsafe_leave(void) { gc_unsafe_leave(); }

/* `int` is 64-bit and never wraps: overflow is a runtime error, the same
 * spirit as `bigint as int` / `int as byte` clamping rather than wrapping
 * silently. Use `bigint` when a value can outgrow 64 bits. */
static void int_overflow(int line, int col, const char *op) {
    ziman_error("runtime", line, col, "integer overflow in '%s' (int is 64-bit; use bigint for larger values)", op);
}
static ExecResult exec_stmt(Module *module, Env *env, Stmt *s);

/* The root of every function call's own scope (see call_function) --
 * holds every module-level `val`'s already-evaluated Value (see
 * init_global_vals), so a bare reference to one falls out of an
 * ordinary env_lookup exactly the way a reference to an enclosing
 * block's local already does, with no special-casing anywhere else in
 * this file. Built once, before `main` runs, from a single thread;
 * never mutated afterward, so concurrent reads from a `spawn`ed thread
 * are safe with no locking. */
static Env *g_global_env = NULL;

/* If `fn`'s last parameter is variadic, collapses `args`/`argc` (the raw
 * values a call site evaluated, one per written-out argument) down to
 * exactly `fn->params.count` values -- one per fixed parameter, plus one
 * final array Value for the whole variadic tail -- so every other piece
 * of call-handling code (native dispatch, ordinary env-binding below)
 * never needs to know a variadic call happened at all; it just sees the
 * same "one Value per declared parameter" shape it always has. Mirrors
 * check_call's own already-verified logic -- typecheck already proved
 * this call is well-formed, so no error-checking is needed here, only
 * the collapse itself. Returns `args` unchanged (no allocation) for a
 * non-variadic function, so the common case costs nothing extra.
 * `tail_is_array_spread` is check_call's own recorded verdict for the
 * one genuinely ambiguous shape (exactly one trailing argument) --
 * NOT re-derived here from `args[fixed_count].type->kind == TY_ARRAY`,
 * which used to be this function's own check and is wrong whenever the
 * variadic element type is itself an array (`MutableArray<[T]>`'s
 * `push<[T]>`, for instance: a single element of type `[T]` also
 * satisfies "is an array", so that heuristic silently treated one
 * legitimate element as if it were the whole tail, discarding
 * everything else appended before or after it in the same call -- the
 * bug that surfaced this). Meaningless (and ignored) for a fixed-arity
 * call, or when `tail_count != 1`, so callers with no `Expr` of their
 * own to read it from (spawn, `main`) just pass 0 -- neither can ever
 * reach a variadic call in the first place (spawn requires exact
 * positional argument counts, see typecheck.c's STMT_SPAWN case). */
static Value *collapse_variadic_args(FunctionDecl *fn, Value *args, int argc, int tail_is_array_spread, int *out_argc) {
    if (fn->params.count == 0 || !fn->params.items[fn->params.count - 1].is_variadic) {
        *out_argc = argc;
        return args;
    }
    int fixed_count = fn->params.count - 1;
    int tail_count = argc - fixed_count;
    Value tail;
    if (tail_count == 1 && tail_is_array_spread) {
        tail = args[fixed_count];
    } else {
        Value *items = tail_count > 0 ? ziman_xmalloc((size_t)tail_count * sizeof(Value)) : NULL;
        for (int i = 0; i < tail_count; i++) {
            items[i] = args[fixed_count + i];
        }
        tail = value_array(type_ref(type_array(fn->params.items[fixed_count].type)), array_obj_new(items, tail_count));
    }
    Value *collapsed = ziman_xmalloc((size_t)fn->params.count * sizeof(Value));
    for (int i = 0; i < fixed_count; i++) {
        collapsed[i] = args[i];
    }
    collapsed[fixed_count] = tail;
    *out_argc = fn->params.count;
    return collapsed;
}

/* Runs `env`'s pending deferred calls, last registered first, then
 * clears them. Called from the only two places a block's statements
 * finish running -- exec_stmts_in_scope (every `{ ... }` and every `for`
 * iteration) and call_function (a function body) -- so falling off the
 * end, `return`, a propagating `?`, `break` and `continue` all pass
 * through it, and nothing else needs to know defer exists. A deferred
 * call's result is discarded. A fatal runtime error exits the process
 * instead of unwinding, so it never gets here (documented shortcoming). */
/* The scopes of this thread that have deferred calls pending, innermost last. A runtime error that ends a spawned thread unwinds the C stack with a jump, which
 * skips the block exits that would have run those calls -- so the thread's boundary runs them from this list (see thread_failed). A scope is on the list from its first
 * `defer` until its calls have run. */
static __thread Env **t_defer_envs = NULL;
static __thread int t_defer_env_count = 0;
static __thread int t_defer_env_capacity = 0;

/* The list is reachable only through the `__thread` pointer above, and the collector does not reliably scan thread-local storage: a list allocated with
 * malloc (the collector's) can be collected under a running thread, and the next `defer` or block exit then writes into memory that has become something
 * else -- an Env, say. So it is allocated uncollectable (a root the collector scans, which also keeps the tracked Envs alive) and given back when the thread ends. */
static void defer_envs_grow(int capacity) {
#ifdef ZIMAN_GC
    Env **fresh = ziman_xcheck(GC_MALLOC_UNCOLLECTABLE(sizeof(Env *) * (size_t)capacity));
    if (t_defer_envs != NULL) {
        memcpy(fresh, t_defer_envs, sizeof(Env *) * (size_t)t_defer_env_count);
        GC_FREE(t_defer_envs);
    }
#else
    Env **fresh = ziman_xrealloc(t_defer_envs, sizeof(Env *) * (size_t)capacity);
#endif
    t_defer_envs = fresh;
    t_defer_env_capacity = capacity;
}

static void defer_envs_release(void) {
#ifdef ZIMAN_GC
    if (t_defer_envs != NULL) GC_FREE(t_defer_envs);
#else
    free(t_defer_envs);
#endif
    t_defer_envs = NULL;
    t_defer_env_count = 0;
    t_defer_env_capacity = 0;
}

static void defer_env_track(Env *env) {
    if (t_defer_env_count == t_defer_env_capacity) defer_envs_grow(t_defer_env_capacity == 0 ? 8 : t_defer_env_capacity * 2);
    t_defer_envs[t_defer_env_count++] = env;
    env->defers_tracked = 1;
}

static void defer_env_untrack(Env *env) {
    for (int i = t_defer_env_count - 1; i >= 0; i--) {
        if (t_defer_envs[i] == env) {
            memmove(t_defer_envs + i, t_defer_envs + i + 1, sizeof(Env *) * (size_t)(t_defer_env_count - i - 1));
            t_defer_env_count--;
            break;
        }
    }
    env->defers_tracked = 0;
}

/* The `Iter` struct value(s) a `for ... in <iter>` loop on this thread is currently driving,
 * innermost last -- a strict LIFO stack, since one such loop nested inside another's body is the
 * only way more than one is ever active on one thread at once. Needed because that `Iter` value is
 * otherwise never bound to anything a root source can walk: it is a bare C local in STMT_FOR's own
 * `exec_stmt` frame (interpreter.c), found by the stack scan well enough to keep its own StructObj
 * header alive, but the stack scan dispatches through gc_mark_object (gc_mark.c), which has no
 * Value and so no type name to look `iter_mark_contents`'s own registration (iter.c) up by -- the
 * seam gc_mark_register_struct_content_marker needs a type-directed visit to fire at all. Pushing it
 * here, instead, means gc_mark_env_chain's root walk never needs to find it: `mark_this_threads_roots`
 * marks this stack directly, through `gc_mark_value`, which *does* have the type name.
 *
 * Binding it into the loop's own `scope` Env instead (tried first) is wrong in a much worse way than
 * just failing to fix the bug: `env_define`-ing anything into a scope the type checker's own
 * `env_lookup_resolved` has already assigned fixed slot indices for (every loop binder reached
 * through `env_at_slot`'s fast path) shifts every later binding's *runtime* index without the type
 * checker ever knowing -- the type checker's own resolution has no idea an extra binding now exists,
 * so a binder's compile-time-resolved slot silently stops meaning the same thing at runtime. Found
 * by the corruption it caused looking nothing like a GC bug at all (reading a stale/wrong slot, not
 * a swept one) -- a different failure signature from everything else is itself a hint one error got
 * fixed into a different one, not that it got fixed. */
static __thread Value *t_active_iters = NULL;
static __thread int t_active_iter_count = 0;
static __thread int t_active_iter_capacity = 0;

/* Same "the collector does not reliably scan thread-local storage" reasoning as t_defer_envs's own
 * comment above -- allocated uncollectable so Boehm itself never reclaims the array out from under a
 * running thread, independent of this module's own gc_heap mark phase reading it correctly. */
static void active_iters_grow(int capacity) {
#ifdef ZIMAN_GC
    Value *fresh = ziman_xcheck(GC_MALLOC_UNCOLLECTABLE(sizeof(Value) * (size_t)capacity));
    if (t_active_iters != NULL) {
        memcpy(fresh, t_active_iters, sizeof(Value) * (size_t)t_active_iter_count);
        GC_FREE(t_active_iters);
    }
#else
    Value *fresh = ziman_xrealloc(t_active_iters, sizeof(Value) * (size_t)capacity);
#endif
    t_active_iters = fresh;
    t_active_iter_capacity = capacity;
}

static void active_iters_release(void) {
#ifdef ZIMAN_GC
    if (t_active_iters != NULL) GC_FREE(t_active_iters);
#else
    free(t_active_iters);
#endif
    t_active_iters = NULL;
    t_active_iter_count = 0;
    t_active_iter_capacity = 0;
}

static void active_iter_push(Value v) {
    if (t_active_iter_count == t_active_iter_capacity) active_iters_grow(t_active_iter_capacity == 0 ? 4 : t_active_iter_capacity * 2);
    t_active_iters[t_active_iter_count++] = v;
}

static void active_iter_pop(void) {
    t_active_iter_count--;
}

/* A per-thread free list of Env structs already sized for a function call's own top-level
 * scope, reused by call_function instead of allocating (and, in a GC build, simply abandoning
 * to eventually become garbage) a fresh one on every call. Call/return is strictly LIFO per
 * thread -- a call's own Env is only ever released after every call it made has already
 * returned -- so a plain stack is enough; recursion is safe because each active call keeps its
 * own Env alive as a local variable on the C stack until it releases it, never handing out the
 * same pooled Env to two calls at once.
 *
 * Same GC-root reasoning as t_defer_envs just above, and for the same reason: the collector
 * does not reliably scan thread-local storage, so the array holding the pool is allocated
 * uncollectable (a root the collector always scans, which keeps every pooled Env -- and its own
 * `items` buffer -- alive between one call releasing it and a later call reusing it) rather
 * than through the ordinary GC_MALLOC a ZIMAN_GC build's `malloc`/`calloc` already redirect to. */
static __thread Env **t_env_pool = NULL;
static __thread int t_env_pool_count = 0;
static __thread int t_env_pool_capacity = 0;

/* A spawned thread's own pending `on_error` handler, from the moment spawn_thread_entry has its own
 * safe, ordinary-allocated copy until either thread_failed has used it or this thread's own call
 * finished without ever needing it -- see spawn_thread_entry's own comment on why this needs to be
 * an explicit root source at all (mirrors t_defer_envs/t_env_pool's own shape). */
static __thread Value t_pending_handler;
static __thread int t_has_pending_handler = 0;

static void env_pool_grow(int capacity) {
#ifdef ZIMAN_GC
    Env **fresh = ziman_xcheck(GC_MALLOC_UNCOLLECTABLE(sizeof(Env *) * (size_t)capacity));
    if (t_env_pool != NULL) {
        memcpy(fresh, t_env_pool, sizeof(Env *) * (size_t)t_env_pool_count);
        GC_FREE(t_env_pool);
    }
#else
    Env **fresh = ziman_xrealloc(t_env_pool, sizeof(Env *) * (size_t)capacity);
#endif
    t_env_pool = fresh;
    t_env_pool_capacity = capacity;
}

/* Releases this thread's own pool array (not the Envs it was holding -- in a GC build those are
 * ordinary GC_MALLOC memory once more, collected in the ordinary way the moment nothing else
 * roots them; in a nogc build they leak exactly as much as env_free's old per-call allocation
 * already would have if this thread never reused them, which pooling only ever reduces, never
 * increases). Called when a spawned thread ends, mirroring defer_envs_release right above, so a
 * long-running program spawning many short-lived threads doesn't accumulate one abandoned
 * uncollectable array per thread that ever ran. */
static void env_pool_release_all(void) {
#ifdef ZIMAN_GC
    if (t_env_pool != NULL) GC_FREE(t_env_pool);
#else
    free(t_env_pool);
#endif
    t_env_pool = NULL;
    t_env_pool_count = 0;
    t_env_pool_capacity = 0;
}

/* call_function's replacement for a bare env_new(parent): reuses a pooled Env (reset, then
 * re-parented for this call) if one is available, falling back to a fresh allocation otherwise. */
static Env *env_pool_acquire(Env *parent) {
    if (t_env_pool_count > 0) {
        Env *env = t_env_pool[--t_env_pool_count];
        env->parent = parent;
        return env;
    }
    return env_new(parent);
}

/* call_function's replacement for env_free: gives `env` back to the pool for a later call to
 * reuse instead of discarding it. env_reset clears its bindings first (freeing each one's own
 * strdup'd name -- a real free in a nogc build, a no-op in a GC build, same as env_reset's other
 * callers) so the next acquirer starts from an empty scope without needing to know this Env has
 * a history; `defers`/`caps`/`defer_count` are left as env_free would have left them too --
 * run_defers has already drained defer_count to 0 by the time a call reaches this, and an
 * interpreter-built Env never touches caps/is_lambda_boundary/is_global at all (those are the
 * type checker's own Env fields; see the Env doc comment in env.h), so there is nothing there
 * for a reuse to see that a fresh env_new's own zeroed fields wouldn't already have shown.
 *
 * `parent` is cleared, which is not cosmetic: the pool is one of this module's own GC root sources
 * (gc_interp_roots.c marks every Env in it), and `gc_mark_env_chain` follows `parent` chains. A
 * pooled Env left pointing at the scope it was last used under is a pointer into a scope that may
 * well be freed before this Env is ever acquired again -- `env_free` runs for an ordinary `while`
 * or `for` body's own scope on every exit path (interpreter.c's STMT_WHILE/STMT_FOR) -- after which
 * the next collection walks the chain straight into freed memory. `env_pool_acquire` re-parents on
 * the way out, so nothing ever observes the NULL; a pooled Env is simply, and now actually,
 * parentless and empty while it waits. This was a real use-after-free, invisible in a `make gc`
 * build only because `free` is a no-op there (zgc.h) and the stale `parent` kept the scope
 * *reachable*, so Boehm never reclaimed it -- correct by accident. In a `nogc` build, where `free`
 * is real, it segfaulted roughly every other run of any non-trivial program (docs/DESIGN.md). */
static void env_pool_release(Env *env) {
    env_reset(env);
    env->parent = NULL;
    if (t_env_pool_count == t_env_pool_capacity) env_pool_grow(t_env_pool_capacity == 0 ? 8 : t_env_pool_capacity * 2);
    t_env_pool[t_env_pool_count++] = env;
}

static void run_defers(Module *module, Env *env) {
    while (env->defer_count > 0) {
        Expr *call = env->defers[--env->defer_count];
        eval_expr(module, env, call);
    }
    if (env->defers_tracked) defer_env_untrack(env);
}

static Value call_function(Module *module, FunctionDecl *fn, Value *raw_args, int raw_argc, int tail_is_array_spread, int call_line, int call_col) {
    ZIMAN_STACK_CHECK();
    /* **`len(x)` before the frame is pushed at all.** It takes one argument, allocates nothing and
     * cannot fail, so there is no error for a frame to be named in and nothing for the collapse to
     * do. Measured: converting `len`'s nine declarations off the builtin id cost **+28.6%** on
     * 9,000,000 calls without this and **the frame push was almost all of it** -- `len` is the
     * hottest function in the language, since every `while i < len(x)` pays it once a pass.
     *
     * Picked by the already-cached native pointer, like the two `push` shortcuts below, so every
     * other call pays one integer test and a relaxed load. On the very first call the pointer is
     * unresolved and this falls through, once. */
    if (fn->is_native && raw_argc == 1 &&
        (NativeFn)__atomic_load_n(&fn->native_impl, __ATOMIC_RELAXED) == ziman_len) {
        return ziman_len(raw_args, 1, call_line, call_col);
    }
    ziman_push_frame(fn->name, fn->source_file, call_line, call_col);
    /* `push(b, x)` with exactly one element, before the variadic collapse allocates a
     * temporary array to carry it -- see ziman_mutable_array_push_one for what that cost is and how
     * it was measured (D18). Picked by comparing the already-cached native pointer rather than by
     * name, so every other call pays two integer tests and a relaxed load. On the very first call
     * the pointer is not resolved yet and this falls through to the general path, once. */
    if (fn->is_native && raw_argc == 2 && !tail_is_array_spread) {
        NativeFn impl = (NativeFn)__atomic_load_n(&fn->native_impl, __ATOMIC_RELAXED);
        if (impl == ziman_mutable_array_push) {
            ziman_mutable_array_push_one(raw_args[0], raw_args[1]);
            ziman_pop_frame();
            return value_int(0);
        }
        /* The array half, since F216 made it a declaration too. Without this the temporary the
         * collapse allocates is 65% of the call: `a = push(a, i)` is the array-building loop, and
         * one value is how it is always written. The result's element type is patched by the
         * caller, as it is for any generic native. */
        if (impl == ziman_array_push_values) {
            Value one = ziman_array_push_one(raw_args[0], raw_args[1]);
            ziman_pop_frame();
            return one;
        }
    }
    int argc;
    Value *args = collapse_variadic_args(fn, raw_args, raw_argc, tail_is_array_spread, &argc);
    if (fn->is_native) {
        /* Resolved once per declaration, not once per call -- see FunctionDecl.native_impl (ast.h)
         * for the measurement that motivated it. Two threads reaching an unresolved declaration at
         * the same time both do the lookup and both store the same pointer, so the relaxed atomic
         * pair is here to make that benign race well-defined rather than to coordinate anything:
         * every registration happens before any Ziman code runs (native.h), so there is exactly one
         * correct answer and it never changes. */
        NativeFn native = (NativeFn)__atomic_load_n(&fn->native_impl, __ATOMIC_RELAXED);
        if (native == NULL) {
            native = native_lookup(fn->name);
            /* The declaration's name is the *vocabulary*; the C symbol is a different thing, and
             * since F180 they are allowed to differ. Two natives can share one declared name --
             * `filter` is implemented once for `[T]` and once for `MutableArray<T>` -- and
             * `native_register` is keyed by a single string, so the symbol keeps the world prefix
             * the vocabulary gave up. This is the whole of the declaration-to-symbol link, and it
             * is deliberately the last place in the compiler where that prefix means anything:
             * nothing a reader writes, completes or sees in `--api builtins` carries it. */
            if (native == NULL && fn->params.count > 0) {
                Type p0 = fn->params.items[0].type;
                const char *prefix = NULL;
                /* **An array of bytes is its own world.** `number`'s `to_hex` has a `[byte]`
                 * member beside its `byte`, `int` and `bigint` ones (2026-10-24), and the symbol
                 * it wants is `bytes_to_hex` -- which is what this library calls that world
                 * everywhere else: `to_bytes`, `bytes_to_string`, `bytes_from_hex`,
                 * `bytes_equal`. Deriving `array_to_hex` would have been a symbol nobody
                 * registers, which is exactly what happened before this branch existed. The
                 * generic array natives are unaffected: their element is a type parameter, not
                 * `byte`, so they still derive `array_`. */
                if (p0.kind == TY_ARRAY && p0.elem != NULL && p0.elem->kind == TY_BYTE) prefix = "bytes_";
                else if (p0.kind == TY_ARRAY) prefix = "array_";
                /* The scalar worlds, added for `bits` (2026-10-24), which is the first module to
                 * give one declared name three native implementations: `and` is a `byte`, an `int`
                 * and a `bigint` operation. A scalar had no prefix until now because nothing
                 * needed one -- prelude.c's own comment says so -- and these three are enough,
                 * since `float`/`bool`/`string`/`decimal` have no overloaded native. */
                else if (p0.kind == TY_BYTE) prefix = "byte_";
                else if (p0.kind == TY_INT) prefix = "int_";
                else if (p0.kind == TY_BIGINT) prefix = "bigint_";
                /* `math` completed the scalar set (2026-10-24): `floor` is a `float` operation
                 * answering an `int` and a `decimal` operation answering a `bigint`, which is two
                 * implementations under one name. **Parameter one is all this reads**, so two
                 * members of the same first type -- `floor(d)` and `floor(d, scale)` -- land on
                 * the same symbol and that one C function dispatches on `argc`, the way
                 * `to_upper`'s two arities already do. */
                else if (p0.kind == TY_FLOAT) prefix = "float_";
                else if (p0.kind == TY_DECIMAL) prefix = "decimal_";
                /* F213: `index_of` is the first set whose members are a `string` and an `[T]`, so
                 * the string world needed a prefix of its own. Every other string native is still
                 * registered under its bare name and so never reaches this fallback at all. */
                else if (p0.kind == TY_STRING) prefix = "string_";
                else if (p0.kind == TY_MAP) prefix = "map_";
                else if (p0.kind == TY_SET) prefix = "set_";
                else if (p0.kind == TY_STRUCT && p0.struct_name != NULL) {
                    /* The builder worlds, added as the Map/Set pairing pass reached them (F195).
                     * One table rather than a chain of strcmps because the next pass will add to
                     * it, and a chain is where the fifth entry gets forgotten. */
                    static const struct { const char *type_name; const char *prefix; } WORLDS[] = {
                        {"MutableArray", "mutable_array_"}, {"MutableMap", "mutable_map_"},
                        {"MutableSet", "mutable_set_"},     {"MutableString", "mutable_string_"},
                        /* The cell, added when `mutable_get`/`mutable_set` became `get`/`set`
                         * (2026-10-22). Its prefix has no second underscore, so `get` on a
                         * `Mutable<T>` lands on the `mutable_get` that was already registered and
                         * the rename costs no C at all. Exact matching is what keeps this from
                         * colliding with `MutableSet`: `mutable_` + `set` is `mutable_set`, the
                         * cell's setter, where the set's own family is `mutable_set_add` and
                         * friends -- two names a letter apart, and the table never guesses.
                         *
                         * That pair stopped being hypothetical the same day. `mutable_set_add` was
                         * a declared Ziman name reached by its own spelling; it is now *only* a C
                         * symbol, derived here from `add` on a `MutableSet`. Both halves of the
                         * near-collision are live and both are reached through this table. */
                        {"Mutable", "mutable_"},
                    };
                    for (size_t w = 0; w < sizeof WORLDS / sizeof WORLDS[0]; w++) {
                        if (strcmp(p0.struct_name, WORLDS[w].type_name) == 0) {
                            prefix = WORLDS[w].prefix;
                            break;
                        }
                    }
                }
                if (prefix != NULL) {
                    char symbol[128];
                    /* **The prefix goes on the bare name, inside the module's own namespace.** A
                     * prelude declaration's `fn->name` is bare, so `array_` + `len` is the symbol
                     * and this split is a no-op. A bundled module's is `<path>::<name>`
                     * (`self_qualify_module`), and its registrations carry that same path --
                     * `native_register("bits::byte_and", ...)` -- so the symbol wanted is
                     * `bits::byte_and`, not `byte_bits::and`. `bits` is the first module to reach
                     * this fallback at all: until one declared name had two native
                     * implementations, every module native was found by its own spelling. */
                    const char *sep = strstr(fn->name, "::");
                    if (sep != NULL) {
                        snprintf(symbol, sizeof symbol, "%.*s%s%s",
                                 (int)(sep + 2 - fn->name), fn->name, prefix, sep + 2);
                    } else {
                        snprintf(symbol, sizeof symbol, "%s%s", prefix, fn->name);
                    }
                    native = native_lookup(symbol);
                }
            }
            if (native == NULL) {
                ziman_error("runtime", call_line, call_col, "native function '%s' has no registered implementation", fn->name);
            }
            __atomic_store_n(&fn->native_impl, (void (*)(void))native, __ATOMIC_RELAXED);
        }
        Value result = native(args, argc, call_line, call_col);
        if (args != raw_args) free(args);
        ziman_pop_frame();
        return result;
    }
    Env *env = env_pool_acquire(g_global_env);
    ziman_set_frame_env(env);
    for (int i = 0; i < argc; i++) {
        Type bound_type = fn->params.items[i].is_variadic ? type_array(fn->params.items[i].type) : fn->params.items[i].type;
        env_define(env, fn->params.items[i].name, bound_type, 0, args[i]);
    }
    if (fn->self_name != NULL) {
        /* A recursive lambda's own binding, rebuilt fresh on every call rather than captured
         * once at construction time -- there is no self-referential value to capture before
         * this call exists, so it is never captured at all. `args[0..self_ncaptured)` are
         * exactly the captured values *this* invocation itself received; copied (not aliased)
         * into their own array, the same way EXPR_LAMBDA's own creation-time capture copy
         * works, since `args` may be freed the line after this runs. */
        Value *self_captured = fn->self_ncaptured > 0 ? ziman_xmalloc((size_t)fn->self_ncaptured * sizeof(Value)) : NULL;
        for (int i = 0; i < fn->self_ncaptured; i++) self_captured[i] = args[i];
        Value self_value = value_closure(fn, self_captured, fn->self_ncaptured);
        env_define(env, fn->self_name, *self_value.type, 0, self_value);
    }
    if (args != raw_args) free(args);
    ExecResult result = {CTRL_NONE, value_int(0)};
    for (int i = 0; i < fn->body->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(module, env, fn->body->as.block.stmts.items[i]);
    }
    run_defers(module, env);
    env_pool_release(env);
    ziman_pop_frame();
    if (result.kind == CTRL_RETURN) return result.value;
    return fn->return_type.kind == TY_UNIT ? value_unit() : value_int(0);
}

/* `spawn`'s payload: `args` is heap-allocated by the spawning thread (it
 * has to outlive that thread's own stack frame, unlike an ordinary call's
 * `args`, which is freed before the call even returns -- see EXPR_CALL
 * below) and is owned by the new thread from here on, freed once that
 * thread is done with it. */
typedef struct SpawnArgs {
    Module *module;
    FunctionDecl *fn;
    Value *args;
    int argc;
    int line, col;
    Value handler; /* the `on_error` function value, when has_handler */
    int has_handler;
} SpawnArgs;

/* A function value (a named function or a lambda with what it captured) called from C with `args`.
 * Guarded around the actual call, not just this function's own local `all` buffer: `args` itself
 * belongs to *this call's own caller* (a native function's own parameters, or a temp buffer it is
 * still building -- e.g. prelude.c's array_map/filter/flat_map, iter.c's own callback sites), which
 * has no Env of its own to root it in and is otherwise invisible to both root sources for as long
 * as this call is on the stack. This is the one choke point every such caller already goes
 * through, so guarding it here protects all of them without touching each one individually. */
static Value call_closure_value(Module *module, Value closure, Value *args, int argc) {
    ClosureObj *clo = closure.as.clo;
    gc_unsafe_enter();
    Value result;
    if (clo->ncaptured == 0) {
        result = call_function(module, clo->fn, args, argc, 0, 0, 0);
    } else {
        Value *all = ziman_xmalloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
        memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
        memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value));
        result = call_function(module, clo->fn, all, clo->ncaptured + argc, 0, 0, 0);
        free(all);
    }
    gc_unsafe_leave();
    return result;
}

/* What a spawned thread does when a runtime error ends it (the C stack has been unwound to the thread's entry): the `on_error` handler gets the message, or, with none,
 * the message goes to stderr; then the deferred calls the thread had pending run (innermost first), so what it opened is closed and what it held is given back. An error
 * in the handler or in a deferred call is reported and does not stop the rest. The program goes on. */
static void thread_failed(struct SpawnArgs *sa, const char *message) {
    if (sa->has_handler) {
        ThreadBoundary inner;
        ziman_thread_boundary_enter(&inner);
        if (setjmp(inner.jb) == 0) {
            Value arg = value_string(ziman_xstrdup(message));
            call_closure_value(sa->module, sa->handler, &arg, 1);
            ziman_thread_boundary_leave();
        } else {
            fprintf(stderr, "ziman: the on_error handler failed: %s\n", inner.message);
        }
    } else {
        char *trace = ziman_trace_string();
        fprintf(stderr, "ziman: %s\n", message);
        if (trace[0] != '\0') fprintf(stderr, "%s\n", trace);
        fprintf(stderr, "ziman: the thread started by 'spawn %s' at %d:%d ended; the program goes on\n", sa->fn->name, sa->line, sa->col);
        free(trace);
    }
    while (t_defer_env_count > 0) {
        Env *env = t_defer_envs[--t_defer_env_count];
        env->defers_tracked = 0;
        while (env->defer_count > 0) {
            Expr *call = env->defers[--env->defer_count];
            ThreadBoundary inner;
            ziman_thread_boundary_enter(&inner);
            if (setjmp(inner.jb) == 0) {
                eval_expr(sa->module, env, call);
                ziman_thread_boundary_leave();
            } else {
                fprintf(stderr, "ziman: an error in a deferred call was ignored: %s\n", inner.message);
            }
        }
    }
}

static void *spawn_thread_entry(void *raw) {
    SpawnArgs *sa = (SpawnArgs *)raw;
    ziman_thread_begin(sa->fn->name);
    ziman_gc_register_thread_roots();
    /* Re-copies every crossing value into this thread's own ordinary allocators before anything
     * else runs (call_function's own parameter binding, and a failure path's own thread_failed both
     * read sa->args/sa->handler directly). The sending thread's own copies are left to the
     * collector: they live on Boehm's shared heap and nothing references them once these replace
     * them (see value_copy.h). */
    for (int i = 0; i < sa->argc; i++) sa->args[i] = value_copy_receive(sa->args[i]);
    if (sa->has_handler) sa->handler = value_copy_receive(sa->handler);
    /* sa->handler, from here until thread_failed either uses it or this thread decides it was
     * never needed, is referenced only by `sa` -- a plain malloc'd struct, invisible to both root
     * sources (not on this thread's own C stack, not bound into any Env) for however long
     * call_function's own body happens to run before it fails, if it ever does. Boehm's own
     * conservative scan would have found it regardless (it scans every GC_MALLOC'd block, sa
     * included, with no explicit rooting needed at all) back when everything crossing a thread
     * boundary lived in an arena Boehm treated as a valid scan target -- but a value_copy_receive
     * copy now lives whichever way this thread's own value constructors do today (this thread's own
     * gc_heap, for a kind already migrated there), which only this module's own two root sources
     * ever find. t_pending_handler is exactly that: a root source scoped to precisely this window,
     * mirroring t_defer_envs/t_env_pool's own shape for the identical reason. Found the same way as
     * every other gap like it this session -- empirically, via a flaky failure that vanished the
     * moment a debug print incidentally kept a stack-resident copy alive, not by inspection. */
    if (sa->has_handler) {
        t_pending_handler = sa->handler;
        t_has_pending_handler = 1;
    }
    ThreadBoundary boundary;
    ziman_thread_boundary_enter(&boundary);
    if (setjmp(boundary.jb) == 0) {
        call_function(sa->module, sa->fn, sa->args, sa->argc, 0, sa->line, sa->col);
        ziman_thread_boundary_leave();
    } else {
        thread_failed(sa, boundary.message);
    }
    t_has_pending_handler = 0;
    defer_envs_release();
    active_iters_release();
    env_pool_release_all();
    ziman_call_stack_release();
    ziman_thread_heap_release();
    ziman_gc_unregister_thread_roots();
    ziman_thread_end();
    /* Last, so this thread stops counting as a possible sender only once it truly cannot send
     * again -- see value.h. A reader blocked on a channel this thread was going to send on learns
     * from this that it is waiting for nothing. */
    ziman_channel_thread_ended();
    free(sa->args);
    free(sa);
    return NULL;
}

static Value call_print(Value arg) {
    char *s = value_display(arg);
    printf("%s\n", s);
    free(s);
    return value_int(0);
}

static Value call_str(Value arg) {
    return value_string(value_display(arg));
}


/* Scalars only (matching type_supports_hashing in typecheck.c) --
 * FNV-1a for strings, the raw bit pattern for a float, the value itself
 * for an int/bool. The sign bit is cleared before returning so the
 * result is always a non-negative int, safe to use directly with '%'
 * for bucket indexing (see the `collections` module's `Map<K, V>`). Not
 * a cryptographic hash, and not stable across a future language version
 * bumping this function's internals -- only ever used within a single
 * program run, never persisted. */
uint64_t value_hash(Value v) {
    ZIMAN_STACK_CHECK();
    uint64_t h;
    switch (v.type->kind) {
        case TY_INT:
            h = (uint64_t)v.as.i;
            break;
        case TY_BOOL:
            h = v.as.b ? 1 : 0;
            break;
        case TY_BYTE:
            h = (uint64_t)v.as.by;
            break;
        case TY_UNIT:
            h = 0;
            break;
        case TY_FLOAT: {
            /* -0.0 == 0.0, so they must hash alike. Every NaN likewise: value_key_equals treats any
             * two NaNs as the same key, so they have to reach the same trie slot to be compared at
             * all, and a double has many NaN encodings (quiet, signalling, any payload, either
             * sign) that would otherwise hash apart. Canonicalised to one of them rather than to a
             * made-up constant, so the hash stays a pure function of the value. */
            if (v.as.f != v.as.f) {
                h = 0x7ff8000000000000ULL; /* the canonical quiet NaN, written out rather than computed */
            } else {
                double f = v.as.f == 0.0 ? 0.0 : v.as.f;
                memcpy(&h, &f, sizeof(h));
            }
            break;
        }
        case TY_ARRAY: {
            h = 1469598103934665603ULL ^ (uint64_t)v.as.arr->count;
            for (int i = 0; i < v.as.arr->count; i++) {
                h ^= value_hash(array_get(v.as.arr, i));
                h *= 1099511628211ULL;
            }
            break;
        }
        case TY_STRUCT: {
            /* Field by field in declaration order: the same shape `==` compares. */
            h = 1469598103934665603ULL ^ (uint64_t)v.as.st->count;
            for (int i = 0; i < v.as.st->count; i++) {
                h ^= value_hash(v.as.st->fields[i]);
                h *= 1099511628211ULL;
            }
            break;
        }
        case TY_MAP:
        case TY_SET: {
            /* Order-independent (equal maps hash equal whatever their insertion order): the
             * entries' hashes are combined with a commutative sum. */
            int is_map = v.type->kind == TY_MAP;
            Value *keys, *vals = NULL;
            mapobj_entries(v.as.map, &keys, is_map ? &vals : NULL);
            h = 1469598103934665603ULL ^ (uint64_t)v.as.map->count;
            uint64_t sum = 0;
            for (int i = 0; i < v.as.map->count; i++) {
                uint64_t e = value_hash(keys[i]) * 0x9E3779B97F4A7C15ULL;
                if (is_map) e ^= value_hash(vals[i]) + 0x7F4A7C15ULL;
                e ^= e >> 29;
                e *= 0xBF58476D1CE4E5B9ULL;
                e ^= e >> 32;
                sum += e;
            }
            h ^= sum;
            h *= 1099511628211ULL;
            break;
        }
        case TY_RESULT:
            h = 1469598103934665603ULL ^ (uint64_t)(v.as.res->is_ok ? 1 : 2);
            h ^= value_hash(v.as.res->value);
            h *= 1099511628211ULL;
            break;
        case TY_ENUM: {
            /* The variant name, then the payload if there is one -- the same
             * two things `==` compares. */
            const char *name = v.as.en->variant_name;
            h = 1469598103934665603ULL;
            for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
                h ^= *s;
                h *= 1099511628211ULL;
            }
            if (v.as.en->has_payload) {
                h ^= value_hash(v.as.en->payload);
                h *= 1099511628211ULL;
            }
            break;
        }
        case TY_BIGINT: {
            /* FNV-1a folded over the sign and the normalized (trimmed)
             * digit array -- since two equal BigNums always have an
             * identical sign + digit-array representation (bignum.c
             * always trims leading zero limbs), equal values are
             * guaranteed to hash equal here too. */
            h = 1469598103934665603ULL;
            h ^= (uint64_t)(v.as.bi->sign + 1);
            h *= 1099511628211ULL;
            for (int i = 0; i < v.as.bi->count; i++) {
                h ^= v.as.bi->digits[i];
                h *= 1099511628211ULL;
            }
            break;
        }
        case TY_DECIMAL: {
            /* Unlike BigNum, two numerically-equal Decimals aren't
             * guaranteed identical representations (1.0 vs 1.00) --
             * decimal_normalize first so equal values still hash equal. */
            Decimal n = decimal_normalize(*v.as.dec);
            h = 1469598103934665603ULL;
            h ^= (uint64_t)(n.unscaled.sign + 1);
            h *= 1099511628211ULL;
            h ^= (uint64_t)n.scale;
            h *= 1099511628211ULL;
            for (int i = 0; i < n.unscaled.count; i++) {
                h ^= n.unscaled.digits[i];
                h *= 1099511628211ULL;
            }
            break;
        }
        default: { /* TY_STRING -- the only other hashable kind */
            const unsigned char *s = (const unsigned char *)v.as.str.ptr;
            h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
            for (uint32_t k = 0; k < v.as.str.len; k++) {
                h ^= s[k];
                h *= 1099511628211ULL; /* FNV-1a 64-bit prime */
            }
            break;
        }
    }
    return h;
}

static Value call_hash(Value v) {
    /* Sign bit cleared so the result is always non-negative, safe to use directly
     * with '%' for bucket indexing. */
    return value_int((int64_t)(value_hash(v) & 0x7FFFFFFFFFFFFFFFULL));
}

/* Three-way comparison behind `compare` and `sort`: negative, zero or
 * positive (this implementation happens to return -1/0/1, but the language
 * only promises the sign, and a user `_compare` may return any int). Scalars
 * compare natively (a string bytewise, then by length; a float with NaN
 * sorting after every other value and equal to itself, so the order is
 * total). A struct or enum calls its `<type>_compare` function -- written by
 * hand or by #[derive(Ord)]; the checker has already proved it exists. */
static int cmp_string_values(Value a, Value b) {
    size_t la = a.as.str.len, lb = b.as.str.len;
    int c = memcmp(a.as.str.ptr, b.as.str.ptr, la < lb ? la : lb);
    if (c != 0) return c < 0 ? -1 : 1;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

static int cmp_float_values(double x, double y) {
    int nx = x != x, ny = y != y;
    if (nx || ny) return nx == ny ? 0 : (nx ? 1 : -1);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static Value call_function(Module *module, FunctionDecl *fn, Value *raw_args, int raw_argc, int tail_is_array_spread, int call_line, int call_col);

static int value_compare(Module *module, Value a, Value b, int line, int col) {
    ZIMAN_STACK_CHECK();
    /* Option<T>: None before every Some, Somes by payload. */
    if (a.type->kind == TY_ENUM && a.type->enum_name != NULL && strcmp(a.type->enum_name, "Option") == 0) {
        int a_some = strcmp(a.as.en->variant_name, "Some") == 0;
        int b_some = strcmp(b.as.en->variant_name, "Some") == 0;
        if (a_some != b_some) return a_some ? 1 : -1;
        if (!a_some) return 0;
        return value_compare(module, a.as.en->payload, b.as.en->payload, line, col);
    }
    switch (a.type->kind) {
        case TY_INT: return a.as.i < b.as.i ? -1 : (a.as.i > b.as.i ? 1 : 0);
        case TY_FLOAT: return cmp_float_values(a.as.f, b.as.f);
        case TY_STRING: return cmp_string_values(a, b);
        case TY_BYTE: return a.as.by < b.as.by ? -1 : (a.as.by > b.as.by ? 1 : 0);
        case TY_UNIT: return 0;
        case TY_BIGINT: { int c = bignum_compare(*a.as.bi, *b.as.bi); return c < 0 ? -1 : (c > 0 ? 1 : 0); }
        case TY_DECIMAL: { int c = decimal_compare(*a.as.dec, *b.as.dec); return c < 0 ? -1 : (c > 0 ? 1 : 0); }
        case TY_ARRAY: {
            ArrayObj *x = a.as.arr, *y = b.as.arr;
            int n = x->count < y->count ? x->count : y->count;
            for (int i = 0; i < n; i++) {
                int c = value_compare(module, array_get(x, i), array_get(y, i), line, col);
                if (c != 0) return c;
            }
            return x->count < y->count ? -1 : (x->count > y->count ? 1 : 0);
        }
        case TY_RESULT: {
            ResultObj *x = a.as.res, *y = b.as.res;
            if (x->is_ok != y->is_ok) return x->is_ok ? -1 : 1;
            return value_compare(module, x->value, y->value, line, col);
        }
        case TY_ENUM:
        case TY_STRUCT: {
            const char *type_name_str = a.type->kind == TY_STRUCT ? a.type->struct_name : a.type->enum_name;
            char *fn_name = ziman_compare_function_name(type_name_str);
            FunctionDecl *fn = find_function(module, fn_name);
            if (fn == NULL) fn = find_function(module, ziman_compare_function_name_flat(type_name_str));
            if (fn == NULL) ziman_error("runtime", line, col, "no compare function '%s' for %s", fn_name, type_name_str);
            Value args[2] = {a, b};
            Value r = call_function(module, fn, args, 2, 0, line, col);
            free(fn_name);
            return r.as.i < 0 ? -1 : (r.as.i > 0 ? 1 : 0);
        }
        default:
            ziman_error("runtime", line, col, "internal error: compare on an unordered type");
            return 0;
    }
}

/* `error_message(e)`'s dispatch: the same naming-convention lookup `value_compare`'s TY_ENUM
 * case uses, just for `<snake_type>_message` instead of `_compare`, and enums only (checked
 * already at type-check time by find_message_function, so `fn` is never NULL here). */
static Value value_error_message(Module *module, Value e, int line, int col) {
    char *fn_name = ziman_message_function_name(e.type->enum_name);
    FunctionDecl *fn = find_function(module, fn_name);
    if (fn == NULL) ziman_error("runtime", line, col, "internal error: no message function '%s' for %s", fn_name, e.type->enum_name);
    Value args[1] = {e};
    return call_function(module, fn, args, 1, 0, line, col);
}

/* push/insert/set never mutate their `arr` argument -- they always build a
 * brand new ArrayObj and return a Value wrapping it, per docs/DESIGN.md's
 * value-semantics decision for arrays. Sharing `old->items[k]` Values by
 * plain struct copy into the new buffer is safe precisely because ArrayObj
 * is immutable once built (see value.h) -- there's nothing to alias into. */
static Value call_ok(Type result_type, Value x) {
    return value_result(type_ref(result_type), result_obj_new(1, x));
}

static Value call_err(Type result_type, Value e) {
    return value_result(type_ref(result_type), result_obj_new(0, e));
}

static Value call_some(Type option_type, Value x) {
    return value_enum(type_ref(option_type), enum_obj_new("Option", "Some", 1, x));
}

static Value call_is_ok(Value r) {
    return value_bool(r.as.res->is_ok);
}

static Value call_unwrap(Value r, int line, int col) {
    if (!r.as.res->is_ok) {
        char *msg = value_display(r.as.res->value);
        ziman_error("runtime", line, col, "unwrap called on an Err value: %s", msg);
    }
    return r.as.res->value;
}

static Value call_unwrap_err(Value r, int line, int col) {
    if (r.as.res->is_ok) {
        char *msg = value_display(r.as.res->value);
        ziman_error("runtime", line, col, "unwrap_err called on an Ok value: %s", msg);
    }
    return r.as.res->value;
}

/* args[0] is the format string, args[1..argc-1] are the interpolation
 * values. The type checker already verified `{}` placeholder count
 * matches argc-1 and that the format string is well-formed, so this
 * doesn't re-validate -- it trusts that pass. */
/* One placeholder's text: the argument as `print` shows it, or with a precision N (a float or a decimal, which the type
 * checker guarantees) rounded to N places and written with exactly N: a float as C's "%.*f" (correctly rounded from its
 * binary value), a decimal half-up like decimal_round. */
static char *format_placeholder(Value v, int precision) {
    if (precision < 0) return value_display(v);
    if (v.type->kind == TY_DECIMAL) return decimal_to_string(decimal_round(*v.as.dec, precision));
    int len = snprintf(NULL, 0, "%.*f", precision, v.as.f);
    char *out = zalloc_atomic((size_t)len + 1);
    snprintf(out, (size_t)len + 1, "%.*f", precision, v.as.f);
    return out;
}

static Value call_format(Value *args, int argc) {
    const char *fmt = args[0].as.str.ptr;
    size_t fmt_len = args[0].as.str.len;
    size_t cap = 32, len = 0;
    char *buf = zalloc_atomic(cap);
    int arg_i = 1;
    size_t pos = 0;
    FmtPiece piece;
    char err[256];
    while (fmt_next(fmt, fmt_len, &pos, &piece, err, sizeof err) > 0) {
        char *text = NULL;
        const char *src = piece.text;
        size_t src_len = piece.len;
        if (piece.placeholder) {
            text = format_placeholder(args[arg_i++], piece.precision);
            src = text;
            src_len = strlen(text);
        }
        while (len + src_len + 1 > cap) {
            cap *= 2;
            buf = ziman_xrealloc(buf, cap);
        }
        memcpy(buf + len, src, src_len);
        len += src_len;
        free(text);
    }
    buf[len] = '\0';
    (void)argc;
    return value_string(buf);
}

static Value eval_binary(Module *module, Env *env, Expr *e) {
    Value l = eval_expr(module, env, e->as.binary.left);

    /* Short-circuit && / || before evaluating the right side. */
    if (e->as.binary.op == OP_AND) {
        if (!l.as.b) return value_bool(0);
        return eval_expr(module, env, e->as.binary.right);
    }
    if (e->as.binary.op == OP_OR) {
        if (l.as.b) return value_bool(1);
        return eval_expr(module, env, e->as.binary.right);
    }

    Value r = eval_expr(module, env, e->as.binary.right);

    switch (e->as.binary.op) {
        case OP_ADD:
            if (l.type->kind == TY_INT) {
                int64_t sum;
                if (__builtin_add_overflow(l.as.i, r.as.i, &sum)) int_overflow(e->line, e->col, "+");
                return value_int(sum);
            }
            if (l.type->kind == TY_BIGINT) return value_bigint(bignum_add(*l.as.bi, *r.as.bi));
            if (l.type->kind == TY_DECIMAL) return value_decimal(decimal_add(*l.as.dec, *r.as.dec));
            return value_float(l.as.f + r.as.f);
        case OP_SUB:
            if (l.type->kind == TY_INT) {
                int64_t diff;
                if (__builtin_sub_overflow(l.as.i, r.as.i, &diff)) int_overflow(e->line, e->col, "-");
                return value_int(diff);
            }
            if (l.type->kind == TY_BIGINT) return value_bigint(bignum_sub(*l.as.bi, *r.as.bi));
            if (l.type->kind == TY_DECIMAL) return value_decimal(decimal_sub(*l.as.dec, *r.as.dec));
            return value_float(l.as.f - r.as.f);
        case OP_MUL:
            if (l.type->kind == TY_INT) {
                int64_t product;
                if (__builtin_mul_overflow(l.as.i, r.as.i, &product)) int_overflow(e->line, e->col, "*");
                return value_int(product);
            }
            if (l.type->kind == TY_BIGINT) return value_bigint(bignum_mul(*l.as.bi, *r.as.bi));
            if (l.type->kind == TY_DECIMAL) return value_decimal(decimal_mul(*l.as.dec, *r.as.dec));
            return value_float(l.as.f * r.as.f);
        case OP_DIV:
            if (l.type->kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                if (l.as.i == INT64_MIN && r.as.i == -1) int_overflow(e->line, e->col, "/");
                return value_int(l.as.i / r.as.i);
            }
            if (l.type->kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(quot);
            }
            if (l.type->kind == TY_DECIMAL) {
                if (decimal_is_zero(*r.as.dec)) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_decimal(decimal_div(*l.as.dec, *r.as.dec));
            }
            if (r.as.f == 0.0) ziman_error("runtime", e->line, e->col, "division by zero");
            return value_float(l.as.f / r.as.f);
        case OP_MOD:
            if (l.type->kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                if (r.as.i == -1) return value_int(0);
                return value_int(l.as.i % r.as.i);
            }
            if (l.type->kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(rem);
            }
            if (l.type->kind == TY_DECIMAL) {
                if (decimal_is_zero(*r.as.dec)) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_decimal(decimal_mod(*l.as.dec, *r.as.dec));
            }
            ziman_error("runtime", e->line, e->col, "internal error: '%%' on float should have been rejected by the type checker");
            return value_int(0);
        case OP_EQ: return value_bool(value_equals(l, r));
        case OP_NEQ: return value_bool(!value_equals(l, r));
        case OP_LT:
            if (l.type->kind == TY_STRING) return value_bool(cmp_string_values(l, r) < 0);
            if (l.type->kind == TY_BYTE) return value_bool(l.as.by < r.as.by);
            if (l.type->kind == TY_INT) return value_bool(l.as.i < r.as.i);
            if (l.type->kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) < 0);
            if (l.type->kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) < 0);
            return value_bool(l.as.f < r.as.f);
        case OP_GT:
            if (l.type->kind == TY_STRING) return value_bool(cmp_string_values(l, r) > 0);
            if (l.type->kind == TY_BYTE) return value_bool(l.as.by > r.as.by);
            if (l.type->kind == TY_INT) return value_bool(l.as.i > r.as.i);
            if (l.type->kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) > 0);
            if (l.type->kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) > 0);
            return value_bool(l.as.f > r.as.f);
        case OP_LE:
            if (l.type->kind == TY_STRING) return value_bool(cmp_string_values(l, r) <= 0);
            if (l.type->kind == TY_BYTE) return value_bool(l.as.by <= r.as.by);
            if (l.type->kind == TY_INT) return value_bool(l.as.i <= r.as.i);
            if (l.type->kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) <= 0);
            if (l.type->kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) <= 0);
            return value_bool(l.as.f <= r.as.f);
        case OP_GE:
            if (l.type->kind == TY_STRING) return value_bool(cmp_string_values(l, r) >= 0);
            if (l.type->kind == TY_BYTE) return value_bool(l.as.by >= r.as.by);
            if (l.type->kind == TY_INT) return value_bool(l.as.i >= r.as.i);
            if (l.type->kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) >= 0);
            if (l.type->kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) >= 0);
            return value_bool(l.as.f >= r.as.f);
        default:
            ziman_error("runtime", e->line, e->col, "internal error: unhandled binary operator");
            return value_int(0);
    }
}

/* `EnumName.member[(args)]` -- construction if `member` names one of the
 * enum's own variants, or an is_<variant>/unwrap_<variant> consumption
 * call otherwise. All shapes and argument counts were already validated
 * by check_enum_access in typecheck.c; this only needs to actually do it. */
static Value eval_enum_access(Module *module, Env *env, Expr *e) {
    const char *enum_name = e->as.enum_access.enum_name;
    const char *member = e->as.enum_access.member_name;
    EnumDecl *decl = find_enum(module, enum_name);

    for (int i = 0; i < decl->variants.count; i++) {
        EnumVariant *v = &decl->variants.items[i];
        if (strcmp(v->name, member) != 0) continue;
        Value payload = v->has_payload ? eval_expr(module, env, e->as.enum_access.args.items[0]) : value_int(0);
        EnumObj *obj = enum_obj_new(decl->name, v->name, v->has_payload, payload);
        return value_enum(&e->resolved_type, obj);
    }

    Value arg = eval_expr(module, env, e->as.enum_access.args.items[0]);
    EnumObj *obj = arg.as.en;
    static const char *IS_PREFIX = "is_";
    static const char *UNWRAP_PREFIX = "unwrap_";
    if (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0) {
        return value_bool(strcmp(obj->variant_name, member + strlen(IS_PREFIX)) == 0);
    }
    /* unwrap_<variant> -- typecheck already proved the named variant has a
     * payload, but `obj` might currently hold a *different* variant of the
     * same enum at runtime; same fatal-by-default contract as Result's
     * unwrap/unwrap_err. */
    const char *wanted = member + strlen(UNWRAP_PREFIX);
    if (strcmp(obj->variant_name, wanted) != 0) {
        ziman_error("runtime", e->line, e->col, "called '%s.unwrap_%s' on a '%s.%s' value",
                    enum_name, wanted, enum_name, obj->variant_name);
    }
    return obj->payload;
}

/* Every builtin name the dispatch chain in eval_expr tests, as an integer each call node caches
 * (ast.h's `call_builtin`). The chain then compares ints, which GCC can turn into a jump table,
 * instead of comparing names -- and a builtin stops paying for every branch above it.
 *
 * The chain's *order* is still load-bearing and is unchanged: `set`, `insert` and `remove` are each
 * tested in two different branches (an array/map one and a `Set`/`Map` constructor one), so
 * first-match-wins decides which handler a call reaches. Ids do not change that; they only make
 * each test cheap.
 *
 * The measurements this replaces, for the record: with a plain `strcmp` per branch, `len` (5th)
 * cost 19-39 ns/op, `map_get_or` (22nd) 87-119 and `contains` (44th) 182-220 on a 100,000-entry
 * map, and hoisting `contains` to 5th took it to 78-124 -- so position was the whole story.
 * Guarding each `strcmp` with the literal's first character took `contains` to 93-94, and this
 * removes what that left: 44 byte-compares against `map_get_or`'s 22. */
typedef enum {
    B_NONE = 0,
    B_print,
    B_str,
    B_format,
    B_printf,
    B_mutable_array_push_all,
    B_mutable_array_from,
    B_mutable_array_lit,
    B_mutable_map_from,
    B_mutable_string_from,
    B_mutable_set_from,
    B_map_lit,
    B_set_lit,
    B_ok,
    B_err,
    B_Some,
    B_None,
    B_is_ok,
    B_unwrap,
    B_unwrap_err,
    B_hash,
    B_compare,
    B_error_message,
    B_channel_new,
    B_send,
    B_try_send,
    B_channel_close,
    B_channel_len,
    B_try_receive,
    B_receive,
    B_receive_option,
    B_receive_timeout,
    B_select2
} BuiltinId;

static const struct { const char *name; int id; } g_builtins[] = {
    {"print", B_print},
    {"str", B_str},
    {"format", B_format},
    {"printf", B_printf},
    {"$mutable_array_from", B_mutable_array_from},
    {"$mutable_array_lit", B_mutable_array_lit},
    {"$mutable_map_from", B_mutable_map_from},
    {"$mutable_string_from", B_mutable_string_from},
    {"$mutable_set_from", B_mutable_set_from},
    {"$map_lit", B_map_lit},
    {"$set_lit", B_set_lit},
    {"ok", B_ok},
    {"err", B_err},
    {"Some", B_Some},
    {"None", B_None},
    {"is_ok", B_is_ok},
    {"unwrap", B_unwrap},
    {"unwrap_err", B_unwrap_err},
    {"hash", B_hash},
    {"compare", B_compare},
    {"error_message", B_error_message},
    {"channel_new", B_channel_new},
    {"send", B_send},
    {"try_send", B_try_send},
    {"channel_close", B_channel_close},
    {"channel_len", B_channel_len},
    {"try_receive", B_try_receive},
    {"receive", B_receive},
    {"receive_option", B_receive_option},
    {"receive_timeout", B_receive_timeout},
    {"select2", B_select2},
};

/* Runs once per call site, not per call -- the result lands in the node. A linear scan is
 * therefore the right shape: it costs one walk over 63 names the first time a given call
 * expression is evaluated, and nothing ever again. Returns -1 for a name that is not a builtin,
 * which the caller caches too, so an ordinary call never scans twice either. */
static int resolve_builtin(const char *name) {
    for (size_t i = 0; i < sizeof(g_builtins) / sizeof(g_builtins[0]); i++) {
        if (name[0] == g_builtins[i].name[0] && strcmp(name, g_builtins[i].name) == 0) {
            return g_builtins[i].id;
        }
    }
    return -1;
}


static Value eval_expr(Module *module, Env *env, Expr *e) {
    ZIMAN_STACK_CHECK();
    switch (e->kind) {
        case EXPR_INT: return value_int(e->as.int_val);
        case EXPR_FLOAT: return value_float(e->as.float_val);
        case EXPR_STRING: return value_string(e->as.string_val);
        case EXPR_BOOL: return value_bool(e->as.bool_val);
        case EXPR_UNIT: return value_unit();
        case EXPR_BIGINT: return value_bigint(e->as.bigint_val);
        case EXPR_DECIMAL: return value_decimal(e->as.decimal_val);
        case EXPR_BYTE: return value_byte(e->as.byte_val);
        case EXPR_IDENT: {
            if (e->fn_ref != NULL) return value_function((FunctionDecl *)e->fn_ref);
            Binding *b = e->resolved_depth >= 0 ? env_at_slot(env, e->resolved_depth, e->resolved_slot)
                                                 : env_lookup(env, e->as.ident_name);
            /* **Not defensiveness: this is reachable, and it used to segfault.** The type checker
             * proves every name resolves, so the only way here is a name that resolves and has no
             * value *yet* -- a module-level val read before its own initialiser has run. Module
             * vals are initialised in file order (see init_global_vals), so
             *
             *     val a = tag("one")        // runs first
             *     fn tag(t: string) -> string { return concat(base, t) }
             *     val base = "prefix:"      // not initialised when tag() reads it
             *
             * dereferenced NULL. The order is a real constraint of eager initialisation and the
             * message says what to do about it; what it must not be is a crash with no name in it.
             * See init_param_defaults' own comment, which records this as the unaddressed half of
             * the same problem -- the fix it suggests, evaluating globals lazily on first
             * reference, is the one that would remove the constraint rather than report it. */
            if (b == NULL) {
                ziman_error("runtime", e->line, e->col,
                            "'%s' is read before it is initialised: module-level vals are initialised in file "
                            "order, and this one is reached by an initialiser that runs earlier -- move '%s' "
                            "above the val whose initialiser uses it",
                            e->as.ident_name, e->as.ident_name);
                return value_int(0);
            }
            return b->value;
        }
        case EXPR_UNARY: {
            Value v = eval_expr(module, env, e->as.unary.operand);
            if (e->as.unary.op == OP_NEG) {
                if (v.type->kind == TY_INT) {
                    if (v.as.i == INT64_MIN) int_overflow(e->line, e->col, "-");
                    return value_int(-v.as.i);
                }
                if (v.type->kind == TY_BIGINT) return value_bigint(bignum_negate(*v.as.bi));
                if (v.type->kind == TY_DECIMAL) return value_decimal(decimal_negate(*v.as.dec));
                return value_float(-v.as.f);
            }
            return value_bool(!v.as.b);
        }
        case EXPR_BINARY:
            return eval_binary(module, env, e);
        case EXPR_CALL: {
            int argc = e->as.call.args.count;
            /* Small argument lists go on the C stack rather than the heap. Every call used to malloc
             * its argument array: measured at 24.5% of all allocation calls and 314MB of garbage over
             * 2,000 authenticated requests of the reference backend, second only to the environment's
             * own binding names. Under the collector that garbage is what drives collection frequency,
             * and collection frequency is what stops throughput scaling with clients (F7).
             *
             * Correct by the same mechanism the collector already depends on: Boehm scans every
             * thread's C stack conservatively, so Values living in this frame keep their referents
             * alive exactly as the heap array did. ARGS_INLINE is deliberately small -- the array sits
             * in every eval_expr frame whether a call is being evaluated or not, and eval_expr recurses
             * once per nested expression, so a large one would cost recursion depth. */
            Value inline_args[ARGS_INLINE];
            Value *args = argc == 0 ? NULL : (argc <= ARGS_INLINE ? inline_args
                                                                  : ziman_xmalloc((size_t)argc * sizeof(Value)));
            gc_unsafe_enter();
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(module, env, e->as.call.args.items[i]);
            }
            gc_unsafe_leave();
            Value result;
            /* NULL only for a call through a value, which the first branch takes before any
             * dispatch runs. `bid` is this node's cached builtin id -- see ast.h's call_builtin. */
            const char *callee = e->as.call.callee;
            int bid = 0;
            if (callee != NULL) {
                bid = __atomic_load_n(&e->call_builtin, __ATOMIC_RELAXED);
                if (bid == 0) {
                    bid = resolve_builtin(callee);
                    __atomic_store_n(&e->call_builtin, bid, __ATOMIC_RELAXED);
                }
            }
            if (e->as.call.through_value || e->as.call.callee_expr != NULL) {
                /* A call through a function value. Either a local of function type (a parameter, a val),
                 * or -- when `callee_expr` is set -- a struct field holding one, `handler.run(21)`, whose
                 * target is an expression rather than a name (see ast.h). Both end in the same place: a
                 * ClosureObj naming the FunctionDecl to run. */
                ClosureObj *clo;
                if (e->as.call.callee_expr != NULL) {
                    clo = eval_expr(module, env, e->as.call.callee_expr).as.clo;
                } else {
                    Binding *fb = e->resolved_depth >= 0 ? env_at_slot(env, e->resolved_depth, e->resolved_slot)
                                                          : env_lookup(env, e->as.call.local_name);
                    clo = fb->value.as.clo;
                }
                if (clo->ncaptured == 0) {
                    result = call_function(module, clo->fn, args, argc, 0, e->line, e->col);
                } else {
                    /* A lambda's captured values are its first parameters. */
                    Value *all = ziman_xmalloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
                    memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
                    /* `args` is NULL for a zero-argument lambda, and memcpy's second parameter is
                     * declared never-null whatever the length is, so the zero-length copy is
                     * undefined by the letter of C11 7.24.1p2 -- UBSan reports it, harmlessly, on
                     * 12 of the fixtures, which is enough noise to bury a real report
                     * (docs/reviews/tasks.md A22). */
                    if (argc > 0) memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value));
                    result = call_function(module, clo->fn, all, clo->ncaptured + argc, 0, e->line, e->col);
                    free(all);
                }
            } else if (e->call_fn != NULL) {
                /* An ordinary named call to a user-defined function: check_call's own
                 * find_function(module, name) already ran once, at type-check time, and only
                 * ever reaches its ordinary-function fallback (setting e->call_fn) after every
                 * `strcmp` branch below it has already failed to match -- so this can never
                 * fire for an actual builtin, and skips re-running all ~48 of those comparisons
                 * on every single call to any plain function, the same "resolve once, reuse
                 * every time" idea call_fn's own doc comment (ast.h) describes for `find_function`
                 * itself. The `else` chain below is left completely unreached for this case, not
                 * removed, as a fallback with zero cost if this branch is ever wrong about a
                 * name it hasn't seen before. See docs/DESIGN.md's interpreter-speed entry. */
                FunctionDecl *fn = (FunctionDecl *)e->call_fn;
                result = call_function(module, fn, args, argc, e->as.call.variadic_tail_is_array_spread, e->line, e->col);
                if (fn->is_native && fn->type_params.count > 0 && e->resolved_type.kind != TY_TYPEVAR) {
                    result.type = &e->resolved_type;
                }
            } else if (bid == B_print) {
                result = call_print(args[0]);
            } else if (bid == B_str) {
                result = call_str(args[0]);
            } else if (bid == B_format) {
                result = call_format(args, argc);
            } else if (bid == B_printf) {
                Value text = call_format(args, argc);
                fwrite(text.as.str.ptr, 1, text.as.str.len, stdout);
                fflush(stdout);
                result = value_int(0);
            } else if (bid == B_mutable_array_lit) {
                result = ziman_mutable_array_lit(args, argc);
                result.type = &e->resolved_type;
            } else if (bid == B_mutable_array_from) {
                result = ziman_mutable_array_from(args[0], args[0].type->kind == TY_STRUCT);
                result.type = &e->resolved_type;
            } else if (bid == B_mutable_string_from) {
                /* The fourth builder world, 2026-10-23. `mutable(s)` on text answered a
                 * `Mutable<string>` cell until then, because the `mutable` arm's own comment had
                 * listed a string among the things with "no builder to answer" -- which
                 * MutableString has been since it existed. */
                result = ziman_mutable_string_from(args[0], args[0].type->kind == TY_STRUCT);
            } else if (bid == B_mutable_map_from ||
                       bid == B_mutable_set_from) {
                result = ziman_mutable_map_from(args[0], bid == B_mutable_set_from);
                result.type = &e->resolved_type;
            } else if (bid == B_map_lit || bid == B_set_lit) {
                int is_map = bid == B_map_lit;
                MapObj *m = mapobj_empty();
                for (int i = 0; i < argc; i += is_map ? 2 : 1) {
                    m = mapobj_set(m, args[i], is_map ? args[i + 1] : value_int(0));
                }
                result = value_map(&e->resolved_type, m);
            } else if (bid == B_ok) {
                result = call_ok(e->resolved_type, args[0]);
            } else if (bid == B_err) {
                result = call_err(e->resolved_type, args[0]);
            } else if (bid == B_Some) {
                result = call_some(e->resolved_type, args[0]);
            } else if (bid == B_None) {
                result = value_enum(&e->resolved_type, enum_obj_option_none());
            } else if (bid == B_is_ok) {
                result = call_is_ok(args[0]);
            } else if (bid == B_unwrap) {
                result = call_unwrap(args[0], e->line, e->col);
            } else if (bid == B_unwrap_err) {
                result = call_unwrap_err(args[0], e->line, e->col);
            } else if (bid == B_hash) {
                result = call_hash(args[0]);
            } else if (bid == B_compare) {
                /* Three arguments is the locale's collation order; two is the type's own. */
                result = argc >= 3
                             ? value_int(ziman_locale_collate(args[0], args[1], &args[2], e->line, e->col))
                             : value_int(value_compare(module, args[0], args[1], e->line, e->col));
            } else if (bid == B_error_message) {
                result = value_error_message(module, args[0], e->line, e->col);
            } else if (bid == B_channel_new) {
                int limit = 0;
                if (argc > 0) {
                    /* Both ends checked before the narrowing, not after. `(int)args[0].as.i` for a
                     * capacity of 2^32 is 0, and for 2^31 it is negative -- neither of which the
                     * `< 1` test above could see, because it ran on the 64-bit value. The same
                     * shape as the metrics handle in A40 (F132). */
                    if (args[0].as.i < 1) {
                        ziman_error("runtime", e->line, e->col, "channel_new: capacity must be at least 1, found %lld", (long long)args[0].as.i);
                    }
                    if (args[0].as.i > INT_MAX) {
                        ziman_error("runtime", e->line, e->col,
                                    "channel_new: capacity must be at most %d, found %lld", INT_MAX,
                                    (long long)args[0].as.i);
                    }
                    limit = (int)args[0].as.i;
                }
                result = value_channel(&e->resolved_type, channel_obj_new(limit));
            } else if (bid == B_send) {
                /* Deep-copied, not aliased -- see value_copy.h's own comment on why (one thread's
                 * memory must not depend on another's lifetime) and channel_send's own on
                 * needs_receive (what copies it back out again on the receiving side). */
                Value copy = value_copy_send(args[1]);
                if (channel_send(args[0].as.ch, copy, 1)) {
                    ziman_error("runtime", e->line, e->col, "send on a closed channel");
                }
                result = value_int(0);
            } else if (bid == B_try_send) {
                Value copy = value_copy_send(args[1]);
                int status = channel_try_send_status(args[0].as.ch, copy, 1);
                if (status == 2) {
                    ziman_error("runtime", e->line, e->col, "try_send on a closed channel");
                }
                result = value_bool(status == 0);
            } else if (bid == B_channel_close) {
                channel_close(args[0].as.ch);
                result = value_int(0);
            } else if (bid == B_channel_len) {
                result = value_int(channel_length(args[0].as.ch));
            } else if (bid == B_try_receive) {
                Value got;
                if (channel_try_receive(args[0].as.ch, &got)) {
                    result = value_enum(&e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(&e->resolved_type, enum_obj_option_none());
                }
            } else if (bid == B_receive) {
                Value got;
                if (!channel_receive(args[0].as.ch, &got)) {
                    ziman_error("runtime", e->line, e->col,
                                "receive on a closed, empty channel (use receive_option to handle a channel being closed)");
                }
                result = got;
            } else if (bid == B_receive_option) {
                Value got;
                if (channel_receive(args[0].as.ch, &got)) {
                    result = value_enum(&e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(&e->resolved_type, enum_obj_option_none());
                }
            } else if (bid == B_receive_timeout) {
                Value got;
                if (channel_receive_timeout(args[0].as.ch, args[1].as.i, &got)) {
                    result = value_enum(&e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(&e->resolved_type, enum_obj_option_none());
                }
            } else if (bid == B_select2) {
                Value got;
                int first;
                channel_select2(args[0].as.ch, args[1].as.ch, &first, &got);
                result = value_enum(&e->resolved_type, enum_obj_new("Selected", first ? "First" : "Second", 1, got));
            } else {
                FunctionDecl *fn = (FunctionDecl *)e->call_fn;
                result = call_function(module, fn, args, argc, e->as.call.variadic_tail_is_array_spread, e->line, e->col);
                /* A generic native fn's own C implementation has no way
                 * to know which concrete type(s) a given call
                 * instantiated it with -- NativeFn's signature carries
                 * only the already-evaluated argument Values, never the
                 * call's type arguments -- so a native function whose
                 * *return* type mentions one of its own type parameters
                 * (MutableArray<T>'s mutable_array_new<T>() -> MutableArray<T>, for
                 * instance) can only ever build a Value tagged with some
                 * placeholder type for the part that depends on T. Fixed
                 * up here instead, the same way channel_new()'s own
                 * (non-generic, but call-site-dependent) result already
                 * gets its type from e->resolved_type rather than
                 * whatever a builtin's own C code could know in
                 * isolation -- typecheck already computed the fully
                 * substituted, concrete return type for this exact call
                 * (e->resolved_type), so simply trusting that over
                 * whatever the native function happened to construct is
                 * both correct and cheap. Scoped to generic natives only
                 * -- e->resolved_type and a non-generic native's own
                 * hand-built .type already agree by construction, so
                 * this is a genuine no-op for the other ~20 native
                 * functions that existed before this. */
                /* A return type that is a bare type parameter (`thread`'s slot_get<T>) is left
                 * alone: generic bodies run unspecialised, so inside one e->resolved_type is
                 * still the placeholder `T`, while the Value such a native hands back (one it was
                 * given earlier, not built) already carries its true runtime type. */
                if (fn->is_native && fn->type_params.count > 0 && e->resolved_type.kind != TY_TYPEVAR) {
                    result.type = &e->resolved_type;
                }
            }
            /* Only a heap one is freed -- `free` is a no-op under the collector, but the plain build
             * has a real one, and handing it a stack address would be a crash rather than a leak. */
            if (args != inline_args) {
                free(args);
            }
            return result;
        }
        case EXPR_CAST: {
            Value v = eval_expr(module, env, e->as.cast.operand);
            if ((e->as.cast.target_type.kind == TY_BIGINT || e->as.cast.target_type.kind == TY_DECIMAL) && v.type->kind == TY_FLOAT &&
                (isnan(v.as.f) || isinf(v.as.f))) {
                ziman_error("runtime", e->line, e->col, "cannot convert %s to %s", isnan(v.as.f) ? "NaN" : "infinity",
                            e->as.cast.target_type.kind == TY_BIGINT ? "bigint" : "decimal");
            }
            TypeKind to = e->as.cast.target_type.kind;
            if (to == TY_FLOAT) {
                if (v.type->kind == TY_INT) return value_float((double)v.as.i);
                if (v.type->kind == TY_BIGINT) {
                    /* Lossy in the digits, as `as float` always is, but not in the magnitude. This
                     * went through a saturating int64 round trip until 2026-10-06, which reported
                     * 9.2e18 for a bigint of 1.2e29 -- the previous comment reasoned that a huge
                     * bigint cannot be represented exactly by a float anyway, which is true of its
                     * low digits and not of how big it is. bignum_to_double goes through the text,
                     * the same way decimal_to_double already did. */
                    return value_float(bignum_to_double(*v.as.bi));
                }
                if (v.type->kind == TY_DECIMAL) return value_float(decimal_to_double(*v.as.dec));
                return value_float(v.as.f);
            }
            if (to == TY_BIGINT) {
                if (v.type->kind == TY_INT) return value_bigint(bignum_from_i64(v.as.i));
                if (v.type->kind == TY_FLOAT) return value_bigint(decimal_to_bignum_trunc(decimal_from_double_trunc(v.as.f)));
                if (v.type->kind == TY_DECIMAL) return value_bigint(decimal_to_bignum_trunc(*v.as.dec));
                return value_bigint(*v.as.bi); /* bigint as bigint: no-op */
            }
            if (to == TY_DECIMAL) {
                if (v.type->kind == TY_INT) return value_decimal(decimal_from_bignum(bignum_from_i64(v.as.i)));
                if (v.type->kind == TY_FLOAT) return value_decimal(decimal_from_double_trunc(v.as.f));
                if (v.type->kind == TY_BIGINT) return value_decimal(decimal_from_bignum(*v.as.bi));
                return value_decimal(*v.as.dec); /* decimal as decimal: no-op */
            }
            if (to == TY_BYTE) {
                /* check_cast only permits int as byte (never
                 * float/bigint/decimal as byte), so v is always TY_INT
                 * here. Saturates to [0, 255] rather than wrapping. */
                int64_t iv = v.as.i;
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                return value_byte((int)iv);
            }
            /* target is TY_INT */
            if (v.type->kind == TY_FLOAT) return value_int(float_to_int_saturating(v.as.f));
            if (v.type->kind == TY_BIGINT) return value_int(bignum_to_i64_saturating(*v.as.bi));
            if (v.type->kind == TY_DECIMAL) return value_int(bignum_to_i64_saturating(decimal_to_bignum_trunc(*v.as.dec)));
            if (v.type->kind == TY_BYTE) return value_int((int64_t)v.as.by);
            return value_int(v.as.i);
        }
        case EXPR_ARRAY_LIT: {
            int count = e->as.array_lit.elements.count;
            Value *items = count > 0 ? ziman_xmalloc((size_t)count * sizeof(Value)) : NULL;
            gc_unsafe_enter();
            for (int i = 0; i < count; i++) {
                items[i] = eval_expr(module, env, e->as.array_lit.elements.items[i]);
            }
            gc_unsafe_leave();
            ArrayObj *packed = array_try_pack(items, count);
            if (packed != NULL) return value_array(&e->resolved_type, packed);
            return value_array(&e->resolved_type, array_obj_new(items, count));
        }
        case EXPR_INDEX: {
            Value arr = eval_expr(module, env, e->as.index.array);
            Value idx = eval_expr(module, env, e->as.index.index);
            int64_t i = idx.as.i;
            /* The two worlds the checker allows here, told apart by the value rather than by a
             * flag on the Expr: an array holds its elements, a MutableArray holds a handle to a
             * buffer. Reading a builder was a checker rewrite onto a `mutable_array_get` call
             * until F209, which is the only reason that name had to exist -- and the only reason a
             * traceback for `b[5]` carried a frame for a function nobody wrote. */
            if (arr.type != NULL && arr.type->kind == TY_STRUCT) {
                return ziman_mutable_array_index_get(arr, i, e->line, e->col);
            }
            if (i < 0 || i >= arr.as.arr->count) {
                ziman_error("runtime", e->line, e->col, "index %lld out of bounds (array length %d)", (long long)i, arr.as.arr->count);
            }
            return array_get(arr.as.arr, (int)i);
        }
        case EXPR_STRUCT_LIT: {
            StructDecl *decl = find_struct(module, e->as.struct_lit.type_name);
            int count = decl->fields.count;
            char **names = ziman_xmalloc((size_t)count * sizeof(char *));
            Value *fields = ziman_xmalloc((size_t)count * sizeof(Value));
            gc_unsafe_enter();
            for (int i = 0; i < count; i++) {
                names[i] = decl->fields.items[i].name; /* borrowed from the StructDecl, never freed */
                Expr *value_expr = NULL;
                for (int j = 0; j < e->as.struct_lit.fields.count; j++) {
                    if (strcmp(e->as.struct_lit.fields.items[j].name, names[i]) == 0) {
                        value_expr = e->as.struct_lit.fields.items[j].value;
                        break;
                    }
                }
                fields[i] = eval_expr(module, env, value_expr);
            }
            gc_unsafe_leave();
            return value_struct(&e->resolved_type, struct_obj_new(names, fields, count));
        }
        case EXPR_FIELD_ACCESS: {
            if (e->fn_ref != NULL) return value_function((FunctionDecl *)e->fn_ref);
            Value base = eval_expr(module, env, e->as.field_access.base);
            StructObj *st = base.as.st;
            for (int i = 0; i < st->count; i++) {
                if (strcmp(st->field_names[i], e->as.field_access.field_name) == 0) {
                    return st->fields[i];
                }
            }
            ziman_error("runtime", e->line, e->col, "internal error: field '%s' not found at runtime", e->as.field_access.field_name);
            return value_int(0);
        }
        case EXPR_WITH: {
            Value base = eval_expr(module, env, e->as.with_expr.base);
            StructObj *old = base.as.st;
            int count = old->count;
            Value *fields = ziman_xmalloc((size_t)count * sizeof(Value));
            memcpy(fields, old->fields, (size_t)count * sizeof(Value));
            /* Guarded from the memcpy onward, not just the override loop: every entry (memcpy'd or
             * about to be overridden) is equally unrooted until `fields` is fully handed off to
             * struct_obj_new below. */
            gc_unsafe_enter();
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) {
                FieldInit *fi = &e->as.with_expr.overrides.items[i];
                for (int j = 0; j < count; j++) {
                    if (strcmp(old->field_names[j], fi->name) == 0) {
                        fields[j] = eval_expr(module, env, fi->value);
                        break;
                    }
                }
            }
            gc_unsafe_leave();
            /* Reuses old->field_names directly (same struct type, same
             * field order) -- never mutates `old`, so sharing it is safe. */
            return value_struct(&e->resolved_type, struct_obj_new(old->field_names, fields, count));
        }
        case EXPR_ENUM_ACCESS:
            return eval_enum_access(module, env, e);
        case EXPR_VALUE_BLOCK: {
            ExecResult r = exec_stmt(module, env, e->as.value_block.inner);
            return r.value; /* a CTRL_YIELD: the type checker guarantees no arm leaves */
        }
        case EXPR_LAMBDA: {
            FunctionDecl *lam = e->lambda_decl;
            int n = e->lambda_ncaptured;
            Value *captured = n > 0 ? ziman_xmalloc((size_t)n * sizeof(Value)) : NULL;
            for (int i = 0; i < n; i++) captured[i] = env_lookup(env, lam->params.items[i].name)->value;
            return value_closure(lam, captured, n);
        }
        case EXPR_TRY_UNWRAP:
            /* Unreachable in a well-typed program: check_expr's own
             * EXPR_TRY_UNWRAP case (typecheck.c) already rejects any
             * `?` found outside the two statement forms that intercept
             * it directly, before ever calling eval_expr on it (see
             * exec_stmt's own STMT_VAL/STMT_VAR/STMT_EXPR cases below). */
            ziman_error("runtime", e->line, e->col, "internal error: 'try' reached eval_expr directly");
            return value_int(0);
    }
    ziman_error("runtime", e->line, e->col, "internal error: unhandled expression kind");
    return value_int(0);
}

static ExecResult no_ctrl(void) {
    ExecResult r = {CTRL_NONE, value_int(0)};
    return r;
}

/* Evaluates a `?`-suffixed expression's own operand exactly once,
 * deciding between "continue with the unwrapped value" and "propagate
 * an early return" -- shared by STMT_VAL/STMT_VAR (which bind the
 * unwrapped value to a name) and STMT_EXPR (a bare `expr?`, which
 * discards it). `e` is always exactly an EXPR_TRY_UNWRAP node;
 * check_try_unwrap (typecheck.c) already proved `e->as.try_unwrap.inner`'s
 * type is genuinely Option<T> or Result<T, E>, and that the enclosing
 * function's own return type is compatible, so no re-validation happens
 * here -- only re-deriving, cheaply, which of the two `inner` turned out
 * to be from the runtime value itself (a Result's ResultObj vs. an
 * Option's EnumObj), the same way is_ok/Option.is_Some already do.
 * Returns 1 and sets `*unwrapped` on the continue path (`Ok`/`Some`);
 * returns 0 and sets `*propagated` (always CTRL_RETURN, built fresh
 * from `e->as.try_unwrap.propagated_type` -- the enclosing function's
 * own concrete return type, cached at type-check time since nothing
 * here has any other way to learn it) on the propagate path
 * (`Err`/`None`). */
static int eval_try_unwrap(Module *module, Env *env, Expr *e, Value *unwrapped, ExecResult *propagated) {
    Value v = eval_expr(module, env, e->as.try_unwrap.inner);
    Type propagated_type = e->as.try_unwrap.propagated_type;
    if (v.type->kind == TY_RESULT) {
        if (v.as.res->is_ok) {
            *unwrapped = v.as.res->value;
            return 1;
        }
        propagated->kind = CTRL_RETURN;
        Value failure = v.as.res->value;
        if (e->as.try_unwrap.convert_variant != NULL) {
            failure = value_enum(type_ref(*propagated_type.elem_err), enum_obj_new(propagated_type.elem_err->enum_name, e->as.try_unwrap.convert_variant, 1, failure));
        }
        propagated->value = value_result(type_ref(propagated_type), result_obj_new(0, failure));
        return 0;
    }
    /* Option<T> -- the only other type check_try_unwrap ever allows. */
    if (strcmp(v.as.en->variant_name, "Some") == 0) {
        *unwrapped = v.as.en->payload;
        return 1;
    }
    propagated->kind = CTRL_RETURN;
    propagated->value = value_enum(type_ref(propagated_type), enum_obj_option_none());
    return 0;
}

/* Phase 3's collection trigger (docs/MEMORY.md, "one heap per thread") -- narrowly scoped for now,
 * deliberately: only `value_string_copy` (src/value.c) allocates through the calling thread's own
 * `GcHeap` today, so this heap grows slowly and a real collection is a rare event in most programs,
 * exactly the "narrowest possible slice" this was built as. `extra_roots` for `gc_heap_collect` is
 * this thread's own state (src/gc_interp_roots.c's `gc_mark_interpreter_roots` needs
 * `g_global_env`/`t_defer_envs`/`t_env_pool`, all private to this file) -- a thin wrapper capturing
 * exactly that, nothing more. */
static void mark_this_threads_roots(const Arena *a, void *ctx) {
    (void)ctx;
    gc_mark_interpreter_roots(a, g_global_env, t_defer_envs, t_defer_env_count, t_env_pool, t_env_pool_count);
    /* See spawn_thread_entry's own comment on t_pending_handler: a spawned thread's pending
     * on_error handler, for as long as it might still be needed, is reachable through nothing else
     * this function already walks. */
    if (t_has_pending_handler) gc_mark_value(a, t_pending_handler);
    /* See t_active_iters's own comment (just above defer_env_untrack): every `for ... in <iter>`
     * loop currently running on this thread, innermost last. */
    for (int i = 0; i < t_active_iter_count; i++) gc_mark_value(a, t_active_iters[i]);
    /* See gc_guard_roots.h's own comment: every still-live Guard this thread created, protecting
     * its own `message` for as long as Boehm might still get around to finalizing it, independent
     * of whether any Ziman-visible value still points at the guard at all. */
    gc_mark_live_guards(ziman_thread_heap(), a);
}

/* The Boehm-visible counterpart of mark_this_threads_roots just above -- same four root sources
 * (g_global_env plus this thread's own t_defer_envs/t_env_pool/t_active_iters/t_pending_handler),
 * handed back as raw address pairs for gc_boehm_roots.c to re-read (through
 * ziman_interp_root_ranges, at collection time, from whichever thread is doing the collecting)
 * rather than walked here directly -- see gc_boehm_roots.h's own header comment for why a second,
 * independent path to the identical state exists at all: Boehm's own `GC_gcollect` never calls
 * this module's own `gc_heap_collect`/`mark_this_threads_roots`, so without this, nothing ever
 * told Boehm about any of it. */
ZimanInterpRootAddrs ziman_interp_root_addrs(void) {
    ZimanInterpRootAddrs addrs;
    addrs.defer_envs_ptr_addr = (void **)&t_defer_envs;
    addrs.defer_env_count_addr = &t_defer_env_count;
    addrs.env_pool_ptr_addr = (void **)&t_env_pool;
    addrs.env_pool_count_addr = &t_env_pool_count;
    addrs.active_iters_ptr_addr = (void **)&t_active_iters;
    addrs.active_iter_count_addr = &t_active_iter_count;
    addrs.pending_handler_addr = &t_pending_handler;
    addrs.has_pending_handler_addr = &t_has_pending_handler;
    return addrs;
}

int ziman_interp_root_ranges(ZimanInterpRootAddrs addrs, ZimanMemRange *out_ranges, int max_ranges) {
    int n = 0;
    Env **defer_envs = *(Env ***)addrs.defer_envs_ptr_addr;
    int defer_env_count = *addrs.defer_env_count_addr;
    if (defer_envs != NULL && defer_env_count > 0 && n < max_ranges) {
        out_ranges[n].start = defer_envs;
        out_ranges[n].end = defer_envs + defer_env_count;
        n++;
    }
    Env **env_pool = *(Env ***)addrs.env_pool_ptr_addr;
    int env_pool_count = *addrs.env_pool_count_addr;
    if (env_pool != NULL && env_pool_count > 0 && n < max_ranges) {
        out_ranges[n].start = env_pool;
        out_ranges[n].end = env_pool + env_pool_count;
        n++;
    }
    Value *active_iters = *(Value **)addrs.active_iters_ptr_addr;
    int active_iter_count = *addrs.active_iter_count_addr;
    if (active_iters != NULL && active_iter_count > 0 && n < max_ranges) {
        out_ranges[n].start = active_iters;
        out_ranges[n].end = active_iters + active_iter_count;
        n++;
    }
    if (*addrs.has_pending_handler_addr && n < max_ranges) {
        out_ranges[n].start = addrs.pending_handler_addr;
        out_ranges[n].end = (Value *)addrs.pending_handler_addr + 1;
        n++;
    }
    return n;
}

void **ziman_global_env_addr(void) {
    return (void **)&g_global_env;
}

/* How much this thread's own heap must grow since the last collection before another one runs:
 * whichever is larger of a small floor and a fraction of what the heap already holds.
 *
 * The floor alone is what this used to be -- a flat 64KB, with its own comment admitting it was
 * "an arbitrary, unmeasured starting point ... not to be a considered performance choice". It makes
 * total collection work **quadratic in the size of a growing heap**: the number of collections is
 * heap/64KB, and each one marks everything currently live, so a program that builds one large
 * structure pays O(n) marking O(n/64KB) times. Measured on 25k/50k/100k inserts into a
 * `MutableMap`: 0.03s, 0.18s, 1.25s -- six to seven times the work for each doubling of the input,
 * which is not the map's doing (its insert is a HAMT, O(log32 n)) but the collector's. The same
 * benchmark in CPython runs in 0.06s, and the gap had nothing to do with being a tree-walker.
 *
 * Scaling the threshold with the live heap is the standard fix and makes the total amortised: each
 * collection buys a proportional amount of new allocation before the next, so the number of
 * collections over a run that grows to n is O(log n) rather than O(n), and the work is O(n).
 * Measured after: 0.01s, 0.03s, 0.08s -- linear, and 15x faster at 100k. The floor still matters
 * for small heaps, where a fraction of "almost nothing" would collect far too eagerly; the
 * denominator is deliberately generous (collect once the heap has grown by half again) because a
 * collection here walks every root source this interpreter has, and the cost of collecting too
 * often is exactly what this constant got wrong before. */
#define ZIMAN_GC_TRIGGER_FLOOR ((size_t)(64 * 1024))
#define ZIMAN_GC_TRIGGER_DIVISOR ((size_t)2)
static __thread size_t t_reserved_at_last_collect = 0;

static size_t gc_growth_before_next_collect(size_t reserved_now) {
    size_t proportional = reserved_now / ZIMAN_GC_TRIGGER_DIVISOR;
    return proportional > ZIMAN_GC_TRIGGER_FLOOR ? proportional : ZIMAN_GC_TRIGGER_FLOOR;
}

/* Called from exec_stmt's own entry, before that statement's own evaluation has begun anything --
 * but that alone is not sufficient, which is why the t_gc_unsafe_depth check below exists: a
 * *nested* call's own exec_stmt entry is also "the top of a statement," yet an outer, not-yet-
 * finished expression (an EXPR_CALL's own `args` array, still being built one element at a time)
 * can have earlier elements sitting completely unrooted at that exact moment. Skipping entirely
 * whenever any such buffer is in progress anywhere on this thread's call stack -- see
 * gc_unsafe_enter/leave's own comment -- is what makes this safe; "exec_stmt's own entry" alone
 * was not (see docs/DESIGN.md's own entry on the bug this fixes). */
static void maybe_collect(void) {
    if (t_gc_unsafe_depth > 0) return;
    size_t reserved = ziman_thread_heap_reserved();
    if (reserved - t_reserved_at_last_collect < gc_growth_before_next_collect(t_reserved_at_last_collect)) return;
    gc_heap_collect(ziman_thread_heap(), mark_this_threads_roots, NULL);
    t_reserved_at_last_collect = ziman_thread_heap_reserved();
}

void ziman_force_collect(void) {
    if (t_gc_unsafe_depth > 0) return;
    gc_heap_collect(ziman_thread_heap(), mark_this_threads_roots, NULL);
    t_reserved_at_last_collect = ziman_thread_heap_reserved();
}

/* Runs `block`'s statements directly in `scope` (no further scope nesting) --
 * shared by exec_block (which makes its own fresh scope first) and STMT_FOR
 * (which needs a fresh scope per iteration holding that iteration's loop
 * binder(s), ahead of the body's own statements).
 *
 * Also the one place that updates the current frame's own recorded Env (error.c's frame tracking,
 * ziman_set_frame_env) for the duration of this nested scope, restoring it before returning --
 * found necessary the hard way (docs/MEMORY.md, docs/DESIGN.md's own entry on the incident): every
 * call to this function is handed a *child* of whatever Env the frame had recorded, but
 * ziman_set_frame_env is otherwise only ever called once, by call_function, for a call's own
 * top-level scope. Without this, gc_mark_env_chain's root walk -- which only ever walks a
 * recorded Env and its ->parent chain *upward* -- could never reach a variable declared inside any
 * loop or block body at all, since Env has no way to be walked *downward* from a shallower scope
 * to a deeper one. A struct or array bound entirely inside a `for` loop's own body (a completely
 * ordinary shape, not a corner case) was therefore invisible to every real collection cycle from
 * the moment this frame tracking was first wired, regardless of which allocation site produced it
 * -- not found until a real fixture kept one alive, in a nested scope, long enough under real
 * allocation pressure for a collection to actually hit the gap. */
static ExecResult exec_stmts_in_scope(Module *module, Env *scope, Stmt *block) {
    void *saved_env = ziman_frame_env_at(ziman_frame_count() - 1);
    ziman_set_frame_env(scope);
    ExecResult result = no_ctrl();
    for (int i = 0; i < block->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(module, scope, block->as.block.stmts.items[i]);
    }
    run_defers(module, scope);
    ziman_set_frame_env(saved_env);
    return result;
}

/* Was a bare env_new(parent)/env_free(scope) pair -- the one scope allocation left in the whole
 * interpreter that wasn't already reused across executions (call_function's own top-level scope has
 * env_pool_acquire/release; every loop body allocates one scope for its *whole* run, env_reset per
 * iteration -- see STMT_WHILE/STMT_FOR's own comments). Every `if`/`else` body and bare `{...}`
 * block runs through here, so under a real workload this was the dominant source of ongoing Boehm
 * GC_MALLOC/GC_FREE churn, not calls or loops. env_pool_acquire/release is already fully generic
 * (it re-parents on acquire and only assumes defer_count is already 0 on release, true here for the
 * identical reason it's true for call_function: exec_stmts_in_scope's own run_defers call, which
 * every path through here already goes through) -- no new mechanism needed, just extending the one
 * call_function already proved out to its last remaining gap. */
static ExecResult exec_block(Module *module, Env *parent, Stmt *block) {
    Env *scope = env_pool_acquire(parent);
    ExecResult result = exec_stmts_in_scope(module, scope, block);
    env_pool_release(scope);
    return result;
}

static ExecResult exec_stmt(Module *module, Env *env, Stmt *s) {
    ZIMAN_STACK_CHECK();
    maybe_collect();
    /* `--coverage` (src/coverage.h): one increment behind a flag that is false for every ordinary
     * run, so the cost is a branch on a global that the predictor gets right every time. Counted
     * here rather than per line because this is the one place every statement passes through, and
     * because it makes the report's denominator exact -- coverage.c reads these counts back off the
     * same AST it walks to find which lines are executable at all. The flag is read as a global
     * rather than through an accessor on purpose: see coverage.h. */
    if (ziman_coverage_on) s->exec_count++;
    switch (s->kind) {
        case STMT_VAL:
        case STMT_VAR: {
            if (s->as.decl.init->kind == EXPR_TRY_UNWRAP) {
                Value unwrapped;
                ExecResult propagated;
                if (!eval_try_unwrap(module, env, s->as.decl.init, &unwrapped, &propagated)) {
                    return propagated;
                }
                Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : *unwrapped.type;
                env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, unwrapped);
                return no_ctrl();
            }
            Value v = eval_expr(module, env, s->as.decl.init);
            Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : *v.type;
            env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, v);
            return no_ctrl();
        }
        /* **A MutableArray's element write, and only that.** An array is a value, so `a[i] = v`
         * is a rebinding and check_stmt rewrites it into one; a MutableArray is a container, so
         * the write goes through the handle and the name is untouched. That half was a rewrite
         * onto a `mutable_array_set` call until F209, when both element accesses stopped needing
         * a function name at all -- so this case does the work instead of reporting that it was
         * reached. Anything else is still a checker bug, which is what the else is for. */
        case STMT_INDEX_ASSIGN: {
            Expr *target = s->as.index_assign.target;
            Value base = eval_expr(module, env, target->as.index.array);
            if (base.type == NULL || base.type->kind != TY_STRUCT) {
                ziman_error("internal", s->line, s->col, "an index assignment reached the interpreter unrewritten");
            }
            Value idx = eval_expr(module, env, target->as.index.index);
            Value value = eval_expr(module, env, s->as.index_assign.value);
            ziman_mutable_array_index_set(base, idx.as.i, value, s->line, s->col);
            return no_ctrl();
        }
        case STMT_ASSIGN: {
            Binding *b = s->resolved_depth >= 0 ? env_at_slot(env, s->resolved_depth, s->resolved_slot)
                                                 : env_lookup(env, s->as.assign.name);
            b->value = eval_expr(module, env, s->as.assign.value);
            return no_ctrl();
        }
        case STMT_IF: {
            Value cond = eval_expr(module, env, s->as.if_stmt.cond);
            if (cond.as.b) {
                return exec_block(module, env, s->as.if_stmt.then_block);
            }
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    return exec_stmt(module, env, s->as.if_stmt.else_block);
                }
                return exec_block(module, env, s->as.if_stmt.else_block);
            }
            return no_ctrl();
        }
        case STMT_WHILE: {
            /* One scope for the whole loop, not one per iteration: `env_reset` clears last
             * iteration's bindings without freeing the scope itself or its `items` buffer, so after
             * the first iteration a loop with a stable declaration count allocates nothing at all for
             * its own bookkeeping. This was most of a tight loop's cost -- see docs/DESIGN.md's
             * interpreter-speed entry.
             *
             * And the scope itself comes from the pool, as `exec_block` and `call_function` already
             * took theirs: one allocation per loop *entry* is still 234K calloc'd Envs and 209K
             * reallocs of their `items` across a 2,000-request workload, which was the largest
             * remaining source of allocated bytes once the three sites in DESIGN.md's "Three
             * allocation sites" entry were gone. A pooled Env arrives with its `items` buffer already
             * grown, so the reallocs go with the callocs. `env_pool_release` assumes `defer_count` is
             * already 0, which holds here for exactly the reason it holds for `exec_block`: every path
             * out of the body goes through `exec_stmts_in_scope`, which runs the defers. */
            Env *scope = env_pool_acquire(env);
            for (;;) {
                Value cond = eval_expr(module, env, s->as.while_stmt.cond);
                if (!cond.as.b) break;
                env_reset(scope);
                ExecResult result = exec_stmts_in_scope(module, scope, s->as.while_stmt.body);
                if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                if (result.kind == CTRL_BREAK) break;
                /* CTRL_CONTINUE and CTRL_NONE both just fall through to the
                 * next iteration's condition check. */
            }
            env_pool_release(scope);
            return no_ctrl();
        }
        case STMT_FOR: {
            if (s->as.for_stmt.is_range) {
                Value start = eval_expr(module, env, s->as.for_stmt.range_start);
                Value end = eval_expr(module, env, s->as.for_stmt.range_end);
                Env *scope = env_pool_acquire(env);
                for (int64_t i = start.as.i; i < end.as.i; i++) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_pool_release(scope);
                return no_ctrl();
            }

            Value iterable = eval_expr(module, env, s->as.for_stmt.iterable);
            if (iterable.type->kind == TY_STRUCT && iterable.type->struct_name != NULL && strcmp(iterable.type->struct_name, "Iter") == 0) {
                /* a lazy source: ask for one item at a time, and release it however the loop is left */
                int64_t handle = iter_handle_of(iterable);
                Type item_type = s->as.for_stmt.iterable->resolved_type.type_args.items[0];
                Value item;
                Env *scope = env_pool_acquire(env);
                /* See t_active_iters's own comment just above: `iterable` needs this push for the
                 * entire loop, popped on every exit path (both below and the two loop-internal
                 * returns right here). */
                active_iter_push(iterable);
                while (iter_advance(handle, &item, s->line, s->col)) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, item_type, 0, item);
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) {
                        env_pool_release(scope);
                        iter_close_handle(handle);
                        active_iter_pop();
                        return result;
                    }
                    if (result.kind == CTRL_BREAK) {
                        iter_close_handle(handle);
                        break;
                    }
                }
                active_iter_pop();
                env_pool_release(scope);
                return no_ctrl();
            }
            if (iterable.type->kind == TY_STRUCT && iterable.type->struct_name != NULL &&
                strcmp(iterable.type->struct_name, "MutableArray") == 0) {
                /* The live buffer, by index, re-reading its length every pass: a body that appends sees its
                 * own additions (as it would in the array this is becoming) and one that clears ends the
                 * loop, and neither can read past the end. Nothing is copied, which is the whole reason a
                 * builder is iterable at all -- snapshot copies all of it, per call. */
                Type elem_type = s->as.for_stmt.iterable->resolved_type.type_args.items[0];
                Env *scope = env_pool_acquire(env);
                for (int i = 0; i < ziman_array_builder_length(iterable); i++) {
                    env_reset(scope);
                    if (s->as.for_stmt.binder2 == NULL) {
                        env_define(scope, s->as.for_stmt.binder1, elem_type, 0, ziman_array_builder_at(iterable, i));
                    } else {
                        env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                        env_define(scope, s->as.for_stmt.binder2, elem_type, 0, ziman_array_builder_at(iterable, i));
                    }
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_pool_release(scope);
                return no_ctrl();
            }
            if (iterable.type->kind == TY_STRUCT && iterable.type->struct_name != NULL &&
                (strcmp(iterable.type->struct_name, "MutableMap") == 0 ||
                 strcmp(iterable.type->struct_name, "MutableSet") == 0)) {
                /* A snapshot of the entries, in insertion order -- deliberately unlike MutableArray
                 * just above, which walks its live buffer. A hashed handle has no cursor that could
                 * walk it in insertion order (that order lives in a per-entry sequence number the
                 * entries have to be sorted by) and a node's slots shift on insert, so this is a
                 * snapshot by construction. The body may therefore change the handle freely without
                 * affecting what the loop visits. */
                int is_map = strcmp(iterable.type->struct_name, "MutableMap") == 0;
                Value *keys, *vals = NULL;
                int count = ziman_mutable_map_entries(iterable, &keys, is_map ? &vals : NULL);
                const TypeList *targs = &s->as.for_stmt.iterable->resolved_type.type_args;
                Type key_type = targs->items[0];
                Env *scope = env_pool_acquire(env);
                for (int i = 0; i < count; i++) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, key_type, 0, keys[i]);
                    if (s->as.for_stmt.binder2 != NULL) {
                        env_define(scope, s->as.for_stmt.binder2, targs->items[1], 0, vals[i]);
                    }
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_pool_release(scope);
                return no_ctrl();
            }
            if (iterable.type->kind == TY_MAP || iterable.type->kind == TY_SET) {
                /* iterates a snapshot in insertion order, so the body may rebind the collection freely */
                int is_map = iterable.type->kind == TY_MAP;
                Value *keys, *vals = NULL;
                mapobj_entries(iterable.as.map, &keys, is_map ? &vals : NULL);
                Type key_type = *iterable.type->elem;
                Env *scope = env_pool_acquire(env);
                for (int i = 0; i < iterable.as.map->count; i++) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, key_type, 0, keys[i]);
                    if (s->as.for_stmt.binder2 != NULL) env_define(scope, s->as.for_stmt.binder2, *iterable.type->elem_err, 0, vals[i]);
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_pool_release(scope);
                return no_ctrl();
            }
            ArrayObj *arr = iterable.as.arr;
            Type elem_type = *iterable.type->elem;
            Env *scope = env_pool_acquire(env);
            for (int i = 0; i < arr->count; i++) {
                env_reset(scope);
                if (s->as.for_stmt.binder2 == NULL) {
                    env_define(scope, s->as.for_stmt.binder1, elem_type, 0, array_get(arr, i));
                } else {
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    env_define(scope, s->as.for_stmt.binder2, elem_type, 0, array_get(arr, i));
                }
                ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                if (result.kind == CTRL_RETURN) { env_pool_release(scope); return result; }
                if (result.kind == CTRL_BREAK) break;
            }
            env_pool_release(scope);
            return no_ctrl();
        }
        case STMT_BREAK: {
            ExecResult r = {CTRL_BREAK, value_int(0)};
            return r;
        }
        case STMT_CONTINUE: {
            ExecResult r = {CTRL_CONTINUE, value_int(0)};
            return r;
        }
        case STMT_SPAWN: {
            /* args/handler are evaluated here, on the calling thread, then deep-copied before the
             * new thread ever sees them -- the same crossing-point fix as `send`'s own deep copy
             * just above, and for the identical reason (value_copy.h): one thread's own memory must
             * not depend on another's lifetime. spawn_thread_entry re-copies each of them via
             * value_copy_receive into its own allocators before anything else runs.
             *
             * Those copies are reachable only through `sa` until the new thread has it, which is
             * safe without any rooting of its own: `sa` is itself `malloc`'d (GC_MALLOC), it is held
             * in this frame until ziman_spawn_thread returns, and Boehm's own pthread_create wrapper
             * keeps its `start_info` -- which holds `sa` -- on *this* thread's stack until the child
             * has registered and taken it ("we hold onto the stack-allocated si until the child is
             * done with it", vendor/bdwgc/pthread_support.c). So there is no window in which the
             * only reference to a crossing copy is invisible to the collector. */
            /* The checker's own resolution, stamped on the statement -- see ast.h's `spawn_fn`.
             * Re-resolving here answered the first declaration of the name, which for one a
             * program shadows is the bodyless `builtin fn`, so the thread ran nothing. */
            FunctionDecl *fn = s->as.spawn_stmt.spawn_fn != NULL
                                   ? (FunctionDecl *)s->as.spawn_stmt.spawn_fn
                                   : find_function(module, s->as.spawn_stmt.callee);
            int argc = s->as.spawn_stmt.args.count;
            Value *args = argc > 0 ? ziman_xmalloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = value_copy_send(eval_expr(module, env, s->as.spawn_stmt.args.items[i]));
            }
            SpawnArgs *sa = ziman_xmalloc(sizeof(SpawnArgs));
            sa->module = module;
            sa->fn = fn;
            sa->args = args;
            sa->argc = argc;
            sa->line = s->line;
            sa->col = s->col;
            sa->has_handler = s->as.spawn_stmt.on_error != NULL;
            if (sa->has_handler) sa->handler = value_copy_send(eval_expr(module, env, s->as.spawn_stmt.on_error));
            /* Counted before the thread exists, not from inside it: a thread counted only once it
             * starts running is live-but-invisible for a moment, and a reader already blocked on a
             * channel could declare a deadlock while this one was still starting up. Undone
             * immediately if the thread cannot be created at all. */
            ziman_channel_thread_started();
            if (ziman_spawn_thread(spawn_thread_entry, sa) != 0) {
                ziman_channel_thread_ended();
                ziman_error("runtime", s->line, s->col, "cannot start a thread (%s)", strerror(errno));
            }
            return no_ctrl();
        }
        case STMT_RETURN: {
            ExecResult r;
            r.kind = CTRL_RETURN;
            r.value = s->as.ret.value != NULL ? eval_expr(module, env, s->as.ret.value) : value_int(0);
            return r;
        }
        case STMT_EXPR: {
            if (s->as.expr_stmt.expr->kind == EXPR_TRY_UNWRAP) {
                Value unwrapped;
                ExecResult propagated;
                if (!eval_try_unwrap(module, env, s->as.expr_stmt.expr, &unwrapped, &propagated)) {
                    return propagated;
                }
                return no_ctrl();
            }
            eval_expr(module, env, s->as.expr_stmt.expr);
            return no_ctrl();
        }
        case STMT_SWITCH:
            ziman_error("runtime", s->line, s->col, "internal error: an un-desugared switch reached the interpreter");
            return no_ctrl();
        case STMT_DEFER: {
            /* Evaluate the arguments now, into the hidden variables the
             * replay call reads, then register the replay for block exit. */
            Expr *call = s->as.defer_stmt.call;
            for (int i = 0; i < s->as.defer_stmt.hidden_count; i++) {
                Expr *arg = call->as.call.args.items[i];
                env_define(env, s->as.defer_stmt.hidden[i], arg->resolved_type, 0, eval_expr(module, env, arg));
            }
            if (env->defer_count == env->defer_capacity) {
                env->defer_capacity = env->defer_capacity == 0 ? 4 : env->defer_capacity * 2;
                env->defers = ziman_xrealloc(env->defers, sizeof(Expr *) * (size_t)env->defer_capacity);
            }
            env->defers[env->defer_count++] = s->as.defer_stmt.replay;
            if (!env->defers_tracked) defer_env_track(env);
            return no_ctrl();
        }
        case STMT_BLOCK:
            return exec_block(module, env, s);
        case STMT_YIELD: {
            ExecResult y = {CTRL_YIELD, value_int(0)};
            Expr *e = s->as.yield_stmt.value;
            if (e->kind == EXPR_TRY_UNWRAP) {
                ExecResult propagated;
                if (!eval_try_unwrap(module, env, e, &y.value, &propagated)) return propagated;
                return y;
            }
            y.value = eval_expr(module, env, e);
            return y;
        }
        case STMT_VALUE_BLOCK: {
            ExecResult r = exec_stmt(module, env, s->as.value_block.inner);
            if (r.kind != CTRL_YIELD) return r; /* an arm that leaves (return/break/continue) */
            Value v = r.value;
            switch (s->as.value_block.target) {
                case STMT_VAL:
                case STMT_VAR: {
                    Type t = s->as.value_block.has_declared_type ? s->as.value_block.declared_type : *v.type;
                    env_define(env, s->as.value_block.name, t, s->as.value_block.target == STMT_VAR, v);
                    return no_ctrl();
                }
                case STMT_ASSIGN:
                    (s->resolved_depth >= 0 ? env_at_slot(env, s->resolved_depth, s->resolved_slot)
                                             : env_lookup(env, s->as.value_block.name))->value = v;
                    return no_ctrl();
                default: {
                    ExecResult ret = {CTRL_RETURN, v};
                    return ret;
                }
            }
        }
    }
    return no_ctrl();
}

/* Evaluates every module-level `val` in file order (see typecheck.c's
 * check_global_vals for why order matters and why that's a deliberate
 * scope cut), adding to g_global_env (already created by `interpret`,
 * shared with init_param_defaults) as it goes by reusing exec_stmt's
 * own STMT_VAL case wholesale (via a synthetic Stmt) rather than
 * re-deriving its declared-vs-inferred type logic here. Must run before
 * `main` -- typecheck() already proved every initializer well-typed, so
 * nothing here can fail the way check_global_vals's synthetic check
 * could. Runs after init_param_defaults (see that function's own
 * comment for why) -- a global val's own initializer calling a function
 * that relies on one of its own omitted, defaulted parameters
 * (`datetime_format_of` calling `mutable_array_new<T>()`, say) needs that default
 * to already exist; not the other way around. */
static void init_global_vals(Module *module) {
    for (int i = 0; i < module->vals.count; i++) {
        GlobalValDecl *v = module->vals.items[i];
        Stmt synthetic = {0};
        synthetic.kind = STMT_VAL;
        synthetic.line = v->line;
        synthetic.col = v->col;
        synthetic.as.decl.name = v->name;
        synthetic.as.decl.declared_type = v->type;
        synthetic.as.decl.has_declared_type = v->has_declared_type;
        synthetic.as.decl.init = v->init;
        exec_stmt(module, g_global_env, &synthetic);
    }
}

/* Evaluates every parameter's own default expression exactly once,
 * before `main` runs -- the same timing a global val's own initializer
 * gets, and for the same reason (see ast.h's Param.default_key): a
 * default can't reference this function's other parameters, only
 * things already in scope at program start, so re-evaluating it fresh
 * on every omitted call would produce the exact same value every time
 * anyway, just wastefully.
 *
 * Runs *before* init_global_vals, not after -- a real crash (a NULL
 * env_lookup, dereferenced) surfaced the instant a global val's own
 * initializer called a function relying on one of its own omitted
 * parameter defaults: `pub val iso_date_format =
 * datetime_format_of("yyyy-MM-dd")` calls `mutable_array_new<T>()` internally with
 * both parameters omitted, and if init_global_vals ran first, that
 * default_key wouldn't exist in g_global_env yet when this exact call
 * needed it. A default expression could, in principle, also reference
 * an actual global val -- the reverse dependency, unaddressed either
 * way, and not yet hit by anything real in this codebase (every
 * existing default is a self-contained literal, `20`, `2.0`, ...). This
 * ordering fixes the case that actually happened; a genuine dependency
 * cycle between the two would need a real fix (evaluating both lazily,
 * on first reference, rather than two fixed eager passes) if one is
 * ever hit. */
static void init_param_defaults(Module *module) {
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        for (int j = 0; j < fn->params.count; j++) {
            Param *param = &fn->params.items[j];
            if (param->default_expr == NULL) {
                continue;
            }
            Value default_value = eval_expr(module, g_global_env, param->default_expr);
            env_define(g_global_env, param->default_key, param->type, 0, default_value);
        }
    }
}

static Module *g_program = NULL;

/* The choke point every native function that calls back into a Ziman closure goes through
 * (prelude.c's array_map/filter/flat_map/sort_positions's own comparator, iter.c's own callback
 * sites, ...) -- guarded the same way and for the same reason as call_closure_value just above:
 * `args` belongs to the calling native's own parameters or a temp buffer it's still building, with
 * no Env of its own to root it in for as long as this call is on the stack. */
/* `compare`'s answer, for a native. `value_compare` needs a Module to reach a struct/enum's own
 * `<type>_compare` function, and a native is handed only its arguments -- the same gap
 * `native_call_closure` below fills for calling a Ziman function, and filled the same way, from
 * the running program. Exported when `min`/`max` stopped being dispatched builtins (2026-10-22):
 * their whole content is one comparison per candidate, and the comparison is the part that has to
 * see the program. */
int native_value_compare(Value a, Value b, int line, int col) {
    return value_compare(g_program, a, b, line, col);
}

Value native_call_closure(Value fn, Value *args, int argc, int line, int col) {
    ClosureObj *clo = fn.as.clo;
    gc_unsafe_enter();
    Value result;
    if (clo->ncaptured == 0) {
        result = call_function(g_program, clo->fn, args, argc, 0, line, col);
    } else {
        Value *all = ziman_xmalloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
        memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
        if (argc > 0) memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value)); /* see the same guard in eval_expr */
        result = call_function(g_program, clo->fn, all, clo->ncaptured + argc, 0, line, col);
        free(all);
    }
    gc_unsafe_leave();
    return result;
}

void interpret(Module *module) {
    g_program = module;
    g_global_env = env_new(NULL);
    /* **Module initialisation gets a frame of its own**, where it had none: nothing has been called
     * yet, so `t_frame_count` was 0 and every nested scope opened by a global initialiser ran
     * without one. Two separate things needed it, and only the first was visible.
     *
     * `exec_stmts_in_scope` reads the current frame's Env before replacing it, as
     * `ziman_frame_env_at(ziman_frame_count() - 1)` -- which is `t_frames[-1]` with no frame, and
     * segfaulted on any module-level `val` whose initialiser was an `if`, a `switch` or a block
     * used as a value.
     *
     * The quiet half is the GC. `ziman_set_frame_env` is what makes a *nested* scope reachable from
     * a root walk at all, since `gc_mark_env_chain` only ever walks an Env upward through its
     * parents, and it is a no-op with no frame to record into. So a value allocated inside a global
     * initialiser's own block -- the builder in `val x = { val out = mutable_array_new<string>()
     * .. }` -- was not a root, and a collection during initialisation could have taken it. Fixing
     * only the read would have turned a crash into that. */
    ziman_push_frame("<module initialisation>", NULL, 0, 0);
    ziman_set_frame_env(g_global_env);
    init_param_defaults(module);
    init_global_vals(module);
    ziman_pop_frame();
    FunctionDecl *main_fn = find_function(module, "main");
    call_function(module, main_fn, NULL, 0, 0, main_fn->line, main_fn->col);
}

/* One #[test] invocation, already-evaluated `args`/`argc`, inside its own
 * ThreadBoundary -- the same per-thread runtime-error isolation `spawn`
 * already uses (see spawn_thread_entry above), so a runtime error (an
 * assertion failure included -- test.assert* is built on `unwrap`, see
 * lib/core/test.zm) fails only this one test, not the whole `ziman test`
 * run. A `Result<unit, E>` return that comes back `Err` (no runtime error
 * raised) is also a failure, its message the same value_display every other
 * fatal-error message already uses. Prints "PASS <label>" or
 * "FAIL <label>  message" (deliberately no elapsed time -- this project's
 * own tests/lang harness checks stdout byte-for-byte, and a duration
 * would make every fixture for this feature flaky) and returns 1 on
 * failure, 0 on pass. */
/* A failure raised by the bundled `test` module's assertions, rewritten to name the line that
 * asserted. The raw message's own location is test.zm's `unwrap` -- a file the reader never wrote,
 * reported on every failing assertion, and the first thing they read -- while the frame stack
 * still knows where the assertion was called from.
 *
 * NULL for anything else, which is the point of being this specific: an ordinary runtime error in
 * a test (an index out of range, a `None` unwrapped) already carries the reader's own location and
 * losing it would be strictly worse than the noise this removes. Both conditions must hold -- the
 * innermost frames are the bundled test module's, and the message is `unwrap`'s on an Err, which
 * is how `test.fail` raises -- so an error from anywhere else inside test.zm falls through
 * unchanged rather than being reported at a line that did not raise it. */
static char *assertion_failure(ThreadBoundary *b, FunctionDecl *fn) {
    static const char raised_by[] = "unwrap called on an Err value: ";
    const size_t skip = sizeof raised_by - 1;
    if (b->raw == NULL || strncmp(b->raw, raised_by, skip) != 0) return NULL;
    const char *file = NULL;
    int line = 0, col = 0;
    if (!ziman_core_entry_site("test", &file, &line, &col)) return NULL;
    /* The file only when it is not the test's own -- the label already names that one, so printing
     * it again on every failure would be the same kind of noise this is here to remove. */
    int elsewhere = file != NULL && fn->source_file != NULL && strcmp(file, fn->source_file) != 0;
    const char *body = b->raw + skip;
    size_t room = strlen(body) + (elsewhere ? strlen(file) : 0) + 64;
    char *text = ziman_xmalloc(room);
    if (elsewhere) snprintf(text, room, "at %s:%d:%d: %s", file, line, col, body);
    else snprintf(text, room, "at %d:%d: %s", line, col, body);
    return text;
}

static int run_one_test(Module *module, FunctionDecl *fn, Value *args, int argc, const char *label) {
    ThreadBoundary boundary;
    ziman_thread_boundary_enter(&boundary);
    const char *fail_message = NULL;
    char *rewritten = NULL;
    if (setjmp(boundary.jb) == 0) {
        Value result = call_function(module, fn, args, argc, 0, fn->line, fn->col);
        ziman_thread_boundary_leave();
        if (fn->return_type.kind == TY_RESULT && !result.as.res->is_ok) {
            fail_message = value_display(result.as.res->value);
        }
    } else {
        rewritten = assertion_failure(&boundary, fn);
        fail_message = rewritten != NULL ? rewritten : boundary.message;
    }
    if (fail_message == NULL) {
        printf("PASS %s\n", label);
        return 0;
    }
    printf("FAIL %s  %s\n", label, fail_message);
    free(rewritten);
    return 1;
}

/* Runs every #[test] function declared in `module`'s own entry file (see
 * ast.h's FunctionDecl.is_test/test_cases; typecheck.c already rejected any
 * other shape). A plain `#[test]` runs once, labelled "<file_label>::name". A
 * `#[test(cases: NAME)]` one runs once per element of NAME's array (NAME
 * itself a global val, already evaluated by init_global_vals below, or a
 * zero-parameter function called here to build it -- also inside its own
 * boundary, so a broken case-builder is one reported failure, not a crash),
 * labelled "<file_label>::name[i]". Adds to out_run and out_failed rather
 * than resetting them, so `ziman test a.zm b.zm` can tally one summary
 * across every file. */
void interpret_tests(Module *module, const char *file_label, int *out_run, int *out_failed) {
    g_program = module;
    g_global_env = env_new(NULL);
    /* The same frame `interpret` gives module initialisation, for the same two reasons. */
    ziman_push_frame("<module initialisation>", NULL, 0, 0);
    ziman_set_frame_env(g_global_env);
    init_param_defaults(module);
    init_global_vals(module);
    ziman_pop_frame();
    char label[512];
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        if (!fn->is_test) continue;
        if (fn->test_cases == NULL) {
            snprintf(label, sizeof label, "%s::%s", file_label, fn->name);
            (*out_run)++;
            *out_failed += run_one_test(module, fn, NULL, 0, label);
            continue;
        }
        ArrayObj *cases;
        Binding *cases_val = env_lookup(g_global_env, fn->test_cases);
        if (cases_val != NULL) {
            cases = cases_val->value.as.arr;
        } else {
            FunctionDecl *volatile cases_fn = find_function(module, fn->test_cases);
            ThreadBoundary boundary;
            ziman_thread_boundary_enter(&boundary);
            if (setjmp(boundary.jb) == 0) {
                Value cases_value = call_function(module, cases_fn, NULL, 0, 0, cases_fn->line, cases_fn->col);
                ziman_thread_boundary_leave();
                cases = cases_value.as.arr;
            } else {
                snprintf(label, sizeof label, "%s::%s", file_label, fn->name);
                printf("FAIL %s  could not build cases from '%s': %s\n", label, fn->test_cases, boundary.message);
                (*out_run)++;
                (*out_failed)++;
                continue;
            }
        }
        for (int c = 0; c < cases->count; c++) {
            snprintf(label, sizeof label, "%s::%s[%d]", file_label, fn->name, c);
            Value arg = array_get(cases, c);
            (*out_run)++;
            *out_failed += run_one_test(module, fn, &arg, 1, label);
        }
    }
}
