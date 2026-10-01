#ifndef ZIMAN_GC_COPY_H
#define ZIMAN_GC_COPY_H

#include "arena.h"
#include "value.h"

/* `src/value_copy.c` generalised for a real collection cycle: that file deep-copies a Value graph
 * into a destination arena for the two thread-crossing points; a copying collector (docs/MEMORY.md,
 * "Phase 3 design") needs the same recursive copy, but into a *header-tracked* destination
 * (src/gc_object.c's gc_alloc, not bare arena_alloc) and with sharing preserved across the copy --
 * losing sharing on every collection, not just at a thread crossing, would make a long-lived
 * shared struct or array duplicate itself every cycle for as long as the program runs.
 *
 * GcDedup is that sharing-preservation bookkeeping: a source-pointer -> destination-pointer table,
 * valid for exactly one copy pass (or one top-level gc_copy_value call and everything it recurses
 * into). An object already copied once during that pass is looked up and returned again, not
 * re-copied -- so two different paths in the graph that reach the same ArrayObj/StructObj/etc.
 * still reach the same copy afterward, the way they did before the copy.
 *
 * Deliberately NOT deduplicated, even when the exact same pointer recurs: string bytes, a packed
 * array's own buffers (bytes/bools/ints/floats), and any Value[] buffer (ArrayObj.boxed_items/
 * StructObj.fields/ClosureObj.captured). All three can be *interior* pointers -- array_obj_view
 * (src/value.c) hands back a view sharing its parent's buffers at an offset, and a substring does
 * the same for strings -- so the same address can mean a different logical value depending on the
 * length/count paired with it. Deduplicating by address alone would risk conflating two genuinely
 * different values that happen to start at the same byte (e.g. a string and a zero-offset prefix
 * slice of it). Always copying these fresh sidesteps the ambiguity entirely, at the cost of not
 * preserving buffer-sharing for exactly these three shapes across a collection -- the same
 * tradeoff value_copy.c already accepted for the identical reason (see its own header comment on
 * `store`), extended here uniformly rather than solved. */
typedef struct GcDedup GcDedup;

GcDedup *gc_dedup_create(void);
void gc_dedup_destroy(GcDedup *d);

/* Deep-copies `v` into `dest` via gc_alloc, using `dedup` to preserve sharing as described above.
 * `dest` should be a fresh arena for a real collection cycle (the old generation is abandoned, not
 * touched by this call). `Map`/`Set`'s own wrapper is gc_heap-migrated now (gc_mark.c's `GC_MAP`),
 * but this module's own TY_MAP/TY_SET case (`mapobj_empty`/`mapobj_set`) builds it through the
 * *calling thread's own* `GcHeap`, not `dest` -- this module is unused by the live interpreter (dead
 * code kept for its own tested "deep copy with sharing preserved" logic, not currently wired to
 * anything; see this header's own top comment), so nothing exercises this inconsistency today, but a
 * future caller of `gc_copy_value` on a `Map`/`Set` value should not assume the result lives in
 * `dest`. `Channel` is shared by identity, never copied -- same as
 * value_copy.c and for the same reason (see value_copy.h). */
Value gc_copy_value(Arena *dest, GcDedup *dedup, Value v);

#endif
