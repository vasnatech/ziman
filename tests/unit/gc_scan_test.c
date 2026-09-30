/* Standalone correctness test for src/gc_scan.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-gc-scan`.
 *
 * Every object this test wants the scan to find is held in a `volatile` local: at -O2 (this
 * codebase's own build flags, PLAIN_CFLAGS), a plain local pointer the compiler can prove is only
 * read once, right before a call, is free to keep in a register and never actually write to the
 * stack at all -- which would make it invisible to a conservative scan through no fault of
 * gc_scan.c's, and a flaky, meaningless test. `volatile` forces a real memory write, so a real
 * value really sits somewhere in this stack frame for the scan to find, exactly the situation a
 * live Value sitting in an interpreter's own C locals is in for real. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/gc_object.h"
#include "../../src/gc_scan.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

typedef struct {
    const void *targets[8];
    int found[8];
    int n;
    int total_visits;
} ScanResult;

static void visit(const void *obj, void *ctx_raw) {
    ScanResult *r = (ScanResult *)ctx_raw;
    r->total_visits++;
    for (int i = 0; i < r->n; i++) {
        if (r->targets[i] == obj) r->found[i] = 1;
    }
}

int main(void) {
    Arena *a = arena_create();

    void *obj_a = gc_alloc(a, GC_ATOMIC, 32);
    memset(obj_a, 'A', 32);
    void *obj_b = gc_alloc(a, GC_STRUCT, 16);
    memset(obj_b, 'B', 16);
    void *obj_c = gc_alloc(a, GC_ARRAY, 8);
    memset(obj_c, 'C', 8);

    /* A live root, exactly the way `Value.as.arr`/`.as.st`/etc. would sit in a real interpreter
     * C local: an exact-start pointer to a whole object. */
    volatile void *root_a = obj_a;

    /* An interior pointer, exactly the shape `Value.as.str.ptr` can take for a string view (this
     * module's own reason to exist -- see gc_scan.h/gc_lookup.h's own header comments): the scan
     * must still resolve it back to the whole enclosing object, not fail to find it just because
     * it doesn't sit at that object's own start. */
    volatile void *root_interior = (char *)obj_b + 9;

    /* The same object held by two different stack words -- must still be found (at least once;
     * gc_scan_stack_roots deliberately does not deduplicate, see its own header comment on why). */
    volatile void *root_c1 = obj_c;
    volatile void *root_c2 = obj_c;

    ScanResult r;
    memset(&r, 0, sizeof(r));
    r.targets[0] = obj_a;
    r.targets[1] = obj_b;
    r.targets[2] = obj_c;
    r.n = 3;

    int ok = gc_scan_stack_roots(a, visit, &r);
    check("gc_scan_stack_roots reports success", ok == 1);
    check("an object held by an exact-start local is found", r.found[0]);
    check("an object held only by an interior-pointer local is found", r.found[1]);
    check("an object held by two different locals is still found", r.found[2]);
    check("at least as many visits happened as distinct live roots (no dedup, by design)", r.total_visits >= 3);

    /* Keep every volatile local alive across the whole scan (and past it, so nothing above was
     * ever dead code the compiler could have dropped before the call). */
    check("the roots are still exactly what they were set to", root_a == obj_a && root_interior == (char *)obj_b + 9 &&
                                                                     root_c1 == obj_c && root_c2 == obj_c);

    arena_destroy(a);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
