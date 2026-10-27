# Ziman — C implementation review, 2026-10-14

An independent review of the C, not of the language and not of the standard library. Judged as C:
correctness, undefined behaviour, memory and thread safety, portability, and whether a new
maintainer could work in it safely.

Every finding is tagged `[V]` (I ran it, or I am citing a line I read) or `[O]` (opinion). Where I
could not verify something I say so rather than leaving it out.

---

## 0. Starting point

**Commit:** `d4d3b3d` — "The collections module is deleted (F120 stage 3 of 3)".

**`make check`: FAILS.** `[V]` 1m54s, exit 2. Everything passes except one assertion in
`tests/run_api.sh:38`:

```
FAIL a generic signature carries its type parameter and bound: no 'fn sort<T: Ord>(items: [T]) -> [T]' in the output
```

HEAD deleted `lib/core/collections.zm` and did not update `tests/run_api.sh`, which still runs
`ziman --api collections`. The gate caught it; nobody ran the gate. Everything else was green:
1006/1006 fixtures on `ziman`, 1006/1006 on `ziman-nogc`, all 13 unit tests, the LSP suite
(47 assertions), the TLS suite, formatting and every documentation guard.

So: **the repository is committed red.** There is no CI (the Makefile says so at `Makefile:200`),
and `make check` takes under two minutes. `[O]` That is the cheapest fix in this document and the
one that would have caught several things below.

### Compilers and sanitizers used

```
cc (GCC) 15.3.1 20260722 (Red Hat 15.3.1-1)
clang version 21.0.0 (swiftlang build, /home/mfg/.local/share/swiftly/toolchains/6.4.0)
valgrind-3.27.1
```

GCC's sanitizer *runtimes* are not installed on this machine (`/usr/lib64/libasan.so.8` absent;
only the linker scripts under `/usr/lib/gcc/.../15/` are there). The Makefile anticipated exactly
this and threads `$(LDFLAGS)` through each sanitizer recipe. I satisfied it by copying the runtimes
out of a Flatpak runtime image and pointing at them:

```sh
L=/tmp/ziman_review_c/sanlib        # libasan.so.8, libtsan.so.2, libubsan.so.1, liblsan.so.0 + .so symlinks
make asan  LDFLAGS="-L$L -Wl,-rpath,$L"
make ubsan LDFLAGS="-L$L -Wl,-rpath,$L"
make tsan  LDFLAGS="-L$L -Wl,-rpath,$L"
```

**Correction to the brief I was given:** it says "there is no sanitizer target in the Makefile
today". There are five — `asan`, `ubsan`, `tsan`, `sanitizers`, `analyze` — plus `valgrind` and
`ziman-vg`, all with substantial header comments (`Makefile:430-530`). `[V]` It also says the
`__atomic` compare-exchange on a double's bits lives in `native_http.c` and `native_metrics.c`.
There is **no `__atomic` builtin anywhere in `native_http.c`** `[V]`
(`grep -n '__atomic' src/native_http.c` → nothing); the double-bits CAS exists only in
`native_metrics.c:104-125`, and it is correct. The brief is describing the tree as it was before
the 2026-10-08 review's follow-up work landed.

---

## 1. Executive summary

This is unusually careful C. The build is warning-clean, `-Wall -Wextra -Wpedantic -Wvla -Wshadow`
produce nothing; clang at a much higher level yields 6 warnings and GCC 49, almost all noise. TSan
is clean across the whole suite. 4,000 mutated `.zm` inputs produced no crash and no hang. The
ownership conventions, the two-collector arrangement and the limits are documented to a standard I
rarely see, and the project has purpose-built lint tools (`tools/check_libc_free.py`,
`tools/check_xalloc.py`) for its own specific hazards.

And a **one-line Ziman program segfaults the compiler and kills the language server**, because an
out-of-bounds write sits two lines from an early `return` nobody accounted for. Four more pure-Ziman
inputs reach a SIGSEGV or a 613-byte stack over-read; two natives hand a stack buffer to a value
constructor that keeps the pointer. None of these are exotic, and none are in the fixture suite.

**Verdict: the architecture is safe to build on; this commit is not safe to ship.** The defects are
local, the fixes are small, and the discipline to prevent recurrence already exists in the
repository — it just is not pointed at the five natives that bypass it. Fix P0-1 through P0-4, put
`make check` in front of commits, and add fixtures for the four reproductions below.

---

## 2. Undefined and unspecified behaviour

### 2.1 `poison_declaration` writes `items[-1]` — P0 `[V]`

The worst defect in the tree. `src/typecheck.c:4010-4020`:

```c
static void poison_declaration(Env *scope, const Stmt *s) {
    ...
    env_define(scope, name, type_scalar(TY_VOID), 0, value_int(0));
    scope->items[scope->count - 1].poisoned = 1;
}
```

`env_define` has one special case, at `src/env.c:34`:

```c
    if (strcmp(name, "_") == 0) return 1;   /* `_` is a true discard, not a binding */
```

It returns success without appending. So when the statement that raised the error is
`val _ = <something>`, nothing is added, and if `scope` is a fresh block scope with no bindings yet,
`scope->count` is `0` and `scope->items` is `NULL`: the write is `NULL[-1].poisoned = 1`.

```
$ printf 'fn main() { val _ = nope }\n' > p.zm
$ ./ziman --check p.zm
Segmentation fault (core dumped)      # exit 139
$ ./ziman-nogc --check p.zm
Segmentation fault (core dumped)      # exit 139
```

Both builds. And through the LSP, which is the process a user keeps open all day:

```
initialize reply: ok
after didOpen: SERVER CLOSED THE PIPE
server exit: -11
```

When `scope->count > 0` it does not crash — it silently marks **an unrelated, previously declared
binding** as poisoned (`poison2.zm` in the appendix: `keep` gets poisoned instead of the discard),
which corrupts later diagnostics rather than the process.

Fix: have `env_define` report whether it bound anything, or `if (strcmp(name, "_") == 0) return;`
at the top of `poison_declaration`. **Effort S.** The repository's own convention — "a caller that
needs to know must be told" — is already violated by `env_define`'s bare `return 1`, which claims
success for a no-op. `[O]` The `_` case deserves a distinct return value, not a lie.

### 2.2 `buf_appendf` over-reads a stack buffer by up to ~4 GB — P0 `[V]`

`src/native_metrics.c:355-374`:

```c
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (n < 0) return;
    ...
    memcpy(b->data + b->len, tmp, (size_t)n);
```

`vsnprintf` returns the length it *would* have written. `tmp` is truncated at 511 bytes; `n` is not.
The heap side is sized correctly (`need = b->len + n + 1`), so this is purely an over-read whose
product is appended to the rendered metrics text.

ASan, from a pure-Ziman trigger (a 600-byte `help` string, nothing else):

```
ERROR: AddressSanitizer: stack-buffer-overflow on address 0x7bbbdd3f76d0
READ of size 613 at 0x7bbbdd3f76d0 thread T1
    #1 0x4a3922 in buf_appendf src/native_metrics.c:371
    #2 0x4a5785 in native_render src/native_metrics.c:413
  [96, 608) 'tmp' (line 358) <== Memory access at offset 608 overflows this variable
```

Every argument is unbounded Ziman data: `m->help` is `ziman_xstrdup(value_cstr(args[2]))` at
`native_metrics.c:196` with no length check, and `s->labels` is a whole `Map<string, string>`
joined by `metrics_source.h`'s `labels_key` at `native_metrics.c:149`.

Two consequences, and the second is the one that will be noticed first:

- The over-read bytes land in the heap buffer. `native_render` finishes with `value_string`, which
  `strlen`s — and `tmp[511]` is the NUL `vsnprintf` wrote, so the disclosed bytes are *usually*
  hidden behind it. `[O]` The disclosure is therefore narrower than it looks, but it is still a read
  of memory the program does not own, and anything that consumes the buffer by length rather than
  by `strlen` would publish it.
- **The metrics output is silently truncated at the first line over 511 bytes.** Verified: with a
  600-byte help string, `metrics.render()` returns exactly 511 bytes — the `# TYPE` line and every
  series value is gone, because they were written past the embedded NUL. A monitoring endpoint that
  quietly returns a prefix is worse than one that errors.

Fix: clamp (`if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;`) or, better, measure with
`vsnprintf(NULL, 0, ...)` and format straight into the grown buffer — the pattern this codebase
already uses correctly at `error.c:411`, `error.c:543`, `lsp.c:203` and `derive.c:61`. **Effort S.**

Worth naming precisely: `make asan` exists and is green. It did not find this because **no fixture
has a metrics label or help string longer than 511 bytes.** The gap is fixture coverage, not the
sanitizer target.

### 2.3 `value_string` keeps a pointer to a caller's stack frame — P0 `[V]`

`src/value.c:43-53`:

