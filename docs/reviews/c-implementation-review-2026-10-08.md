# C-implementation review of Ziman — 2026-10-08

## 0. What I started from

- **Commit:** `88dca78` ("A third review prompt: the implementation judged as C"). `git status` at the
  start showed one modified file, `logs/health-record.log`, a benchmark artifact that predates this
  review and that I left alone.
- **`make check`:** re-run from scratch on this tree during the review. **Exit 0.** Both fixture
  suites, the 13 standalone C unit tests, `--fmt --check` over the library and examples, and every
  documentation guard pass (`/tmp/ziman_review_c/make_check_run2.log`; the earlier identical run is
  `make_check.log`).
- **Size:** 52,130 lines across `src/*.c` + `src/*.h` including the generated `*_source.h`;
  808 `.zm` fixtures, 884 assertions in the `nogc` suite.
- **Compiler:** `gcc (GCC) 15.3.1 20260722 (Red Hat 15.3.1-1)`, Fedora 43, `Linux 7.2.7`.

### Sanitizer and analysis invocations

There is still no sanitizer target in the `Makefile`, so every sanitizer build here is out-of-tree
under `/tmp/ziman_review_c`. **This machine has no system `libasan`/`libubsan`/`libtsan`** — the one
thing to know before trusting the results below: the runtimes were fetched as RPMs and unpacked into a
local root (`rpms/`, `sanroot/usr/lib64`, import libraries in `sanlink/`), and the binaries link
against them with `-rpath`. I verified that works rather than assuming it — four of the five run a
hello-world to exit 0 and the fifth fails for a reason I characterise below. **UBSan, ASan and TSan
were really exercised here; nothing was quietly skipped.**

| binary | what it is | how I ran it |
| --- | --- | --- |
| `ziman-asan2` | ASan, collector build, with the stack-scanner workaround | `ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0` |
| `ziman-asan-ubsan` | the first ASan+UBSan build | see the note below — unusable as-is |
| `ziman-ubsan-nowrapv` | UBSan on the plain build **without** `-fwrapv` | default options |
| `ziman-gc-ubsan` | UBSan on the collector build (with `-fwrapv`) | default options |
| `ziman-tsan` | ThreadSanitizer | `TSAN_OPTIONS=halt_on_error=0` |

**Honesty note on provenance.** These five binaries were built by an earlier session of this same
review that a usage limit cut off before it wrote anything. I inherited them rather than rebuilding
them, and I did not reconstruct the exact compiler command lines (GCC did not record them in the
objects; I checked). I did re-run every reproduction myself, and every finding below is tagged on the
strength of **my** run. Two of the earlier session's conclusions did not survive that; I say which.

**The ASan/stack-guard interaction — a test-setup fact, not a product bug.** With ASan's default
options `ziman-asan-ubsan` fails a hello-world with `stack depth exceeded: this main thread's 512 MB
stack is used up`, because ASan's fake stack puts locals in a heap frame and `ZIMAN_STACK_CHECK`'s
`&ziman_probe_ < ziman_stack_limit` compare (`src/stack.h:25`) becomes meaningless. Turning it off
produces a *second*, different report — `stack-buffer-overflow ... in gc_scan_stack_roots
src/gc_scan.c:32` — the inherent conflict between a conservative stack scanner and ASan's inter-frame
redzones, also not a defect. Both are why `ziman-asan2` exists; it is clean on collection-heavy
fixtures (`gc_iter_state_survives_collection`, `gc_collect_preserves_thread_crossed_value`,
`array_push_threads`: zero ASan reports).

**Other analysis.** GCC `-fanalyzer` over both configurations: clean, exit 0, empty output
(`analyzer.log`, `analyzer_nogc.log`). A high-warning build over the collector configuration adding
`-Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings -Wcast-qual -Wconversion
-Wsign-conversion -Wdouble-promotion -Wundef -Wredundant-decls -Wvla -Wswitch-enum -Wformat=2
-Wnull-dereference -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wjump-misses-init
-Wold-style-definition` (`warn_all.log`). **Clang could not be tried: it is not installed**
(`clang: command not found`), so §6.2's cross-compiler question stays open and I say so there rather
than omitting it.

---

## 1. Executive summary

1. **This C is safe to build on, with two exceptions that are not safe to ship.** The codebase is
   unusually disciplined for its size: ownership is stated where it matters, the two-collector
   arrangement is reasoned about in comments rather than hoped about, and ASan over all 808 fixtures
   plus the documentation examples finds **zero** memory errors.
2. **P0-1 [V]: `parse_type` (`src/parser.c:162`) recurses with no stack guard.** One
   `textDocument/didOpen` of a 70 KB document kills the language server with SIGSEGV and no
   diagnostic. A 400 KB document of the *guarded* construct survives — a clean A/B.
3. **P0-2 [V]: `add_thread` (`src/regex.c:747-776`) recurses unbounded with a VLA frame.**
   `regex.compile("()" * 6000)` then `is_match` segfaults (exit 139) on `./ziman` and `./ziman-nogc`
   at **every** stack size including the 768 MB maximum. Pure Ziman, no natives.
4. **P1 [V]: one no-op `free` costs 45× memory.** `src/iter.c:133` frees a libc `getline` buffer,
   which the collector build turns into a no-op and cannot reclaim: 1.27 GB RSS under `./ziman`
   against 28 MB under the build whose own `Makefile` comment says "nothing is ever freed".
5. **P1 [V]: `--fmt` and the LSP's formatter buffer a quadratic result in memory.** A 24 KB document
   makes the language server produce a 144 MB reply at 1.0 GB RSS; 80 KB reaches 9.7 GB.
6. **P1 [V]: a real data race in `atomic_add_double` (`src/native_metrics.c:91`)** — a plain load
   seeding an atomic CAS loop — plus a strict-aliasing violation in the same three functions.
7. **P1 [O]: allocation failure is unhandled almost everywhere** (13 NULL checks across 281
   `malloc`/`calloc` sites; 3 across 81 `realloc` sites). Defensible for a CLI, weak for the server
   runtime this also is.
8. **The `-fwrapv` lead does not hold.** `native_bits.c:96` is UB by the standard but
   `-fwrapv` is not what makes it work; GCC defines signed `<<` as a documented extension. P2.
9. Threads, the value representation, the atomic memory orders, the `GC_MALLOC_ATOMIC` choices and
   the non-NUL-terminated string discipline all held up under direct audit. I would maintain this.
10. **Verdict: yes, with P0-1 and P0-2 fixed first.** Both are small fixes.

---

## 2. Undefined and unspecified behaviour

### 2.1 The `zgc.h` macro question, answered: the dangerous shape is closed

`src/zgc.h` is force-included into every translation unit and textually redefines
`malloc`/`calloc`/`realloc`/`strdup`/`free`. The question is whether more bugs of the shape DESIGN.md
records remain — the one where `realloc(node, sizeof(MapNode) + node->n * sizeof(Slot))` expanded
into a call taking *both* the size expression and `GC_REALLOC`'s result, so unspecified
argument-evaluation order let the size be computed from `node->n` after the realloc had moved `node`.

**[V] (by reading all five macros and every `realloc` call site): the shape cannot recur, and the
reason is structural.** Every current definition expands to exactly one function call with each
argument appearing exactly once:

```c
#define realloc(p, n) GC_REALLOC((p), (n))                              /* collector build */
#define realloc(p, n) ziman_alloc_realloc(__FILE__, __LINE__, (p), (n)) /* profile build */
```

In all three configurations (collector, alloc-profile, `nogc`) `realloc(p, n)` is a single call, so
both arguments are sequenced before the call that moves the memory. The two surviving instances of
the exact textual pattern — `src/mapobj.c:353` and `:366`,
`node = realloc(node, sizeof(MapNode) + (size_t)(node->n + 1) * sizeof(Slot))` — are therefore
**safe**, and safe for a reason worth writing down next to them, because the code reads like the
recorded bug.

One thing I would still change there: both lines size a flexible-array struct with `sizeof(MapNode)`
rather than `offsetof(MapNode, slots)`. `sizeof` is never smaller, so this over-allocates by padding
at worst — but `src/arena.c` deliberately uses `offsetof` and DESIGN.md records *why*, so `mapobj.c`
contradicts the project's own stated discipline. P2, trivial.

