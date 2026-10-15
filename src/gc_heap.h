#ifndef ZIMAN_GC_HEAP_H
#define ZIMAN_GC_HEAP_H

#include "gc_object.h"

/* Phase 3's actual collected heap (docs/MEMORY.md) -- the piece a mark-*sweep* design needs that
 * nothing built so far provides. `arena.c` only bump-allocates and mass-frees an entire arena at
 * once; it has no way to reclaim one dead object's space for reuse without discarding everything
 * else alongside it. `GcHeap` adds exactly that on top of an (internal, `arena_create_scanned`)
 * arena: `gc_heap_alloc` hands out memory, `gc_heap_sweep` -- run after a mark phase (not yet
 * built; will reuse `gc_copy.c`'s own per-`Value`-kind traversal, calling `gc_set_marked` instead
 * of copying) has already marked every reachable object -- reclaims everything left unmarked onto
 * a free list, ready for a later `gc_heap_alloc` call of a similar size to reuse, and clears the
 * mark bit on every survivor for the next cycle.
 *
 * Mark-sweep, not the copying design docs/MEMORY.md originally described, because a conservative
 * stack scan's own roots (`gc_scan.c`) can never safely be relocated: rewriting an ambiguous stack
 * word that turns out to have been an unrelated integer, not a real pointer, would silently corrupt
 * real data. Boehm hits this identical wall and resolves it the same way -- never move anything.
 * `gc_object.c`'s header (kind, size, the mark bit) already fits this without any change; `gc_scan.c`'s
 * `visit(obj, ctx)` callback (report only, no way to rewrite the slot it came from) already fits
 * it too. `gc_copy.c`'s specific role as Phase 3's *relocator* does not carry over -- nothing here
 * moves anything -- but its logic remains a correct, tested "deep copy with sharing preserved"
 * primitive that may still find a use elsewhere (`value_copy.c`'s own thread-crossing case, not
 * decided).
 *
 * Segregated free lists by power-of-two size class: `gc_heap_alloc` rounds a request's *total*
 * footprint (header included, via `gc_header_span`) up to the next power of two, and always
 * allocates -- fresh or reused -- exactly that many bytes, never the caller's minimal requested
 * size. This is what makes reuse safe: a freed slot's own recorded size (`gc_size_of`) is always
 * already an exact class size, so any later request whose own class matches can reuse it outright,
 * with no need to track or compare exact sizes. The cost is the usual non-moving-allocator
 * tradeoff (some internal fragmentation from rounding up to a class, mitigated by having many
 * classes rather than few) -- accepted here for the same reason Boehm itself accepts it.
 *
 * `gc_size_of` on an object allocated this way reports its *class* size, not necessarily the exact
 * payload size a caller originally asked for -- a real, deliberate difference from `gc_alloc`'s own
 * exact-size contract. Nothing here or in `gc_copy.c`/`gc_lookup.c` relies on `gc_size_of` being
 * exact (a string's real length always comes from the `Value`'s own `len` field, never from this),
 * so this is safe, but worth remembering before anything new is built expecting otherwise.
 *
 * A precondition worth stating plainly, the same shape as `arena_block_content`'s own: every
 * allocation into a `GcHeap`'s arena must go through `gc_heap_alloc`, never bare `gc_alloc` or
 * `arena_alloc` directly -- `gc_heap_sweep`'s own walk assumes every object's recorded size is
 * already an exact power-of-two class size, which only `gc_heap_alloc` guarantees. */
typedef struct GcHeap GcHeap;

GcHeap *gc_heap_create(void);

/* Releases every block the heap's own arena ever allocated. Nothing allocated from it may be read
 * or written afterward, the same contract `arena_destroy` itself has. */
void gc_heap_destroy(GcHeap *h);

/* `payload_size` bytes of `kind`, reusing a same-size-class chunk `gc_heap_sweep` has freed if one
 * is available, otherwise carving fresh memory from the heap's own underlying arena. Always
 * zero-filled, fresh or reused -- matching `arena_alloc`'s own guarantee, so a caller that only
 * sets some of its own fields explicitly (the same shape `calloc` was chosen for at a plain
 * allocation) doesn't need to know or care which case it got. */
/* Under -DZIMAN_ALLOC_PROFILE every call carries its own file and line, so this heap's
 * allocations show up in src/allocsites.c's dump beside Boehm's. Without it they were invisible,
 * and a profile of a map-and-string-heavy program named only the parser -- this heap is where a
 * `Value`'s own storage increasingly lives (value.c's `value_string_copy`, every MapObj and
 * Leaf in mapobj.c), so leaving it out measured the compiler rather than the program. */