```c
Value value_string_view(const char *ptr, size_t len, int term) {
    ...
    v.as.str.ptr = ptr;          /* no copy */
Value value_string(char *s) { return value_string_view(s, strlen(s), 1); }
```

Two natives hand it a local array:

- `src/native_net.c:882,890` — `char out[260]; ... return ok_result(str_type, value_string(out));`
- `src/native_net.c:918,920` — `char subject[512]; ... value_string(name == NULL ? "" : subject)`

Both `Value`s outlive the frame. I scanned every `value_string`/`value_string_view` call site for a
local-array argument (`scan_borrow.py`, appendix): **31 hits, 29 of which are
`value_string(ziman_xstrdup(buf))` and correct.** The convention is established and almost
universally followed; these two are the only violations. `value_byte_array` copies
(`value.c:285-289`), so the other candidates my scanner flagged are fine.

Reproduced minimally against a local `openssl s_server`:

```
./ziman:      peer=[/CN=a]      # correct by luck
./ziman-nogc: peer=[]           # wrong
./ziman-asan: peer=[/CN=a]      # correct by luck
```

Valgrind on the failing configuration names the mechanism exactly:

```
Invalid read of size 2
   at memmove (vg_replace_strmem.c:1429)
   by 0x487BB4: display_at (value.c:848)
   by 0x41768B: format_placeholder (interpreter.c:1007)
 Address 0x25762950 is on thread 2's stack
 12376 bytes below stack pointer
```

Flag bisection: `-O0` correct, `-O1` correct, `-O2` wrong, `-O2 -fno-strict-aliasing` wrong,
`-O2 -fno-inline` correct. It is not aliasing; it is inlining changing which dead frame the bytes
come from — the textbook signature of a use-after-return. The `ziman-asan` build passes only because
it is `-O1`.

The second site is the one that matters: `net.peer_certificate_subject` returns the peer's subject
DN, which is the thing an application authorizes on. Under `./ziman-nogc -O2` it reliably returns
`""`, and `tls_client_certificate` fails 3/3 runs on that build — a configuration `make check` never
tests, because `tls-if-available` runs `$(BIN)` only. Fix: `ziman_xstrdup`, twice. **Effort S.**

### 2.4 Forged integer handles are dereferenced — P0 `[V]`

Two natives take a handle as a bare `int` out of a `pub struct` field and validate it by reading
through it. `src/native_regex.c:26`:

```c
    RegexHandle *h = (RegexHandle *)(intptr_t)v.as.i;
    if (h == NULL || h->magic != REGEX_MAGIC) ziman_error(...);
```

The comment above it says the magic word makes "a forged or stale handle an error, not a crash".
Reading `h->magic` through a forged address *is* the crash:

```
$ cat t_regex.zm
use regex
fn main() {
    printf("{}\n", regex.is_match(regex.Regex { handle: 1, groups: 0, pattern: "x", names: [] }, "x"))
}
$ ./ziman t_regex.zm
Segmentation fault (core dumped)      # exit 139
```

`src/native_metrics.c:283-288` is the same shape plus an unchecked narrowing:

```c
    long long h = args[0].as.i;
    if (h < 0) return value_unit();
    int mi = (int)(h / 1024);          /* narrowed after the sign check, not before */
    if (mi >= __atomic_load_n(&g_count, __ATOMIC_ACQUIRE)) return value_unit();
    Metric *m = &g_metrics[mi];
```

Both bounds checks are upper-only, and `h / 1024` exceeds `INT_MAX` for `h >= 2^41`, giving a
negative `mi` that passes the check. Reproduced:

```
$ cat t_handle.zm
use metrics
fn main() {
    val _ = metrics.counter("c", "h")
    metrics.add_at(metrics.Series { handle: 2199023255552 }, 1.0)
}
$ ./ziman t_handle.zm
Segmentation fault (core dumped)      # exit 139
```

`bits_add_double(&s->value_bits, v)` at `native_metrics.c:292` is then an 8-byte read-modify-write
at that address. `[O]` With attacker-controlled heap content in a long-lived server this is worse
than a crash, but I did not build an exploit and will not claim one.

The contrast is instructive and already written down: `src/handles.c:19-39` explains that a handle
is validated by *membership in a pointer set*, never by dereferencing, and ends —

> "A native that accepted a handle integer directly would break this, and that is the thing to
> notice before adding one."

`native_regex.c`'s `handle_of` and `native_metrics.c`'s `record_at` are precisely that native.
`iter.c:68`, `native_net.c:284`, `native_process.c:84` and `native_sqlite.c:88` all do it correctly.
Fix: route both through `handleset_has`, and bound `h` in `long long` before narrowing.
**Effort M** (needs a `HandleSet` for the regex cache; the metrics bound is S).

### 2.5 Strict aliasing, type punning, `-fwrapv`, shifts

`[V]` All clean, and each with a comment that earns its place.

- The double-bits CAS at `native_metrics.c:104-125` punning through `memcpy`, never a `double *`
  cast — and the CAS compares the *representation*, which is the one thing a float CAS must get
  right (NaN payloads and `-0.0`). The seed load is atomic, correctly, and the comment says why.
- `native_bits.c:96` converts through `uint64_t` for the left shift, with a comment that correctly
  identifies GCC's bit-pattern extension — not `-fwrapv` — as what used to make it work.
- `-fwrapv`: I found no site that *relies* on it. The Makefile's claim at `Makefile:2-4` ("Ziman's
  own `int` arithmetic does not rely on it — it traps on overflow explicitly") holds as far as I
  checked, and the `ubsan` target deliberately drops `-fwrapv` so the integer checks have something
  to say. That is the right call.
- `value.c:179-192`'s compare-exchange claiming a slot in a shared `ArrayStore` is correct: `used`
  is monotonic and the CAS requires `expected == n`, so exactly one thread can win for a given `n`,
  and the losing thread's `memcpy` reads only slots `0..n-1`.
- Macro argument evaluation: `zgc.h`'s `calloc` is now a *function* (`ziman_gc_calloc_at`), and
  `xalloc.h`'s wrappers are functions, for exactly the reason `zgc.h:19-26` records. I looked for
  more of the "size expression read after the realloc moved the object" shape and found none.

### 2.6 `ziman_error` is not `_Noreturn`, and there is one real uninitialized read — P1 `[V]`

`src/error.h:11` declares `void ziman_error(...)`. Every path out of its body either `longjmp`s or
`exit(1)`s — I read all four (`error.c:430`, `:437`, `:447`, `:488`). It never returns. Nothing in
the tree uses `_Noreturn` at all (`grep -rn '_Noreturn\|noreturn' src/` → nothing).

That costs three things. Two are cosmetic: the compiler cannot prune dead code after it, and the
code is sprinkled with dummy initializers whose comments say why —
`native_metrics.c:168-169`: *"Initialised although every path assigns it: the `else` below ends in
ziman_error, which does not return, but is not declared that way."*

The third is a genuine uninitialized read that only clang sees:

```
src/typecheck.c:4329:13: warning: variable 'declared' is used uninitialized whenever 'if' condition is false
src/typecheck.c:4355:12: note: uninitialized use occurs here
 4355 |     return declared;
```

The `else if (has_declared && (is_option_type(...) || ... TY_RESULT))` branch calls `init_mismatch`
on failure and falls through to `return declared`, returning a `Type` struct read from uninitialized
stack. It is unreachable *only* because `init_mismatch` → `ziman_error` does not return. That is a
property the compiler cannot see and a future refactor can break silently. Fix: `_Noreturn` on
`ziman_error` and on the wrappers that always call it. **Effort S**, and it will delete code.

### 2.7 Things only wrong at `-O2`

Only §2.3. I bisected it specifically and the `-O2` dependence is a symptom, not a separate bug.

---

## 3. Memory

The two-collector arrangement is the riskiest structure here, and it is the part I came away most
confident about. `docs/MEMORY.md` is a real design record, and the hardest thing in it — "a native
handle is kept alive only because its address, in bit-pattern form, sits where Boehm's conservative
scanner looks" — is written down with the incident that taught it
(`docs/MEMORY.md:209-238`) and the one remaining edge stated explicitly: a packed `[int]` array is
`zalloc_atomic` and so roots nothing. `[O]` That paragraph is the single most valuable comment in
the repository.

`handles.c` is correct. `[V]` I checked the three things that would break it and none do: the probe
loops always terminate (`capacity_needed` guarantees `(used+1)*2 <= capacity`, so at least half the
slots are NULL), the rehash releases the shard lock around `alloc_slots` so an allocation-triggered
finalizer cannot re-enter it (`handles.c:72-84` — the only place in the tree that defends against
this), and the deliberately-unscanned slot array is documented at length with what *does* root a
handle.

