#include "gc_lookup.h"

#include "gc_object.h"

const void *gc_lookup(const Arena *a, const void *candidate) {
    const char *addr = (const char *)candidate;
    for (const ArenaBlock *b = arena_first_block(a); b != NULL; b = arena_next_block(b)) {
        const char *start;
        size_t len;
        arena_block_content(b, &start, &len);
        if (addr < start || addr >= start + len) continue; /* not even in this block's own content */

        /* Blocks never overlap, so this is the only block that can hold `addr` -- whatever this
         * block answers is the answer, and there is no reason to look at the rest. */

        /* The question nearly every call is actually asking (`gc_mark.c`'s `is_gc_heap_object`, of
         * essentially every pointer it marks): is `addr` an object's own payload start? Which is
         * exactly "did an allocation start one header-span earlier" -- a single bit test. */
        if (arena_block_is_alloc_start(b, addr - gc_header_span())) return candidate;

        /* Otherwise `addr` may be interior to an object -- a string view, a sliced array (see this
         * module's header comment on why that case has to work at all). The nearest allocation
         * start at or before it is the only object that could possibly contain it, since
         * allocations neither overlap nor are laid out out of order; it still has to be checked
         * for actually reaching this far, rather than ending somewhere before it. An `addr` that
         * lands inside an object's *header* rather than its payload correctly answers NULL here,
         * being no more a reference to that object than a stray word in its padding would be. */
        const void *alloc = arena_block_alloc_start_at_or_before(b, addr);
        if (alloc == NULL) return NULL;
        const char *obj = (const char *)alloc + gc_header_span();
        if (addr >= obj && addr < obj + gc_size_of(obj)) return obj;
        return NULL;
    }
    return NULL;
}
