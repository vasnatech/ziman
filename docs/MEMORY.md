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
(`tests/c/arena_test.c`, `make test-arena`) before the much larger, riskier step of routing the
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
reproduction (`tests/lang/thread_crossing_native_handle.zm`) confirmed to fail with the exact
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
(`tests/c/gc_object_test.c`, `make test-gc-object`), same discipline `arena.c` itself was built
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
the standalone test's 35 checks (`tests/c/gc_copy_test.c`, `make test-gc-copy`), including the
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
and the standalone test's 18 checks (`tests/c/gc_lookup_test.c`, `make test-gc-lookup`), including
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
test's 6 checks (`tests/c/gc_scan_test.c`, `make test-gc-scan`), run five times deliberately (stack
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
standalone test's 17 checks (`tests/c/gc_heap_test.c`, `make test-gc-heap`, using `gc_set_marked`
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
(`tests/c/gc_mark_test.c`, `make test-gc-mark`, including the interior-pointer case and a shared-
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
on both builds.

**The second root source, and a gap found broader than expected (2026-09-30): an `Env` chain.**
Working out exactly what "the explicit thread-local root walk" needed to cover, before writing it,
found the gap is wider than that name suggests. An `Env` (`env.h`) is never allocated in a thread's
`GcHeap` arena -- it lives on the ordinary allocator's own heap. The moment a `Value` is stored into a
binding, its pointer fields move off the C stack (where a conservative scan could see them as raw
words) into `Env.items`' own heap storage -- invisible to a scan that only reads stack bytes and never
dereferences what it finds. **This affects every ordinary variable in the interpreter, not just
`t_defer_envs`/`t_env_pool`**, which is the narrower shape originally named above; those two are a
real instance of the same underlying problem (an `Env` the collector's scan can't see into), but the
cause is structural, not specific to thread-local storage.

`src/gc_env_roots.c`/`.h` (new): `gc_mark_env_chain(const Arena *a, const Env *env)` marks every
`Value` bound in `env` and walks `->parent` iteratively up to wherever the chain terminates. Built and
tested standalone, on purpose, before touching `t_defer_envs`/`t_env_pool`/`error.c`'s frame tracking
or the interpreter at all -- the smallest correct piece of the fix first. Still ahead, not resolved
here: wiring this into `error.c`'s existing call-frame tracking (each active call's own `Env*`, not
just one handed to it directly), walking `t_defer_envs`/`t_env_pool` the same way, and `g_global_env`
once globals are ever routed through a `GcHeap` (Phase 4/5, still design-only). `gc_heap_collect`
itself is not yet updated to call this.

See DESIGN.md's "The second root source" entry for the full reasoning and the standalone test's 7
checks (`tests/c/gc_env_roots_test.c`, `make test-gc-env-roots`), all passing, `valgrind` clean,
full suite (829 fixtures) unchanged on both builds.

**`error.c`'s frame tracking now carries each call's own `Env*` (2026-09-30).** `ZimanFrame` gains a
`void *env` (untyped -- `error.c` is shared by the lexer/parser/type checker too, none of which need
`env.h`), set by a new `ziman_set_frame_env(void *env)` right after `call_function` creates its own
`Env` (its frame is already pushed by then, before argument binding) -- one new line at that one call
site. A native call's frame is left `NULL` (natives have no `Env`). Read back through
`ziman_frame_count()`/`ziman_frame_env_at(int i)` rather than exposing the frame array itself, since
`env` isn't contiguous across frames.

