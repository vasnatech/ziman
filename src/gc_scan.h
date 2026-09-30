#ifndef ZIMAN_GC_SCAN_H
#define ZIMAN_GC_SCAN_H

#include "arena.h"

/* Conservatively scans the *calling* thread's own C stack for roots into `a` -- never another
 * thread's stack, and that is not a limitation to lift later: this design's whole point (see
 * docs/MEMORY.md, "one heap per thread") is that a thread collects only its own heap, on its own
 * time, so there is never a need to pause or inspect any other thread's stack the way a
 * stop-the-world collector would. Every word-aligned value between (approximately) the current
 * stack pointer and the thread's own measured upper bound (src/stack.c's `ziman_stack_bounds`) is
 * treated as a possible pointer into `a`; whichever ones actually resolve via `gc_lookup` are
 * reported to `visit(obj, ctx)`, once per matching stack word (not deduplicated to one call per
 * distinct object -- a caller building a mark phase on top of this already needs an idempotent
 * "already marked, skip" check for the ordinary graph-tracing case, so it costs nothing extra to
 * rely on that same check here rather than deduplicate twice).
 *
 * Returns 0 if the calling thread's stack could not be measured (see `ziman_stack_bounds`'s own
 * comment on when that happens; `visit` is not called at all in that case), 1 otherwise. */
int gc_scan_stack_roots(const Arena *a, void (*visit)(const void *obj, void *ctx), void *ctx);

#endif
