#define _POSIX_C_SOURCE 200809L
#include "interpreter.h"
#include "iter.h"
#include "native_thread.h"
#include "stack.h"

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
#include "error.h"
#include "fmtstr.h"
#include "mapobj.h"
#include "native.h"
#include "value.h"

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

static Value eval_expr(Module *module, Env *env, Expr *e);

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
 * NOT re-derived here from `args[fixed_count].type.kind == TY_ARRAY`,
 * which used to be this function's own check and is wrong whenever the
 * variadic element type is itself an array (`ArrayBuilder<[T]>`'s
 * `ab_append<[T]>`, for instance: a single element of type `[T]` also
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
        Value *items = tail_count > 0 ? malloc((size_t)tail_count * sizeof(Value)) : NULL;
        for (int i = 0; i < tail_count; i++) {
            items[i] = args[fixed_count + i];
        }
        tail = value_array(type_array(fn->params.items[fixed_count].type), array_obj_new(items, tail_count));
    }
    Value *collapsed = malloc((size_t)fn->params.count * sizeof(Value));
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
    Env **fresh = GC_MALLOC_UNCOLLECTABLE(sizeof(Env *) * (size_t)capacity);
    if (t_defer_envs != NULL) {
        memcpy(fresh, t_defer_envs, sizeof(Env *) * (size_t)t_defer_env_count);
        GC_FREE(t_defer_envs);
    }
#else
    Env **fresh = realloc(t_defer_envs, sizeof(Env *) * (size_t)capacity);
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

