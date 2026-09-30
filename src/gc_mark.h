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

/* The seam a disguised-native-handle struct (ArrayBuilder/MapBuilder/SetBuilder, src/prelude.c)
 * needs to keep its own payload's Values alive across a real collection while it's still being
 * built. A builder's payload (an ArrayBuilderBuf's `items`, a MapBuilderBuf's MapObj) is a plain
 * malloc'd/GC_MALLOC'd buffer reachable only through an opaque int64 handle stored in the wrapper
 * struct's own `fields[0]` -- invisible to both root sources this module otherwise relies on (the
 * stack scan sees no pointer shape in an integer; a struct's `fields` walk marks each field by its
 * own Value.type.kind, and an int carries nothing further to trace). Whatever a builder holds is
 * therefore only as safe as whatever *else* independently references it -- true for the *previous*
 * loop iteration's own local once the loop moves on, which is false the moment nothing else does
 * (found via docs/reviews/tasks.md's F2, the real health-record-backend-ziman app: lib/http/server.zm's
 * `routes<C>` builds an `ArrayBuilder<Pattern>` across ~100+ loop iterations, and once `Pattern`'s
 * own StructObj wrapper was migrated to gc_heap allocation, a real collection landing mid-loop swept
 * every earlier-appended Pattern still sitting only in the builder's own buffer).
 *
 * `mark_contents(a, st)` is called once per type-directed visit to a TY_STRUCT value named
 * `type_name` (from `gc_mark_into`'s own TY_STRUCT case, the only place a struct's Value -- and so
 * its type name -- is available at all; the type-less `gc_mark_object` path, reached from a bare
 * stack-scan candidate with no Value, has no type name and so cannot dispatch this at all).
 * Deliberately unconditional -- never gated by `st`'s own mark bit -- because a bare stack pointer
 * to the very same wrapper may already have been marked by `gc_mark_object` earlier in the same
 * cycle (a real, common ordering: `gc_scan_stack_roots` always runs before `extra_roots`/the Env
 * chain in `gc_heap_collect`), and gc_mark_struct's own "already marked, skip" guard would otherwise
 * silently skip content-marking on the one visit that could have done it. Safe to call more than
 * once per cycle for the same reason every other per-kind marking function's own redundant-retrace
 * cost is accepted elsewhere in this module: the value graph is a DAG, so there is no loop risk, only
 * possibly-repeated work on a builder still being appended to. Registered once, at startup
 * (`prelude_register`), before any collection can run -- not meant to be called again afterward. */
void gc_mark_register_struct_content_marker(const char *type_name,
                                             void (*mark_contents)(const Arena *a, StructObj *st));

#endif
