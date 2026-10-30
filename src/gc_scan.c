#include "gc_scan.h"

#include <stdint.h>
#include <string.h>

#include "gc_lookup.h"
#include "stack.h"

static uintptr_t round_up(uintptr_t v, uintptr_t unit) {
    return (v + unit - 1) / unit * unit;
}

/* A conservative scanner reads every pointer-aligned word between the current stack pointer and the
 * top of the thread's stack, which is exactly what AddressSanitizer exists to forbid: the words
 * between live frames are its redzones, and the ones in dead frames belong to calls that have
 * returned. ASan therefore calls the loop below a stack-buffer-overflow on the first collection of
 * *any* program, which made `make asan` useless until this attribute existed -- not a bug and not
 * suppressible at run time, since stack-buffer-overflow has no runtime suppression (see the
 * Makefile's `asan` target, and docs/reviews/tasks/ F14). Excluding this one function from
 * instrumentation leaves the rest of the binary checked, which is the point.
 *
 * Valgrind objects to the same reads for its own reason -- the words are uninitialised, reported
 * through gc_lookup -- and that one needs tools/valgrind.supp rather than an attribute, since
 * valgrind instruments nothing at compile time. */
/* The reads themselves, and nothing else. The exemption sits here rather than on the whole function
 * so that only the loop is uninstrumented: `gc_scan_stack_roots`'s own frame keeps its redzones, so
 * an out-of-bounds write *into* it from instrumented code is still reported -- with the attribute
 * one level up it was not reported at all (docs/reviews/tasks/ F21). `noinline` because inlining
 * this back into an instrumented caller would instrument the loop again; GCC already declines to
 * inline across the mismatch, but the attribute says so rather than relying on that.
 *
 * It must be the loop that is exempt, not the arithmetic around it: ASan instruments memory
 * accesses rather than index computations, so a wrong `limit` or an off-by-one is invisible either
 * way -- moving the arithmetic out buys the frame's redzones, which is a different and smaller
 * thing than it sounds like. The vendored collector draws the line in the same place: bdwgc marks
 * `GC_mark_from`, `GC_mark_and_push_stack` and its two eager pushers, pairs the attribute with
 * `GC_ATTR_NOINLINE` in `mach_dep.c`, and leaves `GC_push_all_stacks` -- the driver -- unmarked. */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
__attribute__((noinline)) __attribute__((no_sanitize_address))
#endif
static void scan_words(const Arena *a, uintptr_t start, uintptr_t limit,
                       void (*visit)(const void *obj, void *ctx), void *ctx) {
    for (uintptr_t p = start; p + sizeof(void *) <= limit; p += sizeof(void *)) {
        void *candidate;
        /* memcpy, not a `void **` cast and dereference, so reading raw stack bytes as a pointer
         * value never risks a strict-aliasing violation -- the bytes at `p` were written as
         * whatever type actually lives there (an int, a double, a real pointer, ...), never as a
         * `void *` through this address. */
        memcpy(&candidate, (const void *)p, sizeof(void *));
        const void *obj = gc_lookup(a, candidate);
        if (obj != NULL) visit(obj, ctx);
    }
}

int gc_scan_stack_roots(const Arena *a, void (*visit)(const void *obj, void *ctx), void *ctx) {
    char *low, *high;
    if (!ziman_stack_bounds(&low, &high)) return 0;
    (void)low; /* the scan only ever runs upward from here toward `high` -- see this file's own header comment */

    /* Same "the address of a local is (near enough) the stack pointer" idiom src/stack.h's own
     * ZIMAN_STACK_CHECK and ziman_stack_remaining already rely on -- not a new technique introduced
     * for this. Rounded up to a pointer-aligned address before the scan starts, defensively (in
     * practice this is already aligned on every platform this runs on). */
    char probe;
    uintptr_t start = round_up((uintptr_t)&probe, sizeof(void *));
    uintptr_t limit = (uintptr_t)high;

    scan_words(a, start, limit, visit, ctx);
    return 1;
}