static void env_pool_grow(int capacity) {
#ifdef ZIMAN_GC
    Env **fresh = GC_MALLOC_UNCOLLECTABLE(sizeof(Env *) * (size_t)capacity);
    if (t_env_pool != NULL) {
        memcpy(fresh, t_env_pool, sizeof(Env *) * (size_t)t_env_pool_count);
        GC_FREE(t_env_pool);
    }
#else
    Env **fresh = realloc(t_env_pool, sizeof(Env *) * (size_t)capacity);
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
 * for a reuse to see that a fresh env_new's own zeroed fields wouldn't already have shown. */
static void env_pool_release(Env *env) {
    env_reset(env);
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
    ziman_push_frame(fn->name, fn->source_file, call_line, call_col);
    int argc;
    Value *args = collapse_variadic_args(fn, raw_args, raw_argc, tail_is_array_spread, &argc);
    if (fn->is_native) {
        NativeFn native = native_lookup(fn->name);
        if (native == NULL) {
            ziman_error("runtime", call_line, call_col, "native function '%s' has no registered implementation", fn->name);
        }
        Value result = native(args, argc, call_line, call_col);
        if (args != raw_args) free(args);
        ziman_pop_frame();
        return result;
    }
    Env *env = env_pool_acquire(g_global_env);
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
        Value *self_captured = fn->self_ncaptured > 0 ? malloc((size_t)fn->self_ncaptured * sizeof(Value)) : NULL;
        for (int i = 0; i < fn->self_ncaptured; i++) self_captured[i] = args[i];
        Value self_value = value_closure(fn, self_captured, fn->self_ncaptured);
        env_define(env, fn->self_name, self_value.type, 0, self_value);
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

/* A function value (a named function or a lambda with what it captured) called from C with `args`. */
static Value call_closure_value(Module *module, Value closure, Value *args, int argc) {
    ClosureObj *clo = closure.as.clo;
    if (clo->ncaptured == 0) return call_function(module, clo->fn, args, argc, 0, 0, 0);
    Value *all = malloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
    memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
    memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value));
    Value result = call_function(module, clo->fn, all, clo->ncaptured + argc, 0, 0, 0);
    free(all);
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
            Value arg = value_string(strdup(message));
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
    ThreadBoundary boundary;
    ziman_thread_boundary_enter(&boundary);
    if (setjmp(boundary.jb) == 0) {
        call_function(sa->module, sa->fn, sa->args, sa->argc, 0, sa->line, sa->col);
        ziman_thread_boundary_leave();
    } else {
        thread_failed(sa, boundary.message);
    }
    defer_envs_release();
    env_pool_release_all();
    ziman_call_stack_release();
    ziman_thread_end();
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

static Value call_len(Value v) {
    if (v.type.kind == TY_STRING) {
        return value_int((int64_t)v.as.str.len);
    }
    if (v.type.kind == TY_MAP || v.type.kind == TY_SET) return value_int(v.as.map->count);
    return value_int(v.as.arr->count);
}

static Value call_contains(Value first, Value second) {
    if (first.type.kind == TY_STRING) {
        return value_bool(value_str_find(first.as.str.ptr, first.as.str.len, second.as.str.ptr, second.as.str.len, 0) >= 0);
    }
    if (first.type.kind == TY_MAP || first.type.kind == TY_SET) {
        Value ignored;
        return value_bool(mapobj_get(first.as.map, second, &ignored));
    }
    ArrayObj *arr = first.as.arr;
    if (arr->bytes != NULL) {
        if (second.type.kind != TY_BYTE) return value_bool(0);
        return value_bool(memchr(arr->bytes, second.as.by, (size_t)arr->count) != NULL);
    }
    for (int i = 0; i < arr->count; i++) {
        if (value_equals(array_get(arr, i), second)) return value_bool(1);
    }
    return value_bool(0);
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
    switch (v.type.kind) {
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
            /* -0.0 == 0.0, so they must hash alike. */
            double f = v.as.f == 0.0 ? 0.0 : v.as.f;
            memcpy(&h, &f, sizeof(h));
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
            int is_map = v.type.kind == TY_MAP;
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
    if (a.type.kind == TY_ENUM && a.type.enum_name != NULL && strcmp(a.type.enum_name, "Option") == 0) {
        int a_some = strcmp(a.as.en->variant_name, "Some") == 0;
        int b_some = strcmp(b.as.en->variant_name, "Some") == 0;
        if (a_some != b_some) return a_some ? 1 : -1;
        if (!a_some) return 0;
        return value_compare(module, a.as.en->payload, b.as.en->payload, line, col);
    }
    switch (a.type.kind) {
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
            const char *type_name_str = a.type.kind == TY_STRUCT ? a.type.struct_name : a.type.enum_name;
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
    char *fn_name = ziman_message_function_name(e.type.enum_name);
    FunctionDecl *fn = find_function(module, fn_name);
    if (fn == NULL) ziman_error("runtime", line, col, "internal error: no message function '%s' for %s", fn_name, e.type.enum_name);
    Value args[1] = {e};
    return call_function(module, fn, args, 1, 0, line, col);
}

static Value call_abs(Value x, int line, int col) {
    if (x.type.kind == TY_INT) {
        if (x.as.i == INT64_MIN) int_overflow(line, col, "abs");
        return value_int(x.as.i < 0 ? -x.as.i : x.as.i);
    }
    if (x.type.kind == TY_BIGINT) {
        return value_bigint(bignum_abs(*x.as.bi));
    }
    if (x.type.kind == TY_DECIMAL) {
        return value_decimal(decimal_abs(*x.as.dec));
    }
    return value_float(x.as.f < 0 ? -x.as.f : x.as.f);
}

/* `min`/`max`: variadic and `Ord`-bounded, not just the four numeric scalars this used to be
 * limited to. One linear scan calling `value_compare` per candidate -- already the native,
 * direct-C-comparison path for int/float/bigint/decimal/byte/string (see its own TY_INT/.../
 * TY_STRING cases above), so this needs no separate fast path for those the way `sort` gives
 * a packed array its own native routine: a single comparison per element is not where sorting's
 * O(n log n) cost lives. Falls back to a struct/enum's own `<type>_compare` exactly as `compare`
 * and `sort` already do. `argc == 1 && args[0]` an array is the same "trailing array is the
 * whole list" convention a declared `T...` parameter gives a caller. */
static Value value_min_max(Module *module, Value *args, int argc, int want_min, int line, int col) {
    Value best;
    int n;
    if (argc == 1 && args[0].type.kind == TY_ARRAY) {
        ArrayObj *arr = args[0].as.arr;
        n = arr->count;
        if (n == 0) ziman_error("runtime", line, col, "'%s' of an empty array", want_min ? "min" : "max");
        best = array_get(arr, 0);
        for (int i = 1; i < n; i++) {
            Value item = array_get(arr, i);
            int c = value_compare(module, item, best, line, col);
            if (want_min ? c < 0 : c > 0) best = item;
        }
        return best;
    }
    best = args[0];
    for (int i = 1; i < argc; i++) {
        int c = value_compare(module, args[i], best, line, col);
        if (want_min ? c < 0 : c > 0) best = args[i];
    }
    return best;
}

/* push/insert/set never mutate their `arr` argument -- they always build a
 * brand new ArrayObj and return a Value wrapping it, per docs/DESIGN.md's
 * value-semantics decision for arrays. Sharing `old->items[k]` Values by
 * plain struct copy into the new buffer is safe precisely because ArrayObj
 * is immutable once built (see value.h) -- there's nothing to alias into. */
static Value call_push(Type array_type, Value arr, Value x) {
    return value_array(array_type, array_push(arr.as.arr, x));
}

static Value call_insert(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i > old->count) {
        ziman_error("runtime", line, col, "insert index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    if (array_pack_kind(old) != PK_NONE) return value_array(array_type, array_packed_edit(old, 1, (int)i, x));
    int new_count = old->count + 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->boxed_items, (size_t)i * sizeof(Value));
    items[i] = x;
    memcpy(items + i + 1, old->boxed_items + i, (size_t)(old->count - (int)i) * sizeof(Value));
    ArrayObj *packed = old->count == 0 ? array_try_pack(items, new_count) : NULL;
    return value_array(array_type, packed != NULL ? packed : array_obj_new(items, new_count));
}

static Value call_set(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "set index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    if (array_pack_kind(old) != PK_NONE) return value_array(array_type, array_packed_edit(old, 2, (int)i, x));
    Value *items = malloc((size_t)old->count * sizeof(Value));
    memcpy(items, old->boxed_items, (size_t)old->count * sizeof(Value));
    items[i] = x;
    return value_array(array_type, array_obj_new(items, old->count));
}

static Value call_remove(Type array_type, Value arr, Value idx, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "remove index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    if (array_pack_kind(old) != PK_NONE) return value_array(array_type, array_packed_edit(old, 3, (int)i, value_int(0)));
    int new_count = old->count - 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->boxed_items, (size_t)i * sizeof(Value));
    memcpy(items + i, old->boxed_items + i + 1, (size_t)(new_count - (int)i) * sizeof(Value));
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_ok(Type result_type, Value x) {
    return value_result(result_type, result_obj_new(1, x));
}

static Value call_err(Type result_type, Value e) {
    return value_result(result_type, result_obj_new(0, e));
}

static Value call_some(Type option_type, Value x) {
    return value_enum(option_type, enum_obj_new("Option", "Some", 1, x));
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
    if (v.type.kind == TY_DECIMAL) return decimal_to_string(decimal_round(*v.as.dec, precision));
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
            buf = realloc(buf, cap);
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
            if (l.type.kind == TY_INT) {
                int64_t sum;
                if (__builtin_add_overflow(l.as.i, r.as.i, &sum)) int_overflow(e->line, e->col, "+");
                return value_int(sum);
            }
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_add(*l.as.bi, *r.as.bi));
            if (l.type.kind == TY_DECIMAL) return value_decimal(decimal_add(*l.as.dec, *r.as.dec));
            return value_float(l.as.f + r.as.f);
        case OP_SUB:
            if (l.type.kind == TY_INT) {
                int64_t diff;
                if (__builtin_sub_overflow(l.as.i, r.as.i, &diff)) int_overflow(e->line, e->col, "-");
                return value_int(diff);
            }
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_sub(*l.as.bi, *r.as.bi));
            if (l.type.kind == TY_DECIMAL) return value_decimal(decimal_sub(*l.as.dec, *r.as.dec));
            return value_float(l.as.f - r.as.f);
        case OP_MUL:
            if (l.type.kind == TY_INT) {
                int64_t product;
                if (__builtin_mul_overflow(l.as.i, r.as.i, &product)) int_overflow(e->line, e->col, "*");
                return value_int(product);
            }
            if (l.type.kind == TY_BIGINT) return value_bigint(bignum_mul(*l.as.bi, *r.as.bi));
            if (l.type.kind == TY_DECIMAL) return value_decimal(decimal_mul(*l.as.dec, *r.as.dec));
            return value_float(l.as.f * r.as.f);
        case OP_DIV:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                if (l.as.i == INT64_MIN && r.as.i == -1) int_overflow(e->line, e->col, "/");
                return value_int(l.as.i / r.as.i);
            }
            if (l.type.kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(quot);
            }
            if (l.type.kind == TY_DECIMAL) {
                if (decimal_is_zero(*r.as.dec)) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_decimal(decimal_div(*l.as.dec, *r.as.dec));
            }
            if (r.as.f == 0.0) ziman_error("runtime", e->line, e->col, "division by zero");
            return value_float(l.as.f / r.as.f);
        case OP_MOD:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                if (r.as.i == -1) return value_int(0);
                return value_int(l.as.i % r.as.i);
            }
            if (l.type.kind == TY_BIGINT) {
                if (bignum_is_zero(*r.as.bi)) ziman_error("runtime", e->line, e->col, "division by zero");
                BigNum quot, rem;
                bignum_divmod(*l.as.bi, *r.as.bi, &quot, &rem);
                return value_bigint(rem);
            }
            if (l.type.kind == TY_DECIMAL) {
                if (decimal_is_zero(*r.as.dec)) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_decimal(decimal_mod(*l.as.dec, *r.as.dec));
            }
            ziman_error("runtime", e->line, e->col, "internal error: '%%' on float should have been rejected by the type checker");
            return value_int(0);
        case OP_EQ: return value_bool(value_equals(l, r));
        case OP_NEQ: return value_bool(!value_equals(l, r));
        case OP_LT:
            if (l.type.kind == TY_STRING) return value_bool(cmp_string_values(l, r) < 0);
            if (l.type.kind == TY_BYTE) return value_bool(l.as.by < r.as.by);
            if (l.type.kind == TY_INT) return value_bool(l.as.i < r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) < 0);
            if (l.type.kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) < 0);
            return value_bool(l.as.f < r.as.f);
        case OP_GT:
            if (l.type.kind == TY_STRING) return value_bool(cmp_string_values(l, r) > 0);
            if (l.type.kind == TY_BYTE) return value_bool(l.as.by > r.as.by);
            if (l.type.kind == TY_INT) return value_bool(l.as.i > r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) > 0);
            if (l.type.kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) > 0);
            return value_bool(l.as.f > r.as.f);
        case OP_LE:
            if (l.type.kind == TY_STRING) return value_bool(cmp_string_values(l, r) <= 0);
            if (l.type.kind == TY_BYTE) return value_bool(l.as.by <= r.as.by);
            if (l.type.kind == TY_INT) return value_bool(l.as.i <= r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) <= 0);
            if (l.type.kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) <= 0);
            return value_bool(l.as.f <= r.as.f);
        case OP_GE:
            if (l.type.kind == TY_STRING) return value_bool(cmp_string_values(l, r) >= 0);
            if (l.type.kind == TY_BYTE) return value_bool(l.as.by >= r.as.by);
            if (l.type.kind == TY_INT) return value_bool(l.as.i >= r.as.i);
            if (l.type.kind == TY_BIGINT) return value_bool(bignum_compare(*l.as.bi, *r.as.bi) >= 0);
            if (l.type.kind == TY_DECIMAL) return value_bool(decimal_compare(*l.as.dec, *r.as.dec) >= 0);
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
        return value_enum(e->resolved_type, obj);
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
            return b->value;
        }
        case EXPR_UNARY: {
            Value v = eval_expr(module, env, e->as.unary.operand);
            if (e->as.unary.op == OP_NEG) {
                if (v.type.kind == TY_INT) {
                    if (v.as.i == INT64_MIN) int_overflow(e->line, e->col, "-");
                    return value_int(-v.as.i);
                }
                if (v.type.kind == TY_BIGINT) return value_bigint(bignum_negate(*v.as.bi));
                if (v.type.kind == TY_DECIMAL) return value_decimal(decimal_negate(*v.as.dec));
                return value_float(-v.as.f);
            }
            return value_bool(!v.as.b);
        }
        case EXPR_BINARY:
            return eval_binary(module, env, e);
        case EXPR_CALL: {
            int argc = e->as.call.args.count;
            Value *args = argc > 0 ? malloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(module, env, e->as.call.args.items[i]);
            }
            Value result;
            if (e->as.call.through_value) {
                /* A call through a local of function type (a parameter, a val):
                 * the value names the FunctionDecl to run. */
                Binding *fb = e->resolved_depth >= 0 ? env_at_slot(env, e->resolved_depth, e->resolved_slot)
                                                      : env_lookup(env, e->as.call.local_name);
                ClosureObj *clo = fb->value.as.clo;
                if (clo->ncaptured == 0) {
                    result = call_function(module, clo->fn, args, argc, 0, e->line, e->col);
                } else {
                    /* A lambda's captured values are its first parameters. */
                    Value *all = malloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
                    memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
                    memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value));
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
                    result.type = e->resolved_type;
                }
            } else if (strcmp(e->as.call.callee, "print") == 0) {
                result = call_print(args[0]);
            } else if (strcmp(e->as.call.callee, "str") == 0) {
                result = call_str(args[0]);
            } else if (strcmp(e->as.call.callee, "format") == 0) {
                result = call_format(args, argc);
            } else if (strcmp(e->as.call.callee, "printf") == 0) {
                Value text = call_format(args, argc);
                fwrite(text.as.str.ptr, 1, text.as.str.len, stdout);
                fflush(stdout);
                result = value_int(0);
            } else if (strcmp(e->as.call.callee, "len") == 0) {
                result = call_len(args[0]);
            } else if (strcmp(e->as.call.callee, "push") == 0) {
                result = call_push(e->resolved_type, args[0], args[1]);
            } else if ((strcmp(e->as.call.callee, "set") == 0 || strcmp(e->as.call.callee, "insert") == 0) &&
                       (args[0].type.kind == TY_MAP || args[0].type.kind == TY_SET)) {
                Value stored = args[0].type.kind == TY_MAP ? args[2] : value_int(0);
                result = value_map(args[0].type, mapobj_set(args[0].as.map, args[1], stored));
            } else if (strcmp(e->as.call.callee, "remove") == 0 && (args[0].type.kind == TY_MAP || args[0].type.kind == TY_SET)) {
                result = value_map(args[0].type, mapobj_remove(args[0].as.map, args[1]));
            } else if (strcmp(e->as.call.callee, "map_get") == 0) {
                Value found;
                if (mapobj_get(args[0].as.map, args[1], &found)) {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "Some", 1, found));
                } else {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "None", 0, value_int(0)));
                }
            } else if (strcmp(e->as.call.callee, "map_get_or") == 0) {
                Value found;
                result = mapobj_get(args[0].as.map, args[1], &found) ? found : args[2];
            } else if (strcmp(e->as.call.callee, "map_merge") == 0 || strcmp(e->as.call.callee, "set_union") == 0) {
                MapObj *m = args[0].as.map;
                Value *keys, *vals = NULL;
                int is_map = args[1].type.kind == TY_MAP;
                mapobj_entries(args[1].as.map, &keys, is_map ? &vals : NULL);
                for (int i = 0; i < args[1].as.map->count; i++) m = mapobj_set(m, keys[i], is_map ? vals[i] : value_int(0));
                result = value_map(args[0].type, m);
            } else if (strcmp(e->as.call.callee, "set_intersect") == 0 || strcmp(e->as.call.callee, "set_difference") == 0) {
                int keep_present = e->as.call.callee[4] == 'i';
                Value *keys;
                Value ignored;
                MapObj *m = mapobj_empty();
                mapobj_entries(args[0].as.map, &keys, NULL);
                for (int i = 0; i < args[0].as.map->count; i++) {
                    if (mapobj_get(args[1].as.map, keys[i], &ignored) == keep_present) m = mapobj_set(m, keys[i], value_int(0));
                }
                result = value_map(args[0].type, m);
            } else if (strcmp(e->as.call.callee, "set_is_subset") == 0) {
                Value *keys;
                Value ignored;
                int subset = 1;
                mapobj_entries(args[0].as.map, &keys, NULL);
                for (int i = 0; i < args[0].as.map->count && subset; i++) subset = mapobj_get(args[1].as.map, keys[i], &ignored);
                result = value_bool(subset);
            } else if (strcmp(e->as.call.callee, "map_filter") == 0 || strcmp(e->as.call.callee, "set_filter") == 0) {
                int is_map = args[0].type.kind == TY_MAP;
                Value *keys, *vals = NULL;
                mapobj_entries(args[0].as.map, &keys, is_map ? &vals : NULL);
                ClosureObj *clo = args[1].as.clo;
                MapObj *m = mapobj_empty();
                for (int i = 0; i < args[0].as.map->count; i++) {
                    Value call_args[2] = {keys[i], is_map ? vals[i] : value_int(0)};
                    int nargs = is_map ? 2 : 1;
                    Value verdict;
                    if (clo->ncaptured == 0) {
                        verdict = call_function(module, clo->fn, call_args, nargs, 0, e->line, e->col);
                    } else {
                        Value *all = malloc((size_t)(clo->ncaptured + nargs) * sizeof(Value));
                        memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
                        memcpy(all + clo->ncaptured, call_args, (size_t)nargs * sizeof(Value));
                        verdict = call_function(module, clo->fn, all, clo->ncaptured + nargs, 0, e->line, e->col);
                        free(all);
                    }
                    if (verdict.as.b) m = mapobj_set(m, keys[i], call_args[1]);
                }
                result = value_map(args[0].type, m);
            } else if (strcmp(e->as.call.callee, "map_remove_all") == 0) {
                MapObj *m = args[0].as.map;
                for (int i = 0; i < args[1].as.arr->count; i++) m = mapobj_remove(m, array_get(args[1].as.arr, i));
                result = value_map(args[0].type, m);
            } else if (strcmp(e->as.call.callee, "set_insert_all") == 0) {
                MapObj *m = args[0].as.map;
                for (int i = 0; i < args[1].as.arr->count; i++) m = mapobj_set(m, array_get(args[1].as.arr, i), value_int(0));
                result = value_map(args[0].type, m);
            } else if (strcmp(e->as.call.callee, "set_from_array") == 0) {
                MapObj *m = mapobj_empty();
                for (int i = 0; i < args[0].as.arr->count; i++) m = mapobj_set(m, array_get(args[0].as.arr, i), value_int(0));
                result = value_map(e->resolved_type, m);
            } else if (strcmp(e->as.call.callee, "map_from_arrays") == 0) {
                if (args[0].as.arr->count != args[1].as.arr->count) {
                    ziman_error("runtime", e->line, e->col, "map_from_arrays: %d keys but %d values", args[0].as.arr->count, args[1].as.arr->count);
                }
                MapObj *m = mapobj_empty();
                for (int i = 0; i < args[0].as.arr->count; i++) m = mapobj_set(m, array_get(args[0].as.arr, i), array_get(args[1].as.arr, i));
                result = value_map(e->resolved_type, m);
            } else if (strcmp(e->as.call.callee, "map_keys") == 0) {
                /* the key set, in the map's insertion order */
                Value *keys;
                mapobj_entries(args[0].as.map, &keys, NULL);
                MapObj *ks = mapobj_empty();
                for (int i = 0; i < args[0].as.map->count; i++) ks = mapobj_set(ks, keys[i], value_int(0));
                result = value_map(e->resolved_type, ks);
            } else if (strcmp(e->as.call.callee, "map_values") == 0 || strcmp(e->as.call.callee, "set_items") == 0) {
                int want_values = e->as.call.callee[0] == 'm';
                Value *keys, *vals;
                mapobj_entries(args[0].as.map, want_values ? NULL : &keys, want_values ? &vals : NULL);
                int n = args[0].as.map->count;
                Value *items = want_values ? vals : keys;
                ArrayObj *packed = array_try_pack(items, n);
                result = value_array(e->resolved_type, packed != NULL ? packed : array_obj_new(items, n));
            } else if (strcmp(e->as.call.callee, "$map_lit") == 0 || strcmp(e->as.call.callee, "$set_lit") == 0) {
                int is_map = e->as.call.callee[1] == 'm';
                MapObj *m = mapobj_empty();
                for (int i = 0; i < argc; i += is_map ? 2 : 1) {
                    m = mapobj_set(m, args[i], is_map ? args[i + 1] : value_int(0));
                }
                result = value_map(e->resolved_type, m);
            } else if (strcmp(e->as.call.callee, "insert") == 0) {
                result = call_insert(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "set") == 0) {
                result = call_set(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "remove") == 0) {
                result = call_remove(e->resolved_type, args[0], args[1], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "ok") == 0) {
                result = call_ok(e->resolved_type, args[0]);
            } else if (strcmp(e->as.call.callee, "err") == 0) {
                result = call_err(e->resolved_type, args[0]);
            } else if (strcmp(e->as.call.callee, "Some") == 0) {
                result = call_some(e->resolved_type, args[0]);
            } else if (strcmp(e->as.call.callee, "None") == 0) {
                result = value_enum(e->resolved_type, enum_obj_new("Option", "None", 0, value_int(0)));
            } else if (strcmp(e->as.call.callee, "is_ok") == 0) {
                result = call_is_ok(args[0]);
            } else if (strcmp(e->as.call.callee, "unwrap") == 0) {
                result = call_unwrap(args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "unwrap_err") == 0) {
                result = call_unwrap_err(args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "contains") == 0) {
                result = call_contains(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "hash") == 0) {
                result = call_hash(args[0]);
            } else if (strcmp(e->as.call.callee, "compare") == 0) {
                result = value_int(value_compare(module, args[0], args[1], e->line, e->col));
            } else if (strcmp(e->as.call.callee, "error_message") == 0) {
                result = value_error_message(module, args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "abs") == 0) {
                result = call_abs(args[0], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "min") == 0) {
                result = value_min_max(module, args, argc, 1, e->line, e->col);
            } else if (strcmp(e->as.call.callee, "max") == 0) {
                result = value_min_max(module, args, argc, 0, e->line, e->col);
            } else if (strcmp(e->as.call.callee, "channel_new") == 0) {
                int limit = 0;
                if (argc > 0) {
                    if (args[0].as.i < 1) {
                        ziman_error("runtime", e->line, e->col, "channel_new: capacity must be at least 1, found %lld", (long long)args[0].as.i);
                    }
                    limit = (int)args[0].as.i;
                }
                result = value_channel(e->resolved_type, channel_obj_new(limit));
            } else if (strcmp(e->as.call.callee, "send") == 0) {
                if (channel_send(args[0].as.ch, args[1])) {
                    ziman_error("runtime", e->line, e->col, "send on a closed channel");
                }
                result = value_int(0);
            } else if (strcmp(e->as.call.callee, "try_send") == 0) {
                int status = channel_try_send_status(args[0].as.ch, args[1]);
                if (status == 2) {
                    ziman_error("runtime", e->line, e->col, "try_send on a closed channel");
                }
                result = value_bool(status == 0);
            } else if (strcmp(e->as.call.callee, "channel_close") == 0) {
                channel_close(args[0].as.ch);
                result = value_int(0);
            } else if (strcmp(e->as.call.callee, "channel_len") == 0) {
                result = value_int(channel_length(args[0].as.ch));
            } else if (strcmp(e->as.call.callee, "try_receive") == 0) {
                Value got;
                if (channel_try_receive(args[0].as.ch, &got)) {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "None", 0, value_int(0)));
                }
            } else if (strcmp(e->as.call.callee, "receive") == 0) {
                Value got;
                if (!channel_receive(args[0].as.ch, &got)) {
                    ziman_error("runtime", e->line, e->col,
                                "receive on a closed, empty channel (use receive_option to handle a channel being closed)");
                }
                result = got;
            } else if (strcmp(e->as.call.callee, "receive_option") == 0) {
                Value got;
                if (channel_receive(args[0].as.ch, &got)) {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "None", 0, value_int(0)));
                }
            } else if (strcmp(e->as.call.callee, "receive_timeout") == 0) {
                Value got;
                if (channel_receive_timeout(args[0].as.ch, args[1].as.i, &got)) {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "Some", 1, got));
                } else {
                    result = value_enum(e->resolved_type, enum_obj_new("Option", "None", 0, value_int(0)));
                }
            } else if (strcmp(e->as.call.callee, "select2") == 0) {
                Value got;
                int first;
                channel_select2(args[0].as.ch, args[1].as.ch, &first, &got);
                result = value_enum(e->resolved_type, enum_obj_new("Selected", first ? "First" : "Second", 1, got));
            } else {
                FunctionDecl *fn = (FunctionDecl *)e->call_fn;
                result = call_function(module, fn, args, argc, e->as.call.variadic_tail_is_array_spread, e->line, e->col);
                /* A generic native fn's own C implementation has no way
                 * to know which concrete type(s) a given call
                 * instantiated it with -- NativeFn's signature carries
                 * only the already-evaluated argument Values, never the
                 * call's type arguments -- so a native function whose
                 * *return* type mentions one of its own type parameters
                 * (ArrayBuilder<T>'s ab_new<T>() -> ArrayBuilder<T>, for
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
                    result.type = e->resolved_type;
                }
            }
            free(args);
            return result;
        }
        case EXPR_CAST: {
            Value v = eval_expr(module, env, e->as.cast.operand);
            if ((e->as.cast.target_type.kind == TY_BIGINT || e->as.cast.target_type.kind == TY_DECIMAL) && v.type.kind == TY_FLOAT &&
                (isnan(v.as.f) || isinf(v.as.f))) {
                ziman_error("runtime", e->line, e->col, "cannot convert %s to %s", isnan(v.as.f) ? "NaN" : "infinity",
                            e->as.cast.target_type.kind == TY_BIGINT ? "bigint" : "decimal");
            }
            TypeKind to = e->as.cast.target_type.kind;
            if (to == TY_FLOAT) {
                if (v.type.kind == TY_INT) return value_float((double)v.as.i);
                if (v.type.kind == TY_BIGINT) {
                    /* Lossy via a saturating int64 round-trip -- the same
                     * precision ceiling casting a very large int to float
                     * already has; a genuinely huge bigint can't be
                     * represented exactly by a float regardless. */
                    return value_float((double)bignum_to_i64_saturating(*v.as.bi));
                }
                if (v.type.kind == TY_DECIMAL) return value_float(decimal_to_double(*v.as.dec));
                return value_float(v.as.f);
            }
            if (to == TY_BIGINT) {
                if (v.type.kind == TY_INT) return value_bigint(bignum_from_i64(v.as.i));
                if (v.type.kind == TY_FLOAT) return value_bigint(decimal_to_bignum_trunc(decimal_from_double_trunc(v.as.f)));
                if (v.type.kind == TY_DECIMAL) return value_bigint(decimal_to_bignum_trunc(*v.as.dec));
                return value_bigint(*v.as.bi); /* bigint as bigint: no-op */
            }
            if (to == TY_DECIMAL) {
                if (v.type.kind == TY_INT) return value_decimal(decimal_from_bignum(bignum_from_i64(v.as.i)));
                if (v.type.kind == TY_FLOAT) return value_decimal(decimal_from_double_trunc(v.as.f));
                if (v.type.kind == TY_BIGINT) return value_decimal(decimal_from_bignum(*v.as.bi));
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
            if (v.type.kind == TY_FLOAT) return value_int(float_to_int_saturating(v.as.f));
            if (v.type.kind == TY_BIGINT) return value_int(bignum_to_i64_saturating(*v.as.bi));
            if (v.type.kind == TY_DECIMAL) return value_int(bignum_to_i64_saturating(decimal_to_bignum_trunc(*v.as.dec)));
            if (v.type.kind == TY_BYTE) return value_int((int64_t)v.as.by);
            return value_int(v.as.i);
        }
        case EXPR_ARRAY_LIT: {
            int count = e->as.array_lit.elements.count;
            Value *items = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
            for (int i = 0; i < count; i++) {
                items[i] = eval_expr(module, env, e->as.array_lit.elements.items[i]);
            }
            ArrayObj *packed = array_try_pack(items, count);
            if (packed != NULL) return value_array(e->resolved_type, packed);
            return value_array(e->resolved_type, array_obj_new(items, count));
        }
        case EXPR_INDEX: {
            Value arr = eval_expr(module, env, e->as.index.array);
            Value idx = eval_expr(module, env, e->as.index.index);
            int64_t i = idx.as.i;
            if (i < 0 || i >= arr.as.arr->count) {
                ziman_error("runtime", e->line, e->col, "index %lld out of bounds (array length %d)", (long long)i, arr.as.arr->count);
            }
            return array_get(arr.as.arr, (int)i);
        }
        case EXPR_STRUCT_LIT: {
            StructDecl *decl = find_struct(module, e->as.struct_lit.type_name);
            int count = decl->fields.count;
            char **names = malloc((size_t)count * sizeof(char *));
            Value *fields = malloc((size_t)count * sizeof(Value));
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
            return value_struct(e->resolved_type, struct_obj_new(names, fields, count));
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
            Value *fields = malloc((size_t)count * sizeof(Value));
            memcpy(fields, old->fields, (size_t)count * sizeof(Value));
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) {
                FieldInit *fi = &e->as.with_expr.overrides.items[i];
                for (int j = 0; j < count; j++) {
                    if (strcmp(old->field_names[j], fi->name) == 0) {
                        fields[j] = eval_expr(module, env, fi->value);
                        break;
                    }
                }
            }
            /* Reuses old->field_names directly (same struct type, same
             * field order) -- never mutates `old`, so sharing it is safe. */
            return value_struct(e->resolved_type, struct_obj_new(old->field_names, fields, count));
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
            Value *captured = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
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
    if (v.type.kind == TY_RESULT) {
        if (v.as.res->is_ok) {
            *unwrapped = v.as.res->value;
            return 1;
        }
        propagated->kind = CTRL_RETURN;
        Value failure = v.as.res->value;
        if (e->as.try_unwrap.convert_variant != NULL) {
            failure = value_enum(*propagated_type.elem_err, enum_obj_new(propagated_type.elem_err->enum_name, e->as.try_unwrap.convert_variant, 1, failure));
        }
        propagated->value = value_result(propagated_type, result_obj_new(0, failure));
        return 0;
    }
    /* Option<T> -- the only other type check_try_unwrap ever allows. */
    if (strcmp(v.as.en->variant_name, "Some") == 0) {
        *unwrapped = v.as.en->payload;
        return 1;
    }
    propagated->kind = CTRL_RETURN;
    propagated->value = value_enum(propagated_type, enum_obj_new("Option", "None", 0, value_int(0)));
    return 0;
}