`GC_MALLOC_ATOMIC` choices: `[V]` 54 sites. Everything allocated atomic holds no pointers — packed
`[byte]`/`[bool]`/`[int]`/`[float]` buffers, string bytes, the handle slot array (deliberately). I
found nothing pointer-bearing allocated as atomic. Every `GC_MALLOC_UNCOLLECTABLE`
(`gc_heap.c:54`, `error.c:207`, `interpreter.c:196/265/323`) is `ziman_xcheck`ed. Both `mmap` sites
(`arena.c:173`, `:339`) test `MAP_FAILED`. No allocation happens inside the mark phase `[V]`.

### 3.1 The mark phase overflows the C stack, inside a collection — P1 `[V]`

`gc_mark_into`/`gc_mark_enum`/`gc_mark_array` (`src/gc_mark.c:119-195`) recurse with no depth guard
and no possible one: you cannot raise a Ziman error from inside a collection. A deeply nested value
reachable when a collection fires is an unrecoverable SIGSEGV. GDB, from a Ziman program that nests
`Json.JArray` 6,000,000 deep:

```
Thread 3 received signal SIGSEGV
#0  arena_block_containing (a=0x7fffd4bcc820, addr=0x7fffbf05ca40) at src/arena.c:381
#1  gc_lookup (...) at src/gc_lookup.c:12
#2  is_gc_heap_object (...) at src/gc_mark.c:85
#3  gc_mark_enum (a=..., en=0x7fffbf05ca40) at src/gc_mark.c:150
#4  gc_mark_array (a=..., arr=0x7fffbf05cb40) at src/gc_mark.c:119
#5  gc_mark_enum (...) at src/gc_mark.c:155
   ... alternating, to the bottom of the stack
```

Honest severity: this needs roughly 5M nesting levels on the main thread's 512 MB stack
(~100 bytes of frame per level), i.e. several hundred MB of nested data. **On a spawned thread the
stack is 1–64 MB** (`stack.c:177-179`), so the threshold there is 10k–600k levels — much more
reachable from a network payload, and spawned threads are where a server does its work. I did not
build that reproduction; I am reporting the main-thread one I ran and the arithmetic for the other.
Fix: make `gc_mark_into` iterative over an explicit worklist. **Effort M.** This is the same shape,
and the same fix, as the regex `add_thread` rewrite the previous review forced (tasks.md A16).

`gc_copy.c:260` has the identical shape; I found no live caller, so it is a trap for whoever revives
it rather than a bug today. `[O]`

### 3.2 A GC-build-only self-deadlock: allocation under `c->mu`, finalizer taking `c->mu` — P1 `[V]`

`main.c:576-578` sets only `GC_set_markers_count`, `GC_set_warn_proc` and `GC_INIT()` — no
`GC_set_finalize_on_demand`, no `GC_set_finalizer_notifier`. So Boehm invokes finalizers
**implicitly from inside `GC_MALLOC`, on whichever thread happens to allocate.** That is the premise
`handles.c:78-80` defends against. Two places do not.

`src/native_sqlite.c:396-403`:

```c
static void cursor_finalize(DbCursor *cur) {
    if (cur->stmt != NULL) {
        pthread_mutex_lock(&cur->conn->mu);
        sqlite3_finalize(cur->stmt);
        pthread_mutex_unlock(&cur->conn->mu);
        cur->stmt = NULL;                      /* after the unlock -- see below */
```

reached from `cursor_finalizer` (`:407-411`, `#ifdef ZIMAN_GC`). Three natives hold that same
`c->mu` across heavy allocation: `query_raw` (`:344-375`, one `ziman_xstrdup` per cell plus
`rewrite_placeholders` and `array_obj_new`), `:426-440`, and `:473-485`. Abandon a cursor on a
connection, then run a query on that connection large enough to allocate past a collection
threshold, and the finalizer locks a non-recursive mutex this thread already holds.

I tried to reproduce this and hit §2.1 instead — my test program's `val _ = next(it)` crashed the
typechecker before the runtime ran. I then minimised that into P0-1 and did not come back to this
one, so: **structurally verified by reading, not reproduced.** The same shape exists at
`prelude.c:1970-1977`, where `guard_finalizer` calls `channel_send`, which allocates while holding
`ch->mutex` (`value.c:393-401`) and can block indefinitely on a full bounded channel, and can
`longjmp` out of Boehm's finalizer pass via `report_deadlock`.

Separately, `cur->stmt = NULL` at `native_sqlite.c:401` is **outside** the lock, while
`cursor_next_raw` tests `cur->stmt` unlocked at `:471` and uses it under the lock at `:480`:
a window in which another thread can `sqlite3_step` an already-finalised statement.
`cursor_columns_raw` (`:455-463`) takes no lock at all. **Effort M** for the lock discipline.

### 3.3 Intermittency hunt: negative `[V]`

- `make unit-tests` × 15: identical output every run, no failures.
- `BIN=./ziman-nogc ./tests/run_tests.sh` × 3: `1006 passed, 0 failed` each time.
- `make asan` / `make ubsan`: `1005 passed, 0 failed, 1 skipped` on the fixture suite both times.

The intermittent-failure signature DESIGN.md describes did not appear. The one build-dependent
failure I found (§2.3) is deterministic, not intermittent, and lives in a suite `make check` does
not run on `nogc`.

### 3.4 Thread-root registrations that are never removed — P2 `[V]`

`main.c:217` (`run_program`) and `main.c:292` (`run_tests_program`) call
`ziman_gc_register_thread_roots()`. Neither ever calls the unregister. Both run on a thread
`ziman_run_on_big_stack` creates and **joins** (`stack.c:216`, `:220`). Registration stores the
*addresses* of that thread's TLS variables (`error.c:284-289`), and `push_other_roots`
(`gc_boehm_roots.c:52-59`) re-reads through them on every collection. After the join those
addresses belong to a thread that no longer exists. The window is live because spawned threads are
`PTHREAD_CREATE_DETACHED` (`stack.c:235`) and nothing joins them, so one can still be allocating.
`interpreter.c:574` gets this right for spawned threads; the two main entry points do not.
**Effort S.** `[O]` I did not observe a crash from this and glibc's stack cache probably keeps the
mapping alive in practice, so I rate it latent rather than live — but it is two lines.

### 3.5 `ziman_call_stack_release` frees before clearing — P2 `[V]`

`error.c:273-281` does `GC_FREE(t_frames)` and *then* `t_frames = NULL; t_frame_count = 0;`. That
is precisely the ordering bug tasks.md A23 fixed in the sibling function `frames_grow`, whose
comment now reads *"Reversing the two statements closes it."* The fix was applied to one of the two
functions that had it. Clear the count, then the pointer, then free. **Effort S.**

---

## 4. Threads

**TSan is clean.** `[V]` `make tsan LDFLAGS=...`, 9m35s: 1006 fixtures plus the 7-test TLS suite,
**0 `WARNING: ThreadSanitizer` reports**, exit 0. That is a real result and it should be in `check`'s
documentation as the evidence it is.

I audited all 62 `__atomic_*` sites. The memory orders are, with the exceptions below, right and
reasoned. `native_metrics.c`'s publication paths are correct release/acquire pairs (`:149` writes
`s->labels`, `:155` release-stores `nseries`, `:131` acquire-loads it). `native_net.c`'s `tls` flag
is a fast-path hint with the pointer itself published under `tls_mu`. Relaxed is correct where it is
used — counters, uniqueness IDs, the idempotent `NativeFn` cache at `interpreter.c:412-418`, the
deadlock heuristic at `value.c:467-468`. `error.c:252`'s `__atomic_signal_fence(__ATOMIC_SEQ_CST)`
is exactly the right tool for same-thread signal delivery.

Signal handling: `[V]` two handler bodies. `native_os.c:152-161` is fully async-signal-safe (errno
save, one relaxed atomic store to a `volatile sig_atomic_t`, errno restore). `profile.c:92-126`
makes no unsafe *call* — no `printf`, no allocation, the sample buffer is allocated before
`sigaction` — but it does read `t_frames`/`t_frame_count`, which are plain `__thread`, and writes a
plain global array. `error.c:185-203` argues its way out of that and the argument holds; §3.5 is the
one hole in it.

### 4.1 `release_handle` is not idempotent, against its own comment — P1 `[V]`

`native_net.c:313-317` asserts:

> "release_handle is idempotent by construction: it takes the descriptor with an atomic exchange
> (A24) and the SSL under `tls_mu` with a NULL check (A32), so whichever arrives second does
> nothing."

It covers the last two statements of the function. It does not cover the first two
(`native_net.c:212-219`):

```c
static void release_handle(NetHandle *h) {
    if (h->server_ctx != NULL) {
        SSL_CTX_free(h->server_ctx);
        h->server_ctx = NULL;
    }
    alpn_free(h->alpn);
    h->alpn = NULL;
```

Plain loads and stores under no lock. Two concurrent callers both see `server_ctx != NULL` and both
call `SSL_CTX_free` — **a double free of an OpenSSL object.** OpenSSL uses its own allocator, so
`zgc.h`'s no-op `free` does not absorb it in either build.

