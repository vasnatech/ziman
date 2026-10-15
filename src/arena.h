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
 * calls it -- landed standalone, with its own test (tests/c/arena_test.c), before the much larger
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

/* Like `arena_create`, except every block is also handed to the Boehm collector as an additional
 * conservatively-scanned range on every collection, in a `ZIMAN_GC` build -- a plain
 * `arena_create` in the plain build, since there is no collector to be scanned by.
 *
 * Scanned arenas are kept on a list this module owns and pushed from
 * `ziman_arena_push_scanned_roots` (below), *not* registered individually with `GC_add_roots`.
 * That was the original design and it had a hard, low ceiling: Boehm's own static-root table holds
 * `MAX_ROOT_SETS` (2048) entries, one per registered range, and `GC_add_roots_inner` `abort()`s
 * the whole process the moment it fills. Since `value_copy_send` gives every value crossing a
 * `Channel` its own scanned arena, that made 2048 *simultaneously queued messages* a
 * process-wide limit on the language's own `Channel` -- reached by six threads each streaming and
 * fully draining one database cursor, with nothing leaked and nothing wrong with the program (see
 * docs/DESIGN.md, which also records the four separate leaks that hit the same ceiling first and
 * were each mistaken for the whole problem in turn). A walked list has no such capacity.
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
 * For an arena made with `arena_create_scanned`, this also takes it off the scanned list before
 * unmapping anything -- required, not optional, and in that order: the collector must never be left
 * scanning memory `munmap` has already given back to the OS. */
void arena_destroy(Arena *a);

/* Total bytes reserved from the OS across every block in `a` (not just what's been handed out) --
 * for Phase 3's own collection-trigger watermark; not otherwise meaningful to a caller. */
size_t arena_reserved(const Arena *a);

/* Pushes every block of every live `arena_create_scanned` arena to Boehm's own mark phase as an
 * additional root range. Call only from a `GC_push_other_roots_proc` (src/gc_boehm_roots.c installs
 * the one this codebase has): it reads the scanned-arena list without locking, which is safe there
 * and only there, because that callback runs with every other thread already stopped. A no-op
 * without the collector. See `arena_create_scanned` above for why this exists rather than a
 * `GC_add_roots` call per block. */
void ziman_arena_push_scanned_roots(void);

/* --- walking an arena's own content, block by block -----------------------------------------
 * For a caller that needs to visit every object an arena currently holds (src/gc_lookup.c,
 * resolving a bare address found by a conservative stack scan back to the gc_alloc'd object it
 * falls within or inside) -- not needed by ordinary allocation, so kept separate from the
 * everyday arena_alloc/arena_destroy API above. `ArenaBlock` stays opaque; these are read-only
 * accessors, in no particular order (a caller visiting every block exactly once needs none). */
typedef struct ArenaBlock ArenaBlock;

const ArenaBlock *arena_first_block(const Arena *a);
const ArenaBlock *arena_next_block(const ArenaBlock *b);

/* --- resolving an address back to the allocation it belongs to ------------------------------
 * Each block records, in a bitmap reserved inline between its own header and its content, which of
 * its `ARENA_ALIGN`-sized offsets an `arena_alloc` result started at. These two answer the only
 * questions a conservative collector asks of it. Both take an address that need not be in `b` at
 * all and simply answer "no" for one that isn't, so a caller may walk every block without
 * range-checking first.
 *
 * `arena_block_is_alloc_start` is exact and constant-time, which is the whole point of the bitmap:
 * `src/gc_mark.c`'s `is_gc_heap_object` asks it of essentially every pointer it marks, and before
 * the bitmap existed the only way to answer was to walk the block's objects from its content start
 * -- measured at 827 objects walked per answer, 115 million object steps in one cursor-streaming
 * benchmark run, together 37% of all CPU time in it (docs/DESIGN.md). It is also what makes that
 * answer *exact* rather than a guess at whether the bytes before an address look like a header:
 * writing a mark bit through a false positive corrupts whatever those bytes really belong to,
 * which is a bug this collector has already shipped once and does not intend to again.
 *
 * `arena_block_alloc_start_at_or_before` resolves an interior address -- a string view, a sliced
 * array -- to the allocation containing it, by scanning the bitmap backwards for the nearest start.
 * The caller must still check the allocation's own span actually reaches the address: the nearest
 * preceding start is the only candidate (allocations never overlap and are laid out in increasing
 * address order), but it may simply end before the address. */
int arena_block_is_alloc_start(const ArenaBlock *b, const void *addr);
const void *arena_block_alloc_start_at_or_before(const ArenaBlock *b, const void *addr);

/* `[*out_start, *out_start + *out_len)`: exactly the bytes `arena_alloc` has handed out of this
 * block's own content -- never this block's own header, and never (on whichever single block
 * happens to embed it) the `Arena` struct's own bytes. Walking this span sequentially is only
 * meaningful when every allocation into this arena went through gc_object.c's `gc_alloc`, never
 * a bare `arena_alloc` call directly (every object then has its own header saying how far to the
 * next one, which is exactly what makes the span walkable at all) -- an arena that ever mixed in
 * a bare `arena_alloc` (src/value_copy.c's outbound arena, for one) is not walkable this way;
 * nothing here stops the call, but the result would not parse into real objects. */
void arena_block_content(const ArenaBlock *b, const char **out_start, size_t *out_len);

/* The block whose content holds `addr`, or NULL -- a binary search of this arena's own
 * address-sorted block index, which is what keeps `gc_lookup` from walking the block list once per
 * live object during marking. See the index's comment in arena.c for why it needs no atomics: the
 * owning thread is the only one that reads or writes it. */
const ArenaBlock *arena_block_containing(const Arena *a, const void *addr);

/* Unlinks `b` from `a`'s own block list and releases it exactly like one iteration of
 * arena_destroy's own loop would -- for a caller (gc_heap_sweep) that has independently established
 * nothing in `b` is still live. Unlinking is also what stops a scanned arena's block being scanned,
 * since the block list is exactly what `ziman_arena_push_scanned_roots` walks. Never relocates
 * anything still reachable; only ever called for a block gc_heap's own mark phase found entirely
 * dead, the same non-moving guarantee every other reclaim in this collector already relies on
 * (docs/MEMORY.md, "Phase 3 switches from copying to mark-sweep") -- see docs/DESIGN.md's own entry
 * on why an ordinary, long-lived thread's own sustained allocation needed this at all.
 *
 * `prev` is whichever block currently has `next == b` in `a`'s own list -- a caller walking blocks
 * via arena_first_block/arena_next_block (visiting newest-first) already has this as the previous
 * iteration's own `b`; passing the wrong one corrupts the list silently, so this trusts the caller
 * rather than re-deriving it by a second walk. Never call this for `a`'s own current block (the one
 * still being bump-allocated into -- `arena_first_block`'s own result, before any `arena_next_block`
 * call) or for the one block with no `next` (it embeds the `Arena` struct itself, created once in
 * `arena_create_impl` and never reclaimed either way) -- neither is checked here. */
void arena_reclaim_block(Arena *a, ArenaBlock *prev, ArenaBlock *b);

#endif
