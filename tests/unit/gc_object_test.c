/* Standalone correctness test for src/gc_object.c. Lives outside src/ (which the Makefile
 * wildcards into the ziman binary) since this file has its own main() that would collide with
 * main.c's at link time. Run via `make test-gc-object`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/arena.h"
#include "../../src/gc_object.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

int main(void) {
    Arena *a = arena_create();

    /* A GC_ATOMIC allocation: kind/size read back correctly, payload usable exactly like a plain
     * arena_alloc result (still 16-byte aligned, still writable across its whole declared size). */
    char *atom = gc_alloc(a, GC_ATOMIC, 37);
    check("gc_kind_of reports GC_ATOMIC", gc_kind_of(atom) == GC_ATOMIC);
    check("gc_size_of reports the requested payload size", gc_size_of(atom) == 37);
    check("the payload is 16-byte aligned", (uintptr_t)atom % 16 == 0);
    memset(atom, 0x5A, 37);
    int atom_ok = 1;
    for (int i = 0; i < 37; i++) {
        if ((unsigned char)atom[i] != 0x5A) atom_ok = 0;
    }
    check("the whole declared payload is writable and reads back", atom_ok);

    /* Distinct objects of different kinds/sizes stay distinct and don't corrupt each other's
     * headers -- the real risk with a header immediately preceding a variable-size payload. */
    char *small = gc_alloc(a, GC_VALUES, 3 * sizeof(int64_t));
    char *big = gc_alloc(a, GC_STRUCT, 500);
    memset(small, 0x11, 3 * sizeof(int64_t));
    memset(big, 0x22, 500);
    int no_corruption = gc_kind_of(small) == GC_VALUES && gc_size_of(small) == 3 * sizeof(int64_t) &&
                         gc_kind_of(big) == GC_STRUCT && gc_size_of(big) == 500;
    for (size_t i = 0; i < 3 * sizeof(int64_t); i++) {
        if ((unsigned char)small[i] != 0x11) no_corruption = 0;
    }
    for (int i = 0; i < 500; i++) {
        if ((unsigned char)big[i] != 0x22) no_corruption = 0;
    }
    check("two objects of different kinds/sizes don't corrupt each other or their headers", no_corruption);

    /* The mark bit: starts clear, settable, clearable, independent per object. */
    check("a fresh object starts unmarked", !gc_is_marked(atom) && !gc_is_marked(small) && !gc_is_marked(big));
    gc_set_marked(atom, 1);
    check("marking one object doesn't mark another", gc_is_marked(atom) && !gc_is_marked(small));
    gc_set_marked(atom, 0);
    check("unmarking works", !gc_is_marked(atom));

    /* Every GcKind round-trips correctly -- a real risk if the header ever aliased with anything
     * else, since an enum stored wrong would still often "look" like a plausible value. */
    GcKind kinds[] = {GC_ATOMIC, GC_VALUES, GC_ARRAY, GC_STRUCT, GC_RESULT, GC_ENUM, GC_CLOSURE};
    int kinds_ok = 1;
    void *objs[7];
    for (int i = 0; i < 7; i++) {
        objs[i] = gc_alloc(a, kinds[i], 16);
    }
    for (int i = 0; i < 7; i++) {
        if (gc_kind_of(objs[i]) != kinds[i]) kinds_ok = 0;
    }
    check("every GcKind round-trips through gc_alloc/gc_kind_of correctly", kinds_ok);

    /* A zero-payload object is valid (an empty Value[] buffer, count == 0, is a real shape
     * ArrayObj/StructObj/ClosureObj already allow today). */
    void *empty = gc_alloc(a, GC_VALUES, 0);
    check("a zero-size payload is valid and reports size 0", gc_size_of(empty) == 0);

    arena_destroy(a);
    check("arena_destroy (through gc_alloc'd objects) runs without crashing", 1);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
