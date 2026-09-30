#ifndef ZIMAN_ARENA_H
#define ZIMAN_ARENA_H

#include <stddef.h>

/* A bump-allocated region: `arena_alloc` never frees anything individually, only forward-allocates
 * into growable, `mmap`'d blocks; `arena_destroy` releases every block in one shot. This is Phase 2
 * of docs/MEMORY.md's thread-owned-heap design -- the per-thread heap itself, before any collection
 * exists on top of it (Phase 3): one `Arena` per thread, created when the thread starts, destroyed
 * (bulk-freeing everything it ever allocated) when the thread ends. Known incomplete on its own,
 * exactly as MEMORY.md says: a long-lived thread's garbage isn't reclaimed until the thread exits,
 * which Phase 3's mark-sweep collector is what actually fixes. This module is inert until something
 * calls it -- landed standalone, with its own test (tests/unit/arena_test.c), before the much larger
 * and riskier step of routing the interpreter's own allocation sites through it.
 *
 * Deliberately built on `mmap`/`munmap`, never `malloc`/`free`: this file is compiled into both the
 * collector build and the plain one, and the collector build force-includes src/zgc.h into every
 * translation unit, which *textually* redefines `malloc`/`calloc`/`realloc`/`free` to route through
 * Boehm's collector. An arena's whole purpose is to be memory the collector never touches -- so it
 * cannot call anything zgc.h would intercept, and `mmap`/`munmap` are the one allocation primitive
 * that isn't. See src/arena.c's own header comment for the block layout.
 *
 * Not thread-safe by design: one `Arena` belongs to exactly one thread (mirroring `ArrayStore`'s
 * own "one thread pushes to a given tip" invariant, and `#[thread_bound]`'s enforcement at the
 * language level) -- callers never share one across threads, so it needs no locking of its own. */
typedef struct Arena Arena;

Arena *arena_create(void);

/* `size` bytes, aligned for any C type this interpreter stores in one (matching `malloc`'s own
 * alignment guarantee). Already zero-filled -- not a documented general contract of arenas, but a
 * genuine, free property of this one: every byte handed out is either freshly `mmap`'d (the kernel
 * zero-fills a new anonymous mapping) or has never been handed out before (bump allocation only
 * ever moves forward within a live block; nothing is reused until the whole arena is destroyed).
 * `size == 0` returns a valid, distinct pointer, same as `malloc(0)` may. */
void *arena_alloc(Arena *a, size_t size);

/* Releases every block `a` ever allocated, and `a` itself. Nothing allocated from it may be read or
 * written afterward -- the same "past this point, everything is gone" contract `munmap` itself has. */
void arena_destroy(Arena *a);

/* Total bytes reserved from the OS across every block in `a` (not just what's been handed out) --
 * for Phase 3's own collection-trigger watermark; not otherwise meaningful to a caller. */
size_t arena_reserved(const Arena *a);

#endif
