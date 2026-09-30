#ifndef ZIMAN_GC_THREAD_HEAP_H
#define ZIMAN_GC_THREAD_HEAP_H

#include "gc_heap.h"

/* The calling thread's own `GcHeap` (docs/MEMORY.md, "one heap per thread") -- created lazily on
 * first use, one per thread, never shared. This is the allocator `value.c`'s own constructors will
 * route through (starting with one, deliberately -- see docs/DESIGN.md's "narrowest possible slice"
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

#endif
