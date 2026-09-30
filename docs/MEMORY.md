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
- Mark-sweep, not a copying/compacting collector — settled, not provisional (2026-09-30): a
  conservative stack scan's own roots can never safely be relocated (rewriting an ambiguous stack
  word that turns out not to be a real pointer would corrupt real data), the same wall Boehm itself
  hits, resolved the same way. See "Phase 3 switches from copying to mark-sweep" in Status below —
  an earlier note in this same document (before that finding) had provisionally called this "a
  first version," with copying floated as a later refinement; it no longer is one.

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

**Resolution, now implemented (see Status below)**: a crossing-point deep copy is not allowed to
land in *either* thread's own per-thread working arena. It goes to a second, per-thread *outbound*
arena instead — created lazily, never destroyed, so each thread's own crossing-point copies survive
regardless of that thread's own working-arena lifecycle, at the cost of leaking exactly the values
that cross a thread boundary, for now. This is the honest, narrow version of "outside any single
thread's collected region" the open question above already called for; a real reclamation story for
these specific values (most plausibly a small, dedicated refcount just for what crosses a channel or a
spawn boundary, not the general refcounting design already rejected for the whole value graph) is
follow-up work once the rest of this design exists, not a Phase 2 blocker.

### A constraint found implementing Phase 2, binding on every future arena, not just the outbound one

**Every per-thread arena this design ever creates must be a *scanned* one (registered with the
collector via `GC_add_roots`), not a plain `mmap`'d region invisible to it — this interpreter
already relies on Boehm's conservative scanning to keep native resource handles alive, and any
arena that isn't scanned silently breaks that the moment a handle-carrying value lands in it.**
Found implementing the outbound arena below, the hard way: `admin_test.py`
(`health-record-backend-ziman`) hung, then failed outright with "invalid or closed connection
handle," the first time a database connection crossed a `spawn`/`Channel` boundary through it. This
codebase represents a native handle (`src/native_sqlite.c`'s `DbConn`, and presumably every other
`HandleSet`-based native resource — sockets, files, TLS sessions) as the handle object's own heap
address, cast to a plain `int64_t` and carried inside an ordinary `Value`. Boehm keeps the
underlying object alive *only* because that address, in bit-pattern form, sits somewhere its
conservative scanner looks — the C stack, or another `GC_MALLOC`'d block. An arena's `mmap`'d memory
is invisible to that scanner by construction (the entire point of not using `GC_MALLOC`); a value
copied into an unscanned arena still numerically encodes the same address, but nothing keeps the
real object alive anymore once every *scanned* copy of that same integer is gone — the collector
correctly (by its own rules) sees no reachable reference left and reclaims it, finalizing (closing)
the resource a program, through the arena copy, still believes is open. See DESIGN.md's own entry
on this for the full incident, including a methodological note: the first reproduction attempt hit
a stale server process left over from earlier debugging squatting on the test's fixed port, a
self-inflicted red herring that had to be ruled out before the real bug was even visible.

This is not specific to `value_deep_copy` or to values that cross threads — it applies to **any**
value an arena ever holds. Phase 2's remaining work (routing *ordinary*, non-crossing allocation
into a per-thread working arena) will need the scanned variant too, or a program that just calls
`sqlite.open()` and keeps using the connection normally, never spawning anything, would break the
same way the moment that arena's own contents include the connection. `arena_create_scanned`
(`src/arena.c`) is the fix already built and proven for the outbound arena; the working arena in
Phase 2's remaining step needs to use it from the start, not discover the same bug a second time.

### What this deliberately does not solve