Two callers firing is the ordinary outcome of a close racing a user, not a reordering artifact:
`handle_unuse` (`:320-325`) releases when it decrements `refs` to 0 and sees `closing`;
`handle_retire` (`:328-334`) releases when it sets `closing` and sees `refs == 0`. For both to fire,
A's decrement must be visible to B before B's load and B's store visible to A before A's load —
plainly possible with no reordering at all. `server_ctx` is non-NULL only for a TLS *listener*, so
the trigger is `net.close` on a TLS listener while another thread sits in `accept` — which is the
shutdown pattern `native_net.c:151-154` documents.

The mirror case is also allowed: on x86 TSO, `handle_retire`'s release-store of `closing` followed
by its acquire-load of `refs` is the one reordering the architecture permits, so "retire sees
`refs==1` and unuse sees `closing==0`" can happen and **neither** releases — the descriptor is never
closed and the `SSL_CTX` never freed. Fix: move the `server_ctx`/`alpn` teardown inside `tls_mu`
alongside the `ssl` teardown, and make the `closing` store / `refs` load pair `SEQ_CST` (or put a
`__atomic_thread_fence(__ATOMIC_SEQ_CST)` between `:331` and `:332`). **Effort S.**
`[O]` TSan did not report it; the suite has no fixture that closes a TLS listener from one thread
while another is blocked in `accept`.

### 4.2 Smaller, real `[V]`

- `src/native_process.c:64` — `static HandleSet g_live;` is the **only** `HandleSet` in the tree
  without `= HANDLE_SET_INIT` (`iter.c:18`, `native_sqlite.c:54`/`:394`, `native_net.c:193` all have
  it). All 64 `pthread_mutex_t`s are therefore zero-filled BSS, and every `handleset_add`/`has`/
  `remove` in that file locks an uninitialised mutex. It works on glibc, where
  `PTHREAD_MUTEX_INITIALIZER` is all-zero; it is UB by POSIX and breaks on any platform whose
  initialiser has a non-zero field. **One word. Effort S.**
- `native_net.c:878` and `:906` read `h->ssl` plainly, against the field's own contract at `:173`
  ("Touched only while holding `tls_mu`, never plainly"), and then call `SSL_get0_alpn_selected` /
  `SSL_get1_peer_certificate` on an `SSL` another thread may be inside `SSL_read` on — which
  `native_net.c:146-149` itself says OpenSSL forbids. Every other access to the field obeys the rule.
- `native_metrics.c:463` — `m->nseries = 0;` is a plain store to an object read atomically at
  `:131`, `:288` and `:416`; `:466` then `pthread_mutex_destroy`es a mutex another thread may be
  blocked on. Documented as test-only (`:453`), which is why it is down here.
- `native_process.c:209-213` holds `c->mu` across a blocking `waitpid`, and `signal_child`
  (`:231-234`) needs that mutex — so `process.terminate()` on a child another thread is waiting on
  blocks until the child exits by itself. The only thing that can unblock the wait is the thing the
  wait blocks.
- `native_net.c:1153-1166` holds the global `g_idle_mu` across `SSL_shutdown` and `close` on a
  socket whose timeouts were cleared to `{0,0}` at `:796` — so a slow peer serialises every other
  HTTP client thread's pool access. Availability, not safety.

Lock order: `[V]` I traced every nesting and found no inconsistent pair. No error path returns while
holding a lock — the twelve `goto done` in `native_sqlite.c` all land on a label that unlocks first.
The only `longjmp` out of held state is `guard_finalizer`'s (§3.2).

---

## 5. Robustness at the edges

The fixed-capacity tables are in better shape than the brief implies. I went through every
`#define` that caps an array or a depth — 30 of them — and found **no off-by-one and no unchecked
index except two**, and in every other case the behaviour at the limit matches the comment beside
it. `MAX_ERRORS 25` collects 25 then flushes and exits. `RATE_SLOTS 4096` / `RATE_KEY_MAX 96`
evict oldest and truncate the key, both as documented; every access in `native_http.c` is inside
`g_rate_mu`. `MAX_DEPTH 12` in `mapobj.c` is exactly 60 of 64 hash bits, as its comment says, and
the trailing-array nodes use `offsetof(MapNode, slots) + n * sizeof(Slot)` at `:34`, `:355` and
`:368` — correct, and a fix from the previous review.

The two exceptions:

- **`gc_mark.c:22-27`** `[V]` — `gc_mark_register_struct_content_marker` writes
  `g_struct_content_markers[g_struct_content_marker_count]` with **no bound check** against
  `GC_MAX_STRUCT_CONTENT_MARKERS 8`. Five registrations today, all once-per-process, so not
  reachable — but a ninth writes a `const char *` and a function pointer past the array into
  whatever `.bss` follows, which the marker then *calls*. Its near-identical twin
  `value_register_handle_display` (`value.c:777`) does check, and the two are registered six lines
  apart in `prelude.c:2151-2159`. **Effort S.**
- **`typecheck.c:290,306,382,401`** `[V]` — `g_eq_check_stack[g_eq_check_depth++] = t;` with no
  comparison against `MAX_TYPE_CHECK_STACK 128`. Safe today only because the *other* constant
  (`MAX_TYPE_CHECK_DEPTH 64`) bounds the recursion, a coupling nothing in the code expresses. Neither
  depth counter is ever reset, so a `--lsp` process that recovers from enough stack-check longjmps
  inside `substitute_type` leaks the depth permanently. `[O]` Unlikely, but it is the only path by
  which the array can overflow, which is why the missing check is not purely cosmetic.

**`MAX_STACK_MB 768` is dead.** `[V]` `grep -rn MAX_STACK_MB src/` returns exactly one line, its own
`#define` at `stack.c:167`. The real refusal is a bare literal `768` at `stack.c:111-112`, 56 lines
*earlier*, with a third copy of the number in `refuse_size`'s message at `:102`. Changing the
constant would silently do nothing. The previous review cited it as if it were live
(`c-implementation-review-2026-10-08.md:442`). **Effort S** — use it or delete it.

### 5.1 Non-NUL-terminated strings — clean, with one hole on the other side `[V]`

The direct form of the bug is **not present.** All 47 `as.str.ptr` uses are length-counted
(`fwrite`, `memcpy`, `memcmp`, `value_str_find`, `regex_search`, `send_all`) or are literals; every
C-API path goes through `value_cstr`. `parser.c`'s `strtoll`/`strtod` on `t->start` read into a
buffer that is NUL-terminated as a whole, and the lexer accepts no digit separators, so each stops
at the token boundary. This is a well-maintained invariant.

The hole is the other half of `value_cstr`'s contract — `value.h` says plainly *"Bytes after an
embedded NUL are invisible to the C side."* `src/native_json.c:117` accepts a **raw** 0x00 inside a
JSON string, three lines above the branch at `:138` that refuses the *escaped* `\u0000`. Verified:

```
$ ./ziman t_nul.zm      # {"p":"a\0b"} fed in as [byte] via json.parse_json_bytes
ziman len=3 ends_with_b=true roundtrip="a\u0000b"
```

So Ziman sees a 3-byte string ending in `b`, and `value_cstr` — which returns `v.as.str.ptr`
directly because `term` is set (`value.c:77`) — hands `"a"` to every syscall. That is a textbook
poison-NUL split: an allow-list that accepts a path because it ends in `.json` and an `open` that
sees something else. Four other chokepoints maintain the invariant deliberately
(`prelude.c:1079`, `native_fs.c:140`, `:336`, `native_net.c:1003`); the two entry points that
bypass them are `json.parse_json_bytes` (documented as "what a body read off a socket is") and
`ndjson.records_raw`. The sinks are the obvious ones: `native_process.c:123-156` (argv, env, cwd),
`native_sqlite.c:102`, `native_fs.c`'s eighteen path arguments, `native_net.c`'s unix-socket paths
and cert file names. **One `if (c == '\0')` next to the branch already there. Effort S.**
Two consequences fall out with it: `native_sqlite.c:157`'s `sqlite3_bind_text(..., -1, ...)` stores
a truncated value, and `native_crypto.c:283`'s `OPENSSL_cleanse(password, strlen(password))` leaves
the password's tail in the heap — which is wrong on principle regardless of reachability.

### 5.2 `snprintf` and allocation failure

`[V]` §2.2 is the **only** overflow. I checked every `snprintf`/`vsnprintf` whose return value is
consumed, every `strcat`/`strcpy`/`sprintf` (there is one `sprintf`, at `parser.c:1927`, correctly
sized), and the dangerous `len += snprintf(buf + len, cap - len, ...)` idiom (one instance,
`bignum.c:268-271`, provably exact). No `strncpy` anywhere. `typecheck.c:4137`'s `strcat` into
`char list[512]` from user-written variant names is guarded with `strlen(list) + strlen(name) + 3 <
sizeof list`, where the `+3` is exactly `", "` plus the NUL.