/* Runs `block`'s statements directly in `scope` (no further scope nesting) --
 * shared by exec_block (which makes its own fresh scope first) and STMT_FOR
 * (which needs a fresh scope per iteration holding that iteration's loop
 * binder(s), ahead of the body's own statements). */
static ExecResult exec_stmts_in_scope(Module *module, Env *scope, Stmt *block) {
    ExecResult result = no_ctrl();
    for (int i = 0; i < block->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(module, scope, block->as.block.stmts.items[i]);
    }
    run_defers(module, scope);
    return result;
}

static ExecResult exec_block(Module *module, Env *parent, Stmt *block) {
    Env *scope = env_new(parent);
    ExecResult result = exec_stmts_in_scope(module, scope, block);
    env_free(scope);
    return result;
}

static ExecResult exec_stmt(Module *module, Env *env, Stmt *s) {
    ZIMAN_STACK_CHECK();
    switch (s->kind) {
        case STMT_VAL:
        case STMT_VAR: {
            if (s->as.decl.init->kind == EXPR_TRY_UNWRAP) {
                Value unwrapped;
                ExecResult propagated;
                if (!eval_try_unwrap(module, env, s->as.decl.init, &unwrapped, &propagated)) {
                    return propagated;
                }
                Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : unwrapped.type;
                env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, unwrapped);
                return no_ctrl();
            }
            Value v = eval_expr(module, env, s->as.decl.init);
            Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : v.type;
            env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, v);
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
             * iteration's bindings (freeing each one's strdup'd name) without freeing the
             * scope itself or its `items` buffer, so after the first iteration a loop with a
             * stable declaration count allocates nothing at all for its own bookkeeping. This
             * was most of a tight loop's cost -- see docs/DESIGN.md's interpreter-speed entry. */
            Env *scope = env_new(env);
            for (;;) {
                Value cond = eval_expr(module, env, s->as.while_stmt.cond);
                if (!cond.as.b) break;
                env_reset(scope);
                ExecResult result = exec_stmts_in_scope(module, scope, s->as.while_stmt.body);
                if (result.kind == CTRL_RETURN) { env_free(scope); return result; }
                if (result.kind == CTRL_BREAK) break;
                /* CTRL_CONTINUE and CTRL_NONE both just fall through to the
                 * next iteration's condition check. */
            }
            env_free(scope);
            return no_ctrl();
        }
        case STMT_FOR: {
            if (s->as.for_stmt.is_range) {
                Value start = eval_expr(module, env, s->as.for_stmt.range_start);
                Value end = eval_expr(module, env, s->as.for_stmt.range_end);
                Env *scope = env_new(env);
                for (int64_t i = start.as.i; i < end.as.i; i++) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_free(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_free(scope);
                return no_ctrl();
            }

            Value iterable = eval_expr(module, env, s->as.for_stmt.iterable);
            if (iterable.type.kind == TY_STRUCT && iterable.type.struct_name != NULL && strcmp(iterable.type.struct_name, "Iter") == 0) {
                /* a lazy source: ask for one item at a time, and release it however the loop is left */
                int64_t handle = iter_handle_of(iterable);
                Type item_type = s->as.for_stmt.iterable->resolved_type.type_args.items[0];
                Value item;
                Env *scope = env_new(env);
                while (iter_advance(handle, &item, s->line, s->col)) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, item_type, 0, item);
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) {
                        env_free(scope);
                        iter_close_handle(handle);
                        return result;
                    }
                    if (result.kind == CTRL_BREAK) {
                        iter_close_handle(handle);
                        break;
                    }
                }
                env_free(scope);
                return no_ctrl();
            }
            if (iterable.type.kind == TY_MAP || iterable.type.kind == TY_SET) {
                /* iterates a snapshot in insertion order, so the body may rebind the collection freely */
                int is_map = iterable.type.kind == TY_MAP;
                Value *keys, *vals = NULL;
                mapobj_entries(iterable.as.map, &keys, is_map ? &vals : NULL);
                Type key_type = *iterable.type.elem;
                Env *scope = env_new(env);
                for (int i = 0; i < iterable.as.map->count; i++) {
                    env_reset(scope);
                    env_define(scope, s->as.for_stmt.binder1, key_type, 0, keys[i]);
                    if (s->as.for_stmt.binder2 != NULL) env_define(scope, s->as.for_stmt.binder2, *iterable.type.elem_err, 0, vals[i]);
                    ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                    if (result.kind == CTRL_RETURN) { env_free(scope); return result; }
                    if (result.kind == CTRL_BREAK) break;
                }
                env_free(scope);
                return no_ctrl();
            }
            ArrayObj *arr = iterable.as.arr;
            Type elem_type = *iterable.type.elem;
            Env *scope = env_new(env);
            for (int i = 0; i < arr->count; i++) {
                env_reset(scope);
                if (s->as.for_stmt.binder2 == NULL) {
                    env_define(scope, s->as.for_stmt.binder1, elem_type, 0, array_get(arr, i));
                } else {
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(i));
                    env_define(scope, s->as.for_stmt.binder2, elem_type, 0, array_get(arr, i));
                }
                ExecResult result = exec_stmts_in_scope(module, scope, s->as.for_stmt.body);
                if (result.kind == CTRL_RETURN) { env_free(scope); return result; }
                if (result.kind == CTRL_BREAK) break;
            }
            env_free(scope);
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
            FunctionDecl *fn = find_function(module, s->as.spawn_stmt.callee);
            int argc = s->as.spawn_stmt.args.count;
            Value *args = argc > 0 ? malloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(module, env, s->as.spawn_stmt.args.items[i]);
            }
            SpawnArgs *sa = malloc(sizeof(SpawnArgs));
            sa->module = module;
            sa->fn = fn;
            sa->args = args;
            sa->argc = argc;
            sa->line = s->line;
            sa->col = s->col;
            sa->has_handler = s->as.spawn_stmt.on_error != NULL;
            if (sa->has_handler) sa->handler = eval_expr(module, env, s->as.spawn_stmt.on_error);
            if (ziman_spawn_thread(spawn_thread_entry, sa) != 0) {
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
                env->defers = realloc(env->defers, sizeof(Expr *) * (size_t)env->defer_capacity);
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
                    Type t = s->as.value_block.has_declared_type ? s->as.value_block.declared_type : v.type;
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
 * (`datetime_format_of` calling `ab_new<T>()`, say) needs that default
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
 * datetime_format_of("yyyy-MM-dd")` calls `ab_new<T>()` internally with
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

Value native_call_closure(Value fn, Value *args, int argc, int line, int col) {
    ClosureObj *clo = fn.as.clo;
    if (clo->ncaptured == 0) return call_function(g_program, clo->fn, args, argc, 0, line, col);
    Value *all = malloc((size_t)(clo->ncaptured + argc) * sizeof(Value));
    memcpy(all, clo->captured, (size_t)clo->ncaptured * sizeof(Value));
    memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value));
    Value result = call_function(g_program, clo->fn, all, clo->ncaptured + argc, 0, line, col);
    free(all);
    return result;
}

void interpret(Module *module) {
    g_program = module;
    g_global_env = env_new(NULL);
    init_param_defaults(module);
    init_global_vals(module);
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
 * own tests/fixtures harness checks stdout byte-for-byte, and a duration
 * would make every fixture for this feature flaky) and returns 1 on
 * failure, 0 on pass. */
static int run_one_test(Module *module, FunctionDecl *fn, Value *args, int argc, const char *label) {
    ThreadBoundary boundary;
    ziman_thread_boundary_enter(&boundary);
    const char *fail_message = NULL;
    if (setjmp(boundary.jb) == 0) {
        Value result = call_function(module, fn, args, argc, 0, fn->line, fn->col);
        ziman_thread_boundary_leave();
        if (fn->return_type.kind == TY_RESULT && !result.as.res->is_ok) {
            fail_message = value_display(result.as.res->value);
        }
    } else {
        fail_message = boundary.message;
    }
    if (fail_message == NULL) {
        printf("PASS %s\n", label);
        return 0;
    }
    printf("FAIL %s  %s\n", label, fail_message);
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
    init_param_defaults(module);
    init_global_vals(module);
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