### 2.2 `calloc` loses its overflow check [O, P2]

`#define calloc(a, b) GC_MALLOC((a) * (b))`. Real `calloc` must detect that `a * b` overflows and
return NULL; this silently truncates and hands back a short block. I audited all 19 non-constant
`calloc` sites and found none where an attacker-reachable count can push `a * b` past `SIZE_MAX` on a
64-bit build (the closest, `src/value.c:196`, would need a capacity near 2^61). A latent hazard rather
than a live bug — but it is a *removed safety property* that nothing documents, and the fix is three
lines in `zgc.h`.

### 2.3 Signed left shift in `native_bits.c:96` — and why the `-fwrapv` framing is wrong [V, P2]

`check_shift_amount` (`src/native_bits.c:15-19`) permits an amount of exactly 63, and the shift at
`:96` is on a signed `int64_t`:

```c
return value_int(args[0].as.i << args[1].as.i);
```

UBSan without `-fwrapv` reports it, from `tests/lang/bitset.zm` and from a four-line repro:

```
$ ./ziman-ubsan-nowrapv shl63.zm
src/native_bits.c:96:35: runtime error: left shift of 1 by 63 places cannot be represented in type 'long int'
-9223372036854775808
```

The earlier session concluded that `-fwrapv` in the `Makefile` is therefore load-bearing for bit 63.
**It is not, and I checked rather than reasoning about it.** `-fwrapv` covers `+ - *`; GCC documents
signed `<<` as a defined extension independently of it. A standalone test gives the identical answer
under `-O2 -fwrapv`, `-O2`, `-O3` and `-O2 -fstrict-overflow`:

```
-O2 -fwrapv            -> -9223372036854775808 -2
-O2                    -> -9223372036854775808 -2
-O3                    -> -9223372036854775808 -2
-O2 -fstrict-overflow  -> -9223372036854775808 -2
```

So the honest finding is narrower than the lead and still worth fixing: **this is UB by the letter of
C11 6.5.7p4, and the comment at `:86-92` justifies only the sign extension of the *right* shift**, so
a reader has no way to know the left shift leans on a compiler extension. `bits.int_shift_left(-1, 1)`
has the same status. Fix: shift through `uint64_t` and convert back — well-defined everywhere, free.
P2, S. The `Makefile`'s own `-fwrapv` comment ("Ziman's own `int` arithmetic does not rely on it — it
traps on overflow explicitly") is accurate; `native_bits` is the one place that reaches past the
standard, and it reaches past `-fwrapv` too.

### 2.4 `memcpy` with a NULL source and a zero length [V, P2]

Ten occurrences at `src/interpreter.c:2274` and two at `:1253`, across 12 fixtures, under UBSan on
**both** the plain and the collector builds, so not a `-fwrapv` artifact
(`sweep_ubsan_nowrapv.log`, `sweep_gc_ubsan.log`). The earlier session never diagnosed these. They
are the benign case, and I confirmed it rather than assuming — a four-line repro:

```ziman
fn main() {
    val cap = 7
    val f = fn() -> int { return cap }
    print(f())
}
```

```
src/interpreter.c:1253:21: runtime error: null pointer passed as argument 2, which is declared to never be null
7
```

A closure that captures and takes **no** arguments: `memcpy(all + clo->ncaptured, args, (size_t)argc * sizeof(Value))`
with `args == NULL` and `argc == 0`. C11 7.24.1p2 requires valid pointers even when `n` is zero, so it
is UB by the letter and harmless on every real implementation. Fix: `if (argc > 0)` at both sites.
P2, S — worth doing so the UBSan sweep is silent and the next real report is not buried under twelve
of these.

### 2.5 Type punning and strict aliasing: clean except in one module

`src/value.h:69-97`'s union is used as a discriminated union (write one member, read the same one),
not for punning. The float work is correct: `value_hash` (`src/interpreter.c:632`) canonicalises
`-0.0` and NaN and then uses `memcpy(&h, &f, sizeof(h))`; `value.c` has 23 `memcpy`s doing the same.
`src/value.c:231-232` writes `int64_t`/`double` through casts from `unsigned char *` into
`GC_MALLOC_ATOMIC` memory, which is legal — allocated memory has no declared type, the store sets the
effective type, and alignment holds because the base is `malloc`-aligned and packed slices are
whole-element offsets.

**The exception is `src/native_metrics.c`** [V, by reading]. Lines 96, 103 and 107 access a `double`
object through an `unsigned long long` lvalue:

```c
} while (!__atomic_compare_exchange_n((unsigned long long *)p, &seen, next, 0, ...));
__atomic_store_n((unsigned long long *)p, bits, __ATOMIC_RELAXED);
unsigned long long bits = __atomic_load_n((const unsigned long long *)p, __ATOMIC_RELAXED);
```

That is a 6.5p7 violation, and the build does not pass `-fno-strict-aliasing`. The surrounding
`memcpy` conversions are careful; these three casts throw that care away. Clean fix: declare the field
`uint64_t` (or a union of the two) and convert at the edges with `memcpy`, as the rest of the file
already does. Note for the next reviewer: the standing hint about a `__atomic` compare-exchange on a
double's bits in `native_http.c` is **stale** — there is no such code there today; `native_metrics.c`
is the only place.

### 2.6 One VLA, and it is the P0

`-Wvla` fires exactly once in the whole tree: `src/regex.c:758`, `int tmp[2 * (vm->ncap + 1)]`. That
single warning is the mechanism of P0-2 (§4.2). It is the best argument in this report for adding
`-Wvla` to the `Makefile`.

### 2.7 A sampler index that can go negative [V, P2]

`src/profile.c:105`:

```c
int idx = __atomic_fetch_add(&g_sample_count, 1, __ATOMIC_RELAXED);
if (idx >= SAMPLE_CAPACITY) { ...; return; }
g_samples[idx] = name;
```

`g_sample_count` is an `int` that is never reset and keeps counting past `SAMPLE_CAPACITY`
(`1 << 19`). `__atomic_fetch_add` on a signed type is defined to wrap, so after `INT_MAX` increments
`idx` becomes negative, the bound check passes, and `g_samples[negative]` is an out-of-bounds write.
At the profiler's tick rate that needs roughly 248 days of continuous `--profile` runtime, so this is
a correctness note rather than an exploitable bug — but the fix is `if (idx < 0 || idx >= SAMPLE_CAPACITY)`.

---

## 3. Memory

### 3.1 The two-collector lifetime divergence, and a case where it costs 45× [V, P1]

Under the collector build `free` is `((void)(p))`; under `nogc` it is real. **Three pointers in the
tree come from libc allocators `zgc.h` does not intercept and are released with that no-op `free`:**

| site | allocator | consequence under `./ziman` |
| --- | --- | --- |
| `src/iter.c:133` | `getline` (`:111`) | the whole line buffer leaks, per file iterated |
| `src/lsp.c:801` | `strndup` (`:799`) | leaks per symbol, per workspace/symbol request |
| `src/lsp.c:576`, `:621` | `strndup` via `dirname_copy` (`:488`) | leaks per analysis |

Boehm does not manage libc `malloc` memory, so it will never reclaim these either. The `iter.c` one is
measurable, and the direction is the surprising part — the *collector* build uses 45× the memory of
the build documented as never freeing anything. 20,000 iterations over a file with 60 KB lines:

```
--- GC build  ---  80000   time=2.44s rss=1265148kB   (1.27 GB)
--- nogc build ---  80000   time=1.91s rss=28056kB    (28 MB)
```

1.27 GB is exactly 20,000 × 60 KB, so the arithmetic confirms the mechanism as well as the number.

The fix is already in the codebase: `src/native_http.c:15-27` suspends the macros with
`#pragma push_macro` / `#undef` / `pop_macro` around `#include <malloc.h>`, with a comment naming the
hazard precisely. `iter.c` needs the runtime half of the same idiom — a real `free` for a buffer a
real `malloc` produced. P1, S.

