#ifndef ZIMAN_VALUE_COPY_H
#define ZIMAN_VALUE_COPY_H

#include "value.h"

/* A value with no heap allocation shared with `v` -- everything `v` points to, recursively, is
 * duplicated, not aliased. Needed at the two places a Value legitimately crosses from one thread
 * to another (a `spawn`'s own `args`/`handler`, evaluated on the calling thread but run on the new
 * one; `send`/`receive` on a `Channel`): today that crossing is a bare pointer copy, safe only
 * because nothing is ever freed. It stops being safe the moment either side's memory can be
 * reclaimed independently of the other's (see docs/MEMORY.md, "one heap per thread") -- this
 * function is that fix, landed ahead of the allocator work it's a prerequisite for, since deep-
 * copying instead of aliasing cannot change any program's observable output under today's
 * never-collects-what-it-shares allocators (GC_MALLOC or plain malloc), only what it costs.
 *
 * Allocates into the calling thread's own *outbound* arena (src/value_copy.c), not the ordinary
 * allocator and not (once per-thread arenas exist for everything else) that thread's own working
 * one -- an arena freed at thread exit would free memory a still-running thread on the other end
 * of the crossing still needs. The outbound arena is never destroyed, so these specific values
 * leak for now; see MEMORY.md's own correction for why that is the honest interim answer.
 * `Map`/`Set` are the one kind not yet routed there (still the ordinary allocator, exactly as
 * before this file existed) -- a separate, later piece of work, not a regression.
 *
 * One deliberate exception unrelated to any of that: a `Channel` value itself is never deep-copied,
 * even when it appears nested inside something that is (a struct field, an array element -- see
 * docs/ROADMAP.md's own `struct Node { peers: Channel<Node> }` cycle example). A channel is the
 * language's one identity-based, intentionally-shared-across-threads handle; copying it would
 * silently disconnect two ends of what the program meant to be a single conduit. */
Value value_deep_copy(Value v);

#endif
