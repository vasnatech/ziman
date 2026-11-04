#ifndef ZIMAN_GC_PENDING_H
#define ZIMAN_GC_PENDING_H

#include "arena.h"
#include "value.h"

/* **Values that only C is holding, made visible to the mark phase instead of forbidding a
 * collection while they exist.**
 *
 * The per-thread heap's mark phase is precise: it walks `Env` chains, active iterators and a few
 * named thread-locals, and it has no conservative scan of the C stack. So a `Value` sitting in a C
 * local, or in a buffer C is part-way through filling, is reachable by nothing it walks. The
 * original answer was `gc_unsafe_enter`/`gc_unsafe_leave` in interpreter.c -- a depth counter that
 * makes `maybe_collect` skip **entirely** while it is nonzero.
 *
 * That is correct and far too blunt, which F267 measured: the guard is held while the *later*
 * elements of a buffer are evaluated, and `call_closure_value` holds it across a whole call, so
 * everything an argument expression or a closure body does -- however long it takes -- happens with
 * the collector switched off. One Apple Health import skipped 26 million due collections and grew a
 * 215 MB heap around a live set that never passed 2.4 MB. The same work with the call hoisted into a
 * `val` was 72 MB. `tests/bench/gc_argument_position.zm` is the forty-line version.
 *
 * So a part-built buffer becomes a **root source**, the way `t_active_iters` already is. A site
 * declares a `PendingValues` **on its own C stack**, links it in, and keeps `count` current as it
 * fills:
 *
 *     PendingValues pending;
 *     gc_pending_push(&pending, args, 0);
 *     for (int i = 0; i < argc; i++) {
 *         args[i] = eval_expr(module, env, e->as.call.args.items[i]);
 *         pending.count = i + 1;
 *     }
 *     gc_pending_pop(&pending);
 *
 * **On the C stack, deliberately**: the chain then costs no allocation, nests correctly through
 * recursion (an argument's own evaluation pushes its own node), and is popped by construction
 * before the frame holding it returns. A growable array would have to be reallocated, which would
 * invalidate the pointers outer frames hold.
 *
 * `count` is read **live** by the mark phase, so a site may raise it one element at a time and a
 * collection in the middle of the loop marks exactly what is initialised. Entries past `count` are
 * never read, so a buffer need not be zeroed first.
 *
 * **Nothing here is needed for Boehm.** Its heap is scanned conservatively and these buffers are
 * either C stack arrays or `ziman_xmalloc`'d -- which is `GC_MALLOC` in a collecting build -- so
 * Boehm already finds them through the C local that points at one. This module exists only for the
 * precise mark phase that cannot.
 *
 * **Non-moving, so marking is enough**: `gc_heap_sweep` links dead slots onto free lists and
 * reclaims whole blocks, and never relocates a live object, so a `Value` in a pending buffer stays
 * valid across a collection and nothing has to be written back.
 *
 * Push and pop are a store each and take no lock: the chain is `__thread`, and the only reader is
 * this thread's own mark phase (`gc_mark_pending`, called from `mark_this_threads_roots`), which
 * runs on this thread between two statements of it. */
typedef struct PendingValues {
    struct PendingValues *next;
    /* The buffer itself, which the pushing site owns and may keep writing into. */
    const Value *items;
    /* How many of `items[0..]` are initialised. Read live by the mark phase. */
    int count;
} PendingValues;

/* Links `node` onto this thread's chain, describing `items` with `count` entries already
 * initialised. `node` must outlive the pop, which is why it belongs on the caller's own stack. */
void gc_pending_push(PendingValues *node, const Value *items, int count);

/* Unlinks the most recently pushed node, which must be `node` -- pushes and pops pair with the C
 * stack's own nesting, so anything else is a bug in the caller rather than a case to handle. */
void gc_pending_pop(PendingValues *node);

/* Marks every initialised entry of every pending buffer on this thread, into `a`. Called from
 * `mark_this_threads_roots` alongside the other root sources. */
void gc_mark_pending(const Arena *a);

/* **Unwinding.** The nodes live on the C stack, so a `longjmp` past one leaves the chain naming a
 * frame that has returned -- and the next push would link that dead node in, after which the mark
 * phase reads whatever has since been written over it. A stale *counter*, which is what the old
 * guard left behind, was harmless; a stale *pointer* is not.
 *
 * So every unwinding path saves the head before it can jump and restores it after: `error.c` does
 * it once, for its recovery stack and for a `ThreadBoundary` both, through the hook
 * `ziman_recovery_set_unwind` -- which is why no individual recovery site has to remember. The
 * two functions below are that hook's halves; nothing else should call them.
 *
 * `gc_pending_install_unwind_hook` wires the two into `error.c` and is called once, from
 * `ziman_runtime_init`. */
void *gc_pending_save(void);
void gc_pending_restore(void *saved);
void gc_pending_install_unwind_hook(void);

/* How many nodes this thread currently has linked. For the unit test, and for an assertion that a
 * site popped what it pushed; nothing in the interpreter's own hot path reads it. */
int gc_pending_depth(void);

#endif
