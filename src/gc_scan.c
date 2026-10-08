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
 * Makefile's `asan` target, and docs/reviews/tasks.md F14). Excluding this one function from
 * instrumentation leaves the rest of the binary checked, which is the point.
 *
 * Valgrind objects to the same reads for its own reason -- the words are uninitialised, reported
 * through gc_lookup -- and that one needs tools/valgrind.supp rather than an attribute, since
 * valgrind instruments nothing at compile time. */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
__attribute__((no_sanitize_address))
#endif
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
    return 1;
}
