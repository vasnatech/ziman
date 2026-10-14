#ifndef ZIMAN_GC_GUARD_ROOTS_H
#define ZIMAN_GC_GUARD_ROOTS_H

#include "arena.h"
#include "value.h"

/* Closes a gap found directly, the hard way, via a fake Postgres-protocol listener driving the
 * real interpreter under sustained load (docs/DESIGN.md's own long GC-roots investigation): a
 * `Guard` (`prelude.c`'s `GuardBuf`, the mechanism a database connection uses to tell its owning
 * thread to close if the connection itself is ever dropped without an explicit `close`) is a plain
 * `malloc`/`GC_MALLOC`'d block, outside every root source Ziman's own Phase 3 collector
 * (`gc_heap_collect`, `mark_this_threads_roots`) already walks. Its own `message` field (the
 * `Request.Close`/`Request.EndCursor(...)` value the finalizer sends once the collector decides
 * nothing references the guard any more) can point into the *creating* thread's own `gc_heap`
 * arena -- `enum_obj_new`/`struct_obj_new` route every wrapper header through it -- which that same
 * thread's own, entirely separate, periodic Phase 3 sweep has no reason to know it must protect.
 * The gap is real and was confirmed directly: a temporary trace on `guard_finalizer` caught its own
 * `message` already corrupted into an unrelated value (`Option.Some`, `db::SqlValue.Int`) by the
 * time the finalizer actually read it, in both a synthetic fake-listener repro and the real
 * Postgres-backed one -- the same signature as this investigation's very first, previously-thought-
 * unrelated SIGSEGV (a `receive`d value's `EnumObj` reading back garbage inside `eval_enum_access`).
 *
 * The content-marker seam `gc_mark_register_struct_content_marker` already provides (used by
 * `MutableArray`/`MutableMap`/`MutableSet` for the identical "disguised handle, invisible payload"
 * shape) is not enough here: it only runs while the `Guard` *value* itself is still reachable from
 * some other root, but the finalizer's entire reason to exist is to fire *after* the value holding
 * it (a dropped `Connection`) is already unreachable, arbitrarily later, whenever Boehm gets around
 * to it -- exactly the window this module protects instead, via an always-active registry (mirroring
 * `native_thread.c`'s own `ThreadInfo`/`g_threads` shape), independent of whether anything
 * Ziman-visible still points at the guard at all. */

/* Registers `message_slot` -- the address of a live `GuardBuf`'s own `message` field, which never
 * moves for the guard's whole lifetime -- as a root `gc_mark_live_guards` protects on every Phase 3
 * collection, on behalf of `owner_heap` (the creating thread's own `ziman_thread_heap()`, captured
 * once here so marking later knows which thread's own arena to mark into, since a guard may be
 * finalized -- and so unregistered -- on a different thread than the one that created it). Call
 * once, right after `message` is set (`native_guard_new`, `prelude.c`). */
void ziman_guard_register(void *owner_heap, Value *message_slot);

/* Removes `message_slot`'s own record. Call once, from `guard_finalizer`, after it has read
 * `message` for the last time (sent it on the guard's own channel) -- not before, so a Phase 3
 * collection racing the finalizer itself still finds `message` protected right up to the point
 * nothing needs it protected any more. */
void ziman_guard_unregister(Value *message_slot);

/* Marks every still-registered guard's own `message` into `a`, for every one whose `owner_heap`
 * matches `heap` -- called once per Phase 3 collection, from `heap`'s own thread, alongside every
 * other root source `mark_this_threads_roots` (`interpreter.c`) already walks. A guard created on a
 * different thread is simply skipped here: that thread's own collection will mark it instead,
 * exactly like every other `__thread`-scoped root this interpreter already has, except this one
 * needs the registry itself to be globally, not thread-locally, visible (a guard's finalizer can run
 * on any thread, not only the one that created it). */
void gc_mark_live_guards(void *heap, const Arena *a);

#endif
