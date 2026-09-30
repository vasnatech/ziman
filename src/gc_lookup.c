#include "gc_lookup.h"

#include "gc_object.h"

const void *gc_lookup(const Arena *a, const void *candidate) {
    const char *addr = (const char *)candidate;
    for (const ArenaBlock *b = arena_first_block(a); b != NULL; b = arena_next_block(b)) {
        const char *start;
        size_t len;
        arena_block_content(b, &start, &len);
        const char *block_end = start + len;
        if (addr < start || addr >= block_end) continue; /* not even in this block's own content */

        const char *cursor = start;
        const void *obj;
        while ((obj = gc_next_object(&cursor, block_end)) != NULL) {
            const char *obj_start = (const char *)obj;
            const char *obj_end = obj_start + gc_size_of(obj);
            if (addr >= obj_start && addr < obj_end) return obj;
            /* Objects are walked in strictly increasing address order with no gaps a real payload
             * could sit in (any bytes between one payload's declared end and the next header are
             * pure alignment padding) -- once the candidate is before this object's start, every
             * object still to come starts even later, so it cannot match any of them either. */
            if (addr < obj_start) break;
        }
    }
    return NULL;
}