Allocation failure: `[O]` the policy — four wrappers that report and `exit` rather than 363
half-checks — is the right call for this program, and `xalloc.h`'s header comment makes the argument
better than I would. `lsp.c`'s `sb_grow`, the previous review's worst case, now uses
`ziman_xrealloc`. The residue is **four unchecked `p = realloc(p, n)` in `regex.c`**
(`:166`, `:174`, `:491`, `:639`), each written through on the next line, which
`tools/check_xalloc.py:46` cannot see because the file carries a whole-file exemption marker. The
exemption's premise ("it checks for NULL itself") holds for the match-time block at `:1005-1023`,
not for the compile path — and `regex_compile` enforces no bound on pattern length, so `class_new`'s
one-realloc-per-class-atom growth is driven by uncapped user input. **Effort S.**

### 5.3 Recursion and fuzzing

`ZIMAN_STACK_CHECK()` (`stack.h:23-28`) works, and its coverage of the front end is good — 20+
call sites across `parser.c`, `typecheck.c`, `interpreter.c`, `loader.c`, `value.c`, `ast.c`. I
threw 14 shapes of pathological nesting at it at depths up to 300,000 `[V]`:

```
ok paren n=300000          rc=0     300k nested parens parse and run
ok paren_unclosed n=300000 rc=1     clean syntax error at col 300023
ok neg n=300000            rc=1     "stack depth exceeded: ... raise it with --stack-mb=N"
ok ternlike n=300000       rc=1     same, clean
ok str_esc n=300000        rc=0
CRASH deep_fn n=300000     TIMEOUT  (not a crash — quadratic, see §7.3)
```

Every depth failure became an ordinary, actionable Ziman runtime error. That is the guard doing
exactly its job, and it is better than most tree-walking interpreters manage.

**Fuzzing: no crashes.** `[V]` 4,000 inputs, 85% mutated from the fixture corpus (bit flips, token
splicing, 400× token repetition, 2,000-byte tails, embedded NULs and invalid UTF-8) and 15% pure
random bytes, each with a 5s timeout. Zero segfaults, zero hangs, zero exit codes outside
{0,1,2}. The lexer and parser stood up. The crashes I did find came from *semantically valid*
programs (§2.1, §2.4) — which is where fuzzing a typed language has to go next.

What has **no** guard, and where the vehicle is an ordinary `Json` value a program can nest in a
loop with no native help: `native_json.c:607` `write_json` (while `parse_value` at `:349` caps at
`MAX_DEPTH 512` — the asymmetry inside one module is clearly unintended), and `value_copy.c:147`
and `:274`, which run on every `channel.send`, every `receive` and every `spawn` argument. I
confirmed `write_json` survives 400,000 levels and produces correct output, so the practical
threshold is high on the main thread `[V]`; on a spawned thread's 1–64 MB stack it is not. The
unguardable one is §3.1. **Three `ZIMAN_STACK_CHECK()` lines close B1/B2. Effort S.**

---

## 6. Portability and the build

### 6.1 Compiles elsewhere / compiles and is wrong

`[V]` The tree builds under clang 21 with `-fsyntax-only` and a much higher warning level than the
Makefile's:

```sh
clang -std=c11 -O1 -fsyntax-only -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow \
      -Wstrict-prototypes -Wwrite-strings -Wcast-qual -Wmissing-prototypes -Wvla src/*.c
```

85 warnings, of which **83 are artifacts of `-Wwrite-strings`** (it makes string literals
`const char[]`, so every literal passed to a `char *` parameter reports
`-Wincompatible-pointer-types-discards-qualifiers`; the C standard gives literals type `char[]`, so
these are not real qualifier discards). The two real ones are `typecheck.c:4329` (§2.6) and
`main.c:415` `printed_fns` set but not used.

GCC at a comparable level (`-Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wcast-qual -Wjump-misses-init -Wduplicated-cond -Wduplicated-branches
-Wnull-dereference -Wdouble-promotion`) gives 49: 29 `-Wcast-qual` (deliberate const-stripping),
10 `-Wsign-conversion`, 9 `-Wjump-misses-init`, 1 unused. Clang with `-Wconversion
-Wsign-conversion`: **6 total.** `[O]` For 34,000 lines of hand-written C that is genuinely
exceptional, and the Makefile's recorded decision to triage the rest as noise (tasks.md F14) was
right. The build is warning-clean on a rebuild and there is no `-Werror`.

GCC's `-fanalyzer` has a `make analyze` target documented as clean on both configurations as of
2026-10-08; I did not re-run it (it takes minutes per configuration and I had already spent the
budget on three sanitizers). **Not verified by me.**

Linux-only, and honestly so: `__GLIBC__` guards, `mallopt`, `/proc`, `MAP_ANONYMOUS`, `crypt_r`,
`SOCK_CLOEXEC`. `[O]` None of that is a defect given the stated scope. The one thing in the
"compiles and is wrong" column is `native_process.c:64` (§4.2), which only works because glibc's
mutex initialiser happens to be all-zero.

### 6.2 Trigraphs silently rewrite the embedded standard library — P2 `[V]`

`PLAIN_CFLAGS` uses `-std=c11`, under which **trigraphs are active**. The corelib generator
(`Makefile:105-110`) escapes `\` and `"` and nothing else, so a `??!` in any `lib/**/*.zm` becomes a
`|` in the embedded string. End-to-end through the actual awk recipe:

```
#define DEMO_SOURCE \
    "    return \"a??!b\"\n" \
...
$ ./usedemo
    return "a|b"
```

No `??` sequence exists in `lib/` today, so this is latent. GCC and clang both emit
`-Wtrigraphs` here, so it is noisy rather than silent — but there is no `-Werror`, the generated
headers are gitignored, and a warning in a 59-file build scrolls past. One `gsub(/\?/, "\\?")` in
the awk program, or `-std=gnu11`, or `-no-trigraphs`. **Effort S.**

### 6.3 The Makefile itself

`[O]` Good, and better documented than most. Specific notes:

- `SRC := $(wildcard src/*.c)` means a stray `.c` left in `src/` joins the build silently. The
  convenience is real (a new file needs no edit); the risk is a half-finished file linking into a
  release. `[O]` I would keep the wildcard and add a `git status`-clean assertion to `check` instead.
- `$(OBJ): $(CORELIB_HEADERS)` is the right dependency and the comment explains why `-MMD -MP` alone
  is not enough on a from-clean build. `-MMD -MP` applies to the `ziman` build only; `nogc`, the
  three sanitizers and `ziman-vg` each compile all 59 sources in one invocation every time. Slow,
  but correct, which is the right trade for a non-default target.
- **The sanitizer and valgrind targets build without `$(GC_FLAGS)`** — so they are *nogc* builds.
  That is a significant and undocumented limitation: `make asan` cannot see anything about the
  default build's collector, its finalizers, or the no-op `free`. §3.2's deadlock and §4.1's double
  free are both `#ifdef ZIMAN_GC` paths that **no sanitizer target in this repository can reach.**
  `[O]` This is the biggest hole in the gates. Boehm under ASan needs care (the conservative stack
  scan trips redzones, as tasks.md A16 records at `gc_scan.c:32`), which is presumably why — but the
  Makefile should say so, and `gc_scan.c` learning about `__asan_get_current_fake_stack` would close it.
- `tools/check_libc_free.py` and `tools/check_xalloc.py` are exactly the right idea: lint the two
  invariants the compiler cannot see. §5.2's finding is that one of them has an escape hatch wide
  enough to hide four live defects.
- **The gates gate, and nothing runs them.** `make check` is red at HEAD; `tools/preflight.sh`
  exists and names the sanitizers, memcheck, the analyzer and both database suites as the things
  `check` leaves out. It is a command a person runs. `[O]` Given this review's findings, a
  pre-commit hook running `make check` would have more effect than any single code fix here.

---

## 7. Maintainability as C

### 7.1 What is good, concretely `[O]`

The module boundaries hold. `value.c`/`value_copy.c`/`mapobj.c` own the representation;
`arena.c` + `gc_*.c` own memory with a clear split (`gc_object`/`gc_lookup`/`gc_scan`/`gc_mark`/
`gc_heap`/`gc_env_roots`/`gc_interp_roots`/`gc_boehm_roots`), each small enough to unit-test and
each with its own `make test-gc-*` target that links only what it needs. That is the thing I would
most want to find in a tree this size and it is here.

The commenting is heavy and mostly earns it. The ones that genuinely help are the ones that record
a *decision and its evidence*: `handles.c:19-39` on what roots a handle (and the edge that does
not), `native_metrics.c:94-103` on why the double CAS exists at all with the throughput numbers that
forced it, `native_bits.c:95-103` on which compiler extension the left shift used to lean on,
`error.c:185-203` on the signal-safety ordering, `zgc.h:19-26` on the macro that read `node->n`
after the realloc moved `node`. A new maintainer reading those learns why the code is shaped the way
it is, which is the only kind of comment worth maintaining.

