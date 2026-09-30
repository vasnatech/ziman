/* Standalone correctness test for src/gc_lookup.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-gc-lookup`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/gc_lookup.h"
#include "../../src/gc_object.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

int main(void) {
    Arena *a = arena_create();

    /* The first object gc_alloc'd into a fresh arena: exact-start lookup must find it. It sits
     * right after the arena's own embedded control block (arena_block_content's own job to skip
     * that correctly) -- if that skip were wrong, this would already fail. */
    char *first = gc_alloc(a, GC_ATOMIC, 40);
    memset(first, 'A', 40);
    check("an exact match on the very first object resolves to itself", gc_lookup(a, first) == first);

    /* An interior address -- not this object's own start -- still resolves to it. This is the
     * whole reason gc_lookup exists: a string view's Value.as.str.ptr can be exactly this shape. */
    check("an address in the middle of a payload resolves to its start", gc_lookup(a, first + 17) == first);
    check("the last valid byte of a payload still resolves to its start", gc_lookup(a, first + 39) == first);
    check("one byte past a payload's declared end does not resolve to it", gc_lookup(a, first + 40) != first);

    /* Several more objects of different kinds/sizes, walked correctly one after another. */
    char *second = gc_alloc(a, GC_STRUCT, 24);
    memset(second, 'B', 24);
    void *third = gc_alloc(a, GC_ARRAY, 8);
    check("a second object's exact start resolves to itself", gc_lookup(a, second) == second);
    check("a second object's interior resolves to its own start, not the first object's", gc_lookup(a, second + 10) == second);
    check("a third object's exact start resolves to itself", gc_lookup(a, third) == third);
    check("looked-up objects report the kind they were allocated with", gc_kind_of(gc_lookup(a, second + 5)) == GC_STRUCT);

    /* Addresses that must NOT resolve to anything: before any real content (this arena's own
     * embedded control-block bytes), and clearly unrelated memory (a local stack variable, and a
     * small/NULL integer masquerading as a pointer -- exactly the kind of value a conservative scan
     * of an ordinary C stack full of ints/bools/floats runs into constantly). */
    check("an address before any real content resolves to nothing", gc_lookup(a, (char *)a) == NULL);
    check("NULL resolves to nothing", gc_lookup(a, NULL) == NULL);
    int unrelated_stack_local = 0;
    check("an unrelated stack address resolves to nothing", gc_lookup(a, &unrelated_stack_local) == NULL);
    check("a small integer masquerading as a pointer resolves to nothing", gc_lookup(a, (void *)(intptr_t)42) == NULL);

    /* Alignment padding between two objects (when a payload size isn't already a multiple of the
     * allocator's own alignment) must not falsely resolve to either neighbour. `third` is 8 bytes,
     * well under 16-byte alignment, so there is real padding between it and whatever comes next. */
    char *fourth = gc_alloc(a, GC_ATOMIC, 16);
    memset(fourth, 'C', 16);
    check("fourth object's start still resolves correctly past the padding gap", gc_lookup(a, fourth) == fourth);
    /* The byte immediately after `third`'s declared 8-byte payload, before `fourth` begins (padding,
     * if `third`'s header+payload wasn't already 16-byte-aligned) or `fourth` itself otherwise --
     * either way this must never resolve to `third` (past its declared end) nor silently corrupt. */
    check("the byte right after a small object's declared end is never attributed to it",
          gc_lookup(a, (char *)third + 8) != third);

    /* A dedicated, oversized block (bigger than arena.c's own ARENA_BLOCK_SIZE) forces a second
     * mmap'd block -- the walk must cross block boundaries correctly, not just work within one. */
    size_t big_size = 1024 * 1024;
    char *big = gc_alloc(a, GC_ATOMIC, big_size);
    memset(big, 'D', 1);
    memset(big + big_size - 1, 'E', 1);
    check("an object in a second, later-allocated block resolves at its start", gc_lookup(a, big) == big);
    check("an object in a second block resolves from its interior too", gc_lookup(a, big + big_size / 2) == big);
    check("an object allocated after crossing into a new block still resolves correctly", gc_lookup(a, fourth) == fourth);

    /* An object allocated into the fresh block that followed the oversized one -- confirms the walk
     * doesn't stop early once more than two blocks exist. */
    char *after_big = gc_alloc(a, GC_ATOMIC, 12);
    memset(after_big, 'F', 12);
    check("an object in a third block resolves correctly", gc_lookup(a, after_big) == after_big);

    arena_destroy(a);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
