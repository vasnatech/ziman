#include "gc_heap.h"

#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "gc_mark.h"
#include "gc_scan.h"

/* Only for GC_MALLOC_UNCOLLECTABLE/GC_FREE (gc_heap_create/gc_heap_destroy) -- see gc_heap_create's
 * own comment on why the GcHeap struct itself needs this. A `make`/`make gc` build already
 * force-includes src/zgc.h into every translation unit; this explicit include is for gc.h's own
 * declarations, same as arena.c's identical comment on its own `GC_add_roots`/`GC_remove_roots` use. */
#ifdef ZIMAN_GC
#include <gc.h>
#endif

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
    /* GC_MALLOC_UNCOLLECTABLE, not plain malloc (which a ZIMAN_GC build's own zgc.h redirects to
     * ordinary, collectable GC_MALLOC): a GcHeap this codebase actually uses (src/gc_thread_heap.c)
     * is reachable only through a `__thread` pointer, and this codebase's own established finding
     * (interpreter.c's t_defer_envs/t_env_pool) is that Boehm does not reliably scan thread-local
     * storage -- an ordinarily-collectable GcHeap struct could be reclaimed by Boehm while the
     * thread that owns it is still actively allocating from its own arena, a use-after-free of the
     * whole heap, not just one object in it. Uncollectable is safe for every other caller too (a
     * stack-local GcHeap*, as every test here uses): it just means gc_heap_destroy is the only way
     * to free it, already true of every GcHeap regardless. */
#ifdef ZIMAN_GC
    GcHeap *h = GC_MALLOC_UNCOLLECTABLE(sizeof(GcHeap));
#else
    GcHeap *h = malloc(sizeof(GcHeap));
#endif
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
#ifdef ZIMAN_GC
    GC_FREE(h);
#else
    free(h);
#endif
}

size_t gc_heap_reserved(const GcHeap *h) {
    return arena_reserved(h->arena);
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
         * them while it's free, the same trick any free-list allocator relies on. Read out first,
         * then zeroed, not left behind: gc_heap_sweep already zeroes the rest of a reclaimed
         * object's payload, but never the link itself (it writes it *after* zeroing, to link the
         * chunk in) -- so a reused chunk's own first sizeof(void*) bytes would otherwise still
         * hold that stale pointer value, the one part of "the whole payload is zero-filled" this
         * allocator would otherwise quietly fail to keep true. Matches arena_alloc's own "always
         * zero-filled" guarantee, which every caller that doesn't set every field explicitly
         * (array_obj_new's calloc-equivalent shape, for one) depends on. */
        h->free_lists[idx] = *(void **)reused;
        memset(reused, 0, sizeof(void *));
        gc_set_kind(reused, kind);
        gc_set_marked(reused, 0); /* already 0 from being on the free list; cleared again defensively */
        return reused;
    }

    return gc_alloc(h->arena, kind, class_size - header);
}

static void mark_visit(const void *obj, void *ctx) {
    gc_mark_object((const Arena *)ctx, obj);
}

/* Splices every free-list node whose address falls in [block_start, block_end) out of every size
 * class, in place -- needed right before gc_heap_sweep reclaims a whole block: an object swept as
 * dead in an *earlier* cycle is already linked into h->free_lists[] from back then, and leaving
 * that link in place after this block is unmapped would dangle the moment something later walks
 * that class's list looking for a chunk to reuse. Standard singly-linked-list splice: `slot` names
 * whichever word currently holds the address of the next node to examine -- either one class's own
 * head (h->free_lists[idx]) or a surviving node's own "next" link -- so removing a node rewrites
 * `*slot` without needing to track a separate "previous" pointer. */
static void remove_block_from_free_lists(GcHeap *h, const char *block_start, const char *block_end) {
    for (size_t idx = 0; idx < GC_HEAP_CLASSES; idx++) {
        void **slot = &h->free_lists[idx];
        while (*slot != NULL) {
            char *node = (char *)*slot;
            void *next = *(void **)node;
            if (node >= block_start && node < block_end) {
                *slot = next; /* drop node; `slot` itself still names the same word */
            } else {
                slot = (void **)node; /* keep node; a later removal rewrites *its* own link instead */
            }
        }
    }
}

