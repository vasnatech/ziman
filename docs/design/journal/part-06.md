# Design journal, part 6 (entries dated 2026-10-05 to 2026-10-22)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## The gc_heap migration, measured and abandoned: a gc_heap object pays *both* collectors, and two independent premises fail (2026-10-05)

Three entries in this file have named "complete the migration -- strings, `Env`, closures, `MapNode`" as the remaining program, the last of them (this session's own `gc_lookup` entry) claiming it is what would finally retire `GC_mark_from`'s 46%. **That claim is wrong, and so is the broader assumption that moving an allocation into a `GcHeap` is a win.** Both were tested rather than argued, and both failed. Nothing is landed from this; the code is exactly as it was. The finding is the deliverable.

**Premise 1, wrong by reading: migration cannot retire the conservative scan, no matter how complete.** `handles.h` states the retention rule in its own words -- the live-handle table "lives in memory the garbage collector does not scan, so being in the set does not keep an object alive: an object nobody holds any more is collected, and its finalizer closes the descriptor or connection." A socket is `GC_MALLOC`'d with a Boehm finalizer (`native_net.c`) and handed to Ziman as `(int64_t)(intptr_t)h`; the **only** thing keeping it alive is a conservative scan finding that integer. `struct_obj_new` is already migrated, so a `net.Socket`'s fields live in the `gc_heap` -- which therefore must stay a Boehm root **permanently**, whatever else moves. The project's own history already recorded the proof without drawing this conclusion: `gc_heap.c` notes `net_dropped_socket_reclaimed.zm` failing exactly when `struct_obj_new` first migrated there. And since `ziman_arena_push_scanned_roots` pushes `[content_offset, used)` per block, migrating *more* in **enlarges** the range Boehm re-scans. The direction is adverse, not neutral.

**Premise 2, wrong by measurement: a migrated object is slower, including under concurrency.** Built the whole thing properly first, so the test would be fair rather than a straw man. `GcHeap` became *two* arenas -- the existing scanned one plus a plain `arena_create()` that is never registered with Boehm at all -- holding `GC_ATOMIC` objects, which by definition contain no pointers (the same guarantee `zalloc_atomic` already relies on for exactly this data). The two were linked by a new `arena_set_sibling`, so `gc_lookup` resolves an address in either while every one of the ~28 `const Arena *a` signatures across `gc_mark`/`gc_lookup`/`gc_scan`/`gc_env_roots`/`gc_interp_roots` -- and the public `gc_mark_register_struct_content_marker` callback that `prelude.c` and `iter.c` implement -- stayed untouched. Free lists were split per arena, because a chunk reclaimed from the unscanned arena must never be handed to a pointer-bearing object. All of it passed: 836/836 on both builds and all nine `gc_*` unit tests, with two of those tests updated (they had asserted cross-kind slot reuse, which the split deliberately forbids) and a new check added for the segregation itself.

Then, with that foundation in place so atomic data cost Boehm's marking *nothing*:

- **`format`'s result into the `gc_heap`'s atomic arena: 1.23-1.24s to 1.36-1.42s, about 10% slower** (4M formats). Higher `sys` time and RSS too.
- **`ClosureObj` into the `gc_heap`: 1.09-1.16s to 1.21-1.31s, about 8-10% slower** (2M closure calls). `GC_CLOSURE` and `gc_mark_closure` were already written and unused, so this was the same mechanical step `struct_obj_new`/`array_obj_new`/`result_obj_new`/`enum_obj_new` each took -- the pattern that was supposed to be a safe win.
- **The same `ClosureObj` migration across 8 threads: 5.07-5.43s to 5.53-5.79s, about 7% slower.** This was the hypothesis most likely to rescue the strategy, since the one migration ever *measured* as a win (`Map`/`Set`'s wrapper, 689 to 802-843 req/s) was measured on the concurrent server benchmark, where what you buy is relief from Boehm's **locked** heap. At eight threads it still loses.

**The mechanism, which explains all three numbers at once: an object in the `gc_heap` pays both collectors.** It sits inside the range Boehm conservatively re-scans as a root (`arena.c`'s push) *and* it is marked and swept by Ziman's own Phase 3 (`gc_heap_collect`). An object on Boehm's heap pays Boehm only. Bump-allocation plus a free list is cheaper than `GC_MALLOC` per call, but not by enough to cover a second collector walking the same object -- and for `GC_ATOMIC` data the comparison is even harsher, since `GC_MALLOC_ATOMIC` is already both cheap and already unscanned, so migration buys nothing there and still adds Ziman's mark and sweep (plus, at the `format` site, a copy, because the buffer grows by `realloc` and `gc_heap_alloc` has no equivalent).

**The part that makes this structural rather than just disappointing, confirmed after the fact.** Ziman's own mark phase makes **zero** calls into Boehm -- not one `GC_push`/`GC_mark` anywhere in `gc_mark.c`, `gc_env_roots.c`, `gc_interp_roots.c` or `gc_scan.c`. So when a `gc_heap` struct holds the only reference to a `zalloc_atomic` string, a `ChannelObj` or a `MapNode`, the single thing keeping that object alive is **the arena being registered as a Boehm root**. The conservative scan is not merely a cost sitting on top of the design; it is load-bearing for cross-heap retention.

That closes the loop on why the measurements come out the way they do, and why they always will. The scan cannot be dropped until nothing in the `gc_heap` points into Boehm's heap -- which needs the migration finished *and* the handle-retention constraint above dealt with. And the migration cannot show a win until the scan is dropped, because until then every object moved in pays both collectors. Each half is the other's prerequisite. **There is no incremental path: every slice measured on its own is a loss, and would be even if the end state were a win.** Anyone revisiting this should treat it as one all-or-nothing change to how the two collectors relate -- or, more likely, as a reason to ask whether v0 should have two at all.

**What this does and does not say.** It does not say the existing migrations were mistakes: `Map`/`Set` was measured as a real concurrent-throughput win, and the string/struct/array/result/enum moves were made for a correctness reason -- getting a crossing value off an allocator a *sending* thread might destroy -- that stands on its own. What it says is that **"migrate the rest" is not a performance strategy**, that the 46% in `GC_mark_from` has no fix on this path, and that the honest next question for that cost is not "what else can move into the `gc_heap`" but "should there be two collectors over one object graph at all" -- which is a far larger question than any migration slice, and one nobody should open without first disposing of the handle-retention constraint above, since that is what pins the conservative scan in place.


## `value_int` was 30% of a profile for carrying a 64-byte type descriptor, and the obvious fix for that made things worse (2026-10-05)

Closing F2 established the honest headline for interpreter speed: loop-heavy code is 1.75x CPython 3.14, call-heavy code about 8x (`fib(32)` 1.14s against 0.14s). Profiling the call-heavy side for the first time since the `Env` pool landed found something that is not a per-call cost at all.

```
30.42%  value_int          13.12%  eval_expr        7.05%  pthread_mutex_trylock
 7.63%  exec_stmt           6.29%  call_function    2.79%  env_define
```

**`value_int` is three lines.** It is 30% of a recursive-fib run because of what it moves, not what it does:

```
sizeof(Value) = 80 bytes
sizeof(Type)  = 64 bytes
```

Every `Value` carries a whole `Type` descriptor inline, and `Value` is the most-copied object in the interpreter -- one per literal, per arithmetic result, per comparison, per parameter bind, per `eval_expr` return. `type_scalar` zeroes all 64 bytes (`Type t = {0}`) before filling in a 4-byte tag. The irony is that Ziman is statically typed: the checker already proved the type, and the runtime pays 80 bytes a value to carry it anyway.

**The fix that worked was the smaller of the two tried: inline the constructors.** This build has no LTO, so `value_int` was a genuine cross-translation-unit call returning 80 bytes through a hidden pointer, and `type_scalar` another returning 64. Making `type_scalar` and `value_int`/`value_unit`/`value_float`/`value_bool`/`value_byte` `static inline` in their headers removes both calls and both return copies. Pinned to one core, four runs each: **`fib(32)` 1.11-1.18s to 0.81-0.88s (about 26%), `loop_bench` 0.26-0.29s to 0.14-0.17s (about 45%), `scalar_arrays_bench` 1.07-1.15s to 0.79-0.89s (about 25%)**. The loop benchmark is now 0.15s against CPython 3.14's 0.16s -- at parity on the microbenchmark F2 opened with at 28x.

**The fix that seemed obviously better made it dramatically worse, and the reason is worth recording.** `Type`'s six payload fields are mutually exclusive by kind -- the comments already said so, and every constructor confirmed it -- so `struct_name`/`enum_name`/`type_var_name` were collapsed into a C11 anonymous union. That is a pure layout change: every `t.struct_name` in the codebase still reads identically, and it takes `Type` to 48 bytes and `Value` to 64, 20% less to copy on every value in the program. It passed 840/840 on both builds. It was also **40-160% slower**: `fib(32)` 0.81s to 1.15s, `loop_bench` 0.15s to 0.40s -- worse than the original baseline -- and `scalar_arrays` 0.81s to 1.24s.

Profiling the regression put `no_ctrl` -- a one-line helper returning `{CTRL_NONE, value_int(0)}` -- at **43%** of the loop profile, and disassembly showed why: instead of storing zeros into the return slot, GCC emitted a long chain of `movdqa`/`movups` bouncing the aggregate through half a dozen stack slots. The union defeats scalar replacement of aggregates, so a struct that was previously kept in registers is now materialised through memory, repeatedly, on every construction and return. Making the struct *smaller* made the code that copies it far worse. Reverted; the inlining alone is what shipped.

Two things were kept from the attempt because they stand on their own, and are in their own commit: `loader.c`'s five hand-copied struct-to-enum reclassifications became one `retype_struct_as_enum` (two of the five wrote the new name before clearing the old one, which the union would have silently turned into erasing it), and `gc_mark.c`'s registered-content lookup is now gated on the type's kind rather than only on its name being non-NULL.

**Verified.** `./tests/run_tests.sh` 840/840 on both builds; all thirteen standalone unit tests including `test-lsp`; `make test-pg` 11/11; `make test-mysql` 6/6; the real app's own suite in full; `make fmt-check` and `make docs-check` clean. **And measured honestly where it does not show:** `tests/bench/server_concurrency.sh` against the real backend is unchanged -- five interleaved paired rounds, c=1 averaging 637 against 639 req/s and c=8 992 against 986. That app is dominated by HTTP parsing, SQLite and the collector rather than by tight interpreter arithmetic, so a 25-45% improvement in value construction has nothing to surface through. The microbenchmarks isolate interpreter dispatch, which is exactly where the win is and exactly what F2 measured.

**Still on the table, and now justified rather than speculative:** interning `Type` so `Value.type` becomes a pointer would take `Value` from 80 bytes to about 24. The copy cost is now demonstrated to be real and large, which is the evidence that refactor needed -- but it is hundreds of `.`-to-`->` edits across the codebase, and the union result above is a warning that a smaller struct does not automatically mean faster code. Measure a prototype before committing to the edit.


## A Ziman-versus-CPython table turned up a quadratic: the collection trigger, not the map (2026-10-05)

Building a five-workload comparison against CPython 3.14 -- the honest version of F2's single loop number -- one row came back absurd: 200,000 `MapBuilder` inserts plus 200,000 lookups took **26.6 seconds** against CPython's 0.06s for the same program over a `dict`. Four hundred times is not a tree-walker-versus-bytecode-VM gap; it is a bug.

**It is not the map.** Scaling said quadratic immediately (12.5k/25k/50k: 0.02s, 0.09s, 0.43s -- four to five times the work per doubling), and splitting insert from lookup put most of it on insert. But `node_insert_mut` is an ordinary HAMT insert, O(log32 n), growing a node by one slot with a `realloc`; `mapobj_set_mut` adds nothing but a count; `native_mapb_set` adds nothing at all. Every layer of the map is the right complexity.

**It is the collector's trigger, and the constant's own comment had already confessed.** `ZIMAN_GC_TRIGGER_BYTES` was a flat 64KB of heap growth between collections, described in place as "an arbitrary, unmeasured starting point ... not to be a considered performance choice". For a program that builds one large structure that stays live, that makes the *number* of collections proportional to the heap (heap/64KB) while *each* collection marks everything live -- O(n) work done O(n) times. Confirmed by raising the constant a hundredfold as an experiment: 100k inserts went from 1.25s to 0.08s and the curve went straight.

**The fix is to scale the threshold with the heap, not to raise the constant.** Raising it only moves the cliff and spends memory everywhere to do it. `gc_growth_before_next_collect` now returns whichever is larger of the 64KB floor and half of what the heap already holds, so each collection buys a proportional amount of new allocation before the next: O(log n) collections over a run that grows to n, and O(n) total work. The floor still governs small heaps, where a fraction of almost nothing would collect far too eagerly.

| | before | after |
| --- | --- | --- |
| `MapBuilder` inserts, 25k / 50k / 100k | 0.03s / 0.18s / 1.25s | 0.01s / 0.03s / 0.12s |
| 200k inserts + 200k lookups | 26.76s | **0.81s** (33x) |
| build-60k-and-drop, 12 rounds | 3.47s | **0.45s** (7.7x) |

**What it costs, measured rather than assumed, because collecting less often is a memory trade.** Peak RSS on the monotonically-growing 200k map: 98.9MB to 125.7MB, +27%. On the grow-then-drop cycle -- a long-running server's actual shape -- 55.2MB to 58.8MB, a small constant rather than a ratchet, because the threshold derives from *reserved-at-last-collect*, which shrinks when `gc_heap_sweep` reclaims whole blocks, so the trigger tightens again as soon as the heap does. On steady-state string churn (4M `format` calls) peak RSS is unchanged at ~41MB. The README's claim that the sample backend stays around 40MB under load is therefore intact; `tests/bench/server_load.sh`, which would have measured that directly, turns out not to run in this environment at all -- its server dies at startup -- and fails identically on the pre-change binary, so it is a pre-existing breakage and its own small task. **Fixed 2026-10-14**: it launched `server.zm`, a path the backend has not had since its entry point became `src/main/ziman/serve.zm`, and it neither checked a status nor failed on one, so three phases of `curl` round trips to a closed port printed as sub-millisecond latencies and exited 0. It now boots the server the way the Python suites do, measures the authenticated allergy endpoints, and exits non-zero on the first bad status.

**Verified.** `./tests/run_tests.sh` 840/840 on both builds; every `gc_*` unit test (these are the ones that would notice a collection that stops happening, and `test-gc-heap` drives real collect/sweep cycles directly); `make test-pg` 11/11; `make test-mysql` 6/6; the real app's suite in full; `make fmt-check` and `make docs-check` clean. `tests/bench/server_concurrency.sh`, three interleaved paired rounds, is flat to about 1-2% better (c=1 629 against 639 req/s, c=8 984 against 993) -- that backend builds nothing large and long-lived per request, so it was never paying the quadratic.

**Worth keeping in view:** this is the second time in two entries that the honest headline number for "how slow is Ziman" was dominated by something other than the interpreter. The eight-times-CPython figure for calls was real; the four-hundred-times figure for maps was a trigger constant nobody had measured. The remaining gaps in the table below should be read with that in mind -- a bad row is a bug to find, not a property to accept.


## `MapBuilder` against CPython's `dict`: the remaining 15x is `gc_lookup`'s block scan, and three cheap fixes for it do nothing (2026-10-05)

With the quadratic collection trigger fixed, the map row of the CPython comparison is 15x rather than 400x. Breaking it down (best of three, pinned, n = 50k/100k/200k):

| | 50k | 100k | 200k |
| --- | --- | --- | --- |
| Ziman, set only | 0.06s | 0.19s | 0.42s |
| CPython, set only | 0.03s | 0.04s | 0.07s |
| Ziman, set + get | 0.17s | 0.57s | 1.49s |
| CPython, set + get | 0.03s | 0.06s | 0.10s |

Insert is about 6x CPython. **Lookup is the problem**: subtracting insert leaves 0.11s / 0.38s / 1.07s for the same number of lookups, which is still growing faster than linearly.

**It is not the map and not the lookup.** `perf` on the 200k case: `arena_block_content` **48.5%**, `gc_lookup` 14.8%, `mapobj_mark_leaves` 8.8%, `arena_next_block` 3.0% -- about two thirds of the run inside `gc_lookup`'s walk, and only 2.6% in `mapobj_get` itself. The shape is: every lookup allocates an `Option` wrapper, that allocation grows the heap, a collection eventually runs, and marking walks all 200,000 leaves (`mapobj_mark_leaves`) calling `gc_lookup` on each -- while `gc_lookup` finds the containing block by **walking the block list linearly**, and a heap grown to this size has a hundred-odd blocks. Two hundred thousand leaves times a hundred blocks is twenty million block visits per collection. The per-block bitmap added earlier made the search *within* a block O(1) and left the search *for* the block linear, which is now the whole cost.

**Three cheap fixes, all measured, none of them anything.** Each looked obvious and each is recorded because the reasoning was wrong in an instructive way:

- **The `strdup` per lookup.** `native_mapb_get` called `type_enum("Option")`, which `strdup`s its argument -- a heap allocation per map lookup purely to spell a constant type's name, on the only two per-operation paths that do it (`mapb_get` and `iter_next`). Replaced with a non-allocating variant for static names. **No measurable change** (0.16/0.56/1.49 against 0.17/0.57/1.49): a seven-byte `GC_MALLOC` is simply not where the time is.
- **A cheap block reject.** `arena_block_content` derives where a block's content starts from its size, which costs two divisions, and it was being called for every block on every lookup. Added an `arena_block_may_contain` doing two pointer compares first. **No change** (0.17/0.58/1.56) -- the cost is not the arithmetic.
- **LTO.** If those accessors are cross-translation-unit calls that cannot inline -- the exact thing that made `value_int` 30% of a profile two entries ago -- then `-flto` should fix the category rather than one accessor. It does not: 1.54s against 1.49s, `fib(30)` 0.71s against 0.61s, loop unchanged, suite still 840/840. No win, so not adopted.

All three reverted; the finding is what is kept. **The cost is the twenty million block visits, not what each one does**, and nothing that shaves the per-visit constant will matter. The fix is to stop the walk being linear: a per-arena index of blocks sorted by address, binary-searched (O(log blocks)), or a one-entry cache of the last block found, since marking walks leaves in an order with real locality. That is a contained change to `arena.c`/`gc_lookup.c` with a benchmark already in place to judge it by, and it is the next thing to try here.

Worth noting what this does *not* say: insert at 6x and the string/array rows at 1.8x/2.8x are ordinary tree-walker overhead. Only the lookup row was pathological, and after the trigger fix what remains of it is one linear scan in the collector rather than anything about maps.


## Three tools that each cost nothing because the interpreter already kept what they needed (2026-10-05)

`--coverage`, `--profile` and `--locals` closed most of F6's tooling half in one day, and the reason
they were a day's work rather than a project each is that none of them needed new bookkeeping. Every
one reuses something the interpreter already maintained for an unrelated purpose:

- **`--coverage`** counts into a field on `Stmt`, then walks the AST afterwards to report. The walk
  is what makes the denominator exact: "executable line" means "a line some statement starts on",
  which is not a second model of the language that could drift from the interpreter, but a
  consequence of the same walk that reads whether the statement ran.
- **`--profile`** samples the frame stack in `error.c` -- maintained unconditionally for error
  traces, so a SIGPROF handler already has somewhere to look and the call path gains nothing.
- **`--locals`** reads the `Env` each frame carries. That field exists because a conservative stack
  scan cannot see into an `Env`'s bindings (docs/MEMORY.md, "the second root source"), so the
  collector needed it -- and it happens to be exactly what a post-mortem inspector needs. Because
  `interpreter.c` swaps it to the innermost active scope, nested-block locals come for free.

The general lesson is worth keeping: before building infrastructure for a tool, check what the
runtime already has to maintain for correctness. Twice here the answer was "all of it".

### Sampling over instrumenting, and why the profile is self time

Instrumenting every call was the obvious design and is the wrong one. Two clock reads cost tens of
nanoseconds against a call costing a few hundred, so the functions it reports as hottest are the
ones it slowed down most. Sampling costs one store per sample, a thousand times a second, and errs
only in the harmless direction -- it needs enough samples to be sure about a small function, and the
report says so. Self time (innermost frame only) answers "which function do I make faster", which is
what a profiler is for; inclusive time would need the handler to walk the whole stack per sample.

### Collection gets its own row, because otherwise the profiler lies

A 400k-insert `MapBuilder` loop first reported `build_map` 48.6%, inviting someone to optimise a
`for` loop containing an add and a call. A flat `perf` profile puts `GC_mark_from` at 27.8%: the
collector, running inside the loop because that is where the allocation was. Boehm collects from
inside whatever allocation crossed its threshold, so no call site can bracket it -- that needs
`GC_set_on_collection_event`. Ziman's own collector is bracketed inside `gc_heap_collect`, and
deliberately not at `maybe_collect`, which `exec_stmt` calls per statement.

What proved the split was real rather than merely moved: giving Boehm a heap large enough never to
collect makes the collection row vanish while `build_map`'s absolute sample count barely moves (56
against 60), so its remaining share is genuinely its own loop and call setup.

### A 4MB static array cost 9% of an unrelated benchmark

The profiler's sample buffer began as `static const char *g_samples[1 << 19]` -- 4MB of BSS in every
`ziman` binary, profiling or not. It cost 9% on call-heavy code: **+1.9% instructions but +10.4%
cycles**, IPC down, which is the signature of hot globals being pushed into conflicting cache sets
rather than of extra work. It is now allocated when profiling starts.

Two confident guesses at the cause were both wrong and both measured: a signal fence on the
frame-push path, and the calls added to `maybe_collect`. Removing each changed nothing. The fence is
gone anyway, replaced by a better invariant -- `frames_grow` zeroes every slot it has not handed out,
so a sampling handler reads a name, NULL, or a valid stale name, but never a torn pointer. Safety
from the data structure rather than from ordering a hot path, which is why the hot path could stay
exactly as it was.

The method mattered more than any single number here. An unrelated change of the same source size to
the same file cost nothing on the same benchmark, which is what ruled out plain code-layout luck and
made the 9% trustworthy as real. And the final "is it free when off" question -- a sub-1% effect --
needed fourteen paired rounds to answer (+0.89%, 95% CI -0.44%..+2.22%); two earlier interleaved runs
of the *same* comparison read +1.6% and -2.0%. Anything measured there with fewer rounds is noise.

### Where each tool writes, and the rule behind it

A tool that does not run the program -- `--check`, `--fmt`, `--lint` -- writes its findings to
stdout, because they are the entire output. A tool that runs the program and reports alongside it
should write to stderr, so the program's own stdout stays byte-for-byte what it would have been and
stays diffable: `ziman --profile prog.zm > data.txt` still yields exactly what the program printed.
All three follow it. `--coverage` did not at first -- it printed to stdout, where it interleaved
with the program's own output -- and was moved the same day rather than defended, because the
argument for it was only that it had shipped that way a few hours earlier. The fixture for it
changed shape as a result: tests/run_tests.sh checks either stdout (`.out`) or stderr (`.err`) but
not both, so coverage_partial asserts the report on stderr and coverage_full, which exits 0, asserts
instead that stdout carries only the program's output -- its exit status is what pins full coverage,
since --coverage exits 1 if any line did not run.


## When a library error gets a type, and why counting `Result<_, string>` misleads (2026-10-06)

The 2026-10-05 developer review tracked `Result<_, string>` across `lib/` as a quality metric -- 281, up
from 242 -- and called it "the one axis that got worse". Auditing all 280 of them for D15 says the metric
is measuring the wrong thing, and the clearest demonstration is D9, the task that fixed exactly the problem
D15 was about:

| `lib/core/fs.zm` | before D9 | after D9 |
| --- | --- | --- |
| `Result<_, string>` total | 21 | **24** |
| of those, in the public API | 18 | **3** |

Giving `fs` a branchable `FsError` *raised* the count, because each native became a private `*_raw` returning
the category string that the public wrapper turns into the enum -- the same arrangement `net` has always used
for `NetError`. The count went up; the API got better. Any metric that moves the wrong way on its own
best-case fix is not the thing to track.

### The rule

**An error gets a type when callers branch on its kind. It stays a string when the message is the whole
information.** The three structured types in the library are exactly the places callers branch:
`FsError` (not there / not allowed / is a directory), `NetError` (refused / not found / timed out / closed),
`DbError`, and `JwtError` on `verify` (expired / bad signature / wrong audience -- a 401 or a 400 depending).
`jwt.sign` keeps a string, because its only failure is "the claims must be a JSON object", which is a
programming mistake and not a branch.

### What the 280 actually are

- **55 internal or in comments.** `net`'s eleven are *all* private `*_raw` natives behind `NetError`;
  `fs`'s twenty-one likewise behind `FsError`; `sqlite`'s five behind `DbError`. Plumbing under a structured
  API, counted as if it were the API.
- **~184 parse and format errors**, where the message is the information and nobody branches: `csv` 51,
  `time` 27, `json` 25, `toml` 16, `properties` 16, `yaml` 14, `crypto` 8, `number` 6, `zone` 5, and the
  rest. The review itself said to leave `json` and `regex` alone; the same reasoning covers all of them.
- **14 in `server`**, almost all *application-supplied callbacks* -- `decode: fn([byte]) -> Result<I, string>`,
  `path: fn(Params) -> Result<P, string>`. The error is the application's message to a client, which the
  library turns into a 400 body. A library enum there would make every application decoder speak the
  library's error type to describe its own validation.
- **13 in `os`**: `load_average`, `uptime_seconds`, `disk_free` and friends. These fail when the platform
  cannot answer, which a program cannot usefully branch on -- it either works on this OS or does not.
- **3 in `fs`**, deliberate and documented: the per-item errors inside `lines`/`chunks`/`walk`. An item error
  is "a read failed partway through iteration", and making it a variant means building the enum in C, which
  is what the `"category|message"` convention exists to avoid.

So there is nothing substantial left to consolidate. If this is to be tracked at all, the number worth
watching is *public* `Result<_, string>` in modules whose errors a caller branches on -- which is currently
zero.


## Three allocation sites, and what they did to F7 (2026-10-07)

F7 -- throughput not scaling with concurrent clients under the collector -- has a long list of
things ruled out with evidence: migrating more objects into the per-thread `gc_heap` (measured
*slower*), heap tuning, `GC_FREE_SPACE_DIVISOR`, per-object root pushing, incremental mode. Both
2026-10-05 reviews re-confirmed the shape and re-scoped it: throughput *falling* as concurrency
rises, and capacity *improving* when fewer clients are admitted, is contention, not per-visit scan
cost. L2's interim suggestion was to cut allocation frequency in "`format`'s output buffer and small
`StructObj`".

That suggestion was worth checking rather than taking. `make ziman-allocprofile` counts every
allocation against its source site, and over 2,000 authenticated reads of the reference backend
three sites were 74% of all allocation calls -- neither named one among them:

| site | share of calls | what it was |
| --- | --- | --- |
| `env.c` | 25.7% | `strdup` of every binding's name, once per binding per call |
| `interpreter.c` | 24.5% | a `malloc`'d argument array per call |
| `mapobj.c` | 24.0% | a scratch array of every leaf, per map, **inside the mark phase** |

All three went away rather than being tuned. Binding names are borrowed from the AST, which outlives
every environment built from it. Argument arrays of four or fewer live in the evaluating frame, which
Boehm already scans conservatively. `mapobj_mark_leaves` visits as it walks, since unlike
`mapobj_entries` beside it, it never sorts.

Allocation calls across that workload fell from 9.96M to 2.57M. Measured paired and interleaved,
`tests/bench/server_concurrency.sh`, two rounds each:

| clients | before | after | change |
| --- | --- | --- | --- |
| 1 | 404-430 | 461-495 | +10 to +15% |
| 4 | 967-1048 | 1229-1308 | +27 to +30% |
| 8 | 1016-1028 | 1213-1496 | +18 to +47% |
| 16 | 782-821 | 998-1384 | +28 to +69% |
| 32 | 736-756 | 1256-1278 | +66 to +74% |

**The gain grows with concurrency**, which is what a fix to collection *frequency* should do: the
cost it removes is the one that scales badly with threads.

A fourth site followed: the five loop scopes in `exec_stmt` still called `env_new` on entry and
`env_free` on exit, while `call_function` and `exec_block` had taken theirs from the per-thread Env
pool for some time. Pooling those too halved the remaining Env allocations (234K to 107K) and cut
their `items` reallocs by a third, for another 14% at c=16 -- and noticeably less run-to-run variance,
a fresh allocation per loop entry having been adding spread as well as cost.

**The shape F2 and F7 named is gone.** Measured paired across the full sweep, 2,500 requests a level:

| clients | 1 | 4 | 8 | 16 | 32 | 64 |
| --- | --- | --- | --- | --- | --- | --- |
| before | 407-429 | 888-988 | 1030-1041 | 763-843 | 620-756 | -- |
| after | 463-535 | 1293-1428 | 1522-1664 | 1429-1574 | 1491-1517 | 1436-1495 |
| `ziman-nogc` | | | 2711 | 2979 | 2745 | 2652 |

Before, throughput peaked at four to eight clients and fell 27-40% by 32. After, it is flat from 8 to
64 within run-to-run noise -- as `ziman-nogc` is, which also plateaus at 8-16. What remains between
the two builds is a roughly 1.8x constant factor at every level, down from 4.6x at c=16: the ordinary
price of a tracing collector rather than a failure to scale.

Two things this does **not** settle. The architectural question the task frames -- whether v0 should
run two collectors over one object graph at all -- is untouched, and the `gc_heap` migration remains
measured and abandoned for the reasons recorded above. And this is one workload (an authenticated read
of an empty collection, new connection per request, which is the pessimistic case for the Env pool);
a write-heavy or keep-alive profile has not been measured.

What this says about method, since the numbers here are easy to misread. Allocation *call counts*
drove every decision; the byte column did not, which was lucky, because the profiler's first version
expanded its size expression twice around `GC_REALLOC` and could compute a size from a pointer the
realloc had already moved -- undefined behaviour that reported a 32GB site which does not exist.
Chasing that number down to a branch that cannot ask for more than ~550 bytes is what found the bug.
Separately, a first reading of the argument-array change looked like +58% because one baseline run
was low, and a first depth probe that doubled its step reported "recursion depth halved" where
bisection says 9%. Every figure above is a median of paired, interleaved rounds for that reason.


## Two regex ceilings, and a free that is impossible in both builds (2026-10-09, A28 and A35)

The memory budget A16 put on the regex engine (64 MB of capture buffers, refused at compile time)
bounds *space* and was mistaken for bounding *time*. A pattern sitting just inside it cost about 9 ms
per input byte -- roughly 2.5 CPU-hours for a 1 MB subject inside a single request -- so there is now
a second ceiling on the **work** a match may do.

**The unit was chosen by measuring, not by taste.** Work is counted in capture-vector int-copies
(`ncap` ints per thread parked and per `I_SAVE`), because that is where a Pike VM spends itself.
Against a 1 MB subject, this repo's own patterns cost between **7 and 34 copies per byte** (`\w+` 7,
`(.*)dog` 12, a three-named-group date 28, `(\w+)@(\w+)\.com` 34). The pathological end is four to
five orders of magnitude away: 400 empty groups on 200 bytes is 647,226 per byte, and 1,000 empty
groups is 4,030,056. That gap is the whole reason a limit is possible without guessing a number, and
it is worth re-measuring before anyone tightens it.

**Shaped as a floor plus a rate, because the two costs are not the same cost.** The first position
pays a full epsilon closure whatever the subject's length, so a flat cap punishes a short subject for
a large pattern, while the per-byte term is what a long subject legitimately spends. 256M copies plus
100,000 per byte leaves about 3,000x headroom over anything real. PCRE2 is the precedent for having
such a limit at all (`match_limit`, `depth_limit`, `heap_limit`); unlike PCRE2's, this one is not
caller-settable, and if that is ever wanted the **rate** is the number to expose, not the floor.

**The mistake worth recording, because it is a general one about limits.** The first version put the
budget on a single `regex_search` call, and `find_all` walked straight through it: 201 searches over a
200-byte subject, each comfortably inside a per-call budget, 8e8 copies in total. **A limit has to
bound the operation the caller asked for, not one of its steps.** The counter is now carried across a
scan (`regex_search_budgeted`, with `search_all_raw` keeping one counter for its whole loop), which is
why regex.h grew a second entry point instead of changing the first.

**And the cache cannot free what it evicts -- in either build, which is the opposite of how it was
filed.** A35 was recorded as an unbounded leak: 2,000 distinct patterns cost 1.14 GB under `nogc`
against 48 MB under the collector. The second look overturns that. A handle goes to Ziman as an
integer and `cache_lookup` hands the same one to every caller, so a program can still be holding a
pattern whose slot has been reused. Under `nogc` nothing can tell, and under the default build the
`free` would be `zgc.h`'s no-op anyway, because the `Regex` is collector memory and is reclaimed on
its own once nothing names it. So the 1.14 GB is **this build's documented behaviour** -- nothing the
interpreter allocates is freed under `nogc`, see "Memory reclamation" -- and not a defect in the
cache; it would look the same for 2,000 distinct arrays. Calling it A17's class in reverse was wrong:
A17 was a libc block the collector *could not* reclaim.

What was genuinely fixable is small and is stated as small: the evicted entry's pattern *text* is
owned by nobody but the table (`cache_lookup` only `memcmp`s it), so it is freed on eviction, worth
3.6 MB of that 1.14 GB. `regex_free` now exists, which was the real gap -- a C caller had no way to
release a compiled pattern, and `tests/c/regex_test.c` could not be leak-checked without one. It
frees its patterns now and has joined `make valgrind`'s leak-checked set.


## The `Row` bound, and why `from_row` cannot work the way `compare` does (2026-10-09, C18)

F1 above decided to grow the closed bound list from three to four, on the strength of a concrete
pain point: `Crud<E, Req, Res>` cannot say "`E` already has a `#[derive(Row)]` pair", so every
migrated resource plumbs `from_row`/`to_params` through the literal by hand. The implementation pass
that decision deferred was done, and it ends in a different place than the decision expected. Three
findings, each checked against the code rather than argued.

**1. `Ord`'s mechanism is runtime dispatch on a value, and one of the two row functions has no
value to dispatch on.** `compare<T: Ord>` works because the *value* carries its own type:
`value_compare` reads `a.type.struct_name` and looks up `<snake_type>_compare` from it
(src/interpreter.c, the `TY_STRUCT` arm). Generics are erased -- `call_function` takes no type
arguments at all, so inside a generic body there is nothing that knows what `T` was bound to. Now
compare the two halves of `#[derive(Row)]`:

- `to_params: fn(E) -> [SqlValue]` takes a value of type `E`, so it dispatches exactly as `compare`
  does. This half genuinely does fit the existing mechanism.
- `from_row: fn(Row) -> Result<E, DbError>` takes a `Row` and *returns* an `E`. `E` appears only in
  the return type, so at the moment of the call there is no value of type `E` in existence. There is
  nothing to dispatch on, and the type that would choose the function has been erased.

A constructor-shaped function cannot be resolved by value dispatch. Making it work needs a witness
passed per bounded type parameter (dictionary passing, how Haskell implements classes) or
monomorphisation, so that the body of `crud_get<Allergy, ...>` is a distinct body that can name
`allergy_from_row`. Both are the general trait machinery F1 spends its first paragraph refusing, and
neither is a four-line addition to a bound list. **The `Row` bound could cover `to_params` alone,
and that is worse than covering nothing**: a bound named after a derive pair that supplies one of
its two functions is a feature whose name lies about what it does.

**2. "The two fields disappear from every `Crud` literal" needs a second, unrelated feature.** A
struct literal here must set every field -- `P { a: 1 }` on a two-field struct is "missing field
'b'" -- and there are no struct-field defaults. So even for the half that does fit, nothing can
*disappear*; it would at best be written more briefly. Delivering the sentence as written is three
features, not one: resolution from a type argument, a defaultable field, and a marker tying the two
together.

**3. The safety argument in F1 is already covered by the type checker, which the entry did not
check.** It says "nothing stops a copy-paste mistake wiring one resource's `from_row` to another's
`to_params`, since both are just function values of matching shape". They are not of matching shape,
and it is caught today: `Crud<Allergy> { from_row: allergy_from_row, to_params: encounter_to_params }`
is rejected with *field 'to_params' of 'Crud' expects fn(Allergy) -> [int], found fn(Encounter) ->
[int]*. Verified directly. So the bound buys no soundness that nominal typing does not already buy.

**What is left of the motivation, measured.** Nine `Crud` literals, two lines each: eighteen lines
of boilerplate in one file of one sibling repository, against a language feature that needs either
dictionary passing or monomorphisation to be honest about its own name. **Decision: `Row` is not
added, and F1's decision to add it is withdrawn** -- recorded there rather than only here, since
that entry is where the next reader will look.

**What would reverse this, in the order that would make it cheapest.** (a) Monomorphisation arriving
for its own reasons -- it is a prerequisite of the bytecode/native compilation entry in ROADMAP.md --
after which both halves are resolvable and this becomes a small addition rather than a new
mechanism. (b) Struct-field defaults arriving for independent reasons, which would make the
`to_params` half nearly free, though still half. (c) F1's own deferral test being tripped properly:
a *second* abstraction of `Crud`'s shape, so the boilerplate is per-resource times per-abstraction
rather than eighteen lines in one place. None of the three has happened; (c) is the one to watch,
because it is the test F1 itself set and it is about real programs rather than about implementation
convenience.


## Pattern matching, measured rather than argued (2026-10-11, F67)

The enum entry in ROADMAP.md has carried "**`match`/pattern matching** ... is its own design pass" since enums shipped. This is that pass. **The answer is "not yet", and it is a measurement rather than a preference.**

**Most of what the bullet asked for arrived by other routes, which is why the bullet reads worse than the language is.** It was written when "the predicate-plus-unwrap style is the only way to consume a variant". Since then: `switch` arrived with exhaustiveness checking and no default arm, so adding a variant to an enum breaks every `switch` over it until each is handled -- the thing `match` is usually wanted for. `switch` is usable as a *value* in most positions, so `val label = switch s { ... }` is ordinary code rather than a statement-only construct. `if Some(x) = opt` binds. A payload binder names its value. What a real `match` would add on top is five separate things, and they are worth separating because they have different answers.

**Nested patterns: measured, and rejected.** `match`'s headline over `switch` is destructuring several levels at once -- `Ok(Some(n))` in one arm instead of a `switch` inside a `switch`. So the question is how often real Ziman nests. Across `lib/` and the 13,000-line `health-record-backend-ziman`: **304 `switch` sites, of which 4 have an arm whose body immediately re-destructures.** Four places in roughly 25,000 lines. A syntax every reader of the language must learn, to flatten four call sites, is not a trade this project makes -- and the deferral rule ("revisit once a concrete program needs it") is designed to answer exactly this way when the evidence says no, rather than being quoted only when it says yes.

**Guards, destructuring of structs and arrays, and or-patterns: no observed demand.** Each is a real feature in other languages and each would carry its own syntax, its own exhaustiveness interaction, and its own failure modes. Nothing in the library or the reference backend is shaped like a workaround for their absence. They are not rejected on principle; there is simply nothing to point at.

**Literal patterns are the honest open question, and the measurement cannot see them.** Switching on an `int` or a `string` -- `match code { 404 { ... } 500 { ... } }` -- is the one piece whose demand is **structurally invisible** to the count above. An if-else chain on a string does not look like a failed `switch`; it looks like an if-else chain. No grep over this tree can distinguish "nobody wants literal patterns" from "everybody writes the chain and thinks nothing of it". So this entry deliberately does **not** claim literal patterns are unwanted. It claims only that the evidence for them has to come from a writer's judgement about writing Ziman, and that absence of evidence here is a property of the instrument, not of the need.

**What this entry is for.** The roadmap bullet was a standing invitation to re-litigate `match` from first principles every time someone read it. Replacing "its own design pass, deliberately not attempted" with a number and a named open question means the next reader inherits the measurement instead of repeating the argument. If the four nesting sites become forty, or if someone makes the case for literal patterns, this entry is what they are revising -- and the number to re-run is in it.


## Four formatter rules that lived only in comments (2026-10-12, F73 and F79)

The 2026-10-11 style review asked for a formatting reference a programmer could follow by hand, and
found four rules that `--fmt` enforces or deliberately declines but that no document stated. Each is
written out here, verified by running the formatter rather than by reading it.

**A broken call goes fully vertical, whatever its width.** Breaking a call or a struct literal
between its arguments *at all* makes the formatter put every argument on its own line and the
brackets on theirs -- even when the whole thing would fit well inside the width:

```ziman ignore: the point is the two layouts, not a program
val short = g(1,
    2, 3)
```

becomes `g(\n 1,\n 2,\n 3\n)`. This is the most surprising thing the formatter does, because the
input fits on one line and the author's own break was only a hint. It is deliberate -- an argument
list is either one line or one argument per line, never a ragged fill -- but it lived at
`src/format.c`'s `normalize_vertical` and nowhere else, so the only way to learn it was to be
surprised by it.

**An array literal is the exception, and fills greedily.** The same over-width `[...]` does *not* go
one element per line; it fills to the width and wraps. So the bracket decides the shape, which is
the one place the formatter is not uniform:

```ziman ignore: the two shapes side by side
val arr = [1, 2, 3,
    4, 5]
```

stays exactly as written. **The reason is the content, not an oversight** (`vertical` is never set
for `[`): an array's elements are homogeneous, and forty integers one per line is a column nobody
wants to read or review, while a call's arguments are heterogeneous and each earns a line of its
own. Menderes confirmed this reading on 2026-10-12 when the review raised it as a question; the
alternative -- one rule for all three brackets -- was considered and declined because it would turn
every numeric and string table in the library into a tall column.

**`<` and `>` keep the spacing the source had, and the house rule is unwritten and unenforced.**
The formatter cannot tell `a < b` from `List<int>` without parsing, so it collapses runs of spaces
and otherwise leaves them alone (see "What it cannot decide without parsing" above). That means the
corpus's actual convention -- generics tight (`Option<int>`), comparisons spaced (`short < 5`), with
no counterexample anywhere in `lib/` -- is a convention held by hand. It is stated here so that a
reader who wants to match the library knows what to match; nothing checks it.

**A trailing comment is normalised to two spaces, or aligned to its run.** The R17 entry above says
a trailing comment "keeps its distance from the code so aligned comments survive", which is the
intent but reads as though any distance is preserved. What actually happens: a *lone* trailing
comment is set to two spaces however far out it was written, and a *run* of them is aligned to the
longest code line in the run plus two. So alignment survives as a group, not per line -- and a
hand-aligned column in a document will usually differ from the formatter's computed one, which is
why a blanket `--fmt --check` over `docs/SYNTAX.md`'s examples disagrees with 40 of its 46 blocks
without any of them being wrong (F74).


## `make tsan`'s collector configuration was linking an uninstrumented collector (2026-10-16)

`spawn_defer_gc_stress` failed in the `ziman-tsan-gc` configuration about one run in three, with
bdwgc's own abort rather than a sanitizer report:

    Signals delivery fails constantly at GC #2

That is `GC_stop_world` giving up (`vendor/bdwgc/pthread_stop_world.c`): it signals every thread,
waits for acknowledgements on a semaphore, resends, and aborts once `RESEND_SIGNALS_LIMIT` passes
with no progress. ThreadSanitizer intercepts signal delivery, so the acknowledgements did not
arrive.

**The cause was in the build, not the collector.** `libgc.a` was compiled once, with no sanitizer
flags, and that same archive was linked into `ziman`, `ziman-asan-gc`, `ziman-ubsan-gc` and
`ziman-tsan-gc` alike. bdwgc *has* ThreadSanitizer adaptations of its own -- see
`include/private/gc_locks.h` and `gc_atomic_ops.h`, guarded on `THREAD_SANITIZER` -- but that macro
is derived from `__SANITIZE_THREAD__`, which a compiler defines **per translation unit**, only for
code it is actually instrumenting. Compiling the collector without the flag therefore compiled every
one of those adaptations out, and the instrumented interpreter was left calling into a collector
that did not know it was running under a sanitizer.

So `make tsan` now builds a second copy, `vendor/bdwgc/libgc-tsan.a`, with `-fsanitize=thread`, and
links the `-gc` configuration against that instead. Measured on the fixture that failed, thirty runs
each with the suite's own `TSAN_OPTIONS`:

    uninstrumented libgc.a       21 passed, 9 aborted, 0 races
    instrumented libgc-tsan.a    30 passed, 0 aborted, 0 races

**Narrow on purpose: only `tsan`.** `asan` and `ubsan` keep the ordinary archive, because neither
showed this failure and instrumenting a collector that does its own `mmap`, its own free lists and a
conservative stack scan is a good way to acquire false positives from AddressSanitizer. The reason
to instrument it here is specific -- bdwgc's own code adapts to TSan when told it is present -- and
does not generalise to the other two.

**What this does not change.** `tools/tsan.supp`'s single entry (`race:push_other_roots`) is still
needed and still correct: the mark phase reads another thread's frame stack while that thread is
stopped, and a signal-based suspend establishes no happens-before edge TSan can see. That is a
report about Ziman's own code and is unaffected by how the collector was compiled. Running a fixture
by hand with `TSAN_OPTIONS='...'` **replaces** the Makefile's `TSAN_OPTS`, suppressions path
included, so that race reappears and looks like a new finding -- it is not. Reproduce with
`TSAN_OPTIONS=halt_on_error=0:suppressions=tools/tsan.supp`.


## `native` and builtin are two different mechanisms, and the difference is who owns the type rule (2026-10-18, F148)

Written down because a reviewer asked what the difference was and the answer was not recorded
anywhere, while the consequence of not knowing it is measurable -- see the end of this entry.

**A `native fn` is a function whose type can be written down.** It is declared in `lib/prelude/`
with a full signature, and registered by name to one C function of one fixed shape:

```c
typedef Value (*NativeFn)(Value *args, int argc, int line, int col);
native_register("parse_float", native_parse_float);
```

Its call goes through the **ordinary call path** -- `check_call` reads the declaration exactly as it
reads a user `fn`'s. That is why a native gets everything an ordinary function gets: generics with
inference (`native fn slice<T>(a: [T], start: int, end: int) -> [T]`), `variadic`
(`native fn concat(pieces: variadic string) -> string`), and default arguments
(`native fn mutable_array_new<T>(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0)`).
The C body sees only evaluated `Value`s.

**A builtin is a rule the checker implements.** There is no declaration anywhere. A name maps to an
enum id (`{"len", B_len}`) and the evaluator branches on it inside `eval_call`, so a builtin holds
the whole call **expression** rather than only the argument values: `e->resolved_type`, the argument
`Expr`s, even the callee's own spelling -- `e->as.call.callee[12] == 'u'` is how one branch picks
union from intersect from difference. Its types are a hand-written arm per name in `typecheck.c`,
with a hand-written error message.

| | `native fn` | builtin |
|---|---|---|
| declaration | prelude, full signature | none |
| type checking | the ordinary call path | a bespoke C arm per name |
| C contract | one fixed shape | arbitrary, inside the evaluator |
| sees the call site | no, only values | yes |
| named arguments | allowed | refused (`BUILTINS_NO_NAMED_ARGS`) |
| `--api builtins` | prints the signature | the name only |

**So the twenty-odd builtins are not one category but three, and only the first could ever be
declared.**

1. **The shape is not a shape.** `len` spans string, `[T]`, `Map`, `Set`, the four builders and
   `Mutable<T>`; `contains` spans the pairs of those. Each signature is writable on its own, and
   there is no single one. `snapshot`, `clear`, `push`, `insert`, `set`, `remove`, `abs`, `min`,
   `max`, `hash`, `compare`, `unwrap` and `is_ok` are all here.
2. **The rule reads more than types.** `format` validates its template string against the argument
   list; `print`, `printf` and `str` accept anything at all. No signature states either, so these
   stay in C whatever else changes.
3. **They are not callable names.** `$map_lit`, `$set_lit`, `$is_variant` and `$unwrap_variant` are
   desugaring targets the parser emits -- `Map<string, int> { "a": 1 }` becomes a `$map_lit` call,
   and `if Some(x) = v` becomes `$is_variant`. They appear in the builtin tables and can never be
   typed by a user.

**Why the distinction is worth a section rather than a comment.** `native` currently conflates two
claims -- "C supplies the body" and "C supplies the type rule" -- and only the first is true of a
native. The second is what makes a builtin a builtin, and it is also what makes a builtin invisible:
`--api builtins` prints signatures *from the prelude*, which `src/main.c` argues for in the right
terms ("The prelude declares all of them with real signatures, so it is the honest source") and then
has to except these twenty from, by name only.

The cost of that invisibility was measured on a real session rather than argued. A model working in
the North Star backend wrote `len(snapshot(out))` and `contains(snapshot(taken), at)` -- converting
a `MutableArray` into an array to ask its length -- hand-wrote a `contains_text(words, word)`
membership helper, and hand-wrote a quadratic `join`. Three separate corrections from the reviewer,
all one mistake: it had enumerated the API by name **prefix**
(`grep -o "mutable_array_[a-z_]*"`, thirty hits, which reads like a complete API) and an unprefixed
builtin cannot appear in that. The types it needed exist only as prose in docs/SYNTAX.md, which says
the thing the code could not: *"asking a collection its size never needs a per-type name or a
conversion."* Nine more hand-rolled membership loops were already in that backend, five of them in
production code and none with a `break`.

These names are also written out in four places with nothing cross-checking them: `typecheck.c` (the
type arms, plus `PURE_BUILTINS` and `BUILTINS_NO_NAMED_ARGS`), the interpreter's dispatch table,
`src/lsp.c`'s `BUILTINS[]`, and SYNTAX.md's prose. The LSP array has already drifted -- `snapshot`
four times, `contains` three, `len` twice -- and it is what drives editor completion, so the
editor's idea of the builtins is the least reliable of the four.

**F148 holds the proposal this entry exists to inform**: a `builtin fn` declaration, overloaded on
the first parameter, which splits the conflation -- the declaration moves to the prelude where it can
be printed and grepped, the rule stays in C where categories 2 and 3 require it. The restriction
that keeps it small is that the interpreter *already* dispatches these on `args[0]`
(`call_len(args[0])`, `clear(args[0])`, `snapshot(args[0])`), so a `builtin fn` set is a dispatch
table keyed on the first parameter's type constructor -- not overload resolution, and deliberately
not available to user code, since a second same-named `fn` in a program would make this C++
overloading and half-undo the "no methods, a function takes its receiver as an argument" decision.

**That last clause did not survive**: F184 made it available to user code the next day, and the
"half-undo the no-methods decision" worry turned out to be about a different thing. A method call
picks an implementation from a *receiver* -- dispatch on one distinguished argument, at the call
site, and the thing that makes `x.foo()` and `foo(x)` read differently. Overload resolution reads
*every* argument, picks a whole declaration, does it statically, and never ranks: there is no
receiver and no privileged position, so a function still visibly takes its arguments. See
"Overloading available to a declaration" below.



## An element write is one statement with two meanings (2026-10-18, F149)

`a[i] = v` reads like one operation and is two, and which one it is cannot be known where it is
parsed.

An array in Ziman is a value. Writing an element of one does not change anything; it produces a
different array, and the name has to be pointed at it. So `a[i] = v` on an array *is*
`a = set(a, i, v)` -- a rebinding, which needs `var`, and which can only be written where there is
a name to rebind. A `MutableArray` is a container reached through a handle. Writing an element of
one changes the thing every other holder of that handle can see, nothing is produced and no name is
touched, so `b[i] = v` writes through the handle and `val b` is correct.

Those are different statements. They differ in what they require of the left-hand side, in what
they do to the binding, and in what a reader should expect of the value afterwards. The only thing
that distinguishes them is the receiver's type, and the parser has no types.

So the statement is parsed as itself -- `STMT_INDEX_ASSIGN`, holding the index expression and the
value -- and `check_stmt` decides from the receiver's type which statement it is. The array form is
rewritten in place into the rebinding and re-checked, which is what gets it the ordinary "declared
with 'val'" diagnostic without index assignment restating it.

> **The builder form stopped being a rewrite in F209 (2026-10-21).** It was rewritten into a
> `mutable_array_set(b, i, v)` call, and `b[i]` into a `mutable_array_get(b, i)` call -- which is
> the only reason those two names had to be *declared*, and the reason they were *advertised*
> rather than `internal`: a traceback for `b[5]` printed a frame for a function the author had not
> written, so the name had to be one a reader could look up. The interpreter evaluates both
> accesses directly now (`EXPR_INDEX` and `STMT_INDEX_ASSIGN`, the one statement kind that reaches
> it as itself), the frame is gone, and `b[5]` out of range reports
> *"index 5 out of bounds (MutableArray length 1)"* beside an array's
> *"index 5 out of bounds (array length 1)"* -- which is this entry's own argument, finally applied
> to the error as well as the syntax. 139 call sites that spelled the access as a call
> are the sugar now.

**The previous arrangement was not neutral about this.** The rejection lived in the parser, so it
was one message for every receiver: *"an array element cannot be assigned: arrays are values, not
mutable containers -- write 'arr = set(arr, i, value)'"*. For a map that named the wrong `set`. For
a `MutableMap` it was simply false -- that *is* a mutable container. A diagnostic that cannot see
types will eventually say something untrue about one, and this one did, about the type the sentence
was contrasting itself against.

The same reasoning sets where the index and the element type are checked: before the rewrite, not
after. Checked after, `b[0] = "nine"` reported *"cannot infer type argument 'T': one argument
requires int, another requires string"* -- accurate about the call the checker had just written and
useless to the author, who wrote an assignment and no call at all. A rewrite may borrow another
construct's implementation; it should not borrow its messages.

**Why these two operations and not the rest of the pair list.** `len`, `contains`, `snapshot` and
`clear` already took either world, on the principle that asking a collection its size never needs a
per-type name. Reading and writing element `i` are questions of the same kind, and they had
per-type names. Appending is too, so `push` and `insert` became shared and `push` became variadic
in both worlds. `set(b, i, v)` deliberately did not: a three-argument copying call reads as though
it leaves its receiver alone, which is exactly the wrong thing to suggest about a builder, so it
reports that the write is `b[i] = v`. The `remove` family stayed prefixed for a sharper reason --
the immutable forms answer with the new collection and the mutable ones answer with the element,
the value, or a bool, so one name would have meant six things, half of them not collections.



## Overloading available to a declaration (2026-10-19, F174-F201)

The language's first principle list says "no overloading", and for most of the project that was
true of what a *declaration* could do and false of what the language actually did. By the end of
the collection-pairing pass there were **28 overloaded builtin names over 87 declarations** --
`len` and `contains` with nine each, `str` with six -- every one resolved by a hand-written arm in
`typecheck.c`, with the `builtin fn` entries in `lib/prelude.zm` as documentation the arms were
free to contradict. Two paragraphs of this file asserted the absence of a feature the language had
in 28 places. So the question F174 opened was not "should Ziman have overloading" but "should the
28 sets it already has be spelled where they can be read, checked and reused".

**What settled it was where the bugs came from.** Nine of the ten things that broke during the
pairing pass trace to the arms, not to overloading: a diagnostic quoting `$mutable_array_lit`, a
name that does not lex; a discarded `mutable(42)` accepted in silence, because the purity gate was
keyed per *name* while purity is per *overload*; `concat`'s spread rule written twice with the two
copies disagreeing; the quadratic lint keyed per name, advising a `MutableString` for an array;
eight named-argument call sites broken and `sort<int>([])` made unspellable, because a builtin
takes neither named arguments nor type arguments. Only the `[string]` ambiguity in `concat` was a
cost of overloading itself. A feature whose absence is simulated by hand in 87 places is being paid
for twice.

**Full resolution rather than first-parameter, and the objection that looked fatal.** The first
design resolved on argument one only; measured against the prelude it covered 43 of the 50 sets,
and the seven it missed were not exotic (four `MutableSet` operations distinguished by argument
two, `concat`'s arity rule, `mutable`'s catch-all arm). The objection to going further was that it
is circular -- an overload must be chosen before a lambda or an empty `[]` can be typed, and the
lambda cannot be typed before the overload is chosen. **Reading the code dissolved it.**
`infer_call_type_args` already walks the arguments skipping exactly the ones
`expr_needs_expected_type` flags, so the information needed was already being gathered for type
inference. Resolution is therefore a **filter, not a search**: it types the self-describing
arguments, reads the syntactic *shape* of the rest (a lambda's arity, `[]`, `ok(x)`, `Some(x)`),
and narrows. No speculative type-checking, no backtracking, no constraint solver, no ranking.
That this cost nothing structurally is the reason the feature was worth having; it is not a
general argument for overloading in a language where it would cost something.

**The three restrictions, each earned.** No return-type dispatch -- `to_set` and `to_mutable_set`
take identical arguments and differ only in what they answer, and the pass showed that keeping
them as two names gave the right answer, so the boundary is kept rather than relaxed. No
specificity ranking -- `fn g<T>(a: [T])` beside `fn g(a: [int])` is a redefinition, reported where
it is written, because ranking is where C++ and Java overloading become unreadable. No implicit
conversion to make a member match, which is the no-coercion principle unchanged. Together these
make the question "which overload does this call pick" answerable by reading the call and the
declarations, with nothing in between.

**Ambiguity is a compile error at both ends.** Two declarations no call could tell apart are
refused where they are written (`ovl_indistinguishable_at`), and `tools/overload_sets.sh` gates
the whole prelude in `make check` -- currently 39 resolver-owned sets, 0 unresolvable. A *call*
left open by an argument that carries no type of its own is refused at the call, naming the
argument that left the choice open. The warning the design took seriously was `concat`'s
`[string]`: a `[string]` is both "the pieces of a string" and "one array of parts", and only
having two names had kept those apart. A second instance has not appeared, and the two gates exist
so that it would be an error rather than a silent second reading -- which is the failure mode this
language refuses most consistently.

**Two populations, and the line between them is the real finding.** An *overload set* is several
real functions sharing a name. A *polymorphic builtin* is one implementation with several
documented signatures. Stage 8 asked how many of the second could become the first and converted
five arms of seventy -- `abs`, `swap`, `slice`, `to_set`, `len` -- before stopping on three
constraints, each found by reading an arm rather than counting names (the estimate ran ~50, then
~3, then 5). The message must be derivable from the declarations; a set carries one message where
an arm may carry many (`contains` carries twelve, and an arm with twelve messages is not stating a
type rule, it is answering differently depending on what you passed); and, the general one:

> An overload set resolves from arguments that describe themselves. A function whose job is to
> accept *anything* receives exactly the expressions that do not.

`str` passed the first two tests, was converted, and broke `str(switch cut.fits { ... })` in real
code, because a value block carries no type of its own -- measured, then reverted. So **the
collection vocabulary is the natural home of overloading in this language and the universal verbs
are not**, and the 65 remaining arms are a boundary rather than a backlog.

**What it removed.** For a converted set, the divergence `tools/check_builtins.py` exists to
catch, since the declaration *is* the rule rather than advice the arm may contradict -- but the
tool is **not** redundant and was not deleted, because the polymorphic builtins are still
dispatched and still need the guard that each one carries a declaration (stage 6 predicted its
deletion and F183 withdrew the prediction). The per-name keying of purity, the discard gate and
the quadratic lint, all of which were wrong for sets whose members differ. And
`editors/vscode/syntaxes/ziman.tmLanguage.json`, the last hand-kept name list, which by then
highlighted ten names deleted during the pairing pass and knew none of the 28 unprefixed ones; it
is generated from `ziman --api builtins` now (`src/lsp.c` had already been derived, in F168).
And the asymmetry that mattered most: `lib/graphics` could not express what
`lib/prelude.zm` did, and now can.

**It was used the same day.** The Map/Set pairing pass (F192-F201) rests on it entirely: one
`filter` over all six collections, one `keys`/`values`/`items`/`get`/`get_or` over both Map worlds,
the four set operations over both Set worlds, variadic `remove_all` and `add`. Names invisible to
`--api builtins` went **20 to 1**. The roadmap entry had said the feature would be worth building
if another row of the collection table wanted an overload; that test was met within a day.

**Two things left deliberately unfinished.** `#[when_unmatched("...")]` (F187) was built to give a
set a per-type hint, and stage 8 then stopped at five conversions, none of which needed it -- it
has no production user, only a fixture, and is owed either a user or a retirement.
`effectful_despite_answering` (F200) is a one-name veto list for the case the purity rule cannot
see -- `remove(mb, k) -> Option<V>` is effectful *and* answers -- carrying a written promise that a
third case means writing the attribute instead of extending the list.



## The prelude is a directory, and the seam is the type a name is about (2026-10-20, F203)

`lib/prelude.zm` had reached 1,971 lines and 269 declarations, which is the point at which "open
the prelude" stops being a useful instruction. Split into `lib/prelude/<part>.zm`, ten files, each
about one type: `array.zm` (873 lines), `string.zm`, `map.zm`, `set.zm`, `option.zm`, `channel.zm`,
`iter.zm`, `mutable.zm`, `collections.zm`, and `prelude.zm` for the universal verbs that belong to
no collection (`print`, `str`, `abs`, `min`, `max`, `hash`, `compare`, and number parsing).

**The seam had to be chosen against the previous two passes, not with them.** F149-F201 spent
themselves making one name cover several types -- `filter` over six collections, `len` over nine.
Splitting by type puts the declarations of one overload set in several files, which undoes exactly
the property those passes bought: that reading what `filter` accepts means reading one place.
There are two coherent seams and they are duals. **By type** answers *"what can I do with a
`Map`?"*; **by question** answers *"what does `filter` take?"*. A file layout can only have one.

**So the rule is: by type, except that a set is never split.** A name whose declarations would
otherwise land in two or more per-type files lives whole in `collections.zm`. Measured against the
prelude as it stood, that is **13 names and 61 declarations** -- `len`, `contains`, `clear`,
`snapshot`, `mutable`, `filter`, `map`, `remove`, `remove_all`, `set`, `get_or`, `update`,
`concat`. Everything else is single-world and goes to its type's file. Two groupings are what keep
that number small: `[T]` and `MutableArray<T>` share `array.zm`, so the five `sort*` names and
`reverse` and `flat_map` stay put rather than being dragged out; `Option` and `Result` share
`option.zm`, so `unwrap` does.

**The first count of this was wrong, and the way it was wrong is the usual way.** A regex allowing
leading whitespace counted `val out` and `val seen` *inside function bodies* as declarations and
reported 27 spanning names over 55. Anchored at column 0 the answer is 13. Estimates made by
counting matches overestimate; this file has said so three times now.

**Parsed per part, not concatenated.** Concatenating the ten into one string at build time would
have been near-zero C change, and was rejected for one reason: a prelude error would then report a
line number in a blob that exists nowhere on disk, and this project edits the prelude constantly.
Each part is lexed on its own, so its line numbers are its own, and registered with
`ziman_register_source` under its real path -- which makes prelude diagnostics *better* than
before, since `print_excerpt` reads from that registry rather than from the filesystem and a
bundled declaration previously carried no file name at all.

Registering the source was not enough on its own, and finding that out is why it is worth writing
down. `typecheck.c` sets the error file per function from `fn->source_file`, which is NULL here,
so it cleared the name the loader had set and a prelude type error still printed a bare
`type error at 58:27` -- now ambiguous across ten files where it had been unambiguous across one.
The fix is a second field, `origin_file`, consulted for the error file **only** when
`source_file` is NULL. Two fields rather than one because `source_file` is not a display field at
all: it is the "the user wrote this" gate, and the whole point is to say where a declaration is
without claiming the user owns it. The parts are merged into one `Module`
before anything resolves against it, because a name may be declared in several of them and the
resolver has to see the whole vocabulary at once to choose between declarations or report an
overlap.

**`source_file` is deliberately still NULL on prelude declarations.** It is not a display field:
`typecheck.c` sets `g_checking_program_body = fn->source_file != NULL`, and `coverage.c` skips a
function without one. Stamping the parts' paths there would have put the prelude into `--coverage`
reports and flipped the user-code gate. The path reaches diagnostics through the source registry
instead, which has no second meaning.

**What the split had to drag with it.** `gen_lib_deps.py` globs `lib/*/*.zm` and promptly listed
all ten parts as modules in the dependency graph -- the old single file missed that glob by shape
rather than on purpose, so the exclusion is now explicit. `gen_lib_manual.py` reads the Makefile's
`PRELUDE_PARTS` rather than keeping its own list, so the manual cannot come to disagree with what
is built; `docs/lib/prelude.md` stays one page, with a section per part. The parts are *not* on the
Makefile's `vpath`, and have their own `$(BUILD)/prelude_%_source.h` rule: keeping them off the
module search path is what guarantees `use map` can never find `lib/prelude/map.zm`.

**Three defects fell out of reading 1,971 lines closely enough to move them.** Two doc comments
compared a name to itself -- *"Named `filter_present` rather than `filter_present`"* and *"Named
`iter` rather than `iter`"* -- both casualties of F170's rename, where the old name was replaced
everywhere including inside the sentence explaining the rename. And ~100 lines of prose about the
array pairing pass sat above `filter_present`, so the split carried it into `option.zm`; it is one
consolidated section in `array.zm` now. A same-name comparison is worth grepping for after any
rename pass: `` `x` rather than `x` `` is always a defect and never a sentence anyone would write.



## A variadic parameter that cannot be empty (2026-10-20, F204)

`add(s)` returned the set unchanged. `remove_all(m)` removed nothing. `mutable_array_push(xs)`
pushed nothing. Each is a call that does exactly nothing and can only be a mistake, and each had
been an arity error until F199 replaced the singular `mutable_set_add(s, item)` and the plural
`add_all(s, variadic T)` with one variadic `add`. The floor went with the singular.

**It was not an oversight, which is the part worth recording.** `canonicalize_named_args` carries
a paragraph explaining that an unfilled trailing variadic means *zero elements, not a missing
argument*, and F199's own fixture pinned `add<int>(s)` answering `{1}`. The behaviour was chosen
and tested. What had not been noticed is that the choice is right for some variadics and wrong for
others, and nothing distinguished them.

**The distinction is whether the parameter has an identity.** `concat()` is `""`. `join()` is
`""`. `filter_present()` is `[]`. A SQL `query(conn, sql)` with no bind parameters is not a
degenerate call, it is the ordinary one — and there are fourteen of those across `sqlite`, `pg`,
`mysql` and `datasource`, so making at-least-one the default would have broken the database API to
fix a set operation. Counted across the prelude and `lib/`: **roughly fifteen declarations where
empty reads correctly against ten where it cannot.** Opt-in, therefore, not opt-out.

**`#[at_least_one]`, an attribute rather than new syntax.** It takes no argument, because a
function has at most one variadic parameter and it is always last, so there is nothing to name.
An attribute was chosen over a parameter marker (`variadic+`) for two reasons. The formatter is a
separate token scanner that would see `+` as a binary operator and respace it, so the marker would
have needed a lexer change to survive `--fmt`; and `#[thread_bound]` is the standing precedent
that a type rule in this language may be spelled as an attribute. It is refused on a function with
no variadic parameter rather than ignored — the same choice `#[when_unmatched]` makes for its bare
form.

**The escape hatch is the empty array, and it is the reason the rule is not in anyone's way.** A
variadic takes an already-built array as its sole trailing argument, so a caller holding a list
that may be empty writes `add(s, items)` and gets the empty case for free. That is one argument,
written out on purpose, rather than an omission — which is exactly the distinction the rule is
drawing, so the hatch and the rule are the same idea seen from two sides.

**Two code paths report it, so they were made to say the same sentence.** A call with too few
arguments is caught in `check_call`; a call that omits the parameter entirely never reaches there,
because `canonicalize_named_args` fills the gaps first and had its own thinner "missing required
argument '%s'". One rule reading two ways is how a language acquires the reputation of being
arbitrary, so the second path now carries the same explanation and the same pointer to the hatch.

**`ovl_min_arity` had to learn it too, not just the final check.** The attribute is part of a
declaration's arity, so a call below the floor must fail to *select* the declaration rather than
select it and then be refused — otherwise an overload set would resolve to one member and report
an arity complaint about it, when what the caller needs to hear is about the name.

Marked: `add` (both worlds), `remove_all` (all three), `push` (both), `mutable_array_push`,
`append`, `append_byte`, `min`, `max`. Left alone: `concat`, `join`,
`bytes_concat`, `filter_present`/`filter_ok`/`filter_err`, and every `params: variadic SqlValue`.



## `append` goes bare, and two names stay two (2026-10-20, F205)

`mutable_string_append` was the last builder append still carrying a world prefix. Every other one
had gone bare across F104-F201 — `push`, `add`, `set`, `get`, `keys` — and this one was missed for
a reason visible in this file: the entry above chose the *verb* deliberately (*"a fifth verb,
taken deliberately, because text genuinely reads as appended"*) and never came back for the
prefix. Choosing the verb read like settling the name. 484 call sites, 452 here and 32 in the
North Star backend.

**They stay two names, and that is the decision rather than the rename.** `append(sb, pieces:
variadic string)` and `append_byte(sb, b: variadic byte)` differ in argument two, so the resolver
could tell them apart and full overloading would allow one name. It should not have one: the
string form answers nothing and the byte form answers a `bool` (false when a byte is `0b`, which a
string cannot hold, so nothing is appended). **A name that sometimes has a result and sometimes
does not is exactly what F149 kept `remove` split over**, and what F199 merged `add` on the
strength of — both worlds there agreed on what they answered. The suffix is not a world prefix
either: `_byte` says *what it appends*, which is F170's distinction, the same one that made
`array_present` into `filter_present`.

A second constraint points the same way and is worth recording because it is a real limit of the
mechanism rather than a judgement: **the native symbol fallback derives from parameter one only**
(`interpreter.c`, the `WORLDS` table). Two `append` declarations both taking a `MutableString`
would both derive `mutable_string_append`, so the byte overload would silently have called the
string implementation. It is the corroborating reason, not the deciding one — had the vocabulary
argument gone the other way, the fallback is what would have had to change.

**The old-name hints had rotted, and nothing was watching them.** `renamed_builder_fn` answers
`mapb_get` with *"'mapb_get' is now 'mutable_map_get'"* — and `mutable_map_get` had not existed
since F195. Four entries pointed at names F195 and F200 deleted, and a fifth offered the
`internal` `mutable_set_add` where the vocabulary is `add`. A hint naming something that is not
there is worse than no hint: it reads as authoritative and costs a search. `check_builtins.py` now
holds every target to being a declaration that exists, and every *old* name to not being one.

**The table's single explanatory sentence had gone stale the same way**, and more quietly. It said
the renames were about *"the four questions every collection answers -- len, contains, snapshot,
clear"*, which was true of F104's entries and false of everything added since. It now states the
rule all of them followed: the prefix said which world the first argument came from, and the
argument says it.

**One keyed-by-name table had to move with the rename**: the quadratic lint's `BUILDER_KINDS`
keys advice on the *appender's name*, so leaving `mutable_string_append` there would have silently
stopped the lint firing for strings. Its fixture caught it, which is the second time that lint's
name-keying has been the thing a rename broke (F162 left two dead branches in it).


## The last prefixed appenders go, and a mutation gets to say it answers something (2026-10-21, F208)

`mutable_array_push`, `mutable_array_insert` and `mutable_array_remove` were the last three
builder operations spelled with a world prefix. They were `internal` — out of `--api builtins`,
out of completion — because the bare names had superseded them: F207 gave `push` and `insert` a
`MutableArray` declaration of their own, and `remove` had one in `collections.zm` since F200. So
each was a second spelling of a name that already worked, and the prelude's own code went on
typing it, which is how **767 call sites across this repo and the North Star** came to use a name
nothing advertised — 509 here (506 in `.zm` files, three more in a fixture shell script that
embeds Ziman) and 258 in the backend, counted as
`git grep -o -E 'mutable_array_(push|insert|remove)[<(]' <before> -- '*.zm'` less the three
declarations and the five prose mentions. A reader learns the vocabulary from the corpus, not from
the listing.

**Deleting the declaration is almost all of the change.** The C symbols keep the prefix, which is
where this library decided years of entries ago that the prefix belongs: `native_lookup` is keyed
by its own string, and interpreter.c's symbol fallback derives `mutable_array_push` from parameter
one. Nothing in the runtime moved. What moved was `src/derive.c`, which *emits* Ziman — three
`#[derive]` bodies wrote `mutable_array_push(...)` into generated code and would have generated a
call to a name that no longer exists.

**`remove` on a MutableArray was not the same shape as the other two, and the difference was
hidden by an arm.** Its declaration said `builtin fn`, but the builtin id never ran: `check_call`'s
arm rewrote the callee to `mutable_array_remove` first, and `call_remove` only ever handled an
array — it would have read an `ArrayObj` straight out of a handle had anything reached it. So the
declaration was documentation and the arm was the implementation. It is a `native fn` now, the arm
branch is gone, and the resolver owns it like the other two.

**That rewrite was also the only reason `pop` compiled.** Discarding a result in statement
position is an error for a prelude overload that answers a value, because the two worlds answer
the same question differently — the `[T]` member computes a new value, the builder member
rearranges a buffer and has nothing to return — so "answers something" means "has no effect"
(F182). `remove` on a builder breaks that: it writes the container *and* answers what it took out,
and `remove(b, len(b) - 1)` is how `pop` is spelled. That call was accepted only because the gate
read the rewritten name, did not recognise it, and said nothing. **An accident that reads as a
decision is worse than a missing feature**, because the fixture asserting it passes.

**`#[effectful]`, an attribute, because the alternative was a list of names and this is the third
case.** F182 asked for a marker on the declaration if a second appeared; a pair of type names in
`effectful_despite_answering` covered `remove` on a MutableMap and a MutableSet, and the third
arrived the moment the MutableArray form stopped being a rewrite. So the fact lives on the three
declarations that have it, the list is gone, and a fourth needs no edit in `typecheck.c`. It takes
no argument and is refused on a function that answers nothing, where it could only mean nothing —
the same choice `#[at_least_one]` and `#[when_unmatched]` make.

**`BUILDER_KINDS` again**, for the third rename running (F162, F205, F208). The quadratic lint
keys its per-type advice on the *appender's name*, so `mutable_array_push` sitting there would
have silently stopped the lint firing for arrays. Its fixture caught it, as it did last time.

**Two name lists were asserting things about names that no longer existed.** `tests/run_api.sh`
and the LSP test each held nine prefix-layer names and checked that none was advertised or
completed. All nine had been deleted by F180, so both loops passed by vacuity — the same rot
`tools/check_builtins.py` was written to stop one file over, in a test whose whole subject is
stale names. Each now names `mutable_set_add`, the one still declared.

**What stays prefixed, and why it is not the same case.** `mutable_array_new` and
`mutable_array_filled` are constructors with no bare form to supersede them. `mutable_array_get`
and `mutable_array_set` are what `b[i]` and `b[i] = value` rewrite to, and what the out-of-range
runtime error and the lint's reader advice name; there is no bare `get` or `set` for a builder,
`set(b, i, v)` being refused on purpose so it can say that an element write is `b[i] = value`.
Hiding those two would point a message at a name the listing denies exists.

> **The second half of that paragraph lasted a day (F209, 2026-10-21).** It is a correct argument
> about the rewrite and a wrong conclusion about the names: the fix was to stop rewriting, not to
> keep the names findable. A MutableArray is indexed rather than called now, so there is no frame
> to name and no declaration to keep — see "An element write is one statement with two meanings"
> for the mechanism and "`b[i]` stops being a call" for the rest.

**Accepted loss: `push` can no longer take named arguments.** `push(ab: b, pieces: ...)` worked
while the prefixed name was a lone native; the bare name is shared with a `builtin fn`, and an
id-dispatched name takes neither a named nor a type argument (F178's own note). Four call sites,
all in fixtures exercising "several shapes", now positional. The reading is unchanged and the
spelling nobody would choose.


## `b[i]` stops being a call (2026-10-21, F209)

F149 made `b[i]` and `b[i] = value` work on a `MutableArray` by **rewriting them into calls** —
`mutable_array_get(b, i)` and `mutable_array_set(b, i, v)` — which is why those two names existed
and, more to the point, why they were `pub`. A rewrite target has to be declared for the sugar to
resolve at all, and an advertised name is what keeps a traceback honest:

```
ziman: runtime error at 4:12: get index 5 out of bounds (MutableArray length 1)
    at mutable_array_get (4:12)        ← for code that only ever wrote b[5]
```

The frame is `fn->name`, so hiding the declaration would have pointed a stack trace at something
`--api builtins` denied existed. F208's entry states that as the reason the two names stay. **It is
a correct argument about the rewrite and the wrong conclusion**: the thing to remove was the
rewrite, not the evidence of it.

**A MutableArray is indexed now, not called.** `EXPR_INDEX` already evaluated an array's `a[i]`
directly; it now tells the two worlds apart by the value it got — an array holds its elements, a
builder holds a handle — and reads the buffer through `ziman_mutable_array_index_get`. The write
side makes `STMT_INDEX_ASSIGN` **the one statement kind that reaches the interpreter as itself**:
the array half is still rewritten into the rebinding it stands for (`a = set(a, i, v)`, which is
what earns it the ordinary `val`/`var` diagnostic for free), and the builder half is evaluated
where it stands. The two C functions keep the world prefix, which is the one place it has meant
anything since F208.

**The error reports what F149's own entry argued for.** With no call there is no frame, and the
message drops a prefix that existed only to tell two function names apart:

```
index 5 out of bounds (MutableArray length 1)      beside an array's
index 5 out of bounds (array length 1)
```

F149 was about `b[i] = v` and `a[i] = v` being one syntax over two statements. The errors now agree
with the syntax, which they did not while one of the two went through a function.

**139 call sites still spelled the access as a call**, which is F208's finding again:
`--api builtins` listing a name is not what teaches it, the corpus is, and the corpus was written
before the sugar existed. They are `t.data[i * 4] = c.red` and `row[x]` now. Converted with a
paren- and quote-aware pass rather than a regex — the arguments are arbitrary expressions, and
`counts[b] = counts[b] + 1` is a `set` whose *value* contains a `get`, so the pass runs to a
fixpoint. It refuses a base that is not an atom rather than inventing `(a + b)[i]`; nothing in the
corpus needed that.

**The old names answer with syntax, which needed a second table.** `renamed_builder_fn` is held by
`tools/check_builtins.py` to naming a declaration that exists — the guard F205 added after four
entries rotted — and `b[i]` is not a declaration. Putting the syntax answers in the same table
would have meant weakening that guard, so they get `renamed_to_syntax` and their own shape:
*"'mutable_array_get' is gone — write 'b[i]' instead: element access is an index rather than a
function, so there is no name left to offer."* `ab_get` and `ab_set`, the generation before, move
there too; they had been pointing at `mutable_array_get`. (F211 gave each entry its own reason
rather than one shared sentence, when `map_from_arrays` joined them with a different story.)

**What this does not change.** There is still no bare `get` or `set` for a builder, and
`set(b, i, v)` is still refused with the message that an element write is `b[i] = value` — that
refusal is now the only place the word appears, which is where it belongs. `mutable_array_new` and
`mutable_array_filled` remain, and are the whole of the `mutable_array_*` vocabulary.

**Two stale repair suggestions found beside the work and fixed.** Indexing a `Set` advised
*"add with 's = set(s, value)'"* and writing one advised *"add with 's = insert(s, value)'"*. F199
made `add` the word in both set worlds and F206 made the checker refuse both of those spellings, so
each message was telling the reader to write a type error. Both say `add(s, value)` now. They are
the same rot as F208's two vacuous name lists: a message that names another part of the language
goes stale when that part moves, and nothing was watching.



## `concat` keeps the decision and gives up the dispatch (2026-10-21)

`concat` was the last bespoke arm this effort had written off. Three sessions of comments in
lib/prelude/string.zm called it *the conversion that cannot be done* and listed three walls to
prove it, and §6 and §10 of docs/design/full-overloading.md both excepted it by name. The walls
were real. The conclusion was wrong, and the mistake is worth recording because it is a kind:
**two things were bundled into one and the argument against one was read as an argument against
both.**

A `builtin fn` buys a *rule that chooses* and a *dispatch that runs*. The three walls are all in
front of choosing:

1. `concat()` answers `""`, and the string form and the array form both accept no arguments.
2. `concat(someStrings)` must answer `"xyz"`, while the array form takes the same argument as one
   part and would answer `[string]`. They differ **only in return type**, which F148 ruled out as a
   basis for resolution — the reason `to_set` and `to_mutable_set` are two names.
3. `concat(b, [5], mutable([6]), [7])` mixes an array and a builder in one variadic tail, and a
   variadic parameter has one type. Two declarations can each state half of that tail. Neither can
   state the whole, and a union type is refused outright.

Not one of them is a reason the name needs a builtin **id**. So the arm stays and chooses exactly
as before, while the four declarations become `native fn` and the arm records its pick on
`e->call_fn` — the same mechanism every conversion since F178 has used, applied to a set the
resolver still cannot resolve.

**What it buys.** The declarations become the signature of record, so `--api builtins`, completion
and the errors read from them. Named arguments work for the first time: `concat(pieces: "p")` was
a *syntax error* while the name was an id, there being no parameter to name — and so was
`concat<int>([1], [2])`. `PURE_BUILTINS` loses an entry, purity now being read off the return type
of the resolved declaration. And the interpreter's ~48-comparison `else if` walk is gone, which is
why `push` and `insert` got faster when they converted rather than slower.

**The fourth wall was the declaration checker**, which enforces the same distinguishability as the
call site: two declarations may share a name only when every call can tell them apart. `builtin fn`
members were exempt because the resolver never looked at them; `native fn` members are not. The
first instinct was a list of names in `typecheck.c` exempting `concat` — exactly the shape every
stage of this effort has been deleting. It became an attribute instead, `#[compiler_resolved]`,
which the declaration carries and which `--api builtins` prints, since a reader comparing four
signatures that cannot be told apart needs to know something else is choosing.

**An attribute that turns off a safety check needs its own check, and gets three.** The guard it
disables is the one that catches a set no call can resolve, so:

- It is **refused where it would mean nothing**, as `#[at_least_one]` and `#[effectful]` are: a
  declaration whose siblings every call can already distinguish — or that has none — is a compile
  error, the attribute having no effect but to disable a working guard. A `builtin fn` wearing it
  is refused too, for the same reason in reverse.
- `tools/overload_sets.sh` counts the new population apart and checks it **inversely**: 52
  resolver-owned sets with zero unresolvable, 9 polymorphic builtins, 1 compiler-resolved.
- **The hazard closes itself.** Marking two genuinely ambiguous declarations in program code does
  not ship them; the error moves from the declaration to the call, where the resolver finds both
  accepting the arguments and reports an ambiguity that *lists* them. The attribute buys a prelude
  name the rule it already had and buys program code nothing, which is the best outcome available
  for an exemption: it cannot be used for the thing it looks like it enables.

1,127 fixtures. `concat`'s own diagnostics are unchanged, being derived from the arm as before, and
every shape is now a fixture rather than a comment: tests/lang/concat_declarations.zm.



## The filing rule finishes, and six against four is where the line fell (2026-10-21)

The prelude became a directory in F203 with the seam "a file is about one type", and collections.zm
was the exception: the file for names that are about *no* one type. The original rule put a name
there as soon as its declarations would otherwise be split across two files, which kept every
overload set whole in one place. That reversed on 2026-10-21 — **a declaration lives in the file for
the type its first argument has** — and this is the other half of that change, the part that moves
the sets the reversal was about.

Five left collections.zm: `update` to mutable.zm and map.zm; `set` to map.zm (two members) and
array.zm; `remove_all` to map.zm (two) and set.zm; `snapshot` and `clear` to four files each, one
member apiece. Seven names stayed — `len` over nine types, `contains` over nine, `filter` over six,
`remove` over six, `mutable` over four of which one takes a bare `T` and so has no file at all, and
`map` with `flat_map` beside it (F216).

**The threshold is not a principle, and the comment in collections.zm says so.** `snapshot` has four
members and splits one per file; `filter` has six and stays whole. What makes those different in
kind rather than in degree is that `filter` and `remove` reach *both* worlds of all three
collections, so every per-type file would hold a fragment and `--api builtins` would be the only
place the whole set exists. Four members over four files is still four files a reader can be sent
to; six over six is a set with no home. Where exactly it tips is a judgement, and recording that it
is one is better than inventing a rule that sounds inevitable.

**One observable thing changed, and it is worth knowing before the next split.** A set's
`#[when_unmatched]` error lists the types it accepts in *declaration* order, and declaration order
is file load order once a set spans files. So `snapshot`'s refusal went from naming
"MutableArray<T>, MutableString, MutableMap<K, V> or MutableSet<T>" — collections.zm's hand-chosen
order, sequence-like first — to "MutableArray<T>, MutableMap<K, V>, MutableSet<T> or MutableString",
which is array, map, set, string: the prelude's files in alphabetical order. Four `.err` fixtures
pinned the old wording and now pin the new. The list is still complete and still deterministic; what
a split set gives up is the ability to choose the order a reader meets the types in, and no amount
of commenting in one file can get it back.

**What is left is the same work one layer in.** map.zm and set.zm still group their own names by
world rather than by set — `get`, `keys`, `values`, `merge` in one, `add`, `union`, `intersect`,
`difference`, `is_subset` in the other, each with a builder block and an immutable block between
them, which is exactly the shape F210 found in array.zm and the sweep found in collections.zm. It
is a larger piece than it looks: most of those immutable declarations carry no doc comment at all,
so the blank entries they publish in `docs/lib/prelude.md` have to be *written* rather than moved,
and that is content work rather than a reordering.

1,127 fixtures, every guard green. iter.zm turned out to still state the rule this replaced, having
been missed when the reversal shipped; all ten headers now say the same thing.



## A lambda argument can answer a type parameter (2026-10-21)

`map(points, Point::x)` did not compile. It had to be written `map<Point, int>(points, Point::x)`,
naming both the type the field is read from and the field's own type — at a call site whose whole
argument for existing is that `fn(p: Point) -> int { return p.x }` names them and `Point::x` does
not. The shorthand shipped two days earlier with that noted as its open gap.

**It was never about `::`.** `Point::x` desugars in the parser into `fn($x: Point) { $x.x }`, so it
inherited a general hole: **a lambda argument never contributed to inferring a generic result.**
Every one of these failed identically, with "cannot infer type argument 'U' of 'map' ... write it
explicitly":

```
map(xs, fn(n: int) { n * 2 })      // parameter annotated, result not
map(xs, fn(n) { n * 2 })           // neither annotated
map(xs, fn(n) -> string { str(n) }) // result annotated, parameter not
map(points, Point::x)
```

**Why the first pass cannot do it.** `infer_call_type_args` walks the arguments once and skips
every one that `expr_needs_expected_type` claims — an empty `[]`, a bare `None`, an `ok(x)`, and a
lambda missing any annotation, because filling a lambda's parameter in needs the expected type and
the expected type is what is being inferred. Skipping is right. Never looking again was the bug.

**The second pass.** Once the other arguments have bound what they can, substitute those bindings
into the declared parameter type: `fn(T) -> U` with `T` bound from `xs` becomes `fn(int) -> U`. A
lambda whose parameters have all become concrete **types on its own** — its body is checkable with
no expectation at all, and a final bare expression answers the result. Then unify that against the
pattern, which binds `U`.

The part that had to be got right is what *not* to pass down. Handing `want` whole to
`check_lambda_expecting` is the obvious move and is wrong: it assigns `*want->elem` as the return
type, so the lambda comes back typed `fn(int) -> U` with a typevar standing where the answer
belongs. Passing `NULL` is the path that reads the body instead. It memoises on `lambda_checked`,
so the ordinary argument loop reuses the result rather than checking the body twice, and the
lambda's own annotations win wherever it has them — which is why all four shapes above work and
argument order does not matter (`fn pair<A, B>(f: fn(A) -> B, x: A)` infers as well as `apply`).

**One refusal had to be added back, and it is the interesting part.** The pass first made this
compile:

```
map(xs, fn(n: int) { print(n) })    // answered [0, 0, 0]
```

A body that answers nothing leaves the lambda's result `void`, `U` bound to void, and `map` built
an array of nothing whose slots printed as zeros. **That is worse than the refusal it replaced** —
an unhelpful message beats a wrong answer — so the pass records such an argument instead of
unifying it, and the error names the argument and what is missing from it rather than asking for a
type argument no body can produce a value of. It fires only when the declared *result* is the
unknown: a parameter written `fn(T)` wants nothing back, which is what `for_each` is, so there is
no type parameter in the result position, the second pass skips the argument, and the void lambda
is accepted as it always was.

**What it cost elsewhere.** One fixture changed its expected message:
`apply(5, fn(n) { return n * 2 })` now reports the lambda's own rule — *"cannot infer the return
type of a lambda that uses 'return' — write `-> T` after the parameters"* — instead of asking for
the call's type arguments. That is a different rule, not this one: a lambda using `return` with no
`->` is refused outright, so the second pass gets nothing from it. The new message is reported at
the lambda and names a smaller edit than `apply<int, int>(...)`. Lifting that `return` restriction
is a separate decision and was not taken here.

`tests/lang/field_accessor_shorthand.zm` lost its type arguments on every line, and its `.out` is
byte-identical — the behaviour was always right, only unsayable. 1,129 fixtures.



## `--api builtins` groups an overload set, and the claim that it already did (2026-10-22)

All ten prelude headers said this, as the reason a declaration could live in the file for its first
argument's type while its siblings lived elsewhere:

> `ziman --api builtins` still prints a set together, so the listing answers "what does this name
> accept" in one place even when the sources do not.

**It was false, and it was false because of the change it was justifying.** The listing is built
from the prelude parts merged in *file order* and printed straight through, so it grouped a set
only while no set was ever split — which was true right up until the filing rule reversed on
2026-10-21 and started splitting them. Measured the day after, with `filter`'s array pair the
occasion for looking: `filter` printed in two blocks 56 lines apart, `concat` in two, `get_or` in
two, and `snapshot` and `clear` in **four** each.

So the sentence was not merely stale. It described a property that the rule citing it destroyed,
and it had been copied into ten files, which is how a justification survives the thing that
falsifies it: nobody runs the command, because the sentence already says what it would show.

**The listing groups by name now**, which made the claim true rather than deleting it. A set prints
where its **first advertised member** does, not in alphabetical order: the file order is topical —
a string's functions together, an array's together — and sorting the whole list would scatter that
to fix the smaller problem. Verified as a pure reordering rather than by eye: 307 lines before and
after, identical as sets.

`tests/run_api.sh` now checks twelve names, on the *sequence of signatures* rather than on line
numbers, because an attribute prints on a line of its own between two signatures — `concat`'s four
and `remove`'s six are deliberately non-consecutive lines while still being one block. The property
is that no other name's signature falls between a name's first and last. Against the ungrouped
listing, 7 of the 12 fail, naming the spread (`snapshot`: 4 declarations over 197 slots); the other
five pass either way, their members all living in one file.

**`docs/lib/prelude.md` is still not grouped**, and the headers now say so. It is organised as one
`##` section per file, so a split set keeps a `###` heading in each — the listing is the one place
a whole set reads as a set. Restructuring the manual is a larger question than this was.

**The transferable part.** A claim about what a tool prints is checkable in one command, and this
one went unchecked through the change that broke it, a day of edits, and ten copies. The same shape
as F212's two vacuous name lists and `showcase.zm`'s three passes of not compiling: *the sentence
that explains why something is safe is the sentence least likely to be tested.*



## `filter` over a Map and a Set, and a guard natives could not reach (2026-10-22)

The last two `builtin fn` members of `filter` become `native fn`. They had shared **one** builtin
id whose branch asked `args[0].type->kind` at run time to tell a Map from a Set; the symbol
fallback reads parameter one, so each now finds a C function of its own, `map_filter` and
`set_filter`, and neither has to ask what it was given. No arm to delete — `filter` never had one,
being declaration-checked already — so this is the id, its roster entry, its 28-line branch, and
three name-list entries (`BUILTINS_NO_NAMED_ARGS`, `BUILTINS_WITH_NAMED_ARGS`, `PURE_BUILTINS`).

Named arguments work for the first time (`filter(m: stock, keep: p)`), type arguments too, and
discarding the result is still refused — now by F182's return-type rule reading the resolved
declaration instead of by a name in a list. Measured at 2,000 entries × 1,200 rounds, interleaved:
old 1.37/1.40/1.37s against new 1.40/1.36/1.49s, which is noise. The predicate is a Ziman call per
entry and that dominates, the same reason `contains`, `snapshot`, `clear` and `items` came out at
+0.8%.

**The part worth recording is what the conversion needed.** A native that walks a collection's
entries *and* calls back into Ziman is the first of its shape. `mapobj_entries` hands back a plain
`malloc`'d array of keys and values, which neither an Env nor a frame holds — and the custom root
sources are Env chains and frames, not the C stack. `native_call_closure` already wraps its own
call in Phase 3's collection guard, but not the gap *between* two calls, and `mapobj_set` allocates
in that gap on every kept entry. The builtin branch had held the guard over its whole loop;
`gc_unsafe_enter`/`gc_unsafe_leave` are static to interpreter.c, so a native had no way to. They
are exported now as `native_gc_unsafe_enter`/`_leave` (native.h), which is the second thing
`native.h` gives a native after `native_call_closure`, and the next Map or Set builtin to convert
will want it too.

**And the honest part: the guard is kept without a demonstrated failure.** Phase 3 is mark-sweep
rather than copying (MEMORY.md, 2026-09-30), so the hazard would be a free rather than a
relocation, and Boehm's own conservative stack scan plausibly keeps the source collection alive
through the native's `args` regardless. I tried to break it: 20,000 entries, a predicate allocating
40 strings and a builder per call, the source map an *unbound temporary* so no Env roots it, three
runs — correct answers every time with the guard removed, and the full suite green too. It stays
because the code it replaces guarded this same loop and cites a real incident for the rule, and
removing a guard one cannot prove unnecessary is the riskier direction. Both comments say that
rather than asserting a mechanism; the first draft of them claimed a *moving* collection would
"leave the entries pointing at their old locations", which MEMORY.md had superseded a month before
this was written.



## collections.zm is retired (2026-10-22)

The prelude became a directory in F203 with one exception built in: a file for the names that are
about no one type. `collections.zm` held them under the rule that an overload set is never split,
and when the filing rule reversed -- a declaration lives in the file for the type its first
argument has -- that file had no reason left. Its names went out one at a time over two days:
`concat`, `get_or`, `update`, `set`, `remove_all`, `snapshot`, `clear`; then `filter` and `remove`,
six members each; then `map`, `flat_map`, `len`, `contains` and `mutable` together, 28 declarations
in one move, which emptied it. Nine parts now, and deleting the tenth took the three edits adding
one takes, reversed: the file, the Makefile's `PRELUDE_PARTS`, one line in prelude.c's `PARTS`.

**Where the twelve-member names ended up**: `len` and `contains` across five files each (array,
string, map, set, mutable -- two per file but for the cell's one), `filter`, `remove` and `mutable`
across three, `map` across three, `flat_map` back in array.zm.

**One thing had to be true first, and it was not when the splitting started.** The reason to keep a
set whole was that nothing else showed it whole: *"every per-type file would hold a fragment and
`--api builtins` would be the only place the whole set exists"*. That listing was in file order, so
it did not group a split set either -- the claim was false for a day, and is the subject of its own
entry. Grouping it by name is what made this restructuring readable rather than merely tidy:
`len`'s nine print as one block across five files, and `tests/run_api.sh` asserts that for twelve
names. **Without the listing fix this change would have scattered the vocabulary with nothing to
reassemble it.**

**F216's exception retired itself.** `flat_map` had been moved *to* collections.zm to sit beside
`map`, both its declarations being array-world and the filing rule therefore pointing at array.zm;
the note recording that said a second such name would mean rewriting the rule rather than adding
another exception. No second name came. Instead `map`'s array members moved to array.zm, so
`flat_map` followed them home and now satisfies the rule *and* sits beside `map`'s array half.

**What the deleted header carried, and where it went.** prelude.zm now holds the parameter-name
scheme (one role, one name, across every declaration that has it), the rule that a set's members
sit together within whichever file they land in, and the one piece of work collections.zm left
behind: **map.zm and set.zm are still grouped by world** -- `get`, `keys`, `values`, `merge` split
in one, `add`, `union`, `intersect`, `difference`, `is_subset` in the other. That is larger than a
reorder, because most of those immutable declarations have no doc comment, so the blank entries
they publish in `docs/lib/prelude.md` have to be written rather than moved.

**Five stale cross-references fell out of the move**, each a comment in another file claiming
something still lived in collections.zm, and one older one that the move exposed: the MutableArray
`remove` had said *"There is no `MutableMap` or `MutableSet` form"*, true the day it was written
(2026-10-18, when those two were still prefixed names) and false from the Map/Set pairing pass
onward, since that pass gave them exactly that bare name. Moving a comment is a good way to find
out whether it is still true.

1,133 fixtures, every guard green. One `.err` fixture changed: `len`'s accepted-type list is in
declaration order, which is file order, so it reads "[T], Map<K, V>, Mutable<T>, Set<T>, string or
a builder" where it had read "string, [T], Map<K, V>, Set<T>, Mutable<T>". The cost of a split set,
recorded when `snapshot` first paid it.



## The prelude stops being outside visibility (2026-10-22)

`pub` said nothing about reachability in `lib/prelude/`. The parts are merged bare into every
program, so a call into the prelude resolves as an ordinary same-module call and there was no
import boundary for a modifier to be checked at — `pub` chose only what `ziman --api builtins`
advertises, and every declaration was callable whatever it said. Asked whether natives are public
by default I first answered from a comment that claimed the same thing, which is how this started.

**Two halves, and the first is what made the second possible.** 143 declarations carried no
modifier — 91 `native fn` and 52 `builtin fn` — against 125 that did, because `pub` is *inert* on
those kinds: they are advertised through `fn_is_global_vocabulary` regardless. One keyword, two
meanings, depending on the kind of declaration it sat on, which is why `snapshot` and `clear` in
map.zm looked like an oversight beside seventeen `pub native` siblings. Writing `pub` on all 268
public declarations is checkable as cosmetic — `--api builtins` is byte-identical across it — and
it buys the reading that **a bare declaration is deliberately not public**.

**Then the rule.** A prelude declaration must be `pub` to be called from outside the prelude;
`internal` reaches the bundled library's own package, so lib/ may call `sort_positions` and a
program may not; a bare declaration belongs to the file that declares it. `g_checking_program_body`
is the boundary — false only while a prelude body is being checked — and `module->is_corelib` is
what makes `internal` mean package rather than nothing. Verified in both directions: a bundled
module can call `mutable_set_add`, a program cannot.

**This reverses F172, which chose the middle state on purpose.** It left names hidden from
completion *and still callable*, and `prelude_internal_still_callable` pinned exactly that, its
comment warning that hiding them "would have broken exactly this, and silently". That fixture is
replaced by two rejection tests, one per kind of refusal.

**Seven names were in the gap**, and every caller outside the prelude moved to the public spelling
before the rule went in: 30 `mutable_set_add` calls across 15 files became `add`, which is what
`add` has meant in both set worlds since F199. Two fixtures that called `array_order_of` directly
now prove the same stability through `sort_by`/`sort_by_desc`, its only consumers; two that used it
as a generic function value declare their own bounded generic, which is better than depending on
prelude machinery to test the language.

**What migrating found.** `BUILDER_KINDS` keys the quadratic-snapshot lint's advice on the appender
in the loop body, and its MutableSet entry was `mutable_set_add` — so that lint had caught nothing
for a MutableSet since F199, and its fixture only reached four warnings because the fixture itself
used the obsolete name. Migrating it dropped the count to three. Third rename to break that table.
And `src/derive.c` *emits* `mutable_set_add`, so `map_json_derive` failed the moment the rule went
in: generated code is attributed to the user's file, which is outside the prelude. Both are the
same lesson as F208's — a rename's blast radius is not the `.zm` files.

**What this does not do.** File-private is not enforced *within* the prelude: the parts are one
merged unit, so `array_order_of` is refused to a program but not to another prelude part. Nothing
needs it today — `sort_by` and `sort_by_desc` are in the same file — and enforcing it would need a
per-part notion the merge deliberately does not keep.

`docs/lib/prelude.md` now requires `pub` like every other page, so the one non-`pub` declaration
stopped being advertised as vocabulary a program is refused for using. 1,142 fixtures.
