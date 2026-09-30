# Memory management and garbage collection

Where Ziman's memory story is today, why the current default (a shared Boehm-collected heap)
doesn't scale under concurrency, and the design for what replaces it: one independently-collected
heap per thread. This is a design document for work not yet started, not a record of something
shipped — see docs/DESIGN.md for what's actually built and measured so far (the collector becoming
the default build, its tuning, the concurrency diagnosis this document continues from).

## Where things stand

Ziman ships two builds: `ziman` (every allocation routed through the vendored Boehm-Demers-Weiser
collector, `vendor/bdwgc`) and `ziman-nogc` (the original design — nothing is ever freed, kept only
for measurement and platforms the collector doesn't support). The collector build is the default
specifically because the no-free design leaks unboundedly the moment a program is meant to run
forever rather than execute-and-exit — see docs/DESIGN.md's "The collector is now the default
build" entry for the measurements that made that decision.

**The collector build genuinely does not leak.** Re-verified directly (2026-09-29): 10,000
requests against the real sample backend, each one permanently adding a row, held the collector
build flat at 65 MB (settling after an initial ramp-up) while the same workload against
`ziman-nogc` grew linearly and unbounded to 3.2 GB over the identical requests. The collector is
doing its job.

**It is not slow for a single client, either.** Same endpoint, single-threaded: median latency is
1 ms either way; the collector build costs about 1.3x on raw throughput (670 vs. 897 req/s) and
worse tail latency (p99 10 ms vs. 2 ms, max 13 ms vs. 3 ms) — the signature of an occasional pause,
not a systemic cost. Fine for ordinary traffic.

**It does not scale with concurrent clients, and that is the open problem.**
`tests/bench/server_concurrency.sh` against the real backend: the collector build peaks around
c=4 and then *declines* as concurrency rises further (c=16 is worse than c=1), while `ziman-nogc`
climbs smoothly the whole way and is several times faster by c=16. This isn't lock contention on
the allocation fast path — confirmed by running with `GC_PRINT_STATS=1` at c=1 vs. c=16 on
identical workloads:

| concurrency | collections | avg pause | total pause time | % of wall-clock paused |
| --- | --- | --- | --- | --- |
| c=1 | 128 | 8.0 ms | 1,023 ms | ~21% of a 4.8 s run |
| c=16 | 114 | 14.9 ms | 1,695 ms | ~30% of a 5.6 s run |

Two effects compound as client count rises: each stop-the-world collection takes longer (more
concurrently-live thread stacks and data to trace), *and* every collection freezes every
concurrently-handling thread at once, not just one — the "opportunity cost" of a single pause
scales directly with client count on top of the pause itself getting more expensive. That
compounding is what produces a decline rather than a plateau. Raising the initial heap
(`GC_INITIAL_HEAP_SIZE=512M`) narrows the gap a lot (a free, already-recommended tuning knob — see
docs/DESIGN.md's tuning-pass entry) but does not remove the shape: still peaks early, still
declines past it, just at higher absolute numbers. This session's raw-dispatch-speed work (slot
resolution, call-dispatch caching, per-thread `Env` pooling — see docs/DESIGN.md) also measurably
helped here as a side effect of allocating less per call, narrowing the gap at c=16 from 7.1x to
about 3.5x combined with the heap tuning — real, free progress, but the underlying structural
problem (one shared heap, stopped for everyone, however briefly, to collect anyone's garbage) is
unaffected by either lever. Closing it needs a different heap architecture, not more tuning.

## Prior approaches considered and revised

Two designs were proposed earlier in the project's history (docs/ROADMAP.md's "Larger, needs its
own design pass" section carries the original reasoning and the corrections below in place, kept
for the history rather than deleted):

**Pure reference counting** — proposed on the reasoning that Ziman's values are immutable and
built strictly bottom-up, so the value graph could never contain a cycle, which would make
refcounting sufficient with no cycle collector needed (unlike Python, which needs one precisely
because its object graph is mutable). **Wrong**: `Channel<T>` is mutable in place, so
`struct Node { peers: Channel<Node> }` with the node sent into its own channel is a genuine,
reproduced cycle. Refcounting alone isn't sufficient without excluding channels from the counted
graph or giving them weak references — a real complication, not fatal, but the premise that made
refcounting attractively simple doesn't hold.

**Per-thread arenas, freed when the thread's work is done** — proposed as an Erlang/BEAM-style
fix: give each thread its own region, bulk-free the whole thing in one shot instead of tracing
anything. **Wrong at the proposed granularity**: `server.zm` is thread-*per-connection*, and with
keep-alive one thread serves thousands of requests before disconnecting, so a per-thread arena
wouldn't free anything until the client disconnects — not meaningfully different from never. The
natural fix was "per-request, not per-thread," with an explicit escape rule for state that
deliberately outlives one request. **That in turn is the wrong generalization for the language as
a whole**: "request" is a concept `server.zm` invented, not something every Ziman thread has. A
`spawn`ed worker consuming a `Channel` forever, a batch job, or a single-threaded CLI script has no
request to hang a reset point on — a design that only reclaims memory for HTTP handlers isn't a
language-level fix, it's a library-specific workaround wearing a language-level costume. The
corrected target, below, generalizes to *any* thread doing *any* kind of work, with zero
dependency on what that work is.

## What Python's experience actually shows

Worth being precise about this rather than hand-waving a comparison. CPython's threads don't hit
this problem, but not because CPython solved it — the GIL means only one thread ever executes
Python bytecode at a time, so refcount mutation never needs to be thread-safe in the first place;
there's no concurrent allocation to contend over because there's no concurrent execution. Where
CPython wants real parallelism, it uses separate OS *processes* (`multiprocessing`), each an
entirely independent interpreter with its own heap — nothing shared, data serialized (`pickle`)
across the boundary when it needs to cross. That's the same shape as thread-owned heaps plus
copy-on-send, just enforced by the OS at the process boundary instead of by the language at the
thread boundary.

The instructive data point is Python 3.13's opt-in free-threaded build (PEP 703), which removes
the GIL and had to make refcounts thread-safe for *any* object reachable from more than one thread
— atomic/biased reference counts, per-thread allocation caches, years of work, and a real
single-threaded performance cost that persists specifically because Python's semantics allow any
object to be mutated and shared by pointer from any thread. Ziman does not have that problem: a
`Value` is immutable once built, and the language already refuses to let a mutable thing
(`#[thread_bound]` types) cross a thread boundary at all. The only place two threads legitimately
touch the same data is enumerated below, and it's short. Ziman gets to skip the expensive part of
what CPython's free-threading effort had to solve, because the language never allowed the general
case that made it expensive.

## Design: one heap per thread

### The invariant everything else depends on

There are exactly three places a `Value` can cross from one thread to another in the language as
it exists today, and no others:

1. **`spawn f(args) on_error handler`** — `args` and `handler` are evaluated on the *calling*
   thread (SYNTAX.md is explicit about this), then handed to the new thread to run with.
2. **`Channel<T>` send/receive** — the language's one designed-in concurrency primitive.
3. Nothing else. `ThreadLocal<T>` never crosses (`get`/`set` only ever touch the calling thread's
   own slot); `#[thread_bound]` types are refused at compile time anywhere they could cross; a
   `spawn`ed function must return `void` (no join, no handle), so no result flows back except
   through (2).

Both (1) and (2) are today a raw pointer handoff, safe only because nothing is ever freed. Both
need to become a deep copy under this design — the *same* operation, two call sites, not two
different mechanisms. Since every value is immutable, the copy is behaviorally invisible; it's a
real cost paid once per crossing instead of never, which is the honest price of not sharing memory
across independently-collected heaps.

### Three kinds of memory, not two

- **Permanent, unmanaged region.** The AST (already never freed today, unchanged), module-level
  `val`s (built once before any thread starts, read-only forever, so they never need to belong to
  any thread's collected heap at all), string/byte literals, function and type tables. Plain
  `malloc`, no header, never touched by any collector — strictly less work than today, where this
  is mixed into the one heap Boehm has to scan.
- **One heap per thread**, main thread included (thread 0 owns one too, same as every spawned
  thread). Every `Value` payload a thread builds — `ArrayObj`, `StructObj`, `EnumObj`, `ResultObj`,
  `ClosureObj`, string buffers, `Env`s — lives here and only here. No other thread ever holds a
  pointer into it.
- **Native resource handles** (sockets, DB connections, TLS sessions — `handles.c`) are unaffected:
  already thread-owned by construction (A3), not part of the traced `Value` graph, with their own
  existing close/finalizer lifecycle.

### Collection needs no cross-thread coordination

A collection on thread A's own heap never pauses thread B, C, ... — not "a shorter pause than
today," *no pause at all* for anyone else, because nothing outside thread A's heap ever points
into it. Concretely:

- **Roots are found conservatively**, the same technique Boehm already uses (flush registers to
  the stack via `setjmp`, scan from the current stack pointer to the thread's recorded stack base)
  — but scoped to *one thread's own stack*, checked only against *one thread's own heap's* address
  range. This keeps the one genuinely hard part of precise GC in a C-hosted tree-walking
  interpreter (finding roots on the C stack without a shadow stack threaded through every
  `eval_expr` call site) solved the way it's already proven to work, rather than inventing a new
  answer to it.
- **Tracing from a confirmed root onward is precise, not conservative** — Ziman already knows the
  exact layout of every value kind, so the mark phase walks real fields once it's inside the heap,
  unlike Boehm's fully conservative approach (which has to guess for the whole process because it
  has no type information at all).
- The channel mutex that already serializes `send`/`receive` today is the only synchronization a
  deep-copy-on-crossing needs; no new cross-thread lock is introduced by collection itself.

### What has to change in the interpreter

- Every allocation site (`value.c`'s constructors, `env_new`, `array_obj_new`, string/map
  building) allocates from "the current thread's heap" instead of `GC_MALLOC`/`malloc` — a
  `__thread`-local heap-context pointer, mechanically the same pattern already established for
  `t_defer_envs`/`t_env_pool` (interpreter.c).
- Each heap object needs a small header (a mark bit, a type discriminant) so the mark phase knows
  how to walk it and the sweep phase knows what survived — new; Boehm gave this for free before.
- A collection trigger: a watermark (collect when this thread's heap has grown by some threshold
  since its last collection), checked at allocation time — the same shape any bump/free-list
  allocator's trigger already takes.
- `spawn`: deep-copy `args`/`handler` into the *new* thread's heap (ideally built directly there,
  not copied twice) before that thread's entry function runs.
- `send`/`receive`: deep-copy the value into the receiver's heap at the moment of the call.
- Mark-sweep for a first version, not a copying/compacting collector — no pointer-rewriting to get
  right yet. A copying collector is a plausible later refinement once precise mark-phase tracing
  already exists (it buys allocation locality and bump-pointer allocation), not a v1 requirement.

### A correction found while scoping the implementation, before writing the arena code

Phase 1 (below, already shipped) does the `value_deep_copy` for a crossing point on the *sending*
thread — `spawn`'s own `STMT_SPAWN` handler, and the `send`/`try_send` call sites — since under a
single shared heap it doesn't matter which thread's "allocation" a copy counts as, there's only one
heap. **That stops being true the moment per-thread arenas exist, and doing it the same way would
be a real bug, not a missed optimization.** An arena freed "at thread exit" (Phase 2's own
deliberately simple model) means: if the copy a `spawn` makes lives in the *calling* thread's
arena, and the calling thread finishes and exits before the *spawned* thread is done with that data
(genuinely unbounded — a long-lived actor might hold it forever), the calling thread's exit frees
the arena out from under a thread that's still reading it. The same reasoning applies to a value
sitting in a `Channel`'s queue, which is exactly the open question already recorded above — this is
that same question, arrived at from the allocator side instead of the collector side, confirming
it's real and not an artifact of the collection design specifically.

**Resolution for now**: a crossing-point deep copy is not allowed to land in *either* thread's own
per-thread arena. It goes to the permanent/unmanaged region instead (pulled forward from its own
later step below) — plain, unmanaged memory nothing ever frees through this mechanism, safe
regardless of either thread's arena lifecycle, at the cost of leaking exactly the values that cross
a thread boundary, for now. This is the honest, narrow version of "outside any single thread's
collected region" the open question above already called for; a real reclamation story for these
specific values (most plausibly a small, dedicated refcount just for what crosses a channel or a
spawn boundary, not the general refcounting design already rejected for the whole value graph) is
follow-up work once the rest of this design exists, not a Phase 2 blocker.

### What this deliberately does not solve

- **A long-lived actor's own accumulating state** across many received messages still needs its
  own thread's collector to run periodically over its whole lifetime — this design handles that
  correctly (ordinary collection on that thread's own heap, on its own schedule), but "per-thread"
  means "collects independently," not "never collects."
- **Debugging/inspection tooling** (a future profiler or heap-dump, F6) will need to know about N
  independent heaps instead of one — not a blocker, a dependency to keep in mind later.

### Open questions for the detailed implementation pass

- Exact object header layout and its per-allocation size cost.
- Whether `ArrayObj`'s separately-allocated backing buffer gets its own header or is treated as
  owned-by-parent for marking.
- Stack-base recording for the main thread vs. a `pthread`-created spawned thread — Boehm already
  solves this per-thread; the same technique needs porting, not inventing.
- Whether a thread's heap ever needs to shrink (return memory to the OS) or only grows and reuses
  freed space in place — deferred to measurement once there's something to measure.
- **A value sitting inside a `Channel`'s own queue, between `send` and `receive`, doesn't belong to
  either thread's heap** the way the crossing-points section above frames it — it's live in a
  genuinely shared structure for an indeterminate time (surfaced implementing Phase 1, below).
  Either the channel's buffered contents need to live outside any single thread's collected region
  (the same way the channel object itself will need to), or the sending thread's collector needs to
  treat its own pending sends as roots until delivery. Not resolved; needed before Phase 3 (the
  collector itself) can be correct, not just before it's fast.

## Status

**Phase 1 done (2026-09-29): deep-copy at the two crossing points.** `spawn`'s own `args`/`handler`
and `send`/`try_send` on a `Channel` now deep-copy (`value_deep_copy`, `src/value_copy.c`) instead
of handing across a bare pointer — see docs/DESIGN.md's "Deep-copy at the two thread-crossing
points" entry for the implementation, what it does and doesn't copy (a `Channel` value itself is
deliberately never deep-copied — see the invariant section above), and how it was verified safe to
land before any of the rest of this design exists (it cannot change observable output under
today's allocators, only what crossing costs).

**Phase 2, first increment done (2026-09-29): a standalone arena allocator, not yet wired into
anything.** `src/arena.c`/`.h` -- `mmap`-backed, bump-allocated, `arena_destroy` releases every
block in one shot. Nothing in the interpreter calls it yet; landed with its own test
(`tests/unit/arena_test.c`, `make test-arena`) before the much larger, riskier step of routing the
interpreter's own allocation sites through it, which is still ahead. See docs/DESIGN.md's "A
standalone arena allocator, landed inert before anything calls it" entry for what it caught (two
real bugs the test found before this went anywhere near the interpreter) and why it has to use
`mmap`/`munmap` rather than `malloc`/`free`.

**A correction found while scoping the rest of Phase 2, recorded above in this document's own
design section rather than only here**: a crossing-point copy (`value_deep_copy`'s output) cannot
be allocated from either thread's own per-thread arena once those exist, or an arena freed "at
thread exit" would free memory a still-running thread on the other end of a `spawn` or `Channel`
still needs. Resolved by routing those specific copies to the permanent/unmanaged region instead,
accepting that they leak for now -- see "A correction found while scoping the implementation,
before writing the arena code" above for the full reasoning. This has to land before allocation
sites get rewired to use per-thread arenas, not after, since `value_deep_copy` already exists.

Phases 2's remaining work (routing the interpreter's own allocation sites through a per-thread
arena, plus giving `value_deep_copy` its permanent-region destination instead of whatever the
ordinary constructors resolve to once they're arena-backed) and phases 3-5 (object headers +
conservative-stack-scan + mark-sweep, moving globals/AST to the permanent region, re-measuring
against the concurrency benchmark) are still design only, not started, not scoped into an estimate
— comparable in size to writing a small garbage collector from scratch, because that's what Phase 3
is: precise inside the heap, conservative on the stack (reusing Boehm's proven technique for that
one part), scoped to a single thread rather than the whole process. Tracked as the concurrency half
of F2 (docs/reviews/tasks.md).