size_t gc_heap_collect(GcHeap *h, void (*extra_roots)(const Arena *a, void *ctx), void *ctx) {
    /* If the calling thread's own stack couldn't be measured at all (see gc_scan_stack_roots/
     * ziman_stack_bounds's own comment on when that happens), nothing was marked from it -- sweeping
     * anyway would treat every live object reachable only through the stack as garbage. Skipping
     * the whole cycle (not just the scan) is the same graceful-degradation choice
     * ziman_stack_slow itself makes for the recursion guard: an unmeasurable thread gets no limit,
     * not a wrong one; here, it gets no collection this cycle, not a wrong one. */
    if (!gc_scan_stack_roots(h->arena, mark_visit, h->arena)) return 0;
    if (extra_roots != NULL) extra_roots(h->arena, ctx);
    return gc_heap_sweep(h);
}

size_t gc_heap_sweep(GcHeap *h) {
    size_t reclaimed = 0;
    /* `prev` is whichever block this loop visited just before `b` -- arena_reclaim_block's own
     * requirement, tracked here rather than re-derived, since this is already a single forward walk
     * (arena_first_block/arena_next_block visit newest-first) with no other reason to look back. */
    ArenaBlock *prev = NULL;
    const ArenaBlock *b = arena_first_block(h->arena);
    while (b != NULL) {
        const ArenaBlock *next = arena_next_block(b);
        const char *start;
        size_t len;
        arena_block_content(b, &start, &len);
        const char *cursor = start;
        const char *end = start + len;
        const void *obj;
        int any_live = 0;
        while ((obj = gc_next_object(&cursor, end)) != NULL) {
            if (gc_kind_of(obj) == GC_FREE) continue; /* already free, already linked -- leave it */
            if (gc_is_marked(obj)) {
                gc_set_marked((void *)obj, 0); /* survives; ready for the next cycle's mark phase */
                any_live = 1;
                continue;
            }
            /* Dead: its own recorded size is already an exact class size (gc_heap_alloc's own
             * invariant), so it links straight onto that class's free list with no recomputation. */
            size_t class_size = gc_header_span() + gc_size_of(obj);
            int idx = class_index_of(class_size);
            void *slot = (void *)obj;
            /* Zeroed before relinking, not left with whatever the object's own bytes last held:
             * this arena is registered with the collector (arena_create_scanned), so a disguised
             * native-handle address (src/native_sqlite.c's own DbConn/g_live comment explains the
             * trick) sitting in an already-dead object's payload would otherwise still look like a
             * live reference to Boehm's own conservative scan for as long as this slot goes
             * unreused -- keeping the underlying resource open well past the point nothing in the
             * program can still reach it (found via tests/fixtures/net_dropped_socket_reclaimed.zm
             * failing once struct_obj_new -- net.Socket's own wrapper -- first migrated here). */
            memset(slot, 0, gc_size_of(obj));
            gc_set_kind(slot, GC_FREE);
            *(void **)slot = h->free_lists[idx];
            h->free_lists[idx] = slot;
            reclaimed++;
        }
        /* Block-level reclaim, not just the per-object one above: once nothing in this block
         * survived this cycle, the whole 256KB (or however large) block can go back to the OS and
         * off Boehm's own root-set table, not just sit around registered forever with every slot on
         * a free list -- see arena_reclaim_block's own comment for why this exists at all. Never for
         * the block still being bump-allocated into (`prev == NULL` on the very first iteration, the
         * one arena_first_block itself returns) or the one block with no `next` (it embeds the
         * Arena struct itself). `remove_block_from_free_lists` runs first: any object swept dead in
         * an *earlier* cycle is already linked into h->free_lists[] from back then, and that link
         * would dangle the moment this block is unmapped if left in place. */
        if (!any_live && next != NULL && prev != NULL) {
            remove_block_from_free_lists(h, start, end);
            arena_reclaim_block(h->arena, prev, (ArenaBlock *)b);
        } else {
            prev = (ArenaBlock *)b;
        }
        b = next;
    }
    return reclaimed;
}
