#ifndef ZIMAN_GC_MARK_H
#define ZIMAN_GC_MARK_H

#include "arena.h"
#include "value.h"

/* Marks every arena-allocated object reachable from `v`, given `a`, the arena `v`'s own heap
 * allocations live in -- the piece `gc_heap_sweep` (docs/MEMORY.md, "Phase 3 switches from copying
 * to mark-sweep") needs run against every live root before it can tell survivors from garbage.
 *
 * Idempotent per object (`gc_is_marked` is checked before recursing into an already-marked one's
 * own children) -- this is also what keeps a value graph where the same struct/array is reachable
 * from more than one place (a real, common shape -- see gc_copy.h's own comment on the identical
 * concern) from doing redundant work proportional to how many places reference it, not just a
 * defence against a cycle this graph should never actually contain: Ziman values are immutable and
 * built bottom-up, so excluding `Channel` (which this never traces into -- see below), the graph is
 * a DAG, never cyclic.
 *
 * `Map`/`Set` are not yet `gc_alloc`-routed -- the same known gap `gc_copy.c`/`value_copy.c` already
 * carry, nothing new. `Channel` is never traced into: it is the language's one identity-based,
 * shared-across-threads handle, and belongs to no single thread's heap to begin with.
 *
 * A real, open performance question, not yet measured against anything: marking a string, or an
 * array's own boxed/packed buffer, resolves its pointer through `gc_lookup` first, since it may be
 * an interior pointer into a larger shared parent (a substring, an `array_obj_view` slice) -- see
 * `gc_lookup.h`'s own header comment on why that resolution is needed at all, and `gc_object.c`'s
 * `gc_alloc`/`GcKind` doc comment on why an interior address can never be marked directly. But
 * `gc_lookup` was designed and documented as fine for a conservative stack scan's own few calls per
 * collection cycle -- not necessarily for one call per string or array view found while tracing an
 * entire live graph, which could make marking itself `O(objects x interior references)` in the
 * worst case. Left as-is until a real measurement (once a full collection cycle exists to measure)
 * shows whether it actually matters, the same "measure before optimizing" discipline this whole
 * effort has followed throughout -- not a claim that it doesn't matter. */
void gc_mark_value(const Arena *a, Value v);

/* The bridge a bare, conservatively-found root needs: `gc_scan_stack_roots` (src/gc_scan.c) resolves
 * a candidate stack word to a real object via `gc_lookup`, but hands back only that bare pointer --
 * it has no `Value`, and so no `Type.kind`, to dispatch on. `gc_mark_value` cannot take that; this
 * can, dispatching purely on `obj`'s own `GcKind` header (`gc_kind_of`) instead. Marks `obj` and
 * traces into it exactly as `gc_mark_value` would once it reaches a Value with a matching payload
 * kind: `GC_ATOMIC` (a leaf, nothing further to trace), `GC_VALUES` (a `Value[]` buffer found
 * directly, with no enclosing `ArrayObj`/`StructObj`/`ClosureObj` around it yet -- a real, not
 * hypothetical shape: exactly what a root looks like mid-construction, after `gc_alloc(GC_VALUES,
 * ...)` but before the wrapper object that will hold it exists -- traced by walking each of its own
 * `gc_size_of(obj) / sizeof(Value)` elements through `gc_mark_value`), or one of the five structural
 * kinds. `GC_FREE` (a stray stack word resolving to an already-reclaimed chunk -- a real, if rare,
 * possibility, the same class of harmless false positive conservative scanning always risks) and a
 * NULL `obj` are both no-ops: there is nothing live there to mark. */
void gc_mark_object(const Arena *a, const void *obj);

#endif
