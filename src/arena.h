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

/* Like `arena_create`, except every block is also registered with the Boehm collector
 * (`GC_add_roots`) as an additional conservatively-scanned range, in a `ZIMAN_GC` build --
 * a plain `arena_create` in the plain build, since there is no collector to register with.
 *
 * Needed wherever a value that might contain a native resource handle can end up in this arena
 * (see src/value_copy.c): this codebase represents a handle (a database connection, ...) as the
 * handle object's own heap address, cast to a plain `int64_t` and carried inside an ordinary
 * `Value` (src/native_sqlite.c's own comment on `DbConn`/`g_live` explains why: "being in the set
 * does not keep it alive... a Connection value nobody holds any more is collected"). Boehm keeps
 * that object alive precisely *because* its address, in bit-pattern form, sits somewhere Boehm's
 * conservative scanner looks -- normally the C stack or another `GC_MALLOC`'d block. An `mmap`'d
 * arena block is invisible to that scanner by construction (the whole point of `arena_create`,
 * for everything that never needs to be seen); copying such a value into an *unscanned* arena
 * would silently break that retention, since nothing would keep the real object alive once every
 * `GC_MALLOC`'d copy of the same integer is gone -- exactly the bug an early version of the
 * outbound arena had, caught by `admin_test.py`'s own account-deletion-with-medications case
 * hanging/erroring against a connection Boehm had already finalized out from under a copy sitting
 * in unscanned memory. Scanning the whole arena is broader than strictly necessary (most of what
 * lives there is never a disguised pointer at all), but a value's own shape isn't known until it's
 * already been copied, and scanning harmless integers as if they might be pointers is exactly what
 * *every* conservative collector already does for the ordinary C stack -- this is not a new kind
 * of imprecision, just the same one applied to one more region. */
Arena *arena_create_scanned(void);

/* `size` bytes, aligned for any C type this interpreter stores in one (matching `malloc`'s own
 * alignment guarantee). Already zero-filled -- not a documented general contract of arenas, but a
 * genuine, free property of this one: every byte handed out is either freshly `mmap`'d (the kernel
 * zero-fills a new anonymous mapping) or has never been handed out before (bump allocation only
 * ever moves forward within a live block; nothing is reused until the whole arena is destroyed).
 * `size == 0` returns a valid, distinct pointer, same as `malloc(0)` may. */
void *arena_alloc(Arena *a, size_t size);

/* Releases every block `a` ever allocated, and `a` itself. Nothing allocated from it may be read or
 * written afterward -- the same "past this point, everything is gone" contract `munmap` itself has.
 * For an arena made with `arena_create_scanned`, this also un-registers every block's roots
 * (`GC_remove_roots`) before unmapping it -- required, not optional: the collector must never be
 * left holding a root that points at memory `munmap` has already given back to the OS. */
void arena_destroy(Arena *a);

/* Total bytes reserved from the OS across every block in `a` (not just what's been handed out) --
 * for Phase 3's own collection-trigger watermark; not otherwise meaningful to a caller. */
size_t arena_reserved(const Arena *a);

#endif