Error handling is coherent: `ziman_error` with a sink for the LSP, a recovery stack for
parse-and-keep-going, and a per-thread `ThreadBoundary` for runtime errors. The `MAX_RECOVERY_DEPTH`
behaviour at the limit is exactly what its comment says (a deeper push is ignored, the jump clamps
to the deepest recorded point, and the skipped frames' pops never run — which keeps the accounting
consistent). What leaks on an unwind is mostly nothing, because `free` is a no-op in the default
build; `[O]` that is the one place where the two-collector arrangement makes the code *simpler*
rather than riskier, and it is worth saying since everything else here is about the risk.

### 7.2 Where the comments substitute for simpler code `[O]`

Two places, and they are the same place twice. `native_net.c:305-317`'s comment asserts an
idempotency property that `native_net.c:212-219` does not have (§4.1); `native_regex.c:18-21`'s
comment asserts that a magic word turns a forged handle into an error, which §2.4 shows it does not.
In both cases a reader who trusts the comment stops looking. **A comment that states an invariant
the code does not enforce is worse than no comment**, because it transfers the reader's attention
away from the gap. The fix in both cases is to make the code match the comment, not to soften the
comment — and in both cases the correct pattern is already in the tree
(`tls_mu` for the first, `handleset_has` for the second).

### 7.3 Function size, and one measured scalability wall

`typecheck.c` is 4,600 lines and `check_expr`/`check_stmt` are long with wide `switch`es. `[O]` I
would not refactor them: the branching follows the AST shape, each arm is short, and the
`check_stmt_recovering` wrapper keeps the control flow honest. The real complaint is that
`check_initializer` (`typecheck.c:4271-4356`) is an eight-branch `if/else if` chain over
`has_declared` where a reader cannot easily tell every branch assigns `declared` — and clang's
warning (§2.6) says one does not.

`[V]` Measured: the typechecker's name lookup is **O(n²)** in the number of top-level declarations.

```
n=2000   0.01s   18 MB
n=4000   0.04s   29 MB
n=8000   0.15s   52 MB
n=16000  0.65s   97 MB
```

4× per doubling. `perf` attributes it to `typecheck` + `__strcmp_avx2` (22% of cycles) — a linear
scan with `strcmp` over the function table. At 300,000 declarations it is a 4-minute compile. `[O]`
Nobody hand-writes that file, but generated code and the LSP (which re-checks on every keystroke)
both hit it. A hash table over function names is a contained change. **Effort M, P2.**

### 7.4 What I would refuse to maintain `[O]`

Nothing structural. Two specific things I would not accept in a review:

1. **A native that takes a resource handle as a bare integer out of a `pub struct` field.** It is
   two natives today (§2.4) and the repository already wrote down why it must not be done. Every
   such native is a crash-from-pure-Ziman waiting to be written, and the language has no field-level
   visibility to prevent the literal.
2. **`free` meaning two different things in two shipped builds, with the sanitizers only ever
   seeing one of them.** The arrangement itself is defensible and well argued in DESIGN.md; the
   unacceptable part is that `make asan`/`ubsan`/`tsan` all drop `$(GC_FLAGS)`, so the
   `#ifdef ZIMAN_GC` finalizer paths — where §3.2 and §4.1 live — have no automated coverage at all.

---

## 8. Prioritised recommendations

| # | item | rationale | effort | smallest reproduction |
| --- | --- | --- | --- | --- |
| **P0-1** | `poison_declaration` (`typecheck.c:4018-4019`) must not index `items[count-1]` after `env_define` declined to bind `_`; give `env_define` a truthful return | Out-of-bounds write from a one-line program; segfaults `--check`, `--lsp` and both builds. Silently poisons an unrelated binding when `count > 0` | **S** | `printf 'fn main() { val _ = nope }\n' > p.zm && ./ziman --check p.zm` → 139 |
| **P0-2** | Clamp `n` in `buf_appendf` (`native_metrics.c:371`), or measure with `vsnprintf(NULL,0,...)` | 613-byte stack over-read with an ASan trace, from a 600-byte metrics `help` string; and it silently truncates all `/metrics` output at the first long line | **S** | `metrics.counter("reqs", <600 A's")` then `metrics.render()` → `len == 511`; ASan reports `stack-buffer-overflow` |
| **P0-3** | `ziman_xstrdup` at `native_net.c:890` and `:920` | Use-after-return. `peer_certificate_subject` returns `""` on `./ziman-nogc` (3/3 runs) — and that value is what an application authorizes on | **S** | `./ziman-nogc t.zm` → `peer=[]` vs `./ziman` → `peer=[/CN=a]`; valgrind: "12376 bytes below stack pointer" at `value.c:848` |
| **P0-4** | Validate the `regex`/`metrics` handles by `handleset_has`, not by dereferencing; bound `h` in `long long` before narrowing at `native_metrics.c:284` | Two pure-Ziman SIGSEGVs; the metrics one is an 8-byte RMW at a computed address. `handles.c:36-39` already names this as the thing not to do | **M** | `regex.is_match(regex.Regex{handle:1,groups:0,pattern:"x",names:[]}, "x")` → 139; `metrics.add_at(metrics.Series{handle:2199023255552}, 1.0)` → 139 |
| **P1-1** | Make `release_handle`'s `server_ctx`/`alpn` teardown happen under `tls_mu`; `SEQ_CST` on the `closing`/`refs` handshake | Double `SSL_CTX_free` on an OpenSSL object (own allocator — the no-op `free` does not absorb it) on the documented TLS-listener shutdown path, against a comment that claims idempotency | **S** | not reproduced — read `native_net.c:212-219` against `:320-334` |
| **P1-2** | Release `c->mu` around allocation in `native_sqlite.c:344/426/473`, or set `GC_set_finalize_on_demand`; move `cur->stmt = NULL` inside the lock | Boehm runs finalizers inside `GC_MALLOC` on the allocating thread (`main.c:576-578`); `cursor_finalize` takes the mutex those three natives hold. `handles.c:78-80` already solves this once | **M** | not reproduced (my attempt hit P0-1 first) |
| **P1-3** | Refuse a raw `0x00` in `native_json.c:117`, beside the `\u0000` branch at `:138` | Poison-NUL split into every `value_cstr` sink: execve argv, sqlite paths, 18 `fs` path arguments, cert filenames | **S** | `json.parse_json_bytes` of `{"p":"a\0b"}` → `len=3 ends_with_b=true`, while C sees `"a"` |
| **P1-4** | `= HANDLE_SET_INIT` at `native_process.c:64` | The only one of five `HandleSet`s missing it; 64 uninitialised mutexes, UB by POSIX, works only because glibc's initialiser is all-zero | **S** | `grep -n 'HandleSet g_' src/*.c` |
| **P1-5** | `_Noreturn` on `ziman_error` and its always-erroring wrappers | Removes a genuine uninitialized-struct return at `typecheck.c:4329` that only clang sees, and deletes the dummy initialisers whose comments apologise for it | **S** | `clang -std=c11 -Wall -Wextra -fsyntax-only src/typecheck.c` |
| **P1-6** | Make `gc_mark_into` iterative (`gc_mark.c:119-195`) | Unrecoverable SIGSEGV inside a collection; no guard is possible there. Threshold is ~5M levels on the main thread but 10k–600k on a spawned thread's 1–64 MB stack | **M** | GDB backtrace: alternating `gc_mark_enum`/`gc_mark_array` to the stack bottom |
| **P1-7** | `make check` in a pre-commit hook; build the sanitizer targets *with* `$(GC_FLAGS)` too | HEAD is committed red. And no sanitizer target can reach any `#ifdef ZIMAN_GC` path, which is where P1-1 and P1-2 live | **S** / **M** | `make check` → exit 2 at `tests/run_api.sh:38` |
| **P2-1** | `ZIMAN_STACK_CHECK()` in `write_json` (`native_json.c:607`) and both `value_copy.c` entry points (`:147`, `:274`) | Three stack-overflow paths on deeply nested `Json`; `parse_value` in the same module is already capped at 512 | **S** | 6M-deep `Json.JArray` nesting |
| **P2-2** | Bound-check `gc_mark.c:24`; bound the four pushes at `typecheck.c:290/306/382/401` | Two unchecked writes into fixed arrays. The `gc_mark` one writes a function pointer past the array that is then called; its guarded twin is six lines away in `prelude.c` | **S** | read `gc_mark.c:22-27` against `value.c:777` |
| **P2-3** | Check the four `p = realloc(p, n)` in `regex.c` (`:166`, `:174`, `:491`, `:639`); narrow `check_xalloc.py`'s whole-file exemption | The exact defect `xalloc.h` was written to kill, hidden by the lint's own escape hatch; pattern length is uncapped at parse time | **S** | `tools/check_xalloc.py:46` |
| **P2-4** | Clear `t_frame_count` before freeing in `ziman_call_stack_release` (`error.c:273-281`) | Literally the bug A23 fixed in `frames_grow`, in its sibling | **S** | read it against `frames_grow`'s comment |
| **P2-5** | Unregister thread roots at `main.c:217`/`:292` | Dangling TLS addresses in `g_roots` after the big-stack thread is joined, re-read on every collection while detached threads still run | **S** | `grep -n ziman_gc_.*register_thread_roots src/*.c` |
| **P2-6** | Hash the function table in `typecheck.c` | Measured O(n²); 4 min at 300k declarations, and the LSP re-checks on every keystroke | **M** | 2000/4000/8000/16000 decls → 0.01/0.04/0.15/0.65 s |
| **P2-7** | Use `MAX_STACK_MB` at `stack.c:111-112` or delete it; `gsub(/\?/, "\\?")` in the corelib generator | A dead constant the previous review cited as live, and a trigraph channel that silently rewrites the embedded stdlib | **S** | `grep -rn MAX_STACK_MB src/`; `"a??!b"` → `"a|b"` through the awk recipe |
| **P2-8** | `h->ssl` under `tls_mu` at `native_net.c:878`/`:906`; plain `m->nseries = 0` at `native_metrics.c:463`; `OPENSSL_cleanse` length at `native_crypto.c:283`; `sqlite3_bind_text` length at `native_sqlite.c:157`; `channel_new`'s `int` narrowing at `interpreter.c:1487` | A cluster of small correctness items, each one or two lines | **S** | — |