This also settles the *compile-time* face of `zgc.h`'s textual `free` (a header re-declaring `free`
after `zgc.h` fails to compile; `freetest.c` demonstrates it with `malloc.h`): real, but **already
known and already solved** at the one place that needed it. The unhandled fragility is the runtime
face above.

### 3.2 `GC_MALLOC` vs `GC_MALLOC_ATOMIC`: correct, and correct by construction [V]

`zalloc_atomic(n)` is `GC_MALLOC_ATOMIC(n)` (`src/value.h:10`), so its contents are never scanned for
pointers. I checked every one of the ~20 call sites. **Nothing that holds a pointer is allocated
atomic.** The discriminating line is `src/value.c:196`:

```c
unsigned char *buf = k == PK_NONE ? calloc((size_t)capacity, esz) : zalloc_atomic(esz * (size_t)capacity);
```

`PK_NONE` means boxed `Value` elements, which hold pointers, and goes to the scanned allocator; the
packed kinds (`PK_BYTE`/`PK_BOOL`/`PK_INT`/`PK_FLOAT`) are all scalars and go to the atomic one. The
right split, expressed in one line rather than in a convention someone has to remember.

### 3.3 The unscanned handle tables: safe today, with one load-bearing assumption worth stating [O]

`src/handles.c:20` allocates the live-handle table with `zalloc_atomic` — "never scanned by the
collector" — deliberately. So **a pointer in a handle table is not a GC root**. What keeps a handle's
object alive is the conservative scan finding the handle *integer* in scanned memory: the Ziman-visible
value is a `struct Socket { handle: int }` / `struct Iter<T> { handle: int }` whose field lives in a
boxed `StructObj`, which is scanned, and the integer is a valid interior pointer under
`ALL_INTERIOR_POINTERS`. Two observations:

1. **The second line of defence is a finalizer, and it is not universal.** `native_net.c:117`,
   `native_sqlite.c:125`/`:445` and `prelude.c:1330` register one, so an unreachable object is removed
   from its table before its memory is reused and `handle_get` (`native_net.c:123`) then returns NULL
   for a clean "invalid or closed handle" error. `src/iter.c:41` registers one **conditionally**:
   `if (!it->no_finalizer)`. For a `no_finalizer` iterator a collected `IterObj` leaves a dangling
   entry in `g_iters`, which `handleset_has` would validate for whatever later lands at that address.
2. **A packed `[int]` array is `zalloc_atomic` (`src/value.c:279`) and therefore not scanned.** The
   rooting argument fails the moment a handle integer's only home is a packed int array rather than a
   boxed `Value`. Whether Ziman code can get there today depends on field visibility and I found no
   route — so this is `[O]`, not `[V]`. But it is the sentence I would want next to `handles.c:20`,
   because the property is "handles live in boxed values", not "handles live in the handle table".

### 3.4 A signal-handler use-after-free window in the frame stack [V by reading, not triggered, P2]

`ziman_push_frame` (`src/error.c:203`) is deliberately one unordered store-and-increment, with a
convincing comment explaining that safety comes from `frames_grow` zeroing unused slots rather than
from ordering, and that a signal fence there cost 9.5% on call-heavy code. I agree with the reasoning.
**The invariant does not cover the grow itself:**

```c
static void frames_grow(int capacity) {
#ifdef ZIMAN_GC
    ZimanFrame *fresh = GC_MALLOC_UNCOLLECTABLE(...);
    if (t_frames != NULL) { memcpy(fresh, t_frames, ...); GC_FREE(t_frames); }   /* line 188 */
    memset(fresh + t_frame_count, 0, ...);
#else
    ZimanFrame *fresh = realloc(t_frames, ...);                                   /* frees the old block */
    memset(fresh + t_frame_count, 0, ...);
#endif
    t_frames = fresh;                                                             /* line 198 */
```

