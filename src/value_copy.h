#ifndef ZIMAN_VALUE_COPY_H
#define ZIMAN_VALUE_COPY_H

#include "value.h"

/* A value with no heap allocation shared with `v` -- everything `v` points to, recursively, is
 * duplicated, not aliased. Needed at the two places a Value legitimately crosses from one thread to
 * another (a `spawn`'s own `args`/`handler`, evaluated on the calling thread but run on the new one;
 * `send`/`receive` on a `Channel`) -- a bare pointer copy would let the crossing value's memory
 * belong to no single thread's own allocator, unsafe once any allocator can reclaim independently
 * of another thread's own lifetime (see docs/MEMORY.md, "one heap per thread").
 *
 * The lifecycle, in two calls instead of one: `value_copy_send` (the sending thread) copies `v`
 * with plain `malloc` -- `GC_MALLOC` under zgc.h, Boehm's own shared heap; `value_copy_receive`
 * (the *other* thread, once it has the crossed value in hand) copies it again, this time into its
 * own ordinary allocators (`GC_MALLOC`/this thread's own `gc_heap`), after which nothing it holds
 * references the sending side's copy at all, which simply becomes garbage.
 *
 * **Why Boehm's shared heap and not the sending thread's own allocators.** The requirement is
 * exactly one thing: a crossing copy must not live in a heap that the *sending* thread may destroy
 * (`ziman_thread_heap_release`, at that thread's own exit) before the receiving side has copied it
 * out. It is not a requirement about rooting. Boehm's heap is tied to no thread, so it meets that
 * directly -- and, unlike the alternative below, a copy there is *traced* rather than *rooted*:
 * Boehm finds it through whatever actually holds it (a `ChannelMsg` in a channel's own `malloc`'d
 * queue, a `SpawnArgs` a new thread is started with, both themselves `GC_MALLOC`'d) and reclaims it
 * once nothing does. Nothing here needs registering, destroying, or accounting for by hand.
 *
 * **What this replaced, and why that mattered.** Two designs preceded it. The first was a `__thread`
 * "outbound" arena, created once per thread and never destroyed, which leaked every crossing value
 * for a thread's whole life. The second -- the one this replaces -- gave every crossing its own
 * dedicated `arena_create_scanned` arena, destroyed the moment the receiving side had its own copy.
 * That was correct and promptly reclaimed, but it made *one scanned arena per value crossing a
 * `Channel`*, and a scanned arena is conservatively scanned root memory. Three separate ceilings
 * followed from that one decision, each discovered as a hard failure in turn and each documented in
 * docs/design/journal/: Boehm's `MAX_ROOT_SETS` table (2048 ranges, `GC_add_roots_inner` `abort()`s the
 * process) filled up; four distinct leaks of such arenas were found and fixed, each initially
 * mistaken for the whole problem; and once the root-set ceiling was lifted by pushing those ranges
 * from `GC_set_push_other_roots` instead, Boehm's *mark stack* (4096 entries, which it declines to
 * grow while parallel marking is on) overflowed and livelocked the collector instead. Every one of
 * those was a consequence of root ranges scaling with queued messages rather than with threads.
 * Allocating the copy where Boehm can simply trace it removes the whole category: a queued message
 * costs a few `GC_MALLOC` blocks sized to the value, instead of an `mmap`'d 256KB block, a root
 * registration, and a mark-stack entry.
 *
 * `Map`/`Set` build their wrapper and leaves through `mapobj_empty_crossing`/`mapobj_set_crossing`
 * (mapobj.h) for the same reason everything else here avoids this thread's own constructors; their
 * `MapNode` trie was never thread-tied and is untouched. A `value_copy_receive` of one naturally
 * rebuilds the trie a second time, accepted as a minor, rare-case cost rather than a reason to
 * track finer-grained per-node ownership.
 *
 * One deliberate exception, in both directions: a `Channel` value itself is never deep-copied, even
 * when it appears nested inside something that is (a struct field, an array element -- see
 * docs/roadmap/'s own `struct Node { peers: Channel<Node> }` cycle example). A channel is the
 * language's one identity-based, intentionally-shared-across-threads handle; copying it would
 * silently disconnect two ends of what the program meant to be a single conduit. */
Value value_copy_send(Value v);

/* The other side of `value_copy_send`: copies `v` (whose own memory is the sending thread's own
 * `value_copy_send` copy, on Boehm's shared heap) into the *calling* thread's ordinary allocators,
 * so that from this thread's own perspective the result is indistinguishable from a value it built
 * itself -- participating in its own `gc_heap`'s collections for the kinds migrated there. Frees
 * nothing and owns nothing: the copy it reads from is ordinary garbage afterwards.
 *
 * Only meaningful for a value `value_copy_send` actually copied. A scalar or a `Channel` crosses as
 * itself, so calling this on one is harmless but pointless; every caller here simply calls it for
 * every crossing value rather than tracking which. */
Value value_copy_receive(Value v);

#endif