---

## 9. What earlier reviews got right and wrong

Read after forming the view above, then checked by re-running their reproductions.

**Right, and genuinely fixed — I verified each `[V]`:**

- **P0-2, the regex `add_thread` blowup.** Their reproduction (`regex.compile("()" × 6000)`) now
  answers with a bounded, actionable refusal on both builds: *"pattern needs too much memory to
  match: 1650 MB for 6000 capture groups over 12003 instructions (the limit is 64 MB; use (?:...)
  for a group whose text you do not need)"*, exit 0. The rewrite to an explicit worklist and the
  `MAX_MATCH_BYTES` budget both landed.
- **P1-3, the metrics double-CAS.** Fixed exactly as recommended: the value is stored as
  `unsigned long long value_bits` with `memcpy` punning, the CAS seed is an atomic load, and TSan is
  now clean over the whole suite. The comment at `native_metrics.c:57-61` records the C11 6.5p7
  citation.
- **P1-4, the allocation wrappers.** `src/xalloc.h` exists, `lsp.c`'s `sb_grow` uses
  `ziman_xrealloc`, and `tools/check_xalloc.py` guards it. The residue is §5.2.
- **P2-1, the `native_bits` left shift.** Goes through `uint64_t` now, with a comment that correctly
  blames GCC's extension rather than `-fwrapv`.
- **P2-3, `frames_grow`'s free-before-publish.** Fixed, with a comment explaining the ordering.
  §3.5 is the half that was missed.
- **P2-4, the warning flags and sanitizer targets.** `-Wvla -Wshadow` added; `asan`/`ubsan`/`tsan`/
  `sanitizers`/`analyze`/`valgrind` all exist, with the `$(LDFLAGS)` escape hatch that let me run
  them on a machine without the GCC runtimes.
- **P2-5, `calloc`'s overflow check and `offsetof` in `mapobj.c`.** Both restored
  (`zgc.h:33`, `mapobj.c:34/355/368`).
- **P2-6, documenting what roots a handle.** `handles.c:19-39` is now the best comment in the tree.

**Where I disagree or the record is now wrong:**

- **`tasks.md`'s framing of A20 ("allocation failure") as settled.** The policy is right and the
  wrappers are right, but the `regex.c` exemption means four unchecked reallocs survive *inside the
  file the exemption was written for*, and the lint cannot see them. The entry reads as closed; it
  is 95% closed, and the missing 5% is in the one file nobody is checking.
- **The previous review's citation of `MAX_STACK_MB` as the live ceiling**
  (`c-implementation-review-2026-10-08.md:442`, `:529`, `:717`). It is dead code (§5). The *number*
  768 is live, three times over, as a literal. A review that cites a constant should check it is
  used.
- **`tasks.md` A16's closing note, "the same test under ASan+UBSan is clean, which is the only check
  that actually validates the new bounds."** True for `regex_test.c`, and the sentence invites the
  wrong generalisation: because `make asan`/`ubsan`/`tsan` drop `$(GC_FLAGS)`, "clean under the
  sanitizers" says nothing about the default shipped build's collector or finalizer paths. That is
  not a criticism of the fix; it is a criticism of what the gates are believed to cover.
- **The brief for this review** (`docs/reviews/prompts/c-implementation-review.prompt.md`) is stale
  in two checkable ways (no sanitizer targets; `__atomic` on a double in `native_http.c`). Worth
  noting because a reviewer who trusted it would have reported a finding that does not exist and
  skipped the three sanitizer runs that produced the evidence here.

---

# Appendix — every experiment

Scratch files are under `/tmp/ziman_review_c/`. Nothing in the repository was modified
(`git status --porcelain` empty apart from this file). Sanitizer runtimes were copied from a Flatpak
runtime image into `/tmp/ziman_review_c/sanlib` with `.so` symlinks, and passed via `LDFLAGS` as the
Makefile's recipes provide for.

### A. Baseline

```
$ git rev-parse --short HEAD                                   → d4d3b3d
$ time make check                                              → exit 2, 1m54.741s
  1006 passed, 0 failed          (./ziman fixtures)
  1006 passed, 0 failed          (./ziman-nogc fixtures)
  0 failure(s)                   (LSP, 47 assertions)
  18 passed, 1 failed            (api-check)
  FAIL a generic signature carries its type parameter and bound: no 'fn sort<T: Ord>(items: [T]) -> [T]'
$ ./ziman --api collections      → "no bundled module or file named 'collections'", exit 1
$ git log --oneline -1 -- lib/core/collections.zm → d4d3b3d (HEAD deleted it; run_api.sh:38 not updated)
```
*Why it matters:* the gate works and was not run before the commit.

### B. Sanitizers

```
$ make asan  LDFLAGS="-L$L -Wl,-rpath,$L"   → 1005 passed, 0 failed, 1 skipped; TLS 6/7 (tls_alpn)
$ make ubsan LDFLAGS="-L$L -Wl,-rpath,$L"   → 1005 passed, 0 failed, 1 skipped; TLS 6/7 (tls_alpn)
$ make tsan  LDFLAGS="-L$L -Wl,-rpath,$L"   → 1006 fixtures + 7/7 TLS, 0 ThreadSanitizer warnings,
                                               exit 0, 9m35.330s
```
`tls_alpn`'s failure under asan/ubsan — `server agreed: M-pM-<` instead of `h2` — is finding §2.3
at `native_net.c:890`. *Why it matters:* TSan clean is a real negative result; the `tls_alpn`
divergence is how I found the stack-borrow bug.

### C. `value_string` use-after-return (§2.3)

```
$ ./ziman      tlsrepro/t.zm   → peer=[/CN=a]
$ ./ziman-nogc tlsrepro/t.zm   → peer=[]
$ ./ziman-asan tlsrepro/t.zm   → peer=[/CN=a]
```
Flag bisection, all nogc (`cc -std=c11 <flags> src/*.c`):
```
-O0                        → peer=[/CN=a]
-O1                        → peer=[/CN=a]
-O2 -fwrapv                → peer=[]
-O2 -fwrapv -D_GNU_SOURCE  → peer=[]
-O2 (no -fwrapv)           → peer=[]
-O2 -fwrapv -fno-strict-aliasing → peer=[]
-O2 -fwrapv -fno-inline    → peer=[/CN=a]
-O2 -fsanitize=undefined   → peer=[/CN=a]
```
Valgrind on `-O2 -g -fno-omit-frame-pointer`:
```
Invalid read of size 2 / size 1
   at memmove; by display_at (value.c:848); by format_placeholder (interpreter.c:1007)
 Address 0x25762950 is on thread 2's stack, 12376 bytes below stack pointer
```
Whole-tree scan (`/tmp/ziman_review_c/scan_borrow.py`, `scan_borrow2.py` — tracks `char X[...]`
locals by brace depth, flags value-constructing calls referencing them): 31 hits, 29 of the form
`value_string(ziman_xstrdup(buf))`; only `native_net.c:890` and `:920` pass the array directly.
`value_byte_array` copies (`value.c:285-289`), so `native_crypto.c:37/69` and
`native_number.c:48/171` are fine.
```
$ BIN=./ziman-nogc ./tests/run_tls_tests.sh   → FAIL tls_client_certificate, 3 runs out of 3
    < trusted client certificate: handshake returned Ok, server is
    > trusted client certificate: handshake returned Ok, server is /CN=a
```
*Why it matters:* a dangling stack pointer inside a `Value`, in the function that reports a peer's identity.