- **A long-lived actor's own accumulating state** across many received messages still needs its
  own thread's collector to run periodically over its whole lifetime — this design handles that
  correctly (ordinary collection on that thread's own heap, on its own schedule), but "per-thread"
  means "collects independently," not "never collects."
- **Debugging/inspection tooling** (a future profiler or heap-dump, F6) will need to know about N
  independent heaps instead of one — not a blocker, a dependency to keep in mind later.

### The channel-buffered-value question is resolved, as a side effect of Phase 1/2

**A value sitting inside a `Channel`'s own queue, between `send` and `receive`, was flagged as an
open question (below, as originally written) before Phase 1/2 existed — it no longer is one.**
`send`'s deep copy (Phase 1) now lands in the sending thread's own *outbound* arena (Phase 2),
which is never destroyed. The value a `Channel`'s queue holds a pointer to is therefore already
living in memory nothing will ever free, regardless of how long it sits there before `receive` —
the exact same reasoning that makes it safe for a `spawn`ed thread to outlive its caller. Recorded
here so the resolution is traceable to the two already-shipped pieces that produced it, rather than
looking unresolved forever in a document that hasn't otherwise been revisited.

### Phase 3 design, worked through before writing any code

Three sub-problems turned out to be genuinely hard, not just "figure it out while implementing" —
resolved here on paper first, given what the native-handle bug (above) already cost from shipping
a smaller piece without this level of scrutiny.

**1. Object headers are necessary, and here's why precisely.** A `Value`'s pointer-bearing fields
(`as.arr`, `as.st`, `as.str.ptr`, ...) sit inside whatever holds the `Value` itself — a C local
variable on the stack, an `Env`'s binding array, another heap object's own fields. Conservative
stack scanning (the same technique used to find roots) finds a raw pointer-sized word that looks
like a valid arena address; it does not know what it points *to*. Ziman's own `Value.type` doesn't
help here, because `Value.type` lives wherever the `Value` itself is stored, not at the address the
pointer refers to. The only way to know "is this address an `ArrayObj`, a `StructObj`, a plain
string buffer, ..." once a pointer into the arena has been found is a header stored at that
address, prepended to the object's own payload by whatever allocates it. Layout: `{kind: enum (one
tag per traced shape, plus one for "atomic, never traced further" -- strings, `BigNum`/`Decimal`
digits, packed arrays), marked: bool}`, built as a thin layer on top of `arena_alloc` (`arena.c`
itself stays a general bump allocator with no opinion on headers), analogous to how `value_copy.c`
sits on top of it today rather than baking this into `arena.c` directly.

**2. The interior-pointer problem: this interpreter's own optimizations don't start allocations at
their own beginning, and a from-scratch collector doesn't get Boehm's answer for free.** A
substring/`trim`/`split` piece is a *view* into a parent string buffer (`value_string_view`), not
necessarily at its start; `array_obj_view` slices work the same way; `ArrayStore` tracks spare
capacity shared between `push` snapshots of the same growing array. Boehm handles all of this via
`ALL_INTERIOR_POINTERS` (its own compile-time option, already enabled in this project's vendored
build) — given *any* address, conservative or not, it can find which `GC_MALLOC`'d block contains
it, header included, via its own internal block-size bookkeeping. A custom arena doesn't have that
for free; building it (a sorted, binary-searchable index of every live allocation's range) is real,
additional machinery with its own correctness surface. **Resolution for a first version: don't
solve interior pointers at all — make Phase 3's collector a copying one, and copy a view's own
*bytes*, not the shared buffer.** A live string view surviving a collection gets exactly the `len`
bytes it actually uses, freshly copied into the new arena; a live array slice the same. This gives
up buffer-sharing *across* a collection cycle (a real, measurable cost if collections are frequent
and slicing is heavy) in exchange for a collector whose correctness doesn't depend on reproducing
Boehm's own interior-pointer bookkeeping. `ArrayStore`'s cross-collection sharing is given up the
same way -- a copied array starts a fresh `ArrayStore` of its own, same as `value_deep_copy` already
does for a value crossing a thread. Revisit only if this specific cost shows up in a real
measurement; building the general mechanism speculatively is exactly the kind of premature
optimization this project's own stated design principles argue against.

**Superseded (2026-09-30): Phase 3 is mark-sweep, not copying (see Status below) -- this resolution
was written before that pivot and described the wrong mechanism, though the underlying problem and
its actual fix turned out to still need solving, just differently.** A non-moving collector doesn't
copy a view's bytes fresh; it has to mark the *real, existing* buffer a view points into, so sweep
knows not to reclaim it. But `gc_set_marked` has the same "exact payload start only" contract
`gc_alloc` always had -- it cannot be called on an arbitrary interior address either. The actual fix:
`gc_lookup.c` (built for a different reason at the time -- resolving a *conservative stack scan's*
own candidate words) turns out to solve this too. Marking a `Value` of kind `TY_STRING` resolves
`v.as.str.ptr` through `gc_lookup` first, to the enclosing buffer's real start, before marking it --
the identical primitive, reused for a second purpose it wasn't originally built for, rather than a
second interior-pointer mechanism invented for marking specifically.

**3. A from-scratch collector inherits this codebase's own already-documented root-finding gap, and
must not repeat the mistake of trusting a scan that's known not to cover everything.** The reason
`t_defer_envs`/`t_env_pool` (interpreter.c) are `GC_MALLOC_UNCOLLECTABLE` today, not ordinary
`GC_MALLOC`, is that *this interpreter's own comments already say* the collector does not reliably
scan thread-local storage. A Phase 3 collector built around conservative C-stack scanning has the
identical blind spot for its *own* `__thread` state (the working arena pointer, `t_defer_envs`,
`t_env_pool`, `error.c`'s call-frame stack) unless those are walked as explicit, enumerated roots
in addition to the stack scan, not assumed to be reachable via it. This is the same *class* of
mistake as the native-handle bug — something silently invisible to whatever does the scanning —
recorded now, before it gets found the same way a second time.

**4. Root-finding reuses stack.c's own proven bounds computation, not new bookkeeping.**
`ziman_stack_slow`/`measure_this_thread` (`src/stack.c`) already computes a thread's real stack
range via `pthread_getattr_np` for the recursion-depth guard, on both the main thread and every
`spawn`ed one. It currently only exposes a margin-adjusted limit (`ziman_stack_limit`), not the raw
low address a conservative scan would need as its lower bound — Phase 3 needs a second accessor
for that, extending this file rather than re-deriving stack bounds independently elsewhere.

**5. Collection triggering reuses `arena_reserved`, already built.** A watermark check --
collect this thread's heap once `arena_reserved` has grown by some threshold since the last
collection -- is the same shape every bump/free-list allocator's own trigger already takes, and the
function it reads already exists (`src/arena.c`, built for exactly this in Phase 2's first
increment).

**6. The native-handle constraint (above) applies here unchanged.** The per-thread working arena
Phase 3 collects must be `arena_create_scanned`, for the identical reason the outbound arena is --
already fixed, already built, nothing new to design.

**Still genuinely open, deferred to the implementation pass itself rather than blocked on here:**
exact object-header size/alignment cost (settled since: 24 bytes, rounded to 32 for alignment --
`src/gc_object.c`); whether a thread's heap ever shrinks (settled since, partially: `gc_heap.c`'s
free lists let a dead object's space be reused by a later allocation, but its underlying `mmap`'d
block is never individually returned to the OS -- only a whole arena's `arena_destroy` does that,
same limitation `arena.c` always had); how `defer`'s own pending calls (`Env`s registered but not
yet run) get treated as roots precisely, given `run_defers` already has its own control flow around
them -- still open, not yet reached by the implementation.

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

**Phase 2, second increment done (2026-09-29): `value_deep_copy` now allocates into a real, safe
destination instead of the ordinary allocator.** Every thread gets a second arena besides its
eventual main one -- an *outbound* arena, created lazily on first use and never destroyed (deliberately: this is the "leak them for now" resolution the correction below already called
for, now actually built rather than just decided). Needs no lock despite outliving its own thread:
only the owning thread ever writes into it; every other thread only ever reads an already-built,
immutable value out of it afterward, the same reasoning that already let a `Value` cross a channel
with no locking before any of this existed. Every `Value` kind except `Map`/`Set` now has its own
arena-aware constructor in `src/value_copy.c` (`Map`/`Set` stay on the ordinary allocator for now --
an explicitly flagged gap, not a regression, since that's exactly how they were allocated before
this file existed). See docs/DESIGN.md's "`value_deep_copy` gets a real, safe destination" entry.

**Caught a real, serious bug before it shipped, from running the actual backend suite, not just
this interpreter's own tests: a plain `arena_create` silently breaks native resource handles.**
The design section above now has a permanent subsection on why ("A constraint found implementing
Phase 2, binding on every future arena, not just the outbound one") -- `arena_create_scanned`
(`src/arena.c`) is the fix, and every arena this design creates from here on needs to use it, not
just the outbound one. Verified directly, not just that the test suite went green again: a minimal
reproduction (`tests/fixtures/thread_crossing_native_handle.zm`) confirmed to fail with the exact
original error when temporarily reverted, and pass once reverted back.

**Sequencing decision (2026-09-30): skip Phase 2's remaining allocation-site rewiring, go straight
to Phase 3's design.** Realized before starting it: a working arena freed only "at thread exit"
(Phase 2's own original plan) does not help `server.zm`'s actual workload at all -- it is
thread-*per-connection*, and with keep-alive one thread serves thousands of requests before it ever
exits, so a thread-exit-freed arena reclaims nothing meaningfully sooner than today's shared
collector does for that exact case. It would only help a genuinely short-lived thread (a one-shot
`spawn`ed worker, a CLI script), and would be a large, risky rewiring (touching most of `value.c`/
`env.c`/`mapobj.c`'s own allocation sites) for no benefit on the concurrency benchmark until Phase 3
(real per-thread collection, which reclaims *during* a thread's life, not just at its end) also
exists on top of it. Rather than build that large a piece twice, or build it once with no payoff
yet, Phase 3's actual design came first.

**Phase 3 design substantially worked through (2026-09-30), not yet implemented.** Three sub-problems
that looked like implementation details turned out to need real resolution before any code: why
object headers are unavoidable (precise tracing needs to know an object's shape from a bare pointer,
which nothing else provides); the interior-pointer problem this interpreter's own view-sharing
optimizations create, resolved by making Phase 3 a *copying* collector that re-copies a view's own
bytes rather than reproducing Boehm's block-bookkeeping; and this codebase's own already-documented
gap (thread-local storage isn't reliably scanned) applying identically to a from-scratch collector's
own `__thread` state, meaning roots must be explicitly enumerated, not assumed found by a stack scan
alone. See "Phase 3 design, worked through before writing any code" above for the full reasoning,
plus two smaller pieces resolved by reuse rather than new design (`stack.c`'s existing stack-bounds
computation for root-finding, `arena_reserved` for the collection trigger). Still not started as
code, and still comparable in size to writing a small garbage collector from scratch — this is that
collector's actual shape now, not a placeholder description of one. Phase 2's own remaining
allocation-site-rewiring step is deferred until Phase 3 exists to make it worth doing at all; phases
4-5 (moving globals/AST to the permanent region, re-measuring against the concurrency benchmark)
remain design-only. Tracked as the concurrency half of F2 (docs/reviews/tasks.md).

**Phase 3, first implementation increment done (2026-09-30): object headers.** `src/gc_object.c`/
`.h` -- a thin layer on top of `arena.c`, giving every allocation a small header (`kind`, `size`,
`marked`) that the eventual mark/copy phases will read from a bare pointer found by a stack scan or
an already-in-progress trace. Nothing calls it yet; landed with its own test
(`tests/unit/gc_object_test.c`, `make test-gc-object`), same discipline `arena.c` itself was built
with. See docs/DESIGN.md's "Object headers, the foundation Phase 3's mark/copy phases will need"
entry for the seven-kind `GcKind` design and why `GC_VALUES` is its own kind rather than folded
into `GC_ARRAY`. The actual mark phase (walking a `Value` graph precisely by `type.kind`, using
these headers) and the copying/relocation phase are the next increments, still ahead.

**Phase 3, second implementation increment done (2026-09-30): the copy/relocate primitive, with
sharing preserved.** `src/gc_copy.c`/`.h` -- realized this is mostly `src/value_copy.c`'s own
per-kind recursive-copy logic already, generalized two ways: every structural allocation goes
through `gc_object.c`'s `gc_alloc` instead of bare `arena_alloc` (so the copy carries the header a
mark/sweep or trigger-side pass needs), and a dedup map (`GcDedup`, same hash/probe scheme as
`src/handles.c`'s `HandleSet`) preserves sharing across the copy -- needed here in a way it wasn't
for a one-off thread-crossing copy, since a value surviving many collection cycles over a thread's
life would otherwise duplicate a shared substructure a little more every cycle, compounding.
Deduplication is deliberately *not* applied to string bytes, a packed array's own buffers, or any
`Value[]` array (`boxed_items`/`fields`/`captured`) -- a second, broader instance of the
interior-pointer problem, found while designing this rather than while debugging it:
`array_obj_view` shares its parent's buffers at an offset the same way a substring shares its
parent's bytes, so the same address can mean a different logical value depending on the paired
length/count. Always copying these three shapes fresh, uniformly, sidesteps the ambiguity rather
than solving it -- the same tradeoff already accepted for strings, extended on purpose rather than
re-litigated per case. A packed array's own raw buffers never actually need this treatment in
practice, because `gc_copy_array` reads every element through `array_get` (which already resolves a
view's own offset) and always rebuilds the copy boxed, the same simplification `value_copy.c`
already made. See docs/DESIGN.md's "The copy/relocate primitive" entry for the full reasoning and
the standalone test's 35 checks (`tests/unit/gc_copy_test.c`, `make test-gc-copy`), including the
interior-pointer ambiguity exercised directly (a string and a same-start-address, shorter view of it
both copy to their own correct content) and sharing preservation verified directly (a struct/array
reachable from two places dedups to the same copied object, not two). Still not wired into anything
that runs a real collection -- no trigger, no root-finding, no relocation of an *existing* live
object (this builds fresh copies into an already-given destination; it doesn't yet decide when to
run, or where the roots come from). Those are the next increments.

**Phase 3, third implementation increment done (2026-09-30): resolving a bare address back to an
object (`src/gc_lookup.c`/`.h`), and a real gap in the design found while scoping root-finding, not
while debugging it.** Root-finding (still ahead) means a conservative scan of a thread's own C
stack, finding bare, untyped words with no length or kind attached -- unlike `gc_copy.c`'s own
tracing, which always starts from an already-typed `Value` with its `len`/`count` in hand and so
never needs to ask "what is this address." Working through what such a scan would actually
encounter surfaced a case the interior-pointer resolution above didn't cover: `Value.as.str.ptr` (a
string view sharing a parent buffer at an offset) is embedded directly in the `Value` union with no
wrapper object around it, unlike every other heap-referencing field, which is always a pointer to
its own wrapper's own exact `gc_alloc`'d start. A transient `Value` holding such a view, sitting
only as a C-stack local mid-evaluation, would present a scanner with exactly this bare interior
pointer -- and without a way to resolve it back to the buffer it lives in, a stack scan would fail
to recognize that buffer as reachable at all, discarding memory a live view still points into. A
real memory-safety gap, not a missed optimization.

Resolved without reproducing Boehm's own general interior-pointer machinery: `gc_lookup` only needs
to make an exact match *and* this one shape work, since everything else already resolves by exact
match through ordinary dereferencing once its wrapper is found. Built with no separate index: `gc_alloc`'s
own bump allocation already lays objects out contiguously and self-describingly, so `gc_lookup` just
walks an arena's blocks (new arena.c accessors: `arena_first_block`/`arena_next_block`/
`arena_block_content`) one object at a time (new gc_object.c primitive: `gc_next_object`) until it
finds the one whose span contains the candidate. `arena_block_content` has to skip the one block
that embeds the `Arena` struct itself past its own reserved bytes -- identified by a property already
true of the existing code (exactly one block ever has `next == NULL`), not a new invariant added for
this. See DESIGN.md's "Resolving a bare stack word back to an object" entry for the full reasoning
and the standalone test's 18 checks (`tests/unit/gc_lookup_test.c`, `make test-gc-lookup`), including
exact and interior matches, addresses that must resolve to nothing, alignment-padding correctness,
and walking across multiple `mmap`'d blocks. Still ahead: the actual stack scan (using `stack.c`'s
existing bounds computation to find the candidate words in the first place), the collection trigger,
and wiring a full cycle end to end.

**Phase 3, fourth implementation increment done (2026-09-30): the conservative stack scan itself**
(`src/gc_scan.c`/`.h`, `gc_scan_stack_roots`) -- scans the *calling* thread's own C stack, word by
word, from (approximately) the current stack pointer up to the thread's real upper bound, feeding
each word through `gc_lookup` and reporting whatever resolves to a real object. Always the calling
thread's own stack, never another's: this design's entire premise (one heap per thread, each
collected on its own time) means there is never a reason to pause or inspect any other thread's
stack the way a stop-the-world collector would, so no locking or thread-suspension machinery exists
here at all, or ever needs to. Reused this codebase's own existing "address of a local is the stack
pointer" idiom (`stack.h`'s `ZIMAN_STACK_CHECK`/`ziman_stack_remaining`) rather than a new technique,
and a new `stack.c` accessor (`ziman_stack_bounds`) for the real bounds -- deliberately not
`ziman_stack_limit`, the recursion guard's own margin-adjusted limit.

**Worth recording plainly: this is the one module in the whole effort that cannot be fully
`valgrind`-clean, and that is inherent to the technique, not a lapse.** Conservative scanning reads
stack memory the current call chain may never have written (a slot left over from an earlier,
unrelated, already-returned call) and treats it as a *possible* pointer -- `valgrind` correctly flags
that as "uninitialised value used," the same interaction Boehm's own collector has with valgrind.
Verified this is the *only* thing found, not cover for something worse, by re-running with
`--undef-value-errors=no` (every other check -- invalid reads/writes, leaks -- still on): zero errors.
See DESIGN.md's "The actual conservative stack scan" entry for the full reasoning and the standalone
test's 6 checks (`tests/unit/gc_scan_test.c`, `make test-gc-scan`), run five times deliberately (stack
contents vary run to run) rather than trusted once, including the interior-pointer-as-root case
`gc_lookup.c` exists for.

**Phase 3 switches from copying to mark-sweep (2026-09-30) -- a design reversal, not a refinement,
found while scoping the collection trigger.** A conservative stack scan's own roots can never safely
be relocated: `gc_scan_stack_roots` cannot tell a real pointer from an integer that happens to look
like one, which is harmless for keeping something alive too long but not for *moving* it -- rewriting
an ambiguous stack slot that turns out not to have been a real pointer would silently corrupt real
data. Boehm hits this identical wall and resolves it the same way every mature conservative collector
does: never move anything. A cheaper-looking alternative -- walk this interpreter's own existing call-
frame tracking (`ziman_push_frame`/`error.c`) precisely instead of scanning the stack, restricting
collection to statement boundaries -- was worked through and found to fail on an ordinary case, not a
contrived one: in `f(x) + g(y)`, while `g(y)` runs (itself hitting its own safe points), `f(x)`'s
already-computed value sits only as a transient C local in the outer expression evaluator, invisible
to any `Env`-chain-only root walk. Fixing that would mean compiling to an explicit operand-stack VM
instead of tree-walking recursion -- far outside this effort's scope. Confirmed with the project owner
across two rounds of discussion before committing to mark-sweep, given how much of the remaining work
this decision reshapes.

**What survives the pivot unchanged**: `gc_object.c`'s header, including the `marked` bit (clearly
sized for this scheme already, before the pivot); `gc_scan.c`'s report-only `visit` callback (never
built to relocate anything, so nothing about it was wrong for the old design either); `gc_lookup.c`,
untouched. **What doesn't**: `gc_copy.c` is no longer *Phase 3's collector* (nothing moves under
mark-sweep), though its deep-copy-with-sharing-preserved logic remains correct and may still find a
use elsewhere (`value_copy.c`'s own thread-crossing copy, undecided, a separate question).

**`src/gc_heap.c`/`.h` (new): the one genuinely new mechanism mark-sweep needs** -- `arena.c` can only
bump-allocate and mass-free a whole arena at once, never reclaim one dead object's space individually,
which a non-moving collector needs constantly. `GcHeap` adds segregated free lists (by power-of-two
*total footprint*, header included, via a new `gc_header_span` accessor) on top of an internal
`arena_create_scanned` arena: `gc_heap_alloc` reuses a same-class freed chunk if one exists, else
carves fresh memory; `gc_heap_sweep` -- run after a mark phase (still ahead) -- walks every object via
the same `arena_block_content`/`gc_next_object` primitives `gc_lookup.c` already established,
reclaiming anything unmarked and clearing survivors' mark bits. A new `GcKind` value, `GC_FREE`, marks
a reclaimed chunk sitting on a free list -- `gc_lookup` resolving a stray stack word to one is a real,
if rare, possibility, and it must never be mistaken for a live, traceable object.

**A real bug caught by reasoning through the minimum size class before writing the sweep code, not by
testing it afterward**: a freed chunk's free-list "next" link has to live in the chunk's own payload
bytes (the standard trick), which doesn't exist at all for a near-zero-byte request -- a real shape
(`GC_VALUES`'s own empty-array case), not hypothetical. `gc_heap_alloc` now enforces a minimum
effective payload of `sizeof(void *)` before computing a size class, so even the smallest class always
has room for its own link once something in it is freed.

See DESIGN.md's "Phase 3 switches from copying to mark-sweep" entry for the full reasoning and the
standalone test's 17 checks (`tests/unit/gc_heap_test.c`, `make test-gc-heap`, using `gc_set_marked`
directly to simulate a mark phase that doesn't exist yet -- the same discipline every earlier layer
here was proven with first), all passing; `valgrind` clean; full suite (829 fixtures) unchanged on
both builds.

**The mark phase done (2026-09-30): `src/gc_mark.c`/`.h`, `gc_mark_value`.** Largely `gc_copy.c`'s own
per-kind traversal structure, with allocate-and-recurse replaced by an idempotent mark-and-recurse
(check `gc_is_marked`, return if already set, else `gc_set_marked` and recurse) -- the mark-bit
equivalent of `gc_copy.c`'s own dedup map, for the identical reason (a shared substructure reachable
from more than one place should not be retraced once per incoming reference). `gc_lookup.c`, built to
resolve a *conservative stack scan's* candidate words, turns out to be exactly the fix the interior-
pointer problem needs here too, for a reason `gc_copy.c` never had to face: marking can't sidestep an
interior pointer by copying fresh bytes of a known length (there is no copy being made at all) -- the
*real, existing* buffer a string view or array slice points into has to be found and marked, or
`gc_heap_sweep` reclaims it out from under a value still using it. Marking a string's `str.ptr`, or an
array's `boxed_items`/packed buffer, now resolves through `gc_lookup` first. This corrects the
"Phase 3 design" section above, whose original interior-pointer resolution ("make Phase 3 a copying
collector") is superseded by the mark-sweep pivot two entries up -- see the inline correction there.

**An explicitly flagged, not-yet-measured performance question**: `gc_lookup`'s own docs call it fine
for a stack scan's handful of calls per cycle, not necessarily for one call per string/view found
while tracing an entire live graph -- marking could be `O(objects × interior references)` in the
worst case. Left as-is until a real end-to-end cycle exists to measure against the actual concurrency
benchmark, not prematurely optimized on suspicion alone -- the same discipline this whole effort has
used throughout (profile first, tune second).

See DESIGN.md's "The mark phase" entry for the full reasoning, the standalone test's 18 checks
(`tests/unit/gc_mark_test.c`, `make test-gc-mark`, including the interior-pointer case and a shared-
substructure termination case) all passing, `valgrind` clean, full suite (829 fixtures) unchanged on
both builds.

**The first real end-to-end cycle done (2026-09-30): scan, mark, sweep, composed and proven
together.** `gc_mark_object` (new, `gc_mark.c`) is the bridge `gc_scan_stack_roots`'s bare-pointer
roots actually need -- it has no `Value`/`Type.kind` for `gc_mark_value` to dispatch on, only the
object's own `GcKind` header, so this dispatches on that instead, handing off to the same per-kind
functions once it reaches a structural kind. `gc_heap_collect` (new, `gc_heap.c`) wires scan → mark →
sweep into one call against a thread's own heap, skipping the whole cycle (not just the scan) if the
stack can't be measured, the same graceful degradation `ziman_stack_slow` already uses.

**Explicitly not yet a complete root set, built and tested anyway to prove the pieces compose first**:
this codebase's own thread-local state (`t_defer_envs`/`t_env_pool`, `error.c`'s call-frame stack) is
not reliably found by a conservative scan and isn't walked as an explicit root source yet -- the exact
gap "Phase 3 design" section 3 above already flagged before any of this was built. Not safe to wire
into the real interpreter until that closes.

**The actual milestone: a real graph -- array containing struct containing string, reachable through
one `volatile` stack root -- survives a genuine scan→mark→sweep cycle with its content fully intact,
while an unreferenced object's slot gets reclaimed and reused.** First point in this whole effort
where "does this actually work end to end" has a tested answer instead of a plan. A real test bug (not
a code bug) was caught building this: comparing a dead object's address to a local read *after*
`gc_heap_collect` forces the compiler to keep it live across that call, spilling it onto the exact
stack the scan reads -- deterministic, not the ordinary run-to-run staleness conservative-scan tests
already expect, which is what gave it away. Fixed by stashing the address in a file-scope `static`
instead, genuinely invisible to the scan.

See DESIGN.md's "`gc_mark_object`, and the first real end-to-end cycle" entry for the full story;
`gc_mark_test.c` grew to 24 checks, `gc_heap_test.c` to 22, both rerun five times (stack contents vary
run to run) with zero failures; `valgrind` clean (`--undef-value-errors=no` where conservative
scanning's own inherent noise applies, same as `gc_scan_test.c`); full suite (829 fixtures) unchanged
on both builds. Still ahead: the collection trigger (when to actually call `gc_heap_collect`), the
explicit thread-local root walk that closes the gap above, and wiring a real per-thread `GcHeap` into
the interpreter's own allocation sites.