Between the free (line 188, or `realloc`'s internal free) and `t_frames = fresh` (line 198),
`t_frames` points at released memory. `ziman_frame_top_name` — which `on_sigprof` calls, and whose own
comment says it is safe to call from a signal handler — reads `t_frames[n - 1].name` from that pointer.
The window is one `memset` long and only exists under `--profile`, so I did **not** trigger it and am
not claiming I did. Fix: assign `t_frames = fresh` before freeing the old block (and in the `nogc`
branch use the explicit allocate-copy-free order the GC branch already has, not `realloc`). P2, S.

### 3.5 What held up

- `nogc` fixture suite run 8 times: **884 passed, 0 failed every time** (`nogc_repeat.log`).
  Intermittent failure is this bug class's signature and there is none.
- C unit tests run 15 times: no failures (`unit_repeat.log`). I additionally ran each GC-specific
  target three times myself (`test-gc-object`/`copy`/`lookup`/`scan`/`heap`/`mark`/`env-roots`/
  `interp-roots`, `test-handles`, `test-arena`) — 30 runs, all green.
- **ASan over every fixture plus the documentation examples: zero memory errors.** `sweep_asan.log`
  records 12 entries and every one is the §2.4 UBSan NULL-`memcpy`. For a 34 KLOC interpreter with a
  conservative collector and manual memory, that is a genuinely good result.
- `GC_MALLOC_UNCOLLECTABLE` for the `__thread` pools (`error.c:184`, `interpreter.c:197`/`266`/`324`)
  is right, and the comments say why: Boehm does not reliably scan thread-local storage.

---

## 4. Robustness at the edges

### 4.1 P0-1: `parse_type` has no stack guard, and the language server pays for it [V]

`grep -n 'ZIMAN_STACK_CHECK' src/*.c` gives 13 sites. Two are in `parser.c` — `parse_unary:870` and
`parse_block:1128`. **`parse_type` (`src/parser.c:162`, recursing at `:171` and throughout) has none.** The contrast at a small stack is
clean, and I ran both:

```
$ ./ziman --stack-mb=8 --check deep_type.zm      # val x: [[[ ... int ]]] = []  (200,000 deep)
exit=139                                          # SIGSEGV, no message

$ ./ziman --stack-mb=8 --check deep_expr.zm      # val y = ((( ... 1 ... )))   (200,000 deep)
exit=1  ziman: runtime error at 0:0: stack depth exceeded: this main thread's 8 MB stack is used
        up (deep or endless recursion?); raise it with --stack-mb=N / ZIMAN_STACK_MB, ...
```

The earlier session framed this as configuration-dependent, because `src/stack.c:171`'s `compute()`
sizes the main stack from available memory (`MemAvailable/8/4`, clamped to 8–512 MB). True as far as it
goes — on this 93 GB machine the main stack is the 512 MB clamp — but it understates the finding twice.

**First, it is reachable at any stack size; it just needs a bigger file.** Measured cost is about
168 bytes of stack per nesting level:

| `--stack-mb` | segfaults at nesting depth | source size |
| --- | --- | --- |
| 8 | 50,000 | 100 KB |
| 32 | 200,000 | 400 KB |
| 64 | 400,000 | 800 KB |
| 512 (this machine's default) | 3,200,000 | **6.4 MB** |

```
$ ./ziman --check dtbig.zm      # 3,200,000 nested '[', 6,400,029 bytes, default stack
Segmentation fault (core dumped)   exit=139
```

**Second, and this is why it is a P0: the language server never gets the big stack at all.**
`src/main.c:400` runs the interpreter through `ziman_run_on_big_stack`, but `src/main.c:386` is
`return lsp_main();` — on the process's own main stack, i.e. `ulimit -s` (8 MB here and 8 MB almost
everywhere), so `--stack-mb` cannot help the LSP even in principle. Driven over the real protocol
(`initialize`, `initialized`, one `textDocument/didOpen`, `shutdown`, `exit`):

```
TYPE nesting 20000 (file 40024B)                 rc=0
TYPE nesting 35000 (file 70024B)                 rc=-11      <-- SIGSEGV
TYPE nesting 50000 (file 100024B)                rc=-11      <-- SIGSEGV
EXPR nesting 50000 (control, guarded)            rc=0
EXPR nesting 200000 (control, guarded)           rc=0
```

A **70 KB document** kills the language server on open, with no diagnostic, taking the editor
session's language support with it. A **400 KB document** of the construct that *is* guarded is fine.
That A/B is the whole finding: the guard works, it is simply absent from one function.

The fuzzer found this independently (`fuzz/crash2/crash_check_5603acefad3e.zm`, 100,000 nested `[`);
at the default 512 MB stack it now exits 1 cleanly for me, at `--stack-mb=8` it is exit 139 — which
also explains how a fuzz run under memory pressure recorded it as a crash while a re-run on an idle
machine does not. **The crash threshold is not deterministic: it moves with free memory at process
start.** Worth a sentence in the docs independently of the fix.

**Fix:** `ZIMAN_STACK_CHECK();` as the first line of `parse_type`. One line; `src/parser.c` already
includes the header. P0, S.

### 4.2 P0-2: `add_thread` recurses unbounded with a VLA frame, and allocates quadratically [V]

`src/regex.c:747-776`. Reachable from pure Ziman, no natives involved:

```ziman
use regex
fn main() {
    val r = regex.compile("()" * 6000)          // written out literally in the repro
    if is_ok(r) { print(regex.is_match(unwrap(r), "zz")) } else { print("rejected") }
}
```

```
n=4000 ziman      -> exit=0 out=true
n=4000 ziman-nogc -> exit=0 out=true
n=6000 ziman      -> exit=139
n=6000 ziman-nogc -> exit=139
```

**Raising the stack does not help** — I checked all the way to the documented maximum:

```
--stack-mb=8   -> exit=139
--stack-mb=256 -> exit=139
--stack-mb=768 -> exit=139      (MAX_STACK_MB, src/stack.c:167)
```

Under `ziman-asan2` the n=9000 case gives the trace the earlier session reported, and I reproduced
it:

```
==2568134==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7bb640baca58
WRITE of size 72008 at 0x7bb640baca58 thread T1
    #0 memcpy
    #1 0x558bc0 in add_thread src/regex.c:759
    #2 0x558dae in add_thread src/regex.c:761
    ... hundreds of frames ...
0x7bb640baca58 is located 0 bytes after 1296360024-byte region [0x7bb5f375e800,0x7bb640baca58)
allocated by thread T1 here:
    #1 0x55b1f7 in regex_search src/regex.c:789
```

**The lead called this a heap-buffer-overflow; the allocation context shows it is two distinct bugs,
and "heap-buffer-overflow" is ASan mislabelling the first.** The failing `sp` is `0x7bb640bab000`,
*inside* the reported region: the thread stack (created at `src/stack.c:216`) has grown down past its
own bottom into the adjacent mapping, which happens to be ASan's large-object region. So:

**(a) Unbounded recursion with a frame proportional to the input.** `add_thread`'s `I_SAVE` case
declares `int tmp[2 * (vm->ncap + 1)]` — the tree's only VLA and `-Wvla`'s only hit. `ncap` is
`2 * (n_groups + 1)`, so at 6,000 groups each frame carries 96 KB, and the `mark[]` dedup bounds depth
by `n_inst` (~12,000) rather than by anything constant. 12,000 × 96 KB ≈ 1.1 GB of stack needed
against a 768 MB ceiling — exactly why no `--stack-mb` value works. There is no `ZIMAN_STACK_CHECK`
anywhere in `regex.c`.

**(b) A quadratic, unchecked allocation.** `regex.c:789`, twice (once per `ThreadList`):

```c
lists[i].capbuf = malloc((size_t)re->n_inst * (size_t)vm.ncap * sizeof(int));
```

`n_inst × ncap × 4` is quadratic in the group count: the 1,296,360,024-byte region ASan names, 2.6 GB
for the pair, from an 18 KB source file. Neither it nor the four other allocations in the same function
(`:784`, `:788`, `:793`, `:794`) is NULL-checked, and `vm.mark` is dereferenced immediately at `:748`.

**The regex compiler's existing depth limit does not cover this**, which is the interesting part. A
deeply *nested* pattern is rejected cleanly ("pattern is nested too deeply at position 201"), but
`"()" * n` is **wide, not nested**, so it walks around the limit into a matcher with no limit of its own.

**Fix — my answer to "capacity check, depth bound, or both" is both, plus a size cap.** (i)
`ZIMAN_STACK_CHECK()` at the top of `add_thread`, already the project's idiom. (ii) Better, convert
the `I_JMP`/`I_SPLIT`/`I_SAVE` recursion to an explicit worklist — a textbook Thompson-VM
transformation that removes the VLA with it. (iii) Reject a compiled program whose `n_inst × ncap`
exceeds a documented ceiling, where the nesting limit already lives, so the quadratic allocation is
bounded rather than merely NULL-checked. P0, M.

### 4.3 Allocation failure is unhandled almost everywhere [O, P1]

Counted over `src/*.c`:

- **281** `malloc`/`calloc` call sites; **13** have a NULL check within two lines.
- **81** `realloc` call sites; **3** have one.

Is that defensible? **Partly.** For a CLI compiler, dying on OOM is reasonable and arguing otherwise
would be style advice. Two things make it P1 anyway:

1. **This binary is also a long-running server and a long-running language server.** `src/lsp.c:187`
   is the worst instance: `sb_grow` does `b->s = realloc(b->s, b->cap)` with no check, so on failure
   the old pointer is lost *and* the following `memcpy(b->s + b->n, ...)` dereferences NULL. For a
   process the user keeps open all day, "segfault with no message" is materially worse than for a
   one-shot compile.
2. **The `realloc` pattern leaks before it crashes.** `p = realloc(p, n)` on failure discards the only
   reference to the old block — a real leak under `nogc`, hidden by the crash that follows.

Rather than 362 NULL checks: one `ziman_xalloc`/`ziman_xrealloc` pair that reports "out of memory"
through `ziman_error` and exits, used everywhere, so the failure mode is *stated* and the server path
at least gets a message. P1, M.

### 4.4 The fixed-capacity tables: documented, and the behaviour matches the document

I checked each named limit against what happens at it.

- **`MAX_ERRORS 25`** (`error.c:13`) — at the cap `ziman_error` stops collecting (`:449`), prints
  "too many errors (N); stopping" and exits. Matches.
- **`MAX_RECOVERY_DEPTH 512`** (`error.c:14`) — a deeper push increments the depth without storing
  (`:97`); `jump_to_recovery` clamps (`:108`) so an error at depth 600 unwinds to the deepest real
  point, which is exactly what the comment at `:105-106` says. Matches.
- **`RATE_SLOTS 4096` / `RATE_KEY_MAX 96`** (`native_http.c:64-65`) — a full table replaces the oldest
  entry, argued against refusing or allowing everything in the 56-line comment above it; over-long
  keys truncate, documented at `:93-95`. Matches. One documentation defect: the struct comment says
  the key is `"bucket\0key"` while `:89` writes `"%s\n%s"` — a newline, not a NUL.
- **`GC_MAX_STRUCT_CONTENT_MARKERS 8`**, **`MAX_DEPTH 12`** (`mapobj.c:11` — 12 levels consume 60 of
  64 hash bits and deeper entries share a list), **`MAX_STACK_MB 768`** (`stack.c:167`, with the
  measurement that chose it: "768 MB works, 900 MB does not") — all behave as written.

This is the best-documented part of the codebase and I have nothing to add to it.

### 4.5 Strings that are not NUL-terminated: the discipline holds [V]

`value.h:82`'s `str` is `{ ptr, len, term }` and `value_cstr` exists because `.ptr` may not be
terminated. I audited all **42** uses of `.str.ptr` in `src/*.c` looking for one handed to a C string
API without a length. **Exactly one `.str.ptr` appears on a line with a `str*`/`printf` call —
`native_json.c:612` — and it passes the length.** Every candidate I checked individually
(`native_number.c:75`/`:118`/`:149`, `native_regex.c:75`, `native_crypto.c:190`, `native_csv.c:95`)
carries `.str.len` on the next line and uses `%.*s` where it formats. A bug class that usually yields
several hits in a codebase this size yielded none. Worth saying plainly.

### 4.6 `--fmt` is quadratic in output and unbounded in memory [V, P1]

Reproduced, with the same numbers as the lead:

| nesting | input | output | time | peak RSS |
| --- | --- | --- | --- | --- |
| 2,000 | 8 KB | 16 MB | 0.12 s | 72 MB |
| 8,000 | 32 KB | 256 MB | 1.60 s | 1.21 GB |
| 20,000 | 80 KB | — | 11.16 s | **9.68 GB** |

At 20,000 the collector itself complains:
`GC Warning: Repeated allocation of very large block (appr. size 750392 KiB)`.

**My answer to "which of the three is the honest finding": the output size is inherent, the memory is
not, and the missing depth limit is the thing to add.** The formatter indents each line by its depth,
so `4 × n²/2 × 2` characters of output is arithmetic, not a bug — 256 MB at n=8,000 is exactly that.
But 1.21 GB of RSS for 256 MB of output is ~4.7×, a string builder doubling by `realloc` while the
collector retains every superseded copy until a cycle runs. That part is fixable, and streaming to
`stdout` instead of buffering would fix it outright for the CLI.

**What makes it P1 rather than a curiosity is the LSP.** `handle_formatting` (`src/lsp.c:1403`) calls
`format_source`, keeps the whole result, `strcmp`s it against the document, then JSON-escapes it into
a second doubling builder. Driven over the real protocol:

```
N=2000 input=8014B  rc=0 reply=16024587B    peak_rss=104768kB
N=6000 input=24014B rc=0 reply=144064589B   peak_rss=1036196kB
```

A **24 KB document** produces a **144 MB** JSON reply at **1.0 GB**; at 80 KB the standalone
measurement says 9.7 GB, which OOM-kills the server. Fix: a depth limit in the formatter reporting
"nesting too deep to format" the way the parser reports nesting, plus a cap on the reply the LSP will
build. P1, S for the limit. The ~10 `hang_fmt_*.zm` inputs in `fuzz/crash2/` are all this one shape.

### 4.7 Fuzzing [V]

Round 1 (earlier session): 12,000 runs of `--check` across 4 seeds — 0 crashes, 0 hangs. Round 2
produced the `crash2/` corpus above. **Round 3, mine**: 4,200 runs across 3 seeds in `--check` and run
modes, with the structural depth/width generators and corpus-splice mutation — **0 crashes, 0 hangs,
0 new corpus entries.** The lexer and parser are in good shape on hostile input; both crashes anyone
has found are depth problems in specific recursive functions, not parsing weaknesses.

---

## 5. Threads

### 5.1 A real race in `atomic_add_double`, and why the server run missed it [V, P1]

`src/native_metrics.c:88-98`. TSan on `metrics_race.zm` (8 spawned threads, 5,000
`add`/`observe`/`set` each) reports 2 races, and I re-ran it:

```
WARNING: ThreadSanitizer: data race (pid=2592586)
  Read of size 8 at 0x659468 by thread T3:
    #0 atomic_add_double src/native_metrics.c:91
    #1 record src/native_metrics.c:221
    #2 native_add src/native_metrics.c:308
  Previous atomic write of size 8 at 0x659468 by thread T2:
    #0 atomic_add_double src/native_metrics.c:96
```

Line 91 is `memcpy(&seen, p, sizeof seen)` — a **plain, non-atomic load** seeding a CAS loop whose own
writes at `:96` are atomic. A non-atomic load concurrent with an atomic store is a data race by the C
memory model, full stop. The practical consequence is small: on x86-64 an aligned 8-byte load cannot
tear, and a stale `seen` costs one extra CAS iteration, so no increment is lost. The fix is one line —
seed with `__atomic_load_n(..., __ATOMIC_RELAXED)` — and it should be taken together with the
strict-aliasing fix from §2.5, since both live in the same three functions.

**Why the clean server run did not catch it — the part that matters for anyone reading the old log as
reassurance.** TSan on the real health-record backend under `ab` (2,000–3,000 requests at concurrency
32 and 64, plus 404s and junk headers) reported **0 warnings over 6,037 lines** (`tsan_srv.log`). A
genuine result for everything it covered — but it did not cover this code at all: line 9 of that very
log reads `server.metrics=false (application.properties:22)`. `atomic_add_double` was never called.
The stress program finds the race because it is the only thing that exercises the function. I could
not re-run the server myself with metrics on: the sample backend's `main.zm` no longer type-checks
against this HEAD (errors at `main.zm:201` and `:208`, `handle_delete` returning
`Result<unit, ApiError>` where `text<bool>` wants `Result<bool, ApiError>`), so I cite the earlier log
rather than claiming a run of my own. That cross-repo drift is worth someone's attention, though it is
outside this review's scope.

### 5.2 The memory orders: 32 relaxed, and I agree with them

I checked every `__ATOMIC_RELAXED` site (32; plus 11 acquire, 8 release, 1 seq_cst, 1 acq_rel). The
two that looked like they might be wrong are both right, and both say why:

- `interpreter.c:410`/`:416`, the lazily-cached `native_impl` pointer. Relaxed is correct because the
  table is built before any Ziman code runs, so there is one answer and it never changes; the atomic
  pair exists only to make a benign double-initialisation well-defined. The comment says so.
- `value.c:466-467`, the deadlock detector reading `g_live_threads` and `g_blocked_threads`
  separately. A torn view across two relaxed loads could in principle mis-declare a deadlock — but §8's
  re-run of the earlier review's B9 shows it firing correctly, with no fixture flakiness across
  8 × 884 runs, so I will not invent a problem here.

`native_metrics.c:75`'s acquire-load of `g_count` paired with the publish-by-count append
(`:113-115`) is the right idiom, and the comment states the invariant ("a published series' `labels`
never changes and the array never shrinks").

### 5.3 Signal handling and the rest

`on_sigprof` (`profile.c:91`) does three defensible things: a relaxed atomic load, a read of
`__thread` frame state, and a relaxed `fetch_add` claiming a slot. No allocation, no stdio, no locks.
The one hole is §3.4's `frames_grow` window. `SA_RESTART` is set with a comment explaining that a
profiler which makes `read` fail changes the program it measures — correct.

Beyond the server run and the metrics stress program, TSan over the concurrency fixtures produced
nothing. 17 files touch `pthread`; `native_http.c`'s fixed tables are guarded by one mutex with a
comment justifying the single lock by access frequency, and the `RATE_SLOTS` linear scan under that
lock is the obvious scalability cost — documented, and the right call at this stage.

---

## 6. Portability, the build, and maintainability as C

### 6.1 The warning triage

Of the ~520 extra diagnostics in `warn_all.log`, **exactly one points at a real bug**, and I had
already found it another way:

| class | count | verdict |
| --- | --- | --- |
| `-Wswitch-enum` | 414 | **noise by design.** The codebase uses `default:` deliberately; enumerating every `TypeKind` at 414 sites would be worse code. Do not add the flag. |
| `-Wdiscarded-qualifiers` | 44 | concentrated in `interpreter.c` (13 sites) and `iter.c`. Const-correctness slippage, not bugs. P2 at most. |
| `-Wcast-qual` | 30 | 12 in `gc_mark.c`, 4 in `gc_heap.c` — a conservative collector casting away `const` is what a conservative collector does. Noise here. |
| `-Wsign-conversion` | 10 | noise. |
| `-Wredundant-decls` | 9 | noise. |
| `-Wjump-misses-init` | 9 | `native_sqlite.c` only. I checked the `done:` targets: they unlock and return, and never touch the skipped `rc`/`cells`. **Not a bug**; worth hoisting the declarations for readability. |
| `-Wlogical-op` | 3 | `native_net.c:106`/`:182`/`:556`, all `EAGAIN \|\| EWOULDBLOCK`, which are equal on Linux. **Correct portability idiom.** Do not "fix". |
| `-Wshadow` | 2 | `parser.c:608`/`:654`, an inner `TypeList type_args` shadowing an outer one inside `parse_primary`. Each is used in its own block, so not a bug — but in a function this long it is one edit away from being one. |
| `-Wvla` | **1** | `regex.c:758`. **The P0-2 mechanism.** |
| `-Wformat-nonliteral` | 1 | `main.c:324`, `fprintf(stderr, msg, arg)` in Boehm's `GC_warn_proc` callback. Correct per libgc's contract (the message is a format taking one `GC_uintptr_t`), but unverifiable by the compiler and a `%s` in a future libgc message would be a wild read. Add a comment. |

**My recommendation for the `Makefile`: add `-Wvla` and `-Wshadow`, and nothing else.** Both are
cheap (1 and 2 hits), both would have caught something, and neither needs the codebase restructured.
`-Wconversion` and `-Wswitch-enum` would drown the signal.

### 6.2 Portability

The project is Linux-only and says so, and the glibc/POSIX dependencies are concentrated and guarded
where it matters: `mallopt` behind `__GLIBC__` with the `push_macro` dance in `native_http.c`; `/proc`
and cgroup v1/v2 reads in `stack.c:144-157` with a 4 GB fallback for "unreadable (not Linux)";
`mmap`/`munmap` in `arena.c` with the recorded reason (the arena must be memory `zgc.h` cannot
intercept). `getline`, `strndup` and `pthread_attr_setstacksize` are POSIX, not glibc-only. "Would not
compile elsewhere" is the dominant category; I found no "would compile and be wrong".

**I could not try clang**: it is not installed. That is a real gap in this review — a second front end
is the cheapest way to find remaining UB in a codebase this size, and the thing most likely to surface
§2.5's strict-aliasing issue. Someone should run it.

### 6.3 The Makefile

`-include src/zgc.h` is the single most consequential line in the build and the one a new maintainer
will be most confused by; it is documented at the top of the `Makefile` and again in `zgc.h`, which is
the right amount. `-MMD -MP` dependency tracking is correct. `$(wildcard src/*.c)` is fine for a tree
with no conditional compilation units, and it is what makes "`arena.c` is compiled with `zgc.h`
force-included whether it likes it or not" true — which DESIGN.md calls out.
`-std=c11 -O2 -fwrapv -Wall -Wextra -Wpedantic -g` is a sensible baseline.

**Do the gates gate?** Yes, and more than most: `make check` runs both builds' fixture suites, the unit
tests, `--fmt --check` over the library and examples, and five documentation guards including two
`git diff --exit-code` checks on generated docs. The `Makefile` comment explains that
`lib-manual-check` once sat broken on main because nothing ran it — lesson learned and encoded. What
is **not** gated: no sanitizer target (the reason this review built five binaries out of tree) and no
fuzz target. `make asan`/`ubsan`/`tsan` would be an afternoon and would have caught §2.4 and §5.1
without a reviewer.

### 6.4 Maintainability

**What is good.** Module boundaries are real: `value.c`/`value_copy.c`/`mapobj.c` own the value
representation, the `gc_*.c` family is separable, `handles.c` is 98 lines and does one thing.
Ownership conventions are stated where they matter (`value.h:202` "Takes ownership of a NUL-terminated
heap buffer"; `:209-217` on views and `value_cstr`). The comments are this codebase's best feature, and
good in the way hardest to fake: they record *measurements* and *rejected alternatives* rather than
restating the code — `error.c:205-218` on why there is no barrier in `ziman_push_frame` (with the 9.5%
number and its CI), `stack.c:165-167` on why `MAX_STACK_MB` is 768, `native_metrics.c:82-87` on why the
CAS loop replaced a mutex (3.1M/s on one thread against 2.1M on eight). There are dozens of these, and
they are what stops the next maintainer re-litigating a settled decision.

**Where commenting substitutes for simpler code.** `native_http.c:58-62` spends five lines justifying
the rate limiter's eviction policy, correctly — but the surrounding function is a linear scan of 4,096
slots under a global mutex, and a hash index would need no justification. `regex.c:757-763` would need
no comment if `add_thread` used a worklist.

**What I would refuse to maintain.** Nothing — which is unusual for 34 KLOC of hand-written C. The
closest is `src/lsp.c`'s string builder (`:183-215`): four functions, no NULL checks, on every response
path of a process the user keeps open all day, where `sb_grow` will segfault rather than report. That
one file's allocation discipline is the gap between "disciplined" and "disciplined everywhere". The
largest functions (`eval_expr`, `exec_stmt`, `parse_primary` at 390-690 and shadowing a local inside
itself, `check_stmt`) are where the remaining maintenance risk sits; I would not rewrite them on this
evidence, I would put `-Wshadow` in the build so the next shadow is caught.

---

## 7. Prioritised recommendations

| # | item | rationale | effort | smallest reproduction |
| --- | --- | --- | --- | --- |
| **P0-1** | `ZIMAN_STACK_CHECK()` at the top of `parse_type` (`src/parser.c:162`) | One `didOpen` of a 70 KB document SIGSEGVs the language server, which never gets the big stack (`main.c:386` vs `:400`). The guard demonstrably works 400 KB deep for expressions. | **S** (one line) | `ziman --stack-mb=8 --check` on `val x: [[[...int]]] = []` 200,000 deep → exit 139 |
| **P0-2** | Bound `add_thread` (`src/regex.c:747`): stack check now, worklist properly, and a documented cap on `n_inst × ncap` | Pure-Ziman SIGSEGV at **every** stack size up to `MAX_STACK_MB`. The 96 KB VLA frame and the 1.3 GB quadratic `malloc` at `:789` are two separate halves of it. The existing nesting limit does not cover a wide pattern. | **M** | `regex.compile("()"*6000)` then `regex.is_match` → exit 139 on `./ziman` and `./ziman-nogc` |
| **P1-1** | Give `src/iter.c:133` a real `free` (the `push_macro`/`#undef` idiom from `native_http.c:15-27`); same for `lsp.c:801`, `:576`, `:621` | A libc `getline` buffer released through a no-op `free`, which Boehm cannot reclaim: **1.27 GB vs 28 MB** between the two builds, in the direction nobody expects. | **S** | `leak.zm`: 20,000 `fs.lines` iterations over a 60 KB-line file |
| **P1-2** | A depth/size limit in the formatter, streaming output in `--fmt`, and a reply cap in `handle_formatting` (`lsp.c:1403`) | A 24 KB document makes the language server build a 144 MB reply at 1.0 GB RSS; 80 KB reaches 9.7 GB and the collector starts warning. The quadratic *output* is inherent; the buffering is not. | **S** for the limit, **M** for streaming | `lspfmt.py 6000`, or `ziman --fmt` on 8,000 nested bare blocks |
| **P1-3** | `__atomic_load_n` for the CAS seed at `native_metrics.c:91`, and store the bits as `uint64_t` instead of casting `double *` at `:96`/`:103`/`:107` | A genuine data race (TSan, 2 reports) plus a strict-aliasing violation in the same three functions, in a build with no `-fno-strict-aliasing`. One fix closes both. | **S** | `metrics_race.zm` under `./ziman-tsan` |
| **P1-4** | One `ziman_xalloc`/`ziman_xrealloc` pair, used everywhere | 13 NULL checks across 281 `malloc`/`calloc` sites and 3 across 81 `realloc` sites. `lsp.c:187` loses the old pointer *and* then dereferences NULL, in the process a user keeps open all day. | **M** | read `src/lsp.c:183-196` |
| **P2-1** | Shift through `uint64_t` in `native_bits.c:96`, and say so in the comment | UB by C11 6.5.7p4, works only by GCC's documented extension — **not** because of `-fwrapv`, which I verified does not affect it. The comment at `:86-92` covers only the right shift. | **S** | `bits.int_shift_left(1, 63)` under `./ziman-ubsan-nowrapv` |
| **P2-2** | `if (argc > 0)` around the second `memcpy` at `interpreter.c:1253` and `:2274` | 12 UBSan reports across 12 fixtures on both builds. Benign (NULL + length 0), but they bury the next real report. | **S** | a zero-argument lambda that captures |
| **P2-3** | Assign `t_frames = fresh` before freeing the old block in `frames_grow` (`error.c:183-199`) | A use-after-free window against `on_sigprof`, in the one function whose comment asserts signal safety. Not triggered; the window is one `memset` long and needs `--profile`. | **S** | reading only |
| **P2-4** | Add `-Wvla` and `-Wshadow` to `PLAIN_CFLAGS`; add `make asan`/`ubsan`/`tsan` targets | `-Wvla` is the P0-2 mechanism and has exactly one hit. A sanitizer target would have caught P1-3 and P2-2 without a review. | **S** | `warn_all.log` |
| **P2-5** | Restore `calloc`'s overflow check in `zgc.h`; bound `profile.c:105`'s index; `offsetof` in `mapobj.c:353`/`:366`; fix the `"bucket\0key"` comment at `native_http.c:68` | Four small removals of removed safety properties and stale documentation. | **S** each | reading only |
| **P2-6** | Write down, next to `handles.c:20`, that a handle is rooted by being a boxed `Value` and **not** by the handle table — and make `iter.c:41`'s finalizer unconditional | The table is `zalloc_atomic` on purpose; a packed `[int]` array is too (`value.c:279`), so the rooting argument has an edge. `no_finalizer` iterators have no second line of defence. | **S** | reading only (`[O]`, not demonstrated) |

---

## 8. What the earlier reviews got right and wrong

Read after forming the view above, then re-checked by re-running their reproductions rather than
trusting the text.

**Right, and now genuinely fixed — I verified each.**

- **`build_trace` is quadratic and unbounded** (language review §4.4, `error.c:289-330`): they
  measured 32.5 s / 1.26 GB / 97,390 stderr lines; `tasks.md` A11 reports finding it *worse* (402 s,
  1.97 GB, 198,578 lines). Re-measured here with `fn deep(n) { return deep(n+1) }`: **2.31 s, 835 MB,
  44 stderr lines.** The right fix too — amortised doubling plus `TRACE_HEAD 32`/`TRACE_TAIL 8`,
  because an 8 MB error message is useless however fast it is built. The remaining 835 MB is the
  512 MB stack and the frame array, not the trace.
- **`GC_set_warn_proc` is never called** (same section): now installed at `main.c:339`, showing the
  first three warnings and counting the rest behind one line. I saw it work — my 20,000-deep `--fmt`
  run produced exactly one Boehm warning line instead of thousands.
- **DX review B9, "a runtime error in a spawned thread hangs its peers forever"** (reported as exit 124
  under `timeout 5`, with the previous reviewer's own process still hung 12 hours later). **No longer
  reproduces.** Their exact two-line program now diagnoses it, well inside the timeout:

  ```
  ziman: runtime error at 0:0: deadlock: every thread is blocked on a channel, so nothing can ever
  send (a spawned thread that was going to send may have ended -- any error it printed is above)
  exit=1
  ```

- **`type_holds_thread_bound`'s `depth > 12` cutoff** (P2-2): gone, replaced by a path-based visited set
  (`typecheck.c:71-94`), with a comment explaining why the task's second half — making unbounded
  recursion an error — stopped being needed once cycles terminated exactly. I agree with that.
- **Annotated lambdas not contributing to type-argument inference** (P1-2, `typecheck.c:1251`): the
  `if (e->kind == EXPR_LAMBDA) return 1;` line they cite no longer exists.

**Where I disagree, or where the claim is now overstated.**

- **The language review's summary table says S1 — "stack overflow is an uncontrolled segfault" — is
  closed, and `tasks.md` A1 says "parser, checker and deep values are guarded too." Both are wrong for
  types.** `parse_type` has no guard, and §4.1 shows a 70 KB document taking the language server down
  with SIGSEGV and no message. This is the most consequential thing the earlier reviews got wrong, and
  wrong in the direction that matters: a reader of `tasks.md` concludes the class is closed.
- **The earlier session's `-fwrapv` conclusion on `native_bits.c:96` does not hold** (§2.3). The UB is
  real; the dependency on `-fwrapv` is not, and I checked with four compilations rather than reasoning.
- **"heap-buffer-overflow in `regex.c:759`" is ASan mislabelling** (§4.2): the overflowed region is the
  `capbuf` from `:789` and the failing stack pointer is inside it — stack exhaustion walking into an
  adjacent mapping, plus a separate quadratic allocation. The distinction matters because "add a
  capacity check at `:759`" would fix neither half.
- **The clean TSan server run should not be read as covering the metrics code** (§5.1): that log says
  `server.metrics=false`.

**Methodological note, since this is the third review.** `tasks.md` is the most useful artifact in
`docs/reviews/`: it records what was deliberately *not* done and why, in enough detail that I could
check claims instead of re-deriving them, and two of its entries correct the review they answer with
their own measurements. The one thing it needs is a convention for a claim that is only *partly*
closed. A1 says the parser is guarded — and it is, in two of the three places that recurse.

---

## Appendix: every experiment

Scratch directory `/tmp/ziman_review_c`. Commands are as run; the repo is
`/home/mfg/workspace/vasna.tech/repos/ziman` (`$R` below).

### A. Baseline

| # | command | result | why it matters |
| --- | --- | --- | --- |
| A1 | `git rev-parse --short HEAD` | `88dca78` | the tree every result below belongs to |
| A2 | `make check` | `EXIT=0`, `make_check_run2.log` | every gate green before I touched anything |
| A3 | `git status --porcelain` | ` M logs/health-record.log` | pre-existing benchmark artifact, not mine |
| A4 | `for t in test-gc-* test-handles test-arena; do make -s $t x3; done` | 30 runs, all ok | intermittent failure is the GC bug signature; there is none |
| A5 | 8 × `nogc` fixture suite (inherited, `nogc_repeat.log`) | 884 passed, 0 failed, every run | same |
| A6 | 15 × C unit tests (inherited, `unit_repeat.log`) | no failures | same |
| A7 | `gcc -fanalyzer` both configs (inherited) | exit 0, empty logs | nothing to triage |
| A8 | `clang --version` | `command not found` | §6.2's gap, stated rather than omitted |

### B. Sanitizer setup

| # | command | result | why it matters |
| --- | --- | --- | --- |
| B1 | each of the 5 binaries on `hello.zm` | 4 × exit 0; `ziman-asan-ubsan` fails | the local `sanroot`/`sanlink`/`rpms` runtime works — UBSan/ASan/TSan were really exercised |
| B2 | `ASAN_OPTIONS=detect_leaks=0 ./ziman-asan-ubsan hello.zm` | `stack depth exceeded: ... 512 MB stack is used up` | ASan's fake stack defeats `stack.h:25`'s `&local < limit` probe — test setup, not a product bug |
| B3 | same with `detect_stack_use_after_return=0` | `stack-buffer-overflow in gc_scan_stack_roots src/gc_scan.c:32` | conservative stack scanning vs ASan redzones — also inherent, and why `ziman-asan2` exists |
| B4 | `ziman-asan2` on 3 collection-heavy fixtures | 0 ASan reports each | `asan2` is usable; the sweep log below is credible |
| B5 | `grep -E 'ERROR\|WARNING\|SUMMARY' sweep_asan.log` | nothing | **ASan over 808 fixtures + examples: zero memory errors** |

### C. P0-1, `parse_type`

| # | command | result |
| --- | --- | --- |
| C1 | `$R/ziman --stack-mb=8 --check deep_type.zm` (200,000 nested `[`) | **exit 139**, no output |
| C2 | `$R/ziman --stack-mb=8 --check deep_expr.zm` (200,000 nested `(`) | exit 1, `stack depth exceeded ... raise it with --stack-mb=N` |
| C3 | same type input at `--stack-mb=` 8 / 32 / 64 | segfaults at depth 50,000 / 200,000 / 400,000 → ~168 B of stack per level |
| C4 | `$R/ziman --check dtbig.zm` (3,200,000 deep, 6.4 MB file, **default** stack) | **exit 139** — reachable at any configured stack |
| C5 | `$R/ziman de2.zm` (runaway Ziman recursion) | error names "this main thread's **512 MB** stack", confirming `compute()`'s clamp on a 93 GB machine |
| C6 | `grep -n ZIMAN_STACK_CHECK src/*.c` | 13 sites; `parser.c:870`, `:1128` only; **none in `parse_type`** |
| C7 | LSP driven over the protocol, `didOpen` only | type 20,000 → rc 0; type **35,000 → rc −11**; type 50,000 → rc −11; expr 50,000 → rc 0; expr 200,000 → rc 0 |
| C8 | `grep -n 'run_on_big_stack\|lsp_main' src/main.c` | `:386 return lsp_main();` vs `:400 ziman_run_on_big_stack(run_program,...)` — the LSP never gets the big stack |
| C9 | `fuzz/crash2/crash_*.zm` at default stack / at `--stack-mb=8` | exit 1 / **exit 139** — the threshold moves with free memory at startup |

### D. P0-2, `regex.c:add_thread`

| # | command | result |
| --- | --- | --- |
| D1 | `$R/ziman r_4000.zm` and `$R/ziman-nogc r_4000.zm` | exit 0, `true` |
| D2 | `$R/ziman r_6000.zm` and `$R/ziman-nogc r_6000.zm` | **exit 139 both** |
| D3 | `$R/ziman --stack-mb={8,256,768} r_6000.zm` | exit 139 at all three, including `MAX_STACK_MB` |
| D4 | `ziman-asan2 rxn.zm` (9,000 groups) | `WRITE of size 72008 ... add_thread src/regex.c:759`, hundreds of `:761` frames |
| D5 | allocation context of D4 | `1296360024-byte region` from `regex_search src/regex.c:789`; failing `sp` **inside** that region → stack exhaustion into an adjacent mapping, not a capacity miss at `:759` |
| D6 | reading `regex.c:784-794` | five allocations, **zero** NULL checks; `vm.mark` dereferenced at `:748` |
| D7 | `grep -c ZIMAN_STACK_CHECK src/regex.c` | 0 |
| D8 | `grep -E '\-Wvla\]' warn_all.log` | one hit: `regex.c:758` — the VLA that sizes the frame |

### E. The two-collector divergence

| # | command | result |
| --- | --- | --- |
| E1 | `grep` for libc allocators `zgc.h` does not intercept | `getline` (`iter.c:111`), `strndup` (`lsp.c:488`, `:799`) |
| E2 | `leak.zm` (20,000 `fs.lines` iterations, 60 KB lines) on `./ziman` | **rss 1,265,148 kB**, 2.44 s |
| E3 | same on `./ziman-nogc` | **rss 28,056 kB**, 1.91 s — 45× less, in the "nothing is ever freed" build |
| E4 | arithmetic check | 20,000 × 60 KB ≈ 1.2 GB — matches E2, confirming `iter.c:133`'s no-op `free` as the mechanism |
| E5 | `src/native_http.c:15-27` | the `push_macro`/`#undef`/`pop_macro` idiom the fix needs, already in the tree |

### F. `--fmt` and the LSP formatter

| # | command | result |
| --- | --- | --- |
| F1 | `/usr/bin/time ./ziman --fmt f_2000.zm` | 0.12 s, 72 MB, 16,016,014 B out |
| F2 | `f_8000.zm` | 1.60 s, 1,213,988 kB, 256,064,014 B out |
| F3 | `f_20000.zm` | 11.16 s, **9,677,584 kB**, plus `GC Warning: Repeated allocation of very large block (appr. size 750392 KiB)` |
| F4 | `lspfmt.py 2000` (real LSP `formatting` request) | rc 0, 16,024,587 B reply, 104,768 kB |
| F5 | `lspfmt.py 6000` | rc 0, **144,064,589 B reply**, **1,036,196 kB** — from a 24 KB document |
| F6 | reading `lsp.c:1403-1418`, `:183-196` | result kept whole, `strcmp`'d, then JSON-escaped into a second doubling builder; `sb_grow` has no NULL check |

### G. UB

| # | command | result |
| --- | --- | --- |
| G1 | `./ziman-ubsan-nowrapv shl63.zm` | `native_bits.c:96:35: left shift of 1 by 63 places cannot be represented in type 'long int'` |
| G2 | `./ziman-gc-ubsan shl63.zm`, `./ziman shl63.zm` | no report; same answer |
| G3 | `gcc {-O2 -fwrapv, -O2, -O3, -O2 -fstrict-overflow} shl.c` | identical results in all four → **`-fwrapv` is not load-bearing here** |
| G4 | `./ziman-gc-ubsan nullmemcpy.zm` (4-line capturing zero-arg lambda) | `interpreter.c:1253:21: null pointer passed as argument 2` then `7` → the benign NULL + length-0 case, confirmed not assumed |
| G5 | `grep -c` the two sites in the sweeps | 10 × `:2274`, 2 × `:1253`, across 12 fixtures, on **both** builds |
| G6 | reading all 5 `zgc.h` macros + every `realloc` site | single call, one expansion per argument, in all three configs → the DESIGN.md bug shape cannot recur; `mapobj.c:353`/`:366` are safe |
| G7 | reading all ~20 `zalloc_atomic` sites + `value.h:10` + `value.c:196` | nothing holding a pointer is allocated atomic; `PK_NONE` → scanned `calloc`, packed kinds → atomic |
| G8 | reading all 42 `.str.ptr` uses | one on a line with a `str*` call (`native_json.c:612`), and it passes the length |

### H. Threads

| # | command | result |
| --- | --- | --- |
| H1 | `./ziman-tsan metrics_race.zm` | 2 data races: non-atomic read at `native_metrics.c:91` vs atomic write at `:96`, via `record:221`/`native_add:308` and `record:225`/`native_observe:331` |
| H2 | `grep 'server.metrics' tsan_srv.log` | `server.metrics=false (application.properties:22)` — the clean 6,037-line server run never called the racing function |
| H3 | `$R/ziman --check <backend>/src/main/ziman/main.zm` | exit 1, 2 type errors at `main.zm:201`/`:208` — I could not re-run the server myself, and say so rather than implying I did |
| H4 | `grep -ho '__ATOMIC_[A-Z_]*' src/*.c \| sort \| uniq -c` | 32 relaxed, 11 acquire, 8 release, 1 seq_cst, 1 acq_rel; each relaxed site read and judged (§5.2) |
| H5 | reading `error.c:183-199` + `profile.c:91-110` | the `frames_grow` / `on_sigprof` window (§3.4); not triggered, and labelled as such |

### I. Fuzzing (mine)

| # | command | result |
| --- | --- | --- |
| I1 | `fuzz2.py $R/ziman 1500 31 check` | 1,500 runs, 0 distinct crashes, 0 hangs |
| I2 | `fuzz2.py $R/ziman 1500 32 check` | 1,500 runs, 0, 0 |
| I3 | `fuzz2.py $R/ziman 1200 41 run` | 1,200 runs, 0, 0 |
| I4 | `ls fuzz/crash2 \| wc -l` before and after | 41 both times — no new corpus entries from 4,200 runs |

### J. Earlier reviews re-checked

| # | command | result |
| --- | --- | --- |
| J1 | `./ziman deeprec.zm` (runaway recursion) | **2.31 s, 835 MB, 44 stderr lines** vs the 402 s / 1.97 GB / 198,578 lines `tasks.md` A11 recorded — fixed |
| J2 | `grep GC_set_warn_proc src/*.c` | `main.c:339` — installed; confirmed by the single warning line in F3 |
| J3 | the DX review's B9 program, `timeout 8` | exit 1 with a proper deadlock diagnosis — **no longer hangs** |
| J4 | `grep 'EXPR_LAMBDA) return 1' src/typecheck.c` | gone (C8 closed) |
| J5 | `grep -A8 type_holds_thread_bound src/typecheck.c` | `depth > 12` replaced by a path visited set (A13 closed) |
| J6 | C1–C9 above against `tasks.md` A1's "parser, checker and deep values are guarded too" | **claim does not hold for `parse_type`** |

### K. Tree state

`git status --porcelain` on finishing: ` M logs/health-record.log` and
`?? docs/reviews/c-implementation-review-2026-10-08.md`. No commits, nothing staged, no repository
file modified. Two temporary code experiments were considered and neither was needed: everything
above was established with out-of-tree builds and inputs under `/tmp/ziman_review_c`.