### D. `poison_declaration` (§2.1)

```
$ printf 'fn main() { val _ = nope }\n' > p3.zm
$ ./ziman --check p3.zm       → Segmentation fault (core dumped), exit 139
$ ./ziman-nogc --check p3.zm  → Segmentation fault (core dumped), exit 139
```
GDB (from the larger program that first hit it):
```
Thread 3 received signal SIGSEGV
0x47ce48 in poison_declaration (scope=0x7ffff5629c30, s=0x7ffff54c1f30) at src/typecheck.c:4019
4019     scope->items[scope->count - 1].poisoned = 1;
#1 check_stmt_recovering (...) at src/typecheck.c:4032
```
LSP (`/tmp/ziman_review_c/lsp_poison.py`, a minimal JSON-RPC driver):
```
initialize reply: ok
after didOpen: SERVER CLOSED THE PIPE
server exit: -11
```
`count > 0` variant (`poison2.zm`: `val keep = 1` then `val _ = nope`) → no crash, exit 1, correct
message — but the poison lands on `keep`. Mechanism: `src/env.c:34`
`if (strcmp(name, "_") == 0) return 1;`.
*Why it matters:* a one-line file kills the compiler and the editor integration.

### E. Metrics stack over-read (§2.2)

```
$ ./ziman m2.zm                 → help length = 600 / render length = 511 / tail = [AAAA…]
$ ./ziman-nogc m2.zm            → identical
$ valgrind -q ... zn-vg m2.zm   → no report (memcheck does not bound stack arrays)
$ ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 ./ziman-asan m2.zm
ERROR: AddressSanitizer: stack-buffer-overflow, READ of size 613
    #1 buf_appendf src/native_metrics.c:371
    #2 native_render src/native_metrics.c:413
  [96, 608) 'tmp' (line 358) <== Memory access at offset 608 overflows this variable
```
*Why it matters:* the only way to see this was ASan's stack redzones, and `render length = 511`
shows the second, more visible consequence (silent truncation of the whole metrics output).

### F. Forged handles (§2.4)

```
$ ./ziman t_handle.zm   # metrics.Series { handle: 2199023255552 }       → 139
$ ./ziman t_regex.zm    # regex.Regex { handle: 1, pattern: "x", ... }   → 139
```
*Why it matters:* two public struct literals, no natives written by the user, instant SIGSEGV.

### G. NUL injection (§5.1)

```
$ ./ziman t_nul.zm      # json.parse_json_bytes([123,34,112,34,58,34,97,0,98,34,125])
ziman len=3 ends_with_b=true roundtrip="a\u0000b"
```
*Why it matters:* `value_cstr` returns `.ptr` directly when `term` is set (`value.c:77`), so C sees
`"a"` while Ziman sees a 3-byte string ending in `b`.

### H. Deep nesting and the collector (§3.1, §5.3)

```
$ ./ziman t_jsondeep.zm   #   400,000 nested Json.JArray → built, 800004 bytes, exit 0
$ ./ziman t_jsondeep2.zm  # 6,000,000 nested             → SIGSEGV
$ gdb -q -batch -ex "handle SIGPWR nostop noprint pass" -ex run -ex "thread apply all bt 12" \
      --args ./ziman t_jsondeep2.zm
Thread 3 received signal SIGSEGV
#0 arena_block_containing (...) at src/arena.c:381
#1 gc_lookup at src/gc_lookup.c:12   #2 is_gc_heap_object at src/gc_mark.c:85
#3 gc_mark_enum at src/gc_mark.c:150 #4 gc_mark_array at src/gc_mark.c:119  … (alternating)
```
Parser/typechecker depth matrix (`deep.py`, 14 shapes × {1000, 50000, 300000}): every failure is a
clean `stack depth exceeded` runtime error or a syntax error with a correct column; no SIGSEGV.
*Why it matters:* the front-end guard works; the collector's mark phase has no equivalent and
cannot have one.

### I. Fuzzing (§5.3)

```
$ timeout 900 python3 -I fuzz.py ./ziman 4000 1
summary: {} of 4000
```
85% mutated from `tests/lang/*.zm` (bit flips, splices, 400× token repetition, 2000-byte tails,
NULs, invalid UTF-8), 15% pure random, 5s timeout each, flagging any `rc < 0`, `rc > 2` or timeout.
Zero. *Why it matters:* the lexer and parser are not the weak point; semantically valid programs are.

### J. Warnings (§6.1)

```
$ clang -std=c11 -O1 -fsyntax-only -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow \
    -Wstrict-prototypes -Wwrite-strings -Wcast-qual -Wmissing-prototypes -Wvla src/*.c
     52 [-Wincompatible-pointer-types-discards-qualifiers]   (all from -Wwrite-strings; noise)
     31 [-Wcast-qual]                                        (deliberate)
      1 [-Wunused-but-set-variable]    main.c:415 'printed_fns'
      1 [-Wsometimes-uninitialized]    typecheck.c:4329 'declared'   ← real
$ clang ... -Wconversion -Wsign-conversion src/*.c           → 6 warnings total
$ cc -std=c11 -O1 -fsyntax-only ... -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -Wcast-qual -Wjump-misses-init -Wduplicated-cond -Wduplicated-branches \
    -Wnull-dereference -Wdouble-promotion src/*.c            → 49 warnings
     29 [-Wcast-qual]  10 [-Wsign-conversion]  9 [-Wjump-misses-init]  1 [-Wunused-but-set-variable]
$ touch src/value.c && make 2>&1 | grep -c "warning:"        → 0
$ grep -c Werror Makefile                                    → 0
```

### K. Trigraphs (§6.2)

```
$ awk <the exact Makefile:105-110 recipe> zone_demo.zm > demo_source.h     # source line: return "a??!b"
$ cc -std=c11 -Wall -Wextra -Wpedantic -o usedemo usedemo.c && ./usedemo
demo_source.h:4:20: warning: trigraph '??!' converted to '|' [-Wtrigraphs]
    return "a|b"
$ grep -rn '??' lib/        → nothing (latent, not live)
```

### L. Quadratic typecheck (§7.3)

```
n=2000  0.01s 17988kB   n=4000  0.04s 29296kB   n=8000  0.15s 52040kB   n=16000  0.65s 97336kB
$ perf report --stdio --no-children -g none
  29.19% GC_mark_from   21.85% __strcmp_avx2   11.78% typecheck   4.90% lex
```

### M. Intermittency (§3.3)

```
$ for i in $(seq 1 15); do make unit-tests | grep -ci 'FAIL|failed|error'; done
24 24 24 24 24 24 24 24 24 24 24 24 24 24 24     (identical every run; all matches are test *names*)
$ for i in 1 2 3; do BIN=./ziman-nogc ./tests/run_tests.sh | tail -1; done
1006 passed, 0 failed   ×3
```

### N. Earlier review's reproductions re-run (§9)

```
$ ./ziman r6000.zm       # regex.compile("()" × 6000)
refused: pattern needs too much memory to match: 1650 MB for 6000 capture groups over 12003
instructions (the limit is 64 MB; use (?:...) for a group whose text you do not need)      exit 0
$ ./ziman-nogc r6000.zm  → identical
$ grep -rn MAX_STACK_MB src/   → src/stack.c:167 only (the #define itself)
$ grep -n '768' src/stack.c    → :102 (message text), :111, :112 (the real checks, as literals)
```

### O. Read-verified, not executed

`native_process.c:64` (missing `HANDLE_SET_INIT`); `native_sqlite.c:344-403` against
`main.c:576-578` (finalizer re-entry); `native_net.c:212-219` against `:320-334` (double
`SSL_CTX_free`); `main.c:217`/`:292` against `gc_boehm_roots.c:52-59` and `stack.c:216-220`
(thread roots); `error.c:273-281` against `frames_grow`; `gc_mark.c:22-27` against `value.c:777`;
`typecheck.c:290/306/382/401`; `regex.c:166/174/491/639` against `tools/check_xalloc.py:46`;
`native_json.c:117` against `:138`; all 62 `__atomic` sites; all 30 capacity `#define`s; all 47
`as.str.ptr` uses.

**Explicitly not verified:** `make analyze` (`-fanalyzer`) — not re-run, the Makefile reports it
clean as of 2026-10-08. The sqlite and `guard_finalizer` deadlocks — structurally verified by
reading, no reproduction. Any non-Linux or non-x86-64 platform. `make test-pg` / `test-mysql`
(need a container runtime). `tests/bench/server_concurrency.sh` (needs the sample backend next
door). The §4.1 double free — no reproduction attempted. The spawned-thread threshold for §3.1 —
arithmetic only.
