#ifndef ZIMAN_GC_THREAD_HEAP_H
#define ZIMAN_GC_THREAD_HEAP_H

#include "gc_heap.h"

/* The calling thread's own `GcHeap` (docs/MEMORY.md, "one heap per thread") -- created lazily on
 * first use, one per thread, never shared. This is the allocator `value.c`'s own constructors will
 * route through (starting with one, deliberately -- see docs/design/journal/'s "narrowest possible slice"
 * entry), kept in its own small module rather than owned by `value.c` or `interpreter.c` directly:
 * `value.c` is lower-level than `interpreter.c` and must not depend on it (a layering inversion),
 * but both need access to "the current thread's heap" -- `value.c` to allocate into it,
 * `interpreter.c` to run collections against it. Neither needs to know how the other one is
 * implemented. */
GcHeap *ziman_thread_heap(void);

/* How many bytes the calling thread's own heap has reserved from the OS (its own `arena_reserved`,
 * through the arena `GcHeap` keeps private) -- for a collection trigger's own watermark check. Not
 * otherwise meaningful to a caller. Creates the heap first if it doesn't exist yet, same as
 * `ziman_thread_heap` -- a thread that has never allocated anything through this heap has reserved
 * 0 bytes, not an error. */
size_t ziman_thread_heap_reserved(void);

/* Destroys the calling thread's own heap (if it ever created one) and forgets it, so a later call
 * to `ziman_thread_heap` on this same OS thread would create a fresh one rather than reuse a freed
 * one -- not expected to happen in practice (nothing calls this except a spawned thread's own exit
 * path, right before that thread actually ends), but correct either way.
 *
 * Needed because nothing else ever calls `gc_heap_destroy` on a per-thread heap: every other piece
 * of this interpreter's own `__thread` state (the env pool, tracked defer scopes, the call stack)
 * already has its own release call in `spawn_thread_entry` (interpreter.c) -- this was the one
 * missing. Each thread's `arena_create_scanned` call registers its arena's blocks with Boehm via
 * `GC_add_roots`, and Boehm's own root-set table has a fixed capacity (`MAX_ROOT_SETS`, 2048 in
 * this build) that is never reclaimed for a root nobody ever calls `GC_remove_roots` for -- a real
 * server that spawns one thread per connection (lib/http/server.zm's `serve`) was leaking one such
 * registration per connection forever, and `GC_add_roots_inner` hard-`abort()`s the whole process
 * outright once that table fills (found running `tests/bench/server_concurrency.sh` for the first
 * time since any `gc_heap`-backed migration shipped -- reproducible at as low as 16 concurrent
 * connections against 3000 total requests). Safe to call once a thread's own work is fully done:
 * nothing on another thread should ever hold a pointer into this thread's own heap, since crossing a
 * `Channel`/`spawn` boundary always deep-copies into its own dedicated, short-lived arena first
 * (`value_copy.c`'s `value_copy_send`/`value_copy_receive`), never sharing a sender's own `gc_heap`
 * objects directly -- and, unlike the fix this comment describes, that arena is destroyed the moment
 * the receiving side is done with it, not left leaking the way this one used to be either. */
void ziman_thread_heap_release(void);

#endif