**The first change in this whole effort to touch `interpreter.c`/`error.c` themselves, not land as a
new, uncalled module** -- `ziman_set_frame_env` runs on every function call now, on the real hot path,
even though nothing yet reads what it stores. Verified accordingly, not just tested in isolation: full
suite (829 fixtures) unchanged on both builds, and the real `health-record-backend-ziman` suite (19
Python files, 823 checks, including `admin_test.py`'s own case) passes against a freshly built binary
-- this session's standing rule for anything touching interpreter behavior.

See DESIGN.md's "Wiring error.c's existing frame tracking" entry for the full reasoning. Still ahead:
actually calling `gc_mark_env_chain` for every frame this exposes, plus `g_global_env` and
`t_defer_envs`/`t_env_pool`, from within a real collection cycle -- `gc_heap_collect` itself is
unchanged by this increment, and the collection trigger and interpreter allocation-site wiring remain
fully ahead too.

**`gc_heap_collect` gains the seam for that second root source, without learning what it is
(2026-09-30).** A third parameter, `extra_roots(const Arena *a, void *ctx)` + `ctx`, called (if
non-`NULL`) after the stack scan and before sweeping. Not a direct call into `interpreter.c`'s own
`g_global_env`/`t_defer_envs`/`t_env_pool`, which are genuinely private (`static`/`static __thread`)
and which `gc_heap.c` has stayed decoupled from through every increment so far -- the same shape
`gc_scan_stack_roots`'s own `visit` callback already uses: `gc_heap_collect` runs whatever a caller
supplies, with no opinion on what it does. Every existing caller still passes `NULL` today, which is a
real, explicitly documented gap (`gc_heap.h`'s own comment says so plainly), not an oversight -- not
yet safe to wire into the real interpreter until a caller supplies a complete one.

Proven with a genuine case, not just a signature change: a string bound in an `Env`, referenced by
nothing else, survives a real `gc_heap_collect` cycle specifically because the new callback (calling
`gc_mark_env_chain`) runs -- the exact shape invisible to conservative scanning alone. See DESIGN.md's
"`gc_heap_collect` gains a seam" entry; `gc_heap_test.c` grew to 24 checks, rerun five times, zero
failures, `valgrind` clean, full suite (829 fixtures) unchanged on both builds. Still ahead: the actual
`interpreter.c`-side caller that supplies a complete `extra_roots` (global env, every frame's env,
`t_defer_envs`/`t_env_pool`), the collection trigger, and wiring a real per-thread `GcHeap` into the
interpreter's own allocation sites.

**The interpreter's own root sources, aggregated (2026-09-30): `src/gc_interp_roots.c`/`.h`,
`gc_mark_interpreter_roots`.** What `gc_heap_collect`'s `extra_roots` seam was waiting for -- marks
the global scope, every active call's own frame (`error.c`'s frame tracking), and `t_defer_envs`/
`t_env_pool`, in one call.

**A real practical constraint, not a style choice, shaped its signature**: `interpreter.c` cannot be
linked into a standalone unit test the way every other module in this effort has been -- it pulls in
every native module, the parser, the type checker, essentially the whole program. Reading
`g_global_env`/`t_defer_envs`/`t_env_pool` directly (genuinely private, `static`/`static __thread`)
would have made this function's own correctness untestable except by exercising the real interpreter
end to end. Fixed by taking that state as explicit parameters instead -- a synthetic `Env**`/count
pair a standalone test builds by hand exercises the identical code path a real `t_defer_envs`/
`t_env_pool` would. Active call frames are the one piece still read directly, via `error.c`'s already-
general `ziman_frame_count`/`ziman_frame_env_at` -- a different, lighter kind of coupling than
`interpreter.c`'s own internals. `interpreter.c`'s eventual wiring becomes a thin wrapper passing its
own state through, one line per field.

**A verification bar this function cannot fully clear on its own, said plainly**: the standalone test
proves the aggregation logic itself is correct against synthetic stand-ins; it cannot prove
`interpreter.c` will one day call it with the *right* state, since nothing calls it yet. That last mile
comes once a real per-thread `GcHeap`, a trigger, and the actual `call_function`/`interpret` wiring all
exist -- a fixture surviving a real, triggered collection will be the implicit end-to-end proof, the
same way `admin_test.py` was for the native-handle bug.

See DESIGN.md's "The interpreter's own root sources, aggregated" entry; standalone test 6 checks
(`tests/c/gc_interp_roots_test.c`, `make test-gc-interp-roots`) all passing, `valgrind` fully clean
(this test's `type_scalar`-only `Value`s allocate nothing, unlike every other real-`Value` test here),
full suite (829 fixtures) unchanged on both builds.

**The narrowest possible slice, wired for real -- and a real, live bug it caught before anything
shipped (2026-09-30).** `src/gc_thread_heap.c` (new) owns a per-thread `GcHeap*`; `value_string_copy`
(`value.c`) now allocates through it, deliberately the one, smallest, most self-contained allocation
site (docs/DESIGN.md's "narrowest possible slice"); a collection trigger checks a threshold at the top
of `exec_stmt` -- the one point in this interpreter's execution proven safe from the `EXPR_CALL`-style
temporary-buffer root-source gap (an `args` array built by plain `malloc`, holding `Value`s neither the
stack scan nor the `Env`-chain walk can see into, but fully unwound and freed by the time any
statement's own evaluation finishes -- verified by reading the code before writing any of this wiring,
not assumed).

**The bug**: `struct Opts { flag: bool }`, crossed through `spawn`, printed `true` instead of `false` --
deterministic, confirmed absent on the prior commit, confirmed to disappear when the trigger was
disabled with everything else left wired. **Root cause**: the mark phase assumed every structural
object it reached had been built by `gc_alloc` (a real header) -- false for a struct crossing `spawn`,
built by `value_copy.c`'s own bare `arena_alloc` with no header at all, and in fact false for *every*
structural object anywhere in the interpreter at the time, since only `value_string_copy` had been
migrated. `gc_set_marked`/`gc_is_marked`, called directly on such a pointer, wrote into whatever bytes
actually preceded it -- here, the struct's own adjacent `bool` field, flipping it. This was a latent,
universal gap in `gc_mark.c`'s per-kind functions from the moment `gc_mark_env_chain` could reach *any*
real `Env` binding, not something specific to thread-crossing; the crossing case only happened to be
the first path exercised. `mark_interior_capable` (strings, buffers) never showed this, because it
already resolves through `gc_lookup` first and only marks a confirmed, non-`NULL` result.

**The fix**: a new `is_gc_heap_object(a, ptr)` (`gc_lookup(a, ptr) == ptr`) gates every direct mark-bit
touch in `gc_mark_array`/`_struct`/`_result`/`_enum`/`_closure` and the boxed `BigNum`/`Decimal` cases.
When false, the object's own header is never touched, but its fields are still traced regardless -- a
plain, not-yet-migrated object can still hold a field that *is* real `gc_heap` content. Costs the
"already visited" idempotency guard for header-less objects specifically (safe: the graph is a DAG, so
this cannot loop, only possibly retrace a shared substructure) and adds a second, unmeasured `gc_lookup`
cost on top of the one already flagged for interior-pointer-capable buffers.

**Verified as a real fix**: the minimal repro (`Opts{flag:false}` through `spawn`) reproduces `true`
with the guards reverted, `false` restored, five runs each way. A permanent regression fixture,
`tests/lang/gc_collect_preserves_thread_crossed_value.zm` (a `spawn`ed worker reading 3,000 lines
via `fs.lines` to reliably cross the trigger threshold before checking its own received struct), does
the identical before/after discrimination -- the same discipline `thread_crossing_native_handle.zm`
already established for the earlier native-handle bug. Full suite 830/830 on both builds (829 plus this
fixture); the real `health-record-backend-ziman` suite (823 checks) passes; `valgrind` against the real
interpreter running the new fixture reports zero errors.

**A second gap found while root-causing this one, fixed proactively before it could bite separately**:
`t_heap` is `__thread`-reachable only -- the exact shape that already required
`GC_MALLOC_UNCOLLECTABLE` for `t_defer_envs`/`t_env_pool`. `gc_heap_create` now allocates the `GcHeap`
struct itself the same way.

See DESIGN.md's "A real, live bug caught by the narrowest possible slice" entry for the full story.
Still open: the `EXPR_CALL`-style temporary-buffer gap remains real for any future trigger point less
restrictive than "only at `exec_stmt`'s own entry"; migrating any allocation site beyond
`value_string_copy` will need the identical `is_gc_heap_object` care, not assumed safe by extension.

**That gap closed for real (2026-09-30), after migrating `struct_obj_new` (the natural next allocation
site) broke two fixtures immediately.** `net_dropped_socket_reclaimed` failed on a timing issue
(resolved below); `csv_extended` failed with `internal error: field 'cells' not found at runtime` --
real corruption, confirming the temporary-buffer gap the previous entry flagged as "still open" was
not theoretical. `EXPR_STRUCT_LIT`'s `fields[]` (and `EXPR_CALL`'s `args[]`, `EXPR_ARRAY_LIT`'s
`items[]`, `EXPR_WITH`'s `fields[]`) is a plain `malloc`'d buffer populated one element at a time by
repeated `eval_expr` calls; if evaluating one element recurses into a call, that call's own body can
trigger `maybe_collect` while earlier elements sit in the buffer, unrooted. The earlier "only at
`exec_stmt`'s own entry" analysis checked buffers never dangle *across* statement boundaries -- it
never considered a collection firing *mid-construction* via a nested call. **Never specific to
`struct_obj_new`**: the already-shipped `value_string_copy` wiring has the identical exposure, just
unlikely to be hit (a crossed string rarely shares an expression with a call-triggering sibling
argument the way structs constantly do in ordinary code).

**The audit stayed bounded by finding the real choke points, not by auditing every file.**
`grep -ln "native_call_closure\|call_closure_value" src/*.c` shows only three files ever call back
into Ziman at all (`prelude.c`, `iter.c`, `interpreter.c`), and `call_function` is never called
outside `interpreter.c` -- so guarding `call_closure_value`/`native_call_closure`'s own internal
`call_function` call protects every native callback automatically (including a native's own `args[]`,
which for natives specifically is *never* bound into an `Env` at all). `interpreter.c`'s own four
`EXPR_*` sites plus one inline `map_filter`/`set_filter` loop needed their own narrower guards. Six
guarded regions total: a new `__thread int t_gc_unsafe_depth`, `gc_unsafe_enter`/`gc_unsafe_leave`
incrementing/decrementing it, `maybe_collect` skipping entirely while nonzero.

**A second, milder bug from the same migration**: `net.Socket`'s own `StructObj` wrapper is now
`gc_heap`-allocated, into an arena Boehm also scans (`arena_create_scanned`). `gc_heap_sweep` reclaimed
a dead object's header but never zeroed its payload, so a disguised socket-handle address sitting in an
already-dead struct's own bytes kept looking live to Boehm's scan of that same arena. Fixed two ways:
`gc_heap_sweep` now zeroes a reclaimed object's payload before relinking it; a new
`ziman_force_collect()` lets `os.collect_garbage()` run a real Phase 3 collection first, cleaning up
this thread's own `gc_heap` content before Boehm's `GC_gcollect()` runs.

**A valgrind finding investigated, not glossed over**: the fixed binary still shows "Invalid read of
size 8" inside Boehm's own `GC_mark_from`, reading the thread's own stack, under
`--undef-value-errors=no`. Confirmed pre-existing, not new: `thread_crossing_native_handle.zm` (predates
all of this session's Phase 3 work) shows the identical pattern -- a known Boehm/valgrind interaction,
unrelated to Phase 3.

See DESIGN.md's "Closing the temporary-buffer gap for real" entry. Verified: full suite 830/830 on both
builds, stable across five repeated runs; all nine standalone `gc_*` unit tests pass; the real
`health-record-backend-ziman` suite (823 checks) passes; `valgrind` clean beyond the confirmed
pre-existing Boehm interaction.

`array_obj_new` migrated next (the wrapper only). A zero-fill fix landed first (`gc_heap_alloc`'s
reused-chunk path never cleared the free-list link written into a dead object's own payload bytes --
fixed, `GcHeap` itself also now `GC_MALLOC_UNCOLLECTABLE` for the same `__thread`-scanning reason as
`t_defer_envs`/`t_env_pool`). The migration itself segfaulted `spawn_defer_gc_stress.zm` --
root-caused via a precise `gdb` backtrace to a fundamental gap in the second root source itself:
`ziman_set_frame_env` only ever records a call's *top-level* `Env` (one call site, `call_function`),
and `gc_mark_env_chain` only ever walks a recorded `Env`'s `->parent` upward -- a variable declared
inside any nested `for`/`while`/`if`/block scope has been invisible to root-finding since this frame
tracking was first built. Fixed in `exec_stmts_in_scope` (the single function every nested scope's
execution funnels through): it now records the *currently executing* scope for the frame, not just
the call's own top one, restoring the saved value on exit. See DESIGN.md's "`array_obj_new` migrates,
and a fundamental gap..." entry. Verified against the exact crashing fixture (3 clean runs) and the
other fixture it broke (`regex_cache`, confirmed matching); full suite 830/830 stable across five runs
on both builds; all nine standalone `gc_*` unit tests pass.

Running the real `health-record-backend-ziman` suite next (the standing rule) failed all 823 checks at
once -- a third root-source gap, this time found by the app itself, not a synthetic fixture:
`ArrayBuilder`/`MapBuilder`/`SetBuilder`'s own backing buffer (a plain `malloc`'d buffer reached only
through a disguised `int64_t` handle in the builder's wrapper struct) is invisible to both existing
root sources -- an appended element was only as safe as whatever *else* independently referenced it,
true only for the loop-local that built it. `lib/http/server.zm`'s `routes<C>` builds an
`ArrayBuilder<Pattern>` across ~100+ routes at start-up; once `Pattern`'s own struct wrapper was
gc_heap-migrated, a real collection landing mid-loop swept every earlier-appended route pattern.
Fixed with a new seam, `gc_mark_register_struct_content_marker` (`gc_mark.c`/`.h`): a struct type can
register a content-marker by name, called unconditionally (never gated by the struct's own mark bit,
since a prior type-less stack-scan visit may have already set it) whenever the mark phase visits a
`TY_STRUCT` value of that name. `prelude.c` registers one for `"ArrayBuilder"` and one (shared) for
`"MapBuilder"`/`"SetBuilder"`. Ordinary (non-builder) `Map`/`Set` values were already safe -- a
pre-existing, documented `gc_mark.c` code path already walks every entry regardless of the `MapObj`'s
own header status; the gap was specific to the three builders' own disguised handle. See DESIGN.md's
"A third root-source gap..." entry. Verified: both minimal reproductions pass; the fix confirmed to
actually discriminate (reverted via `git stash`, reproduced the failure, restored); a new permanent
regression fixture (`gc_arraybuilder_contents_survive_collection.zm`) added and confirmed to
discriminate the same way; the real `health-record-backend-ziman` suite now passes in full (19 files,
~950 checks, sqlite mode); full suite 831/831 (with the new fixture) on both builds; all nine
standalone `gc_*` unit tests pass; `valgrind` clean beyond the same confirmed pre-existing Boehm
interaction (more occurrences only because `spawn_defer_gc_stress.zm` spawns 8 threads, each its own
call-stack context for the identical pattern).

Re-running the concurrency benchmark for the first time since any `gc_heap` migration shipped, the
server hard-`abort()`-ed at c=16: Boehm's own `GC_add_roots` root-set table has a fixed capacity
(`MAX_ROOT_SETS`, 2048), and two independent, pre-existing leaks both burned one entry forever, per
thread, with nothing ever calling `GC_remove_roots`. First: `ziman_thread_heap()`'s own per-thread
`GcHeap` was never destroyed at thread exit -- the one piece of `__thread` state missing its own
release call in `spawn_thread_entry`, unlike the env pool/defer scopes/call stack, which already had
theirs. Fixed directly (`ziman_thread_heap_release()`, safe because a `Channel`/`spawn` crossing
always deep-copies first, never sharing a dying thread's own heap objects).

Second, harder: `value_copy.c`'s own "outbound arena," documented as deliberately leaked since the
day it was built pending "a real reclamation story," was the other half. Given the severity and that
a real fix meant genuine design tradeoffs, asked how to proceed (raise `MAX_ROOT_SETS` as a stopgap,
design real reclamation, or stop and reassess) -- chose real reclamation. `value_deep_copy` (one
`__thread` arena, reused forever) became two functions: `value_copy_send`/`value_copy_receive`, each
crossing value (or group -- a spawn's args plus its `on_error` handler) getting its own dedicated
arena, lazily created only if something actually needs one (a scalar or a `Channel` costs nothing at
all), destroyed the moment the receiving side has its own independent copy. `ChannelObj`'s queue now
carries `{Value, owner_arena}` pairs; every receive-side function finalizes transparently, so no
receive-side interpreter.c call site needed to change. This surfaced one more genuine bug, found only
empirically (a debug print made it vanish -- the signature of a race, confirmed by ten clean reruns):
a spawned thread's pending `on_error` handler, once safely re-copied, was referenced by nothing but a
plain `malloc`'d struct until `thread_failed` read it -- safe by accident under the old design (Boehm
scans every `GC_MALLOC`'d block, no explicit rooting needed), a real gap the moment that string moved
onto this thread's own `gc_heap`. Fixed with `t_pending_handler`, a new root source mirroring
`t_defer_envs`/`t_env_pool`'s own shape exactly. See DESIGN.md's "A fourth root-source gap becomes a
hard crash" entry for the full story.

Verified: the fixture that caught the handler race passes ten runs in a row; both `try_send` failure
paths (closed, full) confirmed to report correctly and not leak their crossing arena
(`valgrind --leak-check=full`, 0 bytes definitely lost); full suite 831/831 stable across five runs on
both builds; all nine standalone `gc_*` unit tests pass; `valgrind` clean on the fixture this segment's
own bug was found through. **And, for the first time since any `gc_heap` migration shipped**:
`tests/bench/server_concurrency.sh` against the real `health-record-backend-ziman` app completes all
four concurrency levels with zero failures and no crash (c=1 474 req/s, c=4 773, c=8 689, c=16 454) --
fixing the crash was the prerequisite for measuring the benchmark at all, not a fix for the underlying
declining-past-its-peak throughput shape it was built to diagnose, which is still present and still
ahead.

"env_new/call frames next" turned out narrower than it sounded: re-reading every call site first
(rather than assuming), `call_function`'s own top-level scope and every loop kind already reuse one
scope across calls/iterations (`env_pool_acquire`/`release`, `env_reset`) -- the one unpooled,
per-execution scope left anywhere was `exec_block` (`if`/`else` bodies, bare blocks). Flagged the
re-scoping and chose pooling that (reusing the exact same `env_pool_acquire`/`release` pair
`call_function` already has) over a full Env-to-`gc_heap` redesign, then re-measuring before deciding
if more was needed. It was a one-line change; full suite and the real app both still pass.

Re-measuring found it didn't move the needle -- three concurrency-benchmark runs at c=16 (454, 432,
528 req/s) show more swing between consecutive runs of the *identical* binary than any effect from
the change itself. Rather than guess at another allocation site blind, profiled the real server under
real concurrent load with `perf record -g --call-graph dwarf` (wrapping the server as perf's own
child -- attaching to an already-running PID was silently restricted in this environment). Confirms
F2's own original diagnosis in current numbers: `GC_mark_from` alone is 18% of sampled CPU time;
Boehm's own locking/bookkeeping around it another ~26%; this session's own `gc_mark_*` functions
(triggered because string/struct/array *are* migrated now) another ~7% -- close to half of all CPU
time under this workload is garbage-collection-related. `env_define`/`env_lookup`/
`exec_stmts_in_scope` together are under 1.3% of the same profile, direct confirmation Env was never
where the volume lives. Caller attribution for Boehm's own allocation came back too fragmented across
many small call paths to single out one obvious next site the way string/struct/array each were in
turn. A second, separate, already-acknowledged cost (`__strcmp_avx2` + `native_lookup`, ~11-13%) is
F2's *other* half -- name-based native dispatch, not yet covered by `env_lookup_resolved`'s own fix
for ordinary variables -- explicitly out of scope here. See DESIGN.md's ""env_new/call frames" turns
out to be narrower..." entry for the full numbers.

Picked `result_obj_new`/`enum_obj_new` as the next migration anyway (the next simple wrapper
constructors still on the ordinary allocator; `gc_mark_result`/`gc_mark_enum` already handle both
kinds correctly, nothing had ever actually allocated one this way yet). Mechanical change, same as
`struct_obj_new`/`array_obj_new` -- and `csv_records.zm` segfaulted immediately. Root cause: `iter.c`'s
`FromState`/`ArrayIterState` (the native state behind `iter_from`/`iter_of_array`) hold whatever
Values their own closures/arrays were built with, reachable only through `it->state`, a plain
`calloc`'d struct invisible to both root sources -- the identical shape to `ArrayBuilder`'s own gap
two entries up, fixed the same way (`gc_mark_register_struct_content_marker`, registered for `"Iter"`
in `iter.c`).

That alone didn't fix it: the registered marker only fires from the *type-directed* path (needs a
Value's own Type to look the registration up by), and the outer `Iter` in a `for ... in <iter>` loop
(`STMT_FOR`, interpreter.c) is a bare C local, found only by the stack scan -- type-*less*, no way to
reach the registration. Tried binding it into the loop's own Env instead; this corrupted a completely
different program -- the type checker's own `env_lookup_resolved` had already assigned every binding
in that scope a fixed compile-time slot index with no knowledge an extra runtime-only binding would
ever exist, silently shifting every later binding's real slot by one. Caught by the failure's own
*shape* changing (garbage stack memory, not a `GC_FREE` header) -- a different signature is itself a
sign one bug got traded for another, not fixed. The real fix: `t_active_iters`, a `__thread` stack of
the `Iter`(s) a for-loop is currently driving on this thread, mirroring `t_defer_envs`'s own exact
shape, marked directly (type-directed, so the registered marker fires) rather than touching Env/slot
machinery at all. See DESIGN.md's "`result_obj_new`/`enum_obj_new` migrate..." entry for the full
story, including the precise `gdb`/`-O0` root-causing of both bugs.

Verified: the crashing fixture passes clean fifteen runs in a row; a new dedicated regression fixture
(`gc_iter_state_survives_collection.zm`) confirmed to discriminate; full suite 832/832 stable across
five runs on both builds; all nine standalone `gc_*` unit tests pass; the real
`health-record-backend-ziman` suite passes in full; `valgrind` clean on the new fixture beyond the
same pre-existing Boehm interaction (a separate, pre-existing, unrelated `getline`-buffer leak in
`csv_records.zm` was checked against the pre-migration baseline and confirmed to already exist there).

Took on `Map`/`Set` next -- a bigger, costlier step than the first four (`MapObj` wraps a persistent
hash trie, `MapNode`/`Leaf`, not a flat struct; needed a real new `GcKind` and mark-phase logic, not
just a `malloc`->`gc_heap_alloc` swap; uncertain payoff since only the 24-byte wrapper migrates, the
trie itself stays on the ordinary allocator). Flagged this explicitly before starting; chose to go
ahead. New `GC_MAP` kind, `gc_mark_map` (`gc_mark.c`) mirroring every other per-kind marker exactly,
wired into *both* dispatch paths from the start this time (a direct lesson from the `Iter` entry just
above: a kind reachable from only one path is a kind with a real gap). Four allocation sites migrated
(`mapobj_empty`/`set`/`remove`/`snapshot`); `mapobj_mut_new` (the builder-private MapObj behind
`MapBuilder`/`SetBuilder`) reasoned to need no migration, since it's never wrapped in a `TY_MAP` Value
directly.

That reasoning missed that `mapobj_mut_new` is just `return mapobj_empty();` -- the function just
migrated. Two fixtures (`map_set_builders.zm`, `http_endpoints.zm`) immediately started segfaulting:
`GC_realloc` crashing on a `root` pointer read out of an already-zeroed wrapper. Root cause: `prelude.c`'s
`mb_mark_contents` (the content-marker already registered for `MapBuilder`/`SetBuilder` since the
`ArrayBuilder` entry) only ever traced the map's *entries* -- it never marked the wrapper's *own*
header, because when it was written `MapObj` had no real header to mark. The third time this session
has found a native-handle-held Value invisible to both root sources (`ArrayBuilder`'s buffer, `Iter`'s
`FromState`/`ArrayIterState`, now this), but the first time the handle itself, not something it points
to, needed protecting. Fixed by reusing existing machinery: wrap `buf->m` in a throwaway `TY_MAP`
Value and call the already-public `gc_mark_value`, running `gc_mark_map`'s own header handling exactly
rather than duplicating it. See DESIGN.md's "`Map`/`Set`'s own wrapper migrates..." entry for the full
story, including a related pre-existing inconsistency found (not fixed, out of scope) in `gc_copy.c`'s
own dead-code `Map`/`Set` handling.

Verified: both broken fixtures pass clean, ten runs each; a new dedicated regression fixture
confirmed to discriminate; full suite 833/833 stable across five runs on both builds; all nine
standalone `gc_*` unit tests pass; the real `health-record-backend-ziman` suite passes in full;
`valgrind` clean beyond the same pre-existing Boehm interaction. The concurrency benchmark, run twice:
every level at or above the best of the three pre-migration runs, `c=8`/`c=16` by a margin clearly
outside the established noise band (689->802-843 req/s, 528->591 req/s) -- a real improvement,
consistent with this app's own dominant `Map`/`Set` use (routing, every JSON response) now allocating
off Boehm's shared, locked heap.

Still ahead: deciding what to migrate next -- `MapNode`/`Leaf` (the trie itself, where a map's real
memory lives, still unmigrated) is one candidate, though each "wrapper only" slice so far has left the
*next* layer as the obvious next question; `iter.c`/`value_copy.c`/`prelude.c`'s builders are now a
known, recurring shape to check for early rather than discover by crash each time: *any* native handle
holding a Value in its own state, reachable by neither root source, and (new this entry) *the handle
itself* needing protecting too, once what it disguises gains a real header.

Took on the one question the last three entries kept deferring: `pg_rows`/`mysql_rows`'s own
`"Too many root sets"` abort, the fourth distinct trigger of that symptom. The previous entry's own
guess at where it happened (the deliberate SQL syntax error, inferred from where the output stopped)
was simply wrong -- counting the expected output's lines back to the statements that print them puts
the failure in the five-round early-`break` loop instead, and `addr2line` settled it in one run.
Measured rather than reasoned about this time: a temporary shadow copy of Boehm's own root-set table
in `arena.c`, plus `GC_set_abort_func` hooked to dump the counters and a `backtrace()` at the abort,
showed the live registered-range count climbing **monotonically to 2039** (a real leak, not a peak),
ruled out two plausible Boehm-side mechanisms outright (`same_start_merge=0`, `remove_nomatch=0`),
and named the leaking call site directly: `lazy_arena` from `value_copy_send`.

Root cause: `db.zm`'s `buffer_page` pushes a whole 500-row cursor page into a `Channel<Option<Row>>`
before the iterator pulls the first row back out, and every one of those sends gets its own
dedicated, `GC_add_roots`-registered arena. Drained to the end they are all received and destroyed;
**left early, every row still in the buffer is stranded in a channel nothing will ever receive from
again, its arena registered forever** -- ~499 a round, five rounds, which is exactly the 2039
measured. The same shape as the already-fixed put-back-`None` leak, one layer out. Fixed in
`end_cursor`, the one place that knows the buffer will never be read again: it takes the buffer and
`try_receive`s it empty before sending `Request.EndCursor`. Peak live ranges for the whole fixture
dropped from 2039 to 500.

With the leak gone the fixture ran to the end and was still one line short, which turned out to be a
**separate, genuine pg driver bug the abort had been hiding**: `finish_page`'s "rows arrived before
the error" branch correctly returns `open: false` alongside the `deferred` error it still owes, but
`serve`'s `FetchCursor` handler guarded the deferred-error branch on `cursor_open &&` -- just set
false by that same step -- so the error was answered with an empty finished page and discarded.
`mysql.zm`'s own `serve` already had the correct form; pg was the outlier. Confirmed pre-existing on
the clean tree before touching it.

Verified: `make test-pg` 11/11 and `make test-mysql` 6/6, **both fully green for the first time in
this whole investigation**; suite 834/834; `fmt-check` clean; all nine `gc_*` unit tests pass. One
thing to flag: the `ziman-nogc` build now segfaults flakily on two or three fixtures per run, a
different subset each time, and does so identically at the clean `d1893c9` -- a separate pre-existing
regression (this file last recorded that build stable across five runs), none of the affected
fixtures touching `lib/db`.

Still ahead, and now demonstrated rather than suspected: a **fifth** trigger of the same symptom with
no leak in it at all. A single fully-drained cursor peaks at ~500 registered ranges, a quarter of
Boehm's 2048-entry table, simply because that is what a 500-row page buffered through a channel
costs; six threads each streaming and fully draining one cursor abort at `live=2039`. At one
`GC_add_roots` slot per queued message, `MAX_ROOT_SETS` is a hard concurrency ceiling on `Channel`
itself, not just where four leaks happened to surface. The fix that removes the ceiling instead of
the next leak: stop registering scanned arenas in Boehm's fixed-capacity static-root table and push
them from the `GC_set_push_other_roots` hook `gc_boehm_roots.c` already installs for `Env` -- an
uncapped, walk-a-list root source, machinery that already exists in this codebase.

Fixed the ceiling itself rather than waiting for the sixth leak to hit it. `arena_create_scanned` no
longer calls `GC_add_roots` per block: every live scanned arena goes on a global list that a new
`ziman_arena_push_scanned_roots` walks from the `GC_set_push_other_roots` callback
`gc_boehm_roots.c` already installs for `Env`, which has no capacity limit at all. `arena_destroy`
unlinks instead of `GC_remove_roots`, and `arena_reclaim_block`'s existing block-list unlink is now
the whole of what stops a block being scanned.

The care went into concurrency, not the list. Unlike a thread registry (which `gc_boehm_roots.c`
gets away with locking loosely because threads start and exit rarely), a scanned arena is created
and destroyed **per value crossing a `Channel`**, so a thread will be suspended mid-link or
mid-unlink by a collection constantly. Both are written so the forward chain the collector walks is
whole at every instant -- one release store each, `prev_scanned` only ever read by the locked
mutators -- and the same reasoning pushed one level down to `arena_alloc`, where `a->current` must
be published *after* `fresh->next`, or a collector would stop at `block_new`'s NULL and silently
miss every older block in the arena.

Two measured wins, both found by checking claims I had already written down and got wrong. Pushing
from a hook makes it possible to read each block's `used` live per collection and scan only the
bytes handed out, never the rest of the 256KB block a one-value crossing copy gets to itself:
**3.4-4.0s to 1.4-1.5s** on twelve streaming cursors. And the first version's singly-linked list
made unlinking walk for the predecessor -- quadratic, under one global lock: **6.5s to 1.5s** once
the links went both ways. Each is a real 2.4x-4.3x win; neither was the "never finished to seconds"
my first comments claimed. Lesson worth keeping: don't write the number down before producing it.

Verified with the thing that matters most for a GC change -- that it costs nothing when unneeded. At
four cursors (the only load both builds can run, being under the old ceiling) clean `bf8f908` and
this are indistinguishable; at sixteen `bf8f908` aborts in 4.35s and this completes.
`server_concurrency.sh` against the real app, both builds back to back, flat within noise (661/1315/
1382/958 vs 666/1324/1355/963 req/s, zero failures) -- an earlier better-looking set for this build
was just variance, caught by rebuilding the baseline instead of trusting numbers recorded here in an
older session. Suite 834/834; test-pg 11/11; test-mysql 6/6; nine `gc_*` unit tests; real app suite
in full (19 files, ~1000 checks); `fmt-check` clean; `valgrind` 0 bytes lost with byte-identical
error contexts to the clean build.

Still ahead, and newly visible because the crash no longer hides it: above ~14 concurrent streaming
cursors the process goes CPU-bound instead of aborting, in **Ziman's own** mark phase --
`gc_next_object`/`gc_lookup` at 20.7%/3.5% of a `perf` profile, resolving an address by walking an
arena's objects linearly. `ziman_arena_push_scanned_roots` is 4.9% of that same profile, so this is
not the new code. A pre-existing scalability wall nothing could previously reach; the obvious next
piece of work, and not a small one.

Went after the `gc_lookup` wall the last entry flagged. Measured it first: `is_gc_heap_object` asks
"is this pointer an object this `gc_heap` allocated" of essentially every pointer the mark phase
touches, and `gc_lookup` answered by walking the containing block's objects from its content start
-- **710,688 calls, 136 million object steps, 827 objects per answer** on a six-thread
cursor-streaming benchmark. The split decided the design: **138,708 of those answers were exact
matches and 20 were genuine interior pointers**, so all of the cost was the one question that needs
no walk. Simulated a memo cache first and the numbers talked me out of it (77% hit rate but 132k
evictions on 140k fills, and that is an upper bound since a real one must reset per collection).

Fix: **one bit per `ARENA_ALIGN` bytes of every arena block**, set by `arena_alloc`, reserved inline
between the block's header and its content. Exact match is one bit test; an interior address is a
short backwards scan for the nearest start, span then checked. Inline rather than a side table
means no second mmap, no block-to-bitmap registry, and nothing to free -- it is unmapped with its
block, so `arena_destroy`/`arena_reclaim_block` needed no new code. Costs 1/128th of a block.

**The honest result: it works and buys no wall-clock time.** `gc_next_object`/`gc_lookup` vanish
from the profile; twelve-thread user CPU drops **5.5-6.0s to 3.3-3.7s (~40%)**, matching their 37%
profile share -- but elapsed time is unchanged, because that benchmark waits on Postgres and the
freed CPU just goes idle. The real app's `server_concurrency.sh` is flat in both directions. A third
of the interpreter's CPU under this load returned, with no throughput number to show for it.

**The cliff was never gc_lookup.** 14 threads 1.7s, 15 never finish, identically with and without
the bitmap. Boehm's stats say it outright: **3,935 "Mark stack overflow; current size: 4096 entries"
and 7 completed collections** -- `GC_signal_mark_stack_overflow` deliberately skips setting
`GC_mark_stack_too_small` when `GC_parallel` is on, and "No room to copy back mark stack" (the only
other place that sets it) appears **zero** times, so the stack never grows and marking restarts
forever with the world stopped. `GC_MARKERS=1` runs the same case in 2.82s.

Worth recording why it appeared now: the trigger is root ranges from `push_other_roots`, one per
queued `Channel` message (15 cursors x 500 rows ~ 7,500 against a 4,096-entry stack). While scanned
arenas used `GC_add_roots`, `MAX_ROOT_SETS` capped that at 2,048 -- **below the mark stack, so the
abort was accidentally protecting the process from this livelock.** Last commit removed the cap and
exposed it; that is a cost of that fix, not an unrelated find.

Still ahead, and the same answer as the last two entries kept pointing at: stop giving every value
crossing a `Channel` its own `mmap`'d scanned arena. That one decision is what makes a queued
message cost 256KB of address space, a root-set slot, and now a mark-stack entry. The precedent is
already in-tree -- the payload-less-enum fix allocates its copy with plain `malloc`, Boehm-visible
through the channel queue, no arena and no root range. Extending that to every crossing copy would
take root ranges from one-per-queued-message to one-per-thread and retire most of this whole
investigation. It changes the copy path every crossing goes through, so it is its own work.

Changed the decision the last three entries all kept pointing at. `value_copy_send` gave every value
crossing a `Channel`/`spawn` its own `arena_create_scanned` arena -- `mmap`'d, registered as a
conservatively-scanned root range, destroyed once the receiver had its own copy. Correct, promptly
reclaimed, and the direct cause of three hard ceilings in a row (`MAX_ROOT_SETS`, four separate
leaks of such arenas, then Boehm's mark stack livelocking once that cap was lifted) because root
ranges scaled with **queued messages** rather than threads.

The requirement was never rooting. Where it is actually written down (`mapobj.h`, on the `_into`
pair that exists for this alone) it is exactly: a crossing copy must not live in a heap the
*sending* thread may destroy at exit before the receiver copies it out. Plain `malloc` --
`GC_MALLOC`, Boehm's shared heap, tied to no thread -- meets that, and better: the copy is
**traced** through whatever holds it (a `ChannelMsg` in a channel's `malloc`'d queue, a `SpawnArgs`)
rather than rooted wholesale. The precedent was already in-tree: the payload-less-enum fix had done
exactly this since the first leak.

Three things verified rather than assumed, since the arena-as-root had made all three moot:
the `spawn` handoff (Boehm's own `pthread_create` wrapper keeps `sa` in a `start_info` on the
*parent's stack* and `sem_wait`s until the child has taken it -- its own comment says so, so no
window exists); disguised native handles (still fine, `GC_MALLOC` blocks are conservatively scanned,
which the receive side has always relied on); and `gc_mark.c` (no change -- a crossing copy has no
header, so it is the existing `is_gc_heap_object == false` path, and `gc_mark_map`/`mark_leaf` were
already gated for exactly that).

**The livelock is gone**: 15/16/24 concurrent streaming cursors, every one of which previously never
finished, now take 2.24s/2.27s/3.99s with parallel marking on -- **0 mark stack overflows and 28
completed collections against 3,935 and 7.** And it is ~3x faster on channel-heavy code: 200,000
struct sends go 1.13s to 0.36s, with user CPU and RSS flat and the whole difference in **system
time, 1.26s to 0.10s** -- the per-crossing `mmap`/`munmap` pair gone.

Honest about the mixed case: on the twelve-thread cursor benchmark elapsed improves ~18% and sys
drops 2.5s to 1.6s, but **user CPU rises 4.1s to 5.3s** -- that workload keeps much more crossing
data live, so Boehm traces what a root range used to cover in one go. Total CPU roughly flat there;
the win is wall-clock plus the ceiling. Real app benchmark flat within noise both ways.

One real regression, in the build whose README already says it "never frees anything": `nogc`'s peak
RSS on that 200k-send program goes 296MB to 455MB, because `arena_destroy` had been a genuine
exception to that contract. The `gc` build is flat. Recorded, not fixed.

`arena_create_scanned` now has exactly one caller left, `gc_heap_create` -- scanned root ranges are
one per thread instead of one per queued message, which is what retires this whole line of
investigation.

Went back at the `ziman-nogc` flakiness I had written off as "pre-existing, 2-3 fixtures" three
entries running. That was wrong and worth the check: **every fixture tried segfaults about half the
time** (toml_basic, yaml_basic, map_set_builders, http_endpoints, csv_extended, regex_cache, each
2-3 crashes in 6 runs). The suite showed only 3-6 failures because most fixtures are too small to
reach it.

Root cause via `valgrind` on the nogc build, not by reasoning: `Invalid read` in
`gc_mark_env_chain` on a 72-byte block (`sizeof(Env)`) freed at `interpreter.c:1862`
(`STMT_WHILE`'s `env_free`). `env_pool_release` called `env_reset` but left `env->parent` pointing
at the scope the Env was last used under, and `gc_mark_env_chain` follows `parent` chains -- so the
pool, which is one of the interpreter's own GC root sources, held a pointer into freed memory for
the next collection to walk. Fix is one line: `env->parent = NULL` on release (acquire already
re-parents, so nothing observes the NULL).

**Why the real build never showed it** is the part to remember: a `make gc` build's `free` is a
no-op (zgc.h), so the dead scope was never reclaimed, *and* the stale parent kept it reachable so
Boehm never collected it either. Wrong ownership, correct behaviour, on two independent
coincidences. Only a real `free` exposes it -- which makes `make test-nogc` load-bearing rather
than a curiosity, and is the strongest argument yet for keeping it in the loop.

Verified: nogc **834/834 five runs in a row** (was 3-6 failures every run); the six crashing
fixtures now 0/8 crashes each; valgrind on yaml_basic under nogc down to 0 invalid reads and 0
use-after-free, leaving only the two documented `gc_lookup` conservative-scan contexts. gc build
unaffected and fully re-verified (suite 3x, test-pg, test-mysql, nine gc_* unit tests, real app
suite, fmt-check, valgrind unchanged).

Two negatives I checked rather than assumed. The retention this removes in the gc build is **not**
measurable (40.9MB peak RSS either way on a loop-heavy program -- pool is bounded, Boehm's 32MB
initial heap dominates), so there is no memory win to claim. And **no regression fixture was added**:
four shapes were tried, including the exact `return`-out-of-a-`while` that valgrind named, and none
reproduces even an invalid read on the buggy binary, so a fixture would have asserted coverage that
does not exist. `make test-nogc` is the coverage; the lesson is that I read its output as flakiness
for three entries instead of looking.

Took on the last unaddressed piece of F2's original profile -- `__strcmp_avx2` plus `native_lookup`,
about a fifth of CPU in every profile since. The cause was plain: `native_lookup` is a linear walk
over every registered native with a `strcmp` per entry, and `call_function` called it **on every
native call**. Measured: 247 natives registered, and one `yaml_basic` run makes 9,595 native calls
costing **1,964,293 string comparisons** (average scan depth 205) -- all but one per declaration
re-deriving an answer that cannot change, since the table is built before any Ziman code runs and a
`FunctionDecl`'s name is fixed.

Fix is the move `env_lookup_resolved` already made for variables, one layer over: `FunctionDecl`
gains `native_impl`, resolved on that declaration's first call and kept. Same run after: **23
lookups, 4,807 comparisons** -- 409x fewer, and the floor (one per distinct native used).

Two details: the field is a bare `void (*)(void)` because `ast.h` cannot include `native.h`
(cycle via value.h), and specifically a *function* pointer rather than `void *` since
function-pointer round-trips are defined where object-pointer ones are not -- `-Wpedantic` caught
the first attempt. The store/load pair is relaxed-atomic only to make the benign same-value race
between two threads well-defined.

Results: `__strcmp_avx2` (16.2%) and `native_lookup` (10.1%) -- **26.3% of CPU -- vanish** from a
csv_extended profile. A 3M-call `math.sqrt`/`cos` loop goes **2.12s to 0.75s (2.8x)**; csv_extended
0.85s to 0.65s (~24%); the real app's server benchmark +6% at c=4 and +2% at c=8.

**Worth remembering, because I nearly reported the opposite:** the first server-benchmark A/B looked
like a 14% *regression*, which is impossible for a change that only deletes comparisons. It was
noise. Building both binaries once and interleaving three rounds each showed the new build ahead in
five of six paired runs. This same benchmark had already produced a misleading non-interleaved pair
earlier in this investigation -- never trust a single non-interleaved pair from it.

`GC_mark_from` is now 46% of that profile (was 37.7%) purely because the denominator shrank. That is
the next thing in front of anyone profiling this interpreter.

Chased `GC_mark_from`'s 46% and it is **irreducible** -- worth recording as a negative result.
`ziman_arena_push_scanned_roots` hands Boehm the thread's whole `gc_heap` as a conservative root
range: measured at **~16 MiB per collection, 187 MiB in a 0.65s `csv_extended` run**. A heap census
shows that is real live data, not garbage -- the heap goes from 0.5 MiB to **57 MiB live / 403,336
objects** parsing a deliberately-large 20,000-row CSV into JSON, about ten objects per cell, which
is just what boxed immutable JSON costs. Also `atomic 0 KiB`: no pointer-free objects in the gc_heap
to exclude. So it is two collectors walking one big live graph, and Boehm must see inside those
objects (disguised native handles, pointers to still-`GC_MALLOC`'d kinds).

Three fixes ruled out with evidence, not argument: `GC_FREE_SPACE_DIVISOR` (swept 1-24, **13
collections at every setting** -- they are expansion-driven, and higher is strictly slower);
atomic-tagging gc_heap blocks (there are none); pushing live objects individually (140k-400k
`GC_push_all` calls against a 4096-entry mark stack = the livelock from two entries ago). This cost
goes away only by finishing the migration (strings, Env, closures, MapNode) until the arena needn't
be pushed at all. That is the remaining program, not a change.

**The win was elsewhere.** A decimal-heavy loop spends *nothing* in marking and **~42% in Boehm
allocation** (`GC_build_fl` 19.8 + `GC_malloc_kind` 14.7 + `GC_allochblk_nth` 7.4). Cause: in
`bignum.c`, `mag_divmod` finds each quotient limb by binary search over `[0, 1e9)` -- ~30 steps --
and every step called `mag_mul` to materialise `mid_limb * divisor` into two fresh `calloc`'d
arrays, read once and dropped. ~60 allocations per dividend limb for a product needing none. New
`mag_mul_limb` writes it into a caller buffer; `mag_divmod` now keeps **one** buffer per division
(on the stack when it fits) for all probes plus the final subtract. `mag_mul`'s scratch accumulator
moved to the stack too -- it is pure scratch, and `free` is a no-op in a collector build.

**Trap avoided by measuring first:** the obvious fix looked like `zalloc_atomic` for digit arrays
(`uint32_t`, never pointers). That would have been a straight bug -- it maps to `GC_MALLOC_ATOMIC`,
which does **not** zero, and every site is `calloc` because the algorithms need zeroed limbs. The
profile said allocation *volume*, not scan volume.

Results: decimal loop **1.18s -> 0.37s (3.2x)**, RSS flat; Boehm allocation 42% -> 4.3%;
`mag_mul_limb` now 45.8%, i.e. the arithmetic itself is on top where it belongs.

Verified with a real differential test, which this kind of change earns: both binaries kept, and
`tests/lang/bignum_mul_divmod_wide.zm` (238 lines of bigint/decimal mul, div and rem up to ~90
digits, 40 divisor sizes) gave **byte-identical output from the pre- and post-change binaries** --
that output is what the `.out` now locks in. Plus suite **835/835 both builds**, test-bignum,
test-decimal, nine gc_* tests, test-pg 11/11, test-mysql 6/6 (both drivers decode numerics through
this), real app suite, fmt-check, valgrind 0 bytes lost.

Went after the gc_heap migration and **abandoned it on evidence**. Nothing landed; the finding is
the deliverable. Two independent premises failed.

**1. It can never retire `GC_mark_from`, which is what I claimed last entry.** `handles.h` spells
the rule out: the live-handle table is deliberately unscanned, so "being in the set does not keep an
object alive". A socket is `GC_MALLOC`'d with a finalizer and handed to Ziman as
`(int64_t)(intptr_t)h` -- a conservative scan finding that integer is the *only* thing keeping it
alive. `struct_obj_new` is already migrated, so a `net.Socket`'s fields live in the gc_heap, so that
arena stays a Boehm root **permanently** however much else moves. The proof was already in-tree and
I had read past it twice: `gc_heap.c` records `net_dropped_socket_reclaimed.zm` failing exactly when
`struct_obj_new` first migrated. Worse, since the push covers `[content_offset, used)` per block,
migrating more *enlarges* what Boehm re-scans.

**2. A migrated object is measurably slower -- single-threaded and concurrent.** I built the proper
foundation first so the test would be fair: GcHeap split into the scanned arena plus a plain
`arena_create()` never registered with Boehm, holding only `GC_ATOMIC` objects, the two linked by a
new `arena_set_sibling` so `gc_lookup` finds either and all ~28 `const Arena *a` signatures (plus
the public content-marker callback) stayed untouched; free lists split per arena so an unscanned
chunk can never be handed to a pointer-bearing object. 836/836 both builds, nine gc_* tests, two
updated (they asserted cross-kind reuse, now forbidden) plus a new segregation check. Then:

- `format` into the atomic arena: **1.23s -> 1.38s (~10% slower)**, 4M formats.
- `ClosureObj` into the gc_heap: **1.12s -> 1.25s (~8-10% slower)**, 2M closure calls. `GC_CLOSURE`
  and `gc_mark_closure` were already written and unused -- the same mechanical step the four earlier
  migrations took.
- Same ClosureObj change on **8 threads: 5.3s -> 5.7s (~7% slower)**. This was the hypothesis most
  likely to save the strategy, since the one migration ever measured as a win (Map/Set, 689 ->
  802-843 req/s) was measured on the *concurrent* server benchmark where you buy relief from
  Boehm's locked heap. It still loses.

**Mechanism, explaining all three: a gc_heap object pays BOTH collectors** -- it is inside the range
Boehm conservatively re-scans as a root *and* marked/swept by Ziman's Phase 3. A Boehm-heap object
pays Boehm only. Bump+freelist beats `GC_MALLOC` per call, but not by enough to cover a second
collector walking the same object; for `GC_ATOMIC` it is worse still, since `GC_MALLOC_ATOMIC` is
already cheap *and* already unscanned.

What it does **not** say: the existing migrations weren't mistakes. Map/Set was a measured concurrent
win, and the string/struct/array/result/enum moves were made for a correctness reason (getting a
crossing value off an allocator the *sending* thread may destroy) that stands alone. What it says is
that **"migrate the rest" is not a performance strategy**, the 46% has no fix on this path, and the
real question for that cost is whether two collectors should walk one object graph at all -- which
nobody should open before disposing of the handle-retention constraint that pins the scan in place.

Profiled the call-heavy side of interpreter speed for the first time since the `Env` pool, now that
closing F2 had established the honest numbers (loop 1.75x CPython, calls ~8x). The top entry was
**`value_int` at 30%** -- a three-line function. The reason is what it moves:
**`sizeof(Value) = 80`, `sizeof(Type) = 64`**. Every `Value` carries a whole `Type` descriptor
inline and `Value` is the most-copied object in the interpreter; `type_scalar` zeroes 64 bytes
before filling in a 4-byte tag. Ziman is statically typed, so the runtime is paying 80 bytes per
value to carry what the checker already proved.

**What worked, and it was the smaller idea: inline the constructors.** No LTO in this build, so
`value_int` was a real cross-TU call returning 80 bytes through a hidden pointer and `type_scalar`
another returning 64. `static inline` for `type_scalar` plus the five scalar `value_*` constructors:
**fib(32) 1.11-1.18 -> 0.81-0.88s (~26%), loop_bench 0.26-0.29 -> 0.14-0.17s (~45%),
scalar_arrays 1.07-1.15 -> 0.79-0.89s (~25%)**, pinned, four runs each. The loop benchmark is now
0.15s against CPython's 0.16s -- parity on the microbenchmark F2 opened with at 28x.

**What failed, and this is the part to remember.** `Type`'s name fields are mutually exclusive by
kind, so unioning `struct_name`/`enum_name`/`type_var_name` is a pure layout change (C11 anonymous
union, every call site reads identically) taking `Type` to 48 and `Value` to 64. It passed 840/840.
It was **40-160% slower** -- loop 0.15 -> 0.40s, worse than baseline. `no_ctrl`, a one-line helper
returning `{CTRL_NONE, value_int(0)}`, became **43%** of the loop profile, and the disassembly shows
GCC bouncing the aggregate through half a dozen stack slots with `movdqa`/`movups` instead of
storing zeros into the return slot: **the union defeats scalar replacement of aggregates**, so a
struct previously kept in registers gets materialised through memory on every construction. Making
the struct smaller made the code that copies it much worse. Reverted.

Kept from the attempt, in its own commit, because both stand alone: `loader.c`'s five hand-copied
struct-to-enum reclassifications became one `retype_struct_as_enum` (two wrote the new name before
clearing the old, which the union would have turned into erasing it), and `gc_mark.c`'s
registered-content lookup is gated on kind rather than only on a non-NULL name.

**Measured honestly where it does not show:** `server_concurrency.sh` on the real backend is
unchanged -- five interleaved paired rounds, c=1 637 vs 639 req/s, c=8 992 vs 986. That app is
dominated by HTTP parsing, SQLite and the collector, so 25-45% off value construction has nothing to
surface through. Same shape as the `gc_lookup` bitmap earlier: a real CPU win the workload can't
convert.

Next, now justified rather than speculative: interning `Type` so `Value.type` is a pointer would
take `Value` 80 -> ~24 bytes. The copy cost is demonstrably real and large, which is the evidence
that refactor needed -- but it is hundreds of `.`-to-`->` edits, and the union result is a standing
warning that a smaller struct is not automatically faster code. Prototype and measure first.

Set out to build an honest Ziman-vs-CPython comparison table (five workloads, not F2's single loop)
and one row came back at **26.6s against CPython's 0.06s** -- 200k `MapBuilder` inserts plus 200k
lookups. Four hundred times is not a tree-walker gap, it is a bug.

**Not the map.** Scaling was quadratic (12.5k/25k/50k: 0.02/0.09/0.43s) but every layer of the map
is the right complexity -- `node_insert_mut` is a plain HAMT insert, O(log32 n), and the layers above
it add nothing. **It was `ZIMAN_GC_TRIGGER_BYTES`**, a flat 64KB of heap growth between collections,
whose own comment already said it was "an arbitrary, unmeasured starting point ... not to be a
considered performance choice". With one large live structure that makes the number of collections
proportional to the heap while each collection marks everything live: O(n) work, O(n) times.
Confirmed by raising it 100x as an experiment -- 100k inserts 1.25s -> 0.08s, curve straight.

Fix is to scale the threshold, not raise the constant (which only moves the cliff and spends memory
everywhere): `max(64KB floor, reserved/2)`, so each collection buys proportional headroom --
O(log n) collections, O(n) total work.

  MapBuilder 25k/50k/100k    0.03/0.18/1.25s -> 0.01/0.03/0.12s
  200k insert + 200k lookup  26.76s -> 0.81s   (33x)
  build-60k-and-drop x12     3.47s  -> 0.45s   (7.7x)

**Memory cost, measured because collecting less often is a trade:** +27% peak RSS on the
monotonically-growing map (98.9 -> 125.7MB); only +6.5% on grow-then-drop (55.2 -> 58.8MB) and
*bounded, not ratcheting*, because the threshold derives from reserved-at-last-collect, which shrinks
when `gc_heap_sweep` reclaims blocks -- so the trigger tightens as soon as the heap does; unchanged
(~41MB) on steady-state string churn. The README's "backend stays around 40MB" claim is intact.

Also found: `tests/bench/server_load.sh` does not run in this environment at all (its server dies at
startup) and fails identically on the pre-change binary -- pre-existing, its own small task, and the
one tool that would have measured the server's memory directly.

Real app throughput flat to +1-2% (three interleaved rounds) -- that backend builds nothing large and
long-lived per request, so it was never paying this.

**The lesson, twice in two entries:** the headline "how slow is Ziman" number was dominated by
something other than the interpreter both times. 8x CPython on calls was real; 400x on maps was an
unmeasured constant. A bad benchmark row is a bug to find, not a property to accept.

Compared `MapBuilder` to CPython's `dict` properly, now that the quadratic trigger is fixed (15x, not
400x). Insert is ~6x CPython; **lookup is the problem** -- subtracting insert leaves 0.11/0.38/1.07s
for 50k/100k/200k lookups, still super-linear.

`perf` says it is not the map: `arena_block_content` **48.5%**, `gc_lookup` 14.8%,
`mapobj_mark_leaves` 8.8%, and only 2.6% in `mapobj_get`. The shape: each lookup allocates an
`Option` wrapper -> heap grows -> collection -> marking walks all 200k leaves calling `gc_lookup` ->
`gc_lookup` finds the containing block by **walking the block list linearly**, and a heap this size
has ~100 blocks. 200k x 100 = **twenty million block visits per collection**. The per-block bitmap
from earlier made the search *within* a block O(1) and left the search *for* the block linear.

Three cheap fixes tried, all measured, **none did anything** -- recorded because each looked obvious:
the `strdup("Option")` per lookup (removed; no change -- a 7-byte GC_MALLOC is not the cost); a
two-pointer-compare block pre-reject before `arena_block_content`'s two divisions (no change -- not
the arithmetic); and `-flto`, on the theory that these accessors are un-inlinable cross-TU calls the
way `value_int` was (no win: 1.54 vs 1.49s, fib 0.71 vs 0.61s; not adopted). All reverted.

The cost is the twenty million *visits*, not what each does, so shaving the per-visit constant cannot
help. Next thing to try: stop the walk being linear -- a per-arena block index sorted by address and
binary-searched, or a one-entry last-block-found cache, since marking walks leaves with real
locality. Contained to arena.c/gc_lookup.c, with the benchmark already in place.

### Design: unboxing single-payload enum variants, and the case against building it

Written after instrumenting the collector (`ziman-allocprofile` now sees both heaps, the live set by
kind, mark against sweep, and enum allocations by variant). The conclusion is not to build it. The
reasoning is recorded because the idea is the obvious one to have, and because three of the
measurements below are worth keeping whatever happens to it.

**What prompted it.** On a workload shaped like the backend's hot path -- decode 40,000 four-field
JSON rows into structs and re-encode them -- the collector is 16.6% of CPU time, and within that
**sweep is 82.5% and mark 17.5%** (10.0ms against 47.2ms over 202 collections). The peak live set is
780 KB in 3,948 objects, so there is almost nothing to mark; the cost is the sweep walk over 2.0M
object visits. Sweep visits every object exactly once per life-cycle, so its total is proportional to
**objects allocated** -- not to bytes, not to how often collection runs. Allocating fewer objects is
the only lever on it.

Of ~2M objects, 642,005 are enums, about 23 per decoded row:

    37.476%        240600  payload   json::Json.JString
    24.923%        160004  payload   Option.Some
    12.523%         80400  payload   json::Number.Int
    12.492%         80200  payload   json::Json.JNumber
    12.492%         80200  payload   json::Json.JObject
     0.062%           401  payload   json::Json.JArray
     0.031%           200  EMPTY     json::Json.JNull

A `Json` node is a heap object, a number is *two* of them (`JNumber(Number.Int(n))` nests), and every
field lookup through `json_object_get` allocates an `Option.Some`. An `EnumObj` is ~104 bytes holding
an 80-byte `Value`, rounded to a 128-byte size class.

**The shape of the change.** A variant carrying one payload would live in the `Value` itself: the
`Type` says which enum and which variant, and the `Value`'s 16-byte union holds the payload's own
union bits. The payload's `Type` is not stored, because it is derivable -- the declaration says what
each variant carries, and a generic enum's `type_args` supply the rest, so `Option<Json>.Some`'s
payload is known to be `Json` from the value's own type.

**The obvious place for the variant tag is closed, and measured closed.** `Type` is exactly 64 bytes
with no spare padding (`func_captures_thread_bound` already took the four bytes after `kind`), so a
new field grows `Type` to 72 and `Value` to 88 -- and `Value` is the most-copied object in the
interpreter. Unioning the three mutually-exclusive name fields to make room is the natural answer and
is **already recorded above as a failure: 40-160% slower**, because the union defeats GCC's scalar
replacement of aggregates and structs that had been living in registers get materialised through
memory. That result stands as a warning against exactly this move.

**The one place left, and what it costs.** For `TY_ENUM`, `type_var_name` is unused (it is meaningful
only for `TY_TYPEVAR`), so the variant name could simply live there: same type, same size, no union,
no regression. The price is an invariant that reads wrong. Per-instance data would be sitting inside
the *type*, so `type_equals` must ignore the variant while `value_equals` must honour it -- two
functions that look symmetric and would have to disagree, with every assignability check,
`Map`-key hash and `type_to_string` call needing to know which side it is on. That is a thin
invariant to hang on a 3% optimisation.

**The nested case is a wall, not a detail.** `Option<Json>.Some(Json.JString(s))` cannot have both
enums unboxed: the outer value's union holds the inner payload's bits and its type reconstructs to
`Json`, which does not say *which* `Json` variant. So the rule has to be "unbox a single-payload
variant only when its payload is not itself an unboxed enum", a static property of each declaration
that has to be computed, kept consistent across modules, and explained. Under it, `JString`,
`Number.Int`, `JObject` and `JArray` unbox and `Option.Some` and `JNumber` do not -- 401,601 of
642,005.

**What it would buy, with the arithmetic shown.** 401,601 fewer objects out of ~2.0M swept is 20% of
sweep, sweep is 82.5% of a collector that is 16.6% of the run: **about 2.7% of wall-clock time**,
plus a little from the allocations themselves and from the heap growing more slowly. And the standing
evidence is that a win this size does not reach a real application: inlining the value constructors
was worth 25-45% on value construction and moved `server_concurrency.sh` from 637 to 639 req/s,
because that workload is dominated by HTTP parsing, SQLite and the collector. A 2.7% microbenchmark
win has less to surface through than that one did.

**Recommendation: do not build it.** The win is small, the obvious implementation is a measured
regression, the alternative implementation weakens a type invariant, and the rule it needs is
partial and contagious across declarations. If object count is worth attacking, **interning `Type` so
`Value.type` is a pointer** is the better-justified change already on this page: it takes `Value` from
80 bytes to ~24, is motivated by a demonstrated copy cost rather than an inferred one, and shrinks
every value rather than one kind of them. It should be prototyped and measured before being committed
to, for the same reason the union is recorded here.

**Three findings from the investigation that stand on their own.**

*Sweep's `memset` is irreducible, and its comment understates why it is there.* It was priced by
cutting it to the eight bytes the free list needs: the interpreter **segfaults immediately**. The
comment explains one job -- a disguised native-handle address left in a dead payload keeps looking
live to Boehm's conservative scan, holding a resource open past reachability. It is doing a second
one it does not mention: `gc_heap_alloc` clears only `sizeof(void *)` when it reuses a chunk and
relies on sweep having zeroed the rest, which is how it honours its documented "always zero-filled,
fresh or reused" guarantee. Every object is therefore zeroed exactly once per life-cycle whichever
phase does it, so there is nothing to win by moving it.

*The collection trigger is already well tuned.* `ZIMAN_GC_TRIGGER_FLOOR` was swept over 64 KB, 256
KB, 1 MB and 4 MB. At 4 MB collections drop from 202 to 48 and mark time halves (9.7ms to 4.4ms) --
and sweep gets *worse* (46.6ms to 56.0ms), wall time worse, RSS 10% higher, because delaying
collection lets more garbage accumulate for each sweep to walk. 64 KB through 1 MB are
indistinguishable. Frequency trades mark against sweep and cannot reduce the total.

*`ResultObj` survives 0% of the time.* 239,000 allocated, 23 MB, not one ever live at a collection:
every `Result` in this workload is consumed before the next cycle. It is the clearest case for
unboxing and still not enough to carry the change.
