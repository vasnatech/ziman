/* Standalone correctness test for src/gc_pending.c. Lives outside src/ (which the Makefile
 * wildcards into the ziman binary) since this file has its own main() that would collide with
 * main.c's at link time. Run via `make test-gc-pending`.
 *
 * What this module exists for is in its own header: the per-thread heap's mark phase is precise
 * and has no conservative scan of the C stack, so a buffer of `Value`s that only C is holding has
 * to be registered or it will be swept. It used to be answered by forbidding a collection
 * altogether, which is F267 -- a call in argument position then ran uncollectable for its whole
 * duration.
 *
 * `value_hash` is stubbed for the same reason gc_mark_test.c and gc_interp_roots_test.c stub it:
 * nothing here constructs a TY_MAP/TY_SET value. Not fully valgrind-clean by construction, not by
 * oversight, for the identical reason those are not -- `type_scalar` builds permanent AST Type
 * nodes this codebase never frees, by design. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/gc_object.h"
#include "../../src/gc_pending.h"
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

static Value gc_string(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *buf = gc_alloc(a, GC_ATOMIC, len + 1);
    memcpy(buf, s, len + 1);
    /* The real constructor, so this helper holds no copy of the string Value's layout. */
    return value_string_view(buf, len, 1);
}

/* A nested push, the shape that matters: evaluating one buffer's element pushes another, and the
 * inner buffer's own entries have to be marked as well as the outer's part-built prefix. */
static void nested(Arena *a, Value *inner, int n, Value *outer_prefix_seen) {
    PendingValues node;
    gc_pending_push(&node, inner, n);
    *outer_prefix_seen = value_int(gc_pending_depth());
    gc_mark_pending(a);
    gc_pending_pop(&node);
}

int main(void) {
    Arena *a = arena_create();

    check("no pending buffers to start with", gc_pending_depth() == 0);

    /* A part-built buffer: three slots, two filled. The third holds a Value nothing initialised,
     * and the mark phase must not read it -- which is the whole reason `count` is read live. */
    Value args[3];
    Value first = gc_string(a, "the first argument, already evaluated");
    Value second = gc_string(a, "the second argument, just evaluated");
    Value never = gc_string(a, "a slot the loop has not reached");
    args[0] = first;
    args[1] = second;
    /* Deliberately NOT args[2] = never: the slot is left uninitialised, as a real argument loop
     * leaves it, and `never` is reachable only from this frame's own local. */

    PendingValues pending;
    gc_pending_push(&pending, args, 0);
    check("a pushed buffer is one level deep", gc_pending_depth() == 1);

    gc_mark_pending(a);
    check("with count 0 nothing in the buffer is marked", !gc_is_marked(first.as.str.ptr));

    pending.count = 1;
    gc_mark_pending(a);
    check("count 1 marks the first entry", gc_is_marked(first.as.str.ptr));
    check("and not the second", !gc_is_marked(second.as.str.ptr));

    pending.count = 2;
    gc_mark_pending(a);
    check("count 2 marks the second entry too", gc_is_marked(second.as.str.ptr));
    check("the slot past count is never read", !gc_is_marked(never.as.str.ptr));

    /* Nested: an argument's own evaluation pushing its own buffer. */
    Value inner[1];
    Value deep = gc_string(a, "an entry of a buffer pushed while filling another");
    inner[0] = deep;
    Value depth_seen;
    nested(a, inner, 1, &depth_seen);
    check("a nested push is two levels deep", depth_seen.as.i == 2);
    check("the nested buffer's entry is marked", gc_is_marked(deep.as.str.ptr));
    check("and the outer buffer is back to one level", gc_pending_depth() == 1);

    /* Unwinding: what `error.c` does through the hook when a longjmp skips a pop. The chain is
     * saved before the jump could happen and restored after, so a node on a frame that has
     * returned is never walked. */
    void *saved = gc_pending_save();
    Value orphan[1];
    Value lost = gc_string(a, "an entry of a buffer whose pop was skipped by a longjmp");
    orphan[0] = lost;
    PendingValues abandoned;
    gc_pending_push(&abandoned, orphan, 1);
    check("the abandoned buffer is linked before the unwind", gc_pending_depth() == 2);
    gc_pending_restore(saved);
    check("restoring unlinks it without needing its pop", gc_pending_depth() == 1);
    gc_set_marked((void *)lost.as.str.ptr, 0);
    gc_mark_pending(a);
    check("and the unwound buffer's entry is no longer marked", !gc_is_marked(lost.as.str.ptr));
    check("while the surviving buffer's entries still are", gc_is_marked(first.as.str.ptr));

    gc_pending_pop(&pending);
    check("popping the last buffer empties the chain", gc_pending_depth() == 0);
    gc_set_marked((void *)first.as.str.ptr, 0);
    gc_mark_pending(a);
    check("an empty chain marks nothing", !gc_is_marked(first.as.str.ptr));

    /* A pop of something that is not the head is the caller's bug, not a case to handle -- it must
     * at least not corrupt the chain. */
    PendingValues one, two;
    gc_pending_push(&one, args, 1);
    gc_pending_push(&two, args, 1);
    gc_pending_pop(&one);
    check("popping out of order leaves the chain alone", gc_pending_depth() == 2);
    gc_pending_pop(&two);
    gc_pending_pop(&one);
    check("and popping in order then empties it", gc_pending_depth() == 0);

    arena_destroy(a);
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all gc_pending checks passed\n");
    return 0;
}
