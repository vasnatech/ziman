/* Standalone correctness test for src/arena.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-arena`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/arena.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

int main(void) {
    Arena *a = arena_create();
    check("arena_create returns non-NULL", a != NULL);

    /* A fresh allocation is zero-filled (a genuine property of bump allocation over mmap'd
     * memory -- see arena.h's own comment on why -- not something a caller has to request). */
    char *zeroed = arena_alloc(a, 64);
    int all_zero = 1;
    for (int i = 0; i < 64; i++) {
        if (zeroed[i] != 0) all_zero = 0;
    }
    check("a fresh allocation is zero-filled", all_zero);

    /* Distinct allocations don't overlap: writing a pattern into one is still there after
     * allocating and writing a different pattern into others. */
    char *p1 = arena_alloc(a, 32);
    char *p2 = arena_alloc(a, 32);
    char *p3 = arena_alloc(a, 32);
    memset(p1, 0xAA, 32);
    memset(p2, 0xBB, 32);
    memset(p3, 0xCC, 32);
    int distinct = 1;
    for (int i = 0; i < 32; i++) {
        if ((unsigned char)p1[i] != 0xAA || (unsigned char)p2[i] != 0xBB || (unsigned char)p3[i] != 0xCC) distinct = 0;
    }
    check("three allocations stay distinct (no overlap)", distinct);

    /* Alignment: every pointer arena_alloc hands out is usable for any C type this interpreter
     * stores in one, same guarantee malloc itself makes. */
    int aligned = ((uintptr_t)p1 % 16 == 0) && ((uintptr_t)p2 % 16 == 0) && ((uintptr_t)p3 % 16 == 0);
    check("allocations are 16-byte aligned", aligned);

    /* Forces at least one new block: an allocation bigger than a single ordinary block, followed
     * by more small ones -- both the oversized allocation and what comes after it must still work
     * and stay distinct from everything already handed out. */
    size_t big_size = 1024 * 1024; /* bigger than ARENA_BLOCK_SIZE, forces a dedicated block */
    char *big = arena_alloc(a, big_size);
    memset(big, 0xDD, big_size);
    char *after_big = arena_alloc(a, 16);
    memset(after_big, 0xEE, 16);
    int big_ok = 1;
    for (size_t i = 0; i < big_size; i++) {
        if ((unsigned char)big[i] != 0xDD) big_ok = 0;
    }
    for (int i = 0; i < 16; i++) {
        if ((unsigned char)after_big[i] != 0xEE) big_ok = 0;
    }
    /* the three original allocations must still read back correctly too -- an oversized
     * allocation must not have clobbered an earlier block. */
    for (int i = 0; i < 32; i++) {
        if ((unsigned char)p1[i] != 0xAA || (unsigned char)p2[i] != 0xBB || (unsigned char)p3[i] != 0xCC) big_ok = 0;
    }
    check("an oversized allocation gets its own block without disturbing earlier ones", big_ok);

    check("arena_reserved grows past what a single ordinary block holds", arena_reserved(a) > big_size);

    /* Many small allocations, enough to force several ordinary blocks besides the oversized one
     * above, all still readable at the end -- the real stress case for "does the block list stay
     * correct as it grows". */
    enum { N = 100000 };
    char *many[N];
    for (int i = 0; i < N; i++) {
        many[i] = arena_alloc(a, 8);
        many[i][0] = (char)(i & 0xFF);
        many[i][1] = (char)((i >> 8) & 0xFF);
    }
    int many_ok = 1;
    for (int i = 0; i < N; i++) {
        if (many[i][0] != (char)(i & 0xFF) || many[i][1] != (char)((i >> 8) & 0xFF)) many_ok = 0;
    }
    check("100,000 small allocations across many blocks all read back correctly", many_ok);

    /* A zero-size request is valid and distinct, same as malloc(0) may be. */
    void *z1 = arena_alloc(a, 0);
    void *z2 = arena_alloc(a, 0);
    check("a zero-size allocation returns a usable, distinct pointer", z1 != NULL && z2 != NULL && z1 != z2);

    arena_destroy(a);
    check("arena_destroy runs without crashing", 1);

    /* A second, independent arena: create/destroy with nothing but the arena itself allocated,
     * the smallest possible lifecycle. */
    Arena *empty = arena_create();
    arena_destroy(empty);
    check("create then destroy with no other allocations doesn't crash", 1);

    /* arena_create_scanned's own scanned-arena registry compiles out entirely in this standalone
     * (non-ZIMAN_GC) test binary -- see arena.c's own scanned_link -- so this only
     * exercises that the API surface behaves the same as the plain one, not the actual root
     * registration; that property is checked at the interpreter level instead
     * (tests/lang/thread_crossing_native_handle.zm, which reproduces the real bug this was
     * for -- a native handle's retention silently breaking when a value crossing a thread landed
     * in unscanned arena memory -- and confirms the fix). */
    Arena *scanned = arena_create_scanned();
    char *sp = arena_alloc(scanned, 100);
    memset(sp, 0x11, 100);
    int scanned_ok = 1;
    for (int i = 0; i < 100; i++) {
        if ((unsigned char)sp[i] != 0x11) scanned_ok = 0;
    }
    arena_destroy(scanned);
    check("arena_create_scanned behaves like arena_create for ordinary allocation", scanned_ok);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
