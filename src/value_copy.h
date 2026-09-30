#ifndef ZIMAN_VALUE_COPY_H
#define ZIMAN_VALUE_COPY_H

#include "arena.h"
#include "value.h"

/* A value with no heap allocation shared with `v` -- everything `v` points to, recursively, is
 * duplicated, not aliased. Needed at the two places a Value legitimately crosses from one thread to
 * another (a `spawn`'s own `args`/`handler`, evaluated on the calling thread but run on the new one;
 * `send`/`receive` on a `Channel`) -- a bare pointer copy would let the crossing value's memory
 * belong to no single thread's own allocator, unsafe once any allocator can reclaim independently
 * of another thread's own lifetime (see docs/MEMORY.md, "one heap per thread").
 *
 * The lifecycle, in two calls instead of one: `value_copy_send` (the sending thread) copies `v`
 * into a fresh, dedicated arena it creates on demand (`*arena_slot`, left NULL if nothing needed
 * one at all -- a scalar, a `Channel` shared by identity, or an all-scalar `Map`/`Set` already
 * rebuilt through the ordinary allocator); `value_copy_receive` (the *other* thread, once it has the
 * crossed value in hand) copies it again, this time into its own ordinary allocators (`GC_MALLOC`/
 * this thread's own `gc_heap`), after which nothing it holds references the first arena at all --
 * the caller is then free to `arena_destroy` that arena, exactly once, having called
 * `value_copy_receive` on every value that arena's own contents were ever passed through (see
 * `interpreter.c`'s `STMT_SPAWN`/`spawn_thread_entry` for the case where several values -- every
 * argument plus an `on_error` handler -- deliberately share one arena and must all be re-copied
 * before it is destroyed).
 *
 * This replaces an earlier design (a `__thread` "outbound" arena, created once per thread and never
 * destroyed at all, deliberately leaked pending exactly this two-call fix) that turned out to make a
 * real server hard-crash under real concurrent load: `arena_create_scanned` registers each arena's
 * blocks with Boehm via `GC_add_roots`, whose own root-set table has a fixed capacity
 * (`MAX_ROOT_SETS`) that a registration nobody ever calls `GC_remove_roots` for permanently
 * consumes -- a thread-per-connection server (`lib/http/server.zm`'s `serve`) leaked one such
 * registration forever per connection, and `GC_add_roots_inner` hard-`abort()`s the whole process
 * once that table fills (found running `tests/bench/server_concurrency.sh` for the first time since
 * any `gc_heap`-backed migration shipped -- reproducible at as low as 16 concurrent connections).
 * `value_copy_send`'s own dedicated, promptly-`arena_destroy`-ed arena per crossing never accumulates
 * this way: its `GC_add_roots` registration is undone (`GC_remove_roots`, inside `arena_destroy`)
 * the moment the receiving side is done with it, not at some later, indefinitely-deferred point.
 *
 * `*arena_slot` is lazily created (never eagerly, even when the caller expects to need one) so that
 * the overwhelmingly common case -- sending a scalar, or a `Channel` itself -- costs nothing beyond
 * an ordinary `Value` copy: no `mmap`, no `GC_add_roots` call, at all. Left NULL by
 * `value_copy_send` for exactly those cases; the caller checks this before deciding whether
 * `value_copy_receive` (and an eventual `arena_destroy`) is needed at all.
 *
 * `Map`/`Set` are the one kind not routed through the dedicated arena for their own trie nodes
 * (still the ordinary allocator, exactly as before this design) -- only a nested structural key or
 * value (a struct, an array, ...) would ever cause `*arena_slot` to become non-NULL for a `Map`/`Set`
 * top-level value, and in that case `value_copy_receive`'s own full re-copy naturally also rebuilds
 * the trie a second time; accepted as a minor, rare-case cost rather than a reason to track finer-
 * grained per-node ownership.
 *
 * One deliberate exception, in both directions: a `Channel` value itself is never deep-copied, even
 * when it appears nested inside something that is (a struct field, an array element -- see
 * docs/ROADMAP.md's own `struct Node { peers: Channel<Node> }` cycle example). A channel is the
 * language's one identity-based, intentionally-shared-across-threads handle; copying it would
 * silently disconnect two ends of what the program meant to be a single conduit. */
Value value_copy_send(Value v, Arena **arena_slot);

/* The other side of `value_copy_send`: copies `v` (whose own memory lives in an arena `arena_slot`
 * pointed to, some other thread's, or this thread's own -- see `value_copy_send`'s own comment)
 * into the *calling* thread's ordinary allocators. Does not touch or destroy any arena itself --
 * when several values share one (a spawn's own args plus its `on_error` handler), the caller must
 * call this once per value *before* destroying that shared arena, not interleaved with destroying
 * it partway through. Only meaningful to call at all when `value_copy_send` reported a non-NULL
 * arena for `v` in the first place; a value that never needed one (a scalar, a `Channel`) needs no
 * further copying and should just be used as `value_copy_send` returned it. */
Value value_copy_receive(Value v);

#endif
