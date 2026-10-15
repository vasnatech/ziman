/* Standalone correctness test for src/gc_heap.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-gc-heap`.
 *
 * The first half of this file (through gc_heap_destroy's first call) predates gc_heap_collect and
 * still simulates a mark phase directly with gc_set_marked, exactly the "prove the layer underneath
 * before building the next one" discipline every earlier layer here was proven with -- kept as-is
 * since it still exercises gc_heap_alloc/gc_heap_sweep on their own, independent of gc_scan.c/
 * gc_mark.c now being wired in for gc_heap_collect specifically. The second half tests the real,
 * composed cycle, including gc_heap_collect's own extra_roots callback -- the seam that will
 * eventually carry interpreter.c's Env-chain roots (docs/MEMORY.md, "the second root source") --
 * proven here with a value reachable only through an Env binding, never the stack. `value_hash` is
 * stubbed for the same reason gc_copy_test.c/gc_mark_test.c stub it: nothing here constructs a
 * TY_MAP/TY_SET value, so its real definition (src/interpreter.c, not linked here) is never
 * actually called. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/env.h"
#include "../../src/gc_env_roots.h"
#include "../../src/gc_heap.h"
#include "../../src/gc_object.h"
#include "../../src/value.h"

uint64_t value_hash(Value v) {
    (void)v;
    return 0;
}

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

static void mark_env_ctx(const Arena *a, void *ctx) {
    gc_mark_env_chain(a, (const Env *)ctx);
}

/* File-scope, not a stack local: gc_scan_stack_roots only ever scans the C stack, so stashing the
 * "dead" test object's address here (rather than comparing a stack-local pointer to it directly
 * after gc_heap_collect runs) keeps that address out of the very memory the scan reads -- see its
 * one use below for why comparing a local directly would be a self-defeating test. */
static void *g_dead_addr;

