#include "gc_object.h"

#include <stddef.h>

/* Matches arena.c's own ARENA_ALIGN: a header's rounded-up size must be a multiple of whatever
 * alignment `arena_alloc` already guarantees its own return address has, or the payload right
 * after the header would land unaligned even though the raw allocation itself didn't. Not shared
 * as a public constant from arena.h -- this is the one other place in the codebase that needs it,
 * and duplicating one `#define` is simpler than exporting arena.c's own internal alignment choice
 * as part of its public API for a single caller. */
#define GC_ALIGN ((size_t)16)

typedef struct {
    GcKind kind;
    size_t size;
    int marked;
} GcHeader;

static size_t round_up(size_t v, size_t unit) {
    return (v + unit - 1) / unit * unit;
}

/* The header always precedes the payload by this many bytes -- computed once as a constant
 * expression, not stored per-object, since it never varies. */
#define GC_HEADER_SPAN round_up(sizeof(GcHeader), GC_ALIGN)

void *gc_alloc(Arena *a, GcKind kind, size_t payload_size) {
    char *raw = arena_alloc(a, GC_HEADER_SPAN + payload_size);
    GcHeader *h = (GcHeader *)raw;
    h->kind = kind;
    h->size = payload_size;
    h->marked = 0;
    return raw + GC_HEADER_SPAN;
}

static GcHeader *header_of(const void *obj) {
    return (GcHeader *)((const char *)obj - GC_HEADER_SPAN);
}

GcKind gc_kind_of(const void *obj) {
    return header_of(obj)->kind;
}

size_t gc_size_of(const void *obj) {
    return header_of(obj)->size;
}

int gc_is_marked(const void *obj) {
    return header_of(obj)->marked;
}

void gc_set_marked(void *obj, int marked) {
    header_of(obj)->marked = marked;
}

const void *gc_next_object(const char **cursor, const char *end) {
    if (*cursor >= end) return NULL;
    GcHeader *h = (GcHeader *)*cursor;
    const void *payload = *cursor + GC_HEADER_SPAN;
    /* Matches gc_alloc's own request to arena_alloc exactly: the header plus the payload, rounded
     * up together as one span -- not the header's own (already-rounded) size plus the payload
     * separately rounded, which can differ and would walk to the wrong next address. */
    *cursor += round_up(GC_HEADER_SPAN + h->size, GC_ALIGN);
    return payload;
}
