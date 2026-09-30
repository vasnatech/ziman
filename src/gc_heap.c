#include "gc_heap.h"

#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "gc_mark.h"
#include "gc_scan.h"

/* One free list per power-of-two size class, indexed by its log2 (`class_index_of`) -- enough
 * slots to cover every representable `size_t` value; in practice only a handful near the small end
 * are ever non-NULL. */
#define GC_HEAP_CLASSES (sizeof(size_t) * 8)

struct GcHeap {
    Arena *arena;
    void *free_lists[GC_HEAP_CLASSES];
};

static size_t next_pow2(size_t n) {
    if (n <= 1) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* `class_size` is always itself a power of two (every caller here computes it via `next_pow2`), so
 * its own single set bit's position is its log2. */
static int class_index_of(size_t class_size) {
    return __builtin_ctzll((unsigned long long)class_size);
}

GcHeap *gc_heap_create(void) {
    GcHeap *h = malloc(sizeof(GcHeap));
    /* Scanned, not plain: this heap can end up holding a native resource handle the same way
     * src/value_copy.c's own outbound arena can (see arena.h's own comment on
     * arena_create_scanned) -- a disguised pointer stored as a plain int64_t Value field, kept
     * alive only by Boehm's conservative scanning finding that bit pattern somewhere it looks. */
    h->arena = arena_create_scanned();
    memset(h->free_lists, 0, sizeof(h->free_lists));
    return h;
}

void gc_heap_destroy(GcHeap *h) {
    arena_destroy(h->arena);
    free(h);
}

void *gc_heap_alloc(GcHeap *h, GcKind kind, size_t payload_size) {
    size_t header = gc_header_span();
    /* Every class must leave room for at least a `void *` of payload, even when the caller asked
     * for less (or nothing): once this chunk is freed, the free list stores its own "next" link in
     * exactly those payload bytes (see gc_heap_sweep), and a payload smaller than a pointer would
     * make that write spill into whatever memory follows -- the next object's own header. */
    size_t effective_payload = payload_size < sizeof(void *) ? sizeof(void *) : payload_size;
    size_t class_size = next_pow2(header + effective_payload);
    int idx = class_index_of(class_size);

    void *reused = h->free_lists[idx];
    if (reused != NULL) {
        /* The "next free" link lives in the chunk's own payload bytes -- nothing else is using
         * them while it's free, the same trick any free-list allocator relies on. */
        h->free_lists[idx] = *(void **)reused;
        gc_set_kind(reused, kind);
        gc_set_marked(reused, 0); /* already 0 from being on the free list; cleared again defensively */
        return reused;
    }

    return gc_alloc(h->arena, kind, class_size - header);
}

static void mark_visit(const void *obj, void *ctx) {
    gc_mark_object((const Arena *)ctx, obj);
}

size_t gc_heap_collect(GcHeap *h) {
    /* If the calling thread's own stack couldn't be measured at all (see gc_scan_stack_roots/
     * ziman_stack_bounds's own comment on when that happens), nothing was marked from it -- sweeping
     * anyway would treat every live object reachable only through the stack as garbage. Skipping
     * the whole cycle (not just the scan) is the same graceful-degradation choice
     * ziman_stack_slow itself makes for the recursion guard: an unmeasurable thread gets no limit,
     * not a wrong one; here, it gets no collection this cycle, not a wrong one. */
    if (!gc_scan_stack_roots(h->arena, mark_visit, h->arena)) return 0;
    return gc_heap_sweep(h);
}

size_t gc_heap_sweep(GcHeap *h) {
    size_t reclaimed = 0;
    for (const ArenaBlock *b = arena_first_block(h->arena); b != NULL; b = arena_next_block(b)) {
        const char *start;
        size_t len;
        arena_block_content(b, &start, &len);
        const char *cursor = start;
        const char *end = start + len;
        const void *obj;
        while ((obj = gc_next_object(&cursor, end)) != NULL) {
            if (gc_kind_of(obj) == GC_FREE) continue; /* already free, already linked -- leave it */
            if (gc_is_marked(obj)) {
                gc_set_marked((void *)obj, 0); /* survives; ready for the next cycle's mark phase */
                continue;
            }
            /* Dead: its own recorded size is already an exact class size (gc_heap_alloc's own
             * invariant), so it links straight onto that class's free list with no recomputation. */
            size_t class_size = gc_header_span() + gc_size_of(obj);
            int idx = class_index_of(class_size);
            void *slot = (void *)obj;
            gc_set_kind(slot, GC_FREE);
            *(void **)slot = h->free_lists[idx];
            h->free_lists[idx] = slot;
            reclaimed++;
        }
    }
    return reclaimed;
}