#ifdef ZIMAN_ALLOC_PROFILE
void *gc_heap_alloc_at(GcHeap *h, GcKind kind, size_t payload_size, const char *file, int line);
#define gc_heap_alloc(h, k, n) gc_heap_alloc_at((h), (k), (n), __FILE__, __LINE__)
#else
void *gc_heap_alloc(GcHeap *h, GcKind kind, size_t payload_size);
#endif

/* Total bytes `h`'s own underlying arena has reserved from the OS (its own `arena_reserved`) -- for
 * a caller's own collection-trigger watermark, mirroring `arena_reserved`'s identical role for
 * Phase 2's own (superseded) design. Not otherwise meaningful to a caller. */
size_t gc_heap_reserved(const GcHeap *h);

/* Walks every object the heap currently holds (both live objects and already-free chunks from an
 * earlier sweep): anything `gc_is_marked` reports false for is reclaimed onto its size class's
 * free list (converted to `GC_FREE`, available to a later `gc_heap_alloc`); anything marked has
 * its mark bit cleared, ready for the next cycle's mark phase, and is left exactly where it is. An
 * already-`GC_FREE` chunk is left untouched -- it is already correctly linked onto a free list,
 * and re-processing it here would corrupt that list, not just waste work. Must only run after a
 * full mark phase has already run against this same heap's current contents. Returns the number
 * of objects reclaimed this pass. */
size_t gc_heap_sweep(GcHeap *h);

/* One full, self-contained collection cycle against `h`: scans the *calling* thread's own C stack
 * for roots into `h`'s arena (`gc_scan_stack_roots`), marks everything reachable from each one
 * found (`gc_mark_object`), calls `extra_roots(h's arena, ctx)` if `extra_roots` is non-NULL, then
 * sweeps (`gc_heap_sweep`). Always the calling thread's own heap and its own stack, for the
 * identical reason `gc_scan_stack_roots` itself always is (see its own header comment) -- there is
 * never a reason for a thread to run this against anything but its own heap.
 *
 * `extra_roots` exists because the stack scan alone is not a complete root set: an `Env` (env.h)
 * lives on the ordinary allocator's own heap, never a thread's `GcHeap` arena, so once a `Value` is
 * stored into a binding, a conservative stack scan can never see it -- it only reads raw stack
 * bytes, never dereferences a pointer it finds to look inside whatever it points to (see
 * `gc_env_roots.h`'s own header comment, "the second root source"). Marking every currently
 * reachable `Env` chain -- the interpreter's own global scope, every active call's own scope
 * (`error.c`'s frame tracking, extended to carry each one's `Env*`), `interpreter.c`'s
 * `t_defer_envs`/`t_env_pool` -- needs knowledge only `interpreter.c` has, which this module
 * deliberately does not (and should not) depend on; `extra_roots` is the seam a caller supplies
 * that knowledge through, using `gc_mark_env_chain`/`gc_mark_value`/`gc_mark_object` on whatever it
 * knows is reachable, with `h`'s own arena passed through for it to mark into. `gc_heap_collect`
 * itself has no opinion on what a caller's roots are, keeping this module exactly as decoupled from
 * `interpreter.c`/`error.c`'s own specifics as `gc_scan_stack_roots`'s `visit` callback already
 * keeps `gc_scan.c` from needing to know what marking does.
 *
 * NOT YET wired to a complete root set by anything that actually calls this today -- passing NULL
 * (as every test here still does) omits `Env`-reachable roots entirely, which would be a real
 * correctness bug for any live value reachable only through a variable binding rather than a still-
 * transient C local. Not yet safe to wire into the real interpreter until a caller supplies a
 * complete `extra_roots` (docs/MEMORY.md's "Phase 3 design" section 3 and "the second root source").
 *
 * If the calling thread's own stack cannot be measured at all (see `gc_scan_stack_roots`'s own
 * comment on when that happens), the whole cycle is skipped, not just the scan -- sweeping without
 * having marked anything reachable only through the stack would reclaim live objects. The same
 * graceful-degradation choice `ziman_stack_slow` already makes for the recursion-depth guard: an
 * unmeasurable thread gets no limit, not a wrong one; this gets no collection that cycle, not a
 * wrong one.
 *
 * Returns the number of objects reclaimed this cycle (0 if the cycle was skipped for the reason
 * above, indistinguishable from a cycle that genuinely reclaimed nothing -- neither is a caller's
 * concern; both mean "nothing to do here"). */
size_t gc_heap_collect(GcHeap *h, void (*extra_roots)(const Arena *a, void *ctx), void *ctx);

#endif
