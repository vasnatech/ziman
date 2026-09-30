#ifndef ZIMAN_GC_OBJECT_H
#define ZIMAN_GC_OBJECT_H

#include "arena.h"

/* What kind of arena-allocated object a header describes -- enough for Phase 3's mark and copy
 * phases (not yet built; see docs/MEMORY.md's "Phase 3 design") to know how to trace or relocate
 * an object once a conservative stack scan, or a precise trace already inside the heap, hands them
 * a bare pointer with no other type information attached. `Value.type` doesn't help here: it lives
 * wherever the `Value` itself is stored, not at the address a pointer inside it refers to -- see
 * MEMORY.md's own explanation of why a header is unavoidable at all. */
typedef enum {
    GC_ATOMIC,  /* a leaf allocation holding no pointers of its own: string bytes, a BigNum's or
                 * Decimal's digit array, a packed [byte]/[bool]/[int]/[float] buffer -- never
                 * traced further, only its own bytes ever need preserving */
    GC_VALUES,  /* a Value[] buffer -- ArrayObj.boxed_items, StructObj.fields, ClosureObj.captured
                 * -- traced by walking each Value by its own type.kind, not by this kind tag */
    GC_ARRAY,   /* ArrayObj */
    GC_STRUCT,  /* StructObj */
    GC_RESULT,  /* ResultObj */
    GC_ENUM,    /* EnumObj */
    GC_CLOSURE  /* ClosureObj */
} GcKind;

/* Allocates `payload_size` bytes of `kind` from `a`, prefixed with a header the mark/copy phases
 * will read -- returns a pointer to the payload itself, past the header, ready to use exactly like
 * a plain `arena_alloc` result (same alignment guarantee: the header's own size is rounded up
 * first, so it never shifts the payload off the alignment `arena_alloc` already gives it).
 * `payload_size` is the exact byte count `gc_size_of` reports back later -- for `GC_VALUES`,
 * that's `count * sizeof(Value)`, not `count` itself; for a fixed-shape kind (`GC_ARRAY`, ...) it's
 * simply `sizeof` that struct. */
void *gc_alloc(Arena *a, GcKind kind, size_t payload_size);

/* What `gc_alloc` recorded for `obj`, a pointer it returned. `obj` must be exactly such a pointer,
 * never an arbitrary interior address -- this layer does not attempt to find an enclosing header
 * from the middle of an allocation (see MEMORY.md's own interior-pointer discussion); every caller
 * here already has the exact start address, either from another `gc_alloc` call or from a `Value`
 * field that was itself built by one. */
GcKind gc_kind_of(const void *obj);
size_t gc_size_of(const void *obj);
int gc_is_marked(const void *obj);
void gc_set_marked(void *obj, int marked);

#endif