int main(void) {
    GcHeap *h = gc_heap_create();

    /* Ordinary allocation: usable exactly like gc_object.c's own gc_alloc result. */
    char *a = gc_heap_alloc(h, GC_ATOMIC, 40);
    memset(a, 'A', 40);
    int a_ok = 1;
    for (int i = 0; i < 40; i++) {
        if ((unsigned char)a[i] != 'A') a_ok = 0;
    }
    check("a fresh allocation is usable across its whole declared payload", a_ok);
    check("a fresh allocation reports the right kind", gc_kind_of(a) == GC_ATOMIC);
    check("a fresh allocation's reported size is at least what was requested", gc_size_of(a) >= 40);
    check("a fresh allocation starts unmarked", !gc_is_marked(a));

    /* A tiny (zero-payload) allocation: must still be safe to free and relink later (this is
     * exactly the case that needs gc_heap_alloc's own minimum-payload guarantee -- a free-list link
     * is a whole pointer, and a genuinely zero-byte payload has no room of its own for one). */
    void *tiny = gc_heap_alloc(h, GC_VALUES, 0);
    check("a zero-payload allocation is still distinct from the others", tiny != a);

    /* A sweep with nothing marked reclaims everything; a fresh allocation of a similar size
     * afterward reuses one of the just-freed slots rather than growing the arena further. */
    size_t reclaimed_all = gc_heap_sweep(h);
    check("sweeping with nothing marked reclaims every object allocated so far", reclaimed_all == 2);

    void *reused = gc_heap_alloc(h, GC_STRUCT, 40);
    check("an allocation after a full sweep reuses a freed slot of the same size class", reused == a);
    check("a reused slot reports the newly requested kind, not its old one", gc_kind_of(reused) == GC_STRUCT);
    check("a reused slot starts unmarked", !gc_is_marked(reused));
    /* The whole payload is zero, not just the part gc_heap_sweep's own memset already covered --
     * the free-list link itself (the first sizeof(void*) bytes, written *after* that memset, to
     * link the chunk in) is a real, separate byte range gc_heap_alloc's own reuse path has to
     * clear too, or a caller relying on "always zero-filled" the way array_obj_new's own
     * calloc-equivalent shape does would see a stale pointer value where it expects zero. */
    int reused_zeroed = 1;
    for (int i = 0; i < 40; i++) {
        if (((unsigned char *)reused)[i] != 0) reused_zeroed = 0;
    }
    check("a reused slot's entire payload is zero-filled, including the former free-list link", reused_zeroed);

    /* Survivors: mark one of two objects, sweep, confirm the marked one is untouched and readable,
     * the unmarked one is reclaimed, and the marked one's own mark bit is cleared for next time.
     * `reused` (still allocated from the step above) is marked too -- a real mark phase would mark
     * anything still reachable, and leaving it unmarked here would just mean this sweep reclaims it
     * as well, throwing off the exact count this check is after. */
    gc_set_marked(reused, 1);
    char *keep = gc_heap_alloc(h, GC_ATOMIC, 24);
    memcpy(keep, "keep me across a sweep!!", 24);
    char *drop = gc_heap_alloc(h, GC_ATOMIC, 24);
    memcpy(drop, "this one should be freed", 24);
    gc_set_marked(keep, 1);

    size_t reclaimed_one = gc_heap_sweep(h);
    check("sweeping with one of two objects marked reclaims exactly the other", reclaimed_one == 1);
    check("the other still-marked object from before also survives, unaffected", gc_kind_of(reused) == GC_STRUCT && !gc_is_marked(reused));
    check("the marked object's content survives the sweep untouched", memcmp(keep, "keep me across a sweep!!", 24) == 0);
    check("the marked object's own mark bit is cleared after surviving", !gc_is_marked(keep));
    check("the marked object still reports its real kind, not GC_FREE", gc_kind_of(keep) == GC_ATOMIC);

    void *reused2 = gc_heap_alloc(h, GC_ARRAY, 24);
    check("the reclaimed object's slot is reused by a later allocation of the same class", reused2 == drop);

    /* A stray stack word resolving to a free chunk (gc_lookup, not exercised directly here -- this
     * just confirms the header itself is coherent) must read back as GC_FREE, not some leftover
     * kind, for as long as it stays on the free list. */
    void *never_reused = gc_heap_alloc(h, GC_ENUM, 8192); /* a distinct, unlikely-to-be-reused size class */
    gc_heap_sweep(h); /* never_reused is unmarked -- reclaimed */
    check("a freed object reads back as GC_FREE while sitting on its free list", gc_kind_of(never_reused) == GC_FREE);

    /* A dedicated block (payload bigger than arena.c's own ARENA_BLOCK_SIZE) still works, exactly
     * like gc_object.c/arena.c's own tests confirm for the layers underneath this one. */
    size_t big_size = 512 * 1024;
    char *big = gc_heap_alloc(h, GC_ATOMIC, big_size);
    memset(big, 'Z', 1);
    memset(big + big_size - 1, 'Z', 1);
    check("a payload bigger than one arena block still allocates correctly", big[0] == 'Z' && big[big_size - 1] == 'Z');

    gc_heap_destroy(h);

    /* --- gc_heap_collect: the real, composed cycle -------------------------------------------
     * Every allocation from here on goes through gc_heap_alloc (never bare gc_alloc), matching
     * gc_heap.h's own documented precondition that everything in a GcHeap's arena must.
     *
     * Not fully valgrind-clean by construction, not by oversight, for the identical reason
     * gc_copy_test.c/gc_mark_test.c aren't: type_struct("Point") below mallocs (via strdup) a Type
     * node this codebase never frees, by design (ast.h's permanent-AST policy) -- not a leak
     * specific to this test or to gc_heap.c. */
    GcHeap *h2 = gc_heap_create();

    static char *point_fields[] = {"x", "label"};

    /* A live graph, reachable only through one volatile local (mimicking a real stack root) --
     * struct { x: int, label: string } inside an array, exactly the shape gc_mark_test.c already
     * proved gc_mark_value walks correctly; this test is about gc_heap_collect actually finding
     * and preserving it through a real cycle, not re-proving the traversal logic itself. */
    char *label_buf = gc_heap_alloc(h2, GC_ATOMIC, strlen("kept across a collection") + 1);
    memcpy(label_buf, "kept across a collection", strlen("kept across a collection") + 1);
    Value label_val;
    label_val.type = type_scalar(TY_STRING);
    label_val.as.str.ptr = label_buf;
    label_val.as.str.len = (uint32_t)strlen("kept across a collection");
    label_val.as.str.term = 1;

    Value *struct_fields = gc_heap_alloc(h2, GC_VALUES, 2 * sizeof(Value));
    struct_fields[0] = value_int(7);
    struct_fields[1] = label_val;
    StructObj *point = gc_heap_alloc(h2, GC_STRUCT, sizeof(StructObj));
    point->field_names = point_fields;
    point->fields = struct_fields;
    point->count = 2;

    Value *arr_items = gc_heap_alloc(h2, GC_VALUES, 1 * sizeof(Value));
    Value point_val;
    point_val.type = type_struct("Point");
    point_val.as.st = point;
    arr_items[0] = point_val;
    ArrayObj *arr = gc_heap_alloc(h2, GC_ARRAY, sizeof(ArrayObj));
    arr->boxed_items = arr_items;
    arr->bytes = NULL;
    arr->bools = NULL;
    arr->ints = NULL;
    arr->floats = NULL;
    arr->count = 1;
    arr->store = NULL;

    volatile void *live_root = arr; /* the only reference to `arr` (and everything under it) */

    /* An unreferenced object of a similar size: nothing holds a pointer to it anywhere once this
     * statement finishes, so it should be reclaimed. As with every conservative-scan test
     * (gc_scan_test.c's own comment says this plainly), there is no way to *guarantee* a stale
     * stack word can never coincidentally alias it -- the check below is on what gc_heap_collect
     * reliably preserves (the live root), not on proving a negative about the dead one beyond the
     * same best-effort "does a later allocation reuse its slot" gc_heap_test.c's own earlier
     * checks already rely on. */
    /* Its address is stashed in a *static* (not stack) variable, not compared against directly
     * here -- comparing `dead` itself after gc_heap_collect would force the compiler to keep it
     * live across that call, spilling it onto the very stack region the scan reads and making it a
     * legitimate root by this test's own construction, not by anything gc_heap_collect does wrong. */
    void *dead = gc_heap_alloc(h2, GC_ATOMIC, 48);
    memset(dead, 0xDD, 48);
    g_dead_addr = dead;

    gc_heap_collect(h2, NULL, NULL);

    check("the live graph's ArrayObj survives a real collection cycle", gc_kind_of((void *)live_root) == GC_ARRAY);
    check("the live graph's struct, reached through it, survives with its mark cleared", !gc_is_marked(point) && gc_kind_of(point) == GC_STRUCT);
    check("the live graph's string content is intact after a real collection cycle",
          strcmp(((ArrayObj *)live_root)->boxed_items[0].as.st->fields[1].as.str.ptr, "kept across a collection") == 0);
    check("the live graph's int field is intact after a real collection cycle",
          ((ArrayObj *)live_root)->boxed_items[0].as.st->fields[0].as.i == 7);

    void *reused_after_collect = gc_heap_alloc(h2, GC_ATOMIC, 48);
    check("a dead, unreferenced object's slot is reused after a real collection cycle", reused_after_collect == g_dead_addr);

    gc_heap_destroy(h2);

    /* --- gc_heap_collect's extra_roots callback ------------------------------------------------
     * A value reachable only through an Env binding, never through any stack-scanned root (the
     * whole reason extra_roots exists -- see gc_heap.h's own header comment): the string's own
     * pointer is stored into `env` and never referenced by any local of this test's own again,
     * so the only thing keeping it alive across gc_heap_collect is the callback actually running
     * and gc_mark_env_chain correctly marking it. */
    GcHeap *h3 = gc_heap_create();
    Env *env = env_new(NULL);
    const char *msg = "reachable only through an Env binding, not the stack";
    char *env_str_buf = gc_heap_alloc(h3, GC_ATOMIC, strlen(msg) + 1);
    memcpy(env_str_buf, msg, strlen(msg) + 1);
    Value env_str_val;
    env_str_val.type = type_scalar(TY_STRING);
    env_str_val.as.str.ptr = env_str_buf;
    env_str_val.as.str.len = (uint32_t)strlen(msg);
    env_str_val.as.str.term = 1;
    env_define(env, "s", env_str_val.type, 0, env_str_val);

    gc_heap_collect(h3, mark_env_ctx, env);

    check("a value reachable only through an Env binding survives via extra_roots", gc_kind_of(env_str_buf) == GC_ATOMIC);
    check("its content is intact after surviving through extra_roots", strcmp(env_str_buf, msg) == 0);

    env_free(env);
    gc_heap_destroy(h3);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
