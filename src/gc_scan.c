#include "gc_scan.h"

#include <stdint.h>
#include <string.h>

#include "gc_lookup.h"
#include "stack.h"

static uintptr_t round_up(uintptr_t v, uintptr_t unit) {
    return (v + unit - 1) / unit * unit;
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
