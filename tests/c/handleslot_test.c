/* Standalone correctness test for src/handleslot.c, the index+generation handle table. Lives
 * outside src/ since it has its own main(). Run via `make test-handleslot`.
 *
 * Built without ZIMAN_GC, so the weak-reference half (a slot cleared by the collector) is not
 * exercised here -- that needs a real collector. What this pins is everything else: packing, reuse,
 * and above all that a stale handle is rejected after its slot is handed to someone else, which is
 * the hazard the address-keyed HandleSet cannot catch.
 *
 * Worth knowing what that gap hid: these assertions all passed on a design that cannot work,
 * because a weak slot roots nothing and so the first collection would take every live handle in the
 * program. A test that never collects cannot see it. See src/handleslot.h, which now records the
 * blocker and what a sound version needs; nothing uses this module. */
#include <stdio.h>
#include <string.h>

#include "../../src/handleslot.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

int main(void) {
    HandleSlots *t = handleslot_table();
    check("a fresh table holds nothing", handleslot_live(t) == 0);
    check("handle 0 never resolves", handleslot_get(t, 0) == NULL);
    check("a handle that was never issued does not resolve", handleslot_get(t, 12345) == NULL);

    int a = 1, b = 2, c = 3;
    int64_t ha = handleslot_add(t, &a);
    int64_t hb = handleslot_add(t, &b);
    check("adding answers a non-zero handle", ha != 0 && hb != 0);
    check("distinct objects get distinct handles", ha != hb);
    check("a handle resolves to what was added", handleslot_get(t, ha) == &a);
    check("and so does the second", handleslot_get(t, hb) == &b);
    check("two live handles are counted", handleslot_live(t) == 2);

    check("releasing a live handle reports it", handleslot_release(t, ha) == 1);
    check("a released handle stops resolving", handleslot_get(t, ha) == NULL);
    check("releasing twice reports nothing the second time", handleslot_release(t, ha) == 0);
    check("its neighbour is untouched", handleslot_get(t, hb) == &b);
    check("the count drops", handleslot_live(t) == 1);

    /* The point of the generation: the freed slot is handed to `c`, and `ha` -- which names the
     * same slot -- must not resolve to it. This is the case an address-keyed table cannot see,
     * since a reused address is indistinguishable from the original. */
    int64_t hc = handleslot_add(t, &c);
    /* Same slot (the low word is the index, biased), different generation. */
    check("a freed slot is reused", (hc & 0xffffffff) == (ha & 0xffffffff));
    check("but the reused handle differs from the stale one", hc != ha);
    check("the new tenant resolves", handleslot_get(t, hc) == &c);
    check("THE STALE HANDLE DOES NOT RESOLVE TO THE NEW TENANT", handleslot_get(t, ha) == NULL);

    /* A handle built by hand from a live index and a wrong generation, which is what a program
     * fabricating one would most plausibly produce. */
    int64_t forged = (hc & 0xffffffff) | (((hc >> 32) + 1) << 32);
    check("a forged generation on a live index does not resolve", handleslot_get(t, forged) == NULL);

    /* Enough adds and releases to cross a block boundary and churn the free list. */
    int many[4000];
    int64_t hs[4000];
    for (int i = 0; i < 4000; i++) {
        many[i] = i;
        hs[i] = handleslot_add(t, &many[i]);
    }
    int all_ok = 1;
    for (int i = 0; i < 4000; i++) {
        if (handleslot_get(t, hs[i]) != &many[i]) all_ok = 0;
    }
    check("4000 handles across several blocks all resolve", all_ok);
    for (int i = 0; i < 4000; i += 2) {
        handleslot_release(t, hs[i]);
    }
    all_ok = 1;
    for (int i = 0; i < 4000; i++) {
        void *got = handleslot_get(t, hs[i]);
        if (i % 2 == 0 ? got != NULL : got != &many[i]) all_ok = 0;
    }
    check("releasing every other one leaves exactly the others resolving", all_ok);

    /* Refilling must reuse the freed slots rather than growing, and no stale handle may come back
     * to life in the process. */
    int refill[2000];
    int64_t rh[2000];
    for (int i = 0; i < 2000; i++) {
        refill[i] = 10000 + i;
        rh[i] = handleslot_add(t, &refill[i]);
    }
    all_ok = 1;
    for (int i = 0; i < 2000; i++) {
        if (handleslot_get(t, rh[i]) != &refill[i]) all_ok = 0;
    }
    for (int i = 0; i < 4000; i += 2) {
        if (handleslot_get(t, hs[i]) != NULL) all_ok = 0;
    }
    check("refilling reuses slots and revives no stale handle", all_ok);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
