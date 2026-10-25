# Ziman — Roadmap

Not commitments, not ordered — ideas and known gaps to pull from as v0
proves out. See docs/DESIGN.md for the principles these should be judged
against before adding any of them.

**Each entry carries its own status after a `--`**: `not started`, `done`, `first step done`, `postponed`.
That status is the most perishable thing in the file, and nothing checks it. Closing a task in
docs/reviews/tasks.md therefore means searching here for what it touched: on 2026-10-07 four entries were
wrong at once because the work had been done and this file not revisited — a per-IP throttle still listed
as needing a `net.peer_address` that existed, a connection cap still listed as blocked on two things that
had landed, metrics listed as not started with half of it shipped, and a superseded collector design still
pointing a reader at the path that was measured and abandoned.

That is not fixable by a script, and it was tried: `tools/check_not_yet.py` catches a stale "Not yet" when
it names an identifier that is now `pub`, and pointing it at this file produced **40 findings, nearly all
wrong** — an entry like "HTTP/2 -- not started" is followed by a paragraph mentioning `listen_tls`,
`byte_and` and a dozen other real things, none of which it claims are missing. The check works on a terse
`Not yet: a, b, c` list and this file is discursive prose, so the guard here is the habit, not a tool.

**What goes stale, learned 2026-10-16 over two sweeps.** The first sweep was prompted by this file
recommending two *finished* items in one sitting, and found four stale claims all in the North Star
section -- the `io` module (replaced by `fs` long ago), "Still missing: `replace`, and a general
`index_of`/`find`" (all three are builtins), the unbounded queue (bounded channels landed
2026-09-24, which R10's own entry already recorded) and generics "in progress, staged struct → enum
→ function" (all five stages shipped 2026-10-10). That looked structural, and the conclusion written
here was that the North Star prose is where the rot lives because it was written once at the start.

**That was too narrow, and the next question asked of this file found four more elsewhere** -- in
"Larger, needs its own design pass": the `Cell<T>` entry (built 2026-10-14 and its headline never
said so), the `collections` refactor (raised and overtaken the same day, F120 at 00:17), the LLM
benchmark (its own body said "done 2026-10-10" two lines above a sentence still calling the
directory a 10-task set) and the memory entry, whose headline stopped being true on 2026-09-29.
"Near-term, likely next" remains the one section with nothing wrong in it.

**The sharper pattern, and the one to check for:** in all four of the second batch the *body* had
been updated and the **headline had not**. That is the more dangerous shape, because an entry whose
body carries a dated "*Update ... done*" note looks maintained, while the headline is the only part
a reader scans when asking what is left -- so a half-revisited entry misleads more reliably than an
untouched one. Closing a task therefore means fixing the status *in the headline*, not only adding
a note underneath, and reading an entry means reading past its headline before trusting it.

A check for exactly this was prototyped and **dropped**: flag an unstruck item whose body says
something landed while its headline carries no status word. After the fixes above it reported three
items, all false positives -- `Everything above the native floor is real Ziman code`,
`Generics: the "just hardcode it" strategy stopped scaling` and `First-class functions / closures`
are topics and principles, not status claims, and headlines like those are common here. Making it
sound would mean requiring every headline to carry a status, which is the convention this preamble
states and dozens of entries do not follow. So this stays in the habit column too, with the
narrower "Still missing" shape being the only part that mechanized (see tools/check_not_yet.py).

That sweep also found the opposite of the 40-false-positive result above, in one narrow case:
`check_not_yet.py` now has a "Still missing" shape, which works for the same reason the "Not yet"
one does -- it reads only the terse list after the marker, inside one item, skipping any item
carrying an Update note. It caught the `replace`/`index_of` clause and reports nothing else in the
file. It also learned that a **builtin** is neither a `pub fn` nor a module, which is why `replace`
was invisible to every earlier shape. That does not reopen the preamble's conclusion: the discursive
prose is still unguardable, and three of the four stale claims above are things no script will find
-- one names no identifier at all, and another names `send`, which exists and always did.

**Opening a task carries the same obligation, which 2026-10-08 is the first demonstration of.** The C
implementation review (docs/reviews/c-implementation-review-2026-10-08.md) opened twelve, and four entries
here read optimistically because of them -- the regex engine's "linear-time", the formatter, the collector's
"does not leak" and the metrics module's lock-free update. Each now carries a dated note pointing at the task,
rather than being quietly contradicted by a file in the same directory. A fifth, platform support, gained a
cheaper first step from the same review instead of a correction.

## North star: write (a slice of) the health-record backend in Ziman

The first concrete target beyond language-design exercises: get Ziman
capable enough to build a real backend service, using the existing
`health-record` backend as the reference for what "capable enough" means.

**Scope, decided:** a representative core slice — a CRUD API, a database,
basic auth, a couple of real endpoints — not full parity with everything
the real backend does (every third-party integration, FHIR, MFA, the
terminology DB, etc.). Expand later once the slice actually works; a
smaller, finishable target is more useful right now than an exhaustive
one that never lands. The architecture to get there:

- ~~**A module system is the load-bearing prerequisite.**~~ Implemented —
  see the full write-up under "Larger, needs its own design pass" below
  (file = module, slash-separated `use` paths, always-qualified calls,
  `pub` for exports, `native fn` for the C boundary). Everything below
  (files, networking, a database client) can now actually be organized as
  a module instead of bolted directly onto the interpreter one builtin at
  a time the way arrays'/structs' builtins were.
- ~~**Only a minimal, fixed set of "native modules" touch C.**~~ The
  registration mechanism is implemented (`src/native.c`), and a first,
  minimal real module exists to prove it: `io` (`read_file`/`write_file`
  — see the module design write-up below for the C-side design, inspired
  by JNI's `RegisterNatives`). A native module still presents ordinary
  typed Ziman function signatures to its callers, who shouldn't need to
  know or care whether a given module is native or pure Ziman. Still
  missing: the actual syscalls (raw sockets, process/env access) beyond
  basic file I/O. The "no bundled-stdlib search path" gap this used to
  note is now closed — see "Bundled core modules" further below: `io.zm`
  is one of the two modules embedded in the `ziman` binary, so no
  project needs its own copy of it anymore.
  `io.read_file` now returns `Result<string, string>` (a missing file is
  recoverable, not fatal — see "Recoverable error handling" below);
  `write_file` is not yet migrated and still fails fatally on a write
  error.
  *Update 2026-10-16: **there is no `io` module any more**, so everything this
  item says about one describes a state that no longer exists. `fs` replaced it:
  `read_text`/`read_bytes`/`write_text`/`write_bytes`, plus
  `write_text_atomic`, each returning `Result<_, FsError>` -- so the
  "`write_file` is not yet migrated and still fails fatally" gap closed by the
  function being replaced rather than migrated, and `lib/core/` now holds 29
  modules rather than the two this item describes as embedded. Kept for the
  reasoning about the native floor, which still holds and is what the entry is
  for; read `fs` for the shape that shipped.*
  Today's built-ins (`print`/`concat`/`str`/`format`/`len`/`push`/
  `insert`/`set`) are conceptually the same idea already, but stay
  special-cased and unqualified for now rather than being migrated into
  the module scheme — see the module design write-up for why that's a
  deliberate non-decision, not an oversight.
- **Everything above the native floor is real Ziman code.** A minimal
  `io` exists now (`read_file`/`write_file`, straight `fopen`/`fread`/
  `fwrite` underneath — no buffering story, no append mode, no directory
  listing). Still ahead: the rest of `io`/`fs` on native file-descriptor
  primitives, `net` (raw sockets) and then `http` (protocol parsing,
  serving, client) on native socket primitives, and a database module
  either as pure Ziman speaking a DB's wire protocol directly over `net`,
  or as a native module wrapping an existing C client library (e.g.
  libpq/sqlite) if hand-rolling the wire protocol turns out not to be
  worth it.
  *Update 2026-10-09: that whole "still ahead" list has landed, and this
  paragraph had gone on describing a state months out of date. `fs` has
  append (`append_text`/`append_bytes`), directory listing (`list_dir`,
  `walk`), atomic writes, byte and line/chunk access, and the path and
  metadata functions. `net` exists with TLS on both ends, and `http`,
  `server` and `client` are pure Ziman on top of it -- the six native
  functions in `lib/http/server.zm` are policy primitives (arena bounds,
  the rate limiter, the per-client connection cap), not protocol parsing.*
  *The open design question in this entry got answered **both ways, split
  by engine**, which is the part worth keeping: `pg` and `mysql` are pure
  Ziman speaking the wire protocol over `net` (`pg` has exactly one native
  function, `float_to_text`, a number-formatting helper), while `sqlite`
  is the native route, nine native functions wrapping the bundled C
  library. So "if hand-rolling the wire protocol turns out not to be worth
  it" was not a single verdict: it was worth it for the two networked
  engines and not for the embedded one, where there is no wire protocol to
  hand-roll in the first place. Both `pg` and `mysql` carry
  `#![experimental]` (see DESIGN.md's F4 entry) -- shipped, not yet
  hardened.* ~~`json` (encode/decode) is flagged separately below since
  it hits a real language gap, not just missing library code.~~
  Implemented — see the `json` core module entry further below; the
  suspected language gap (a self-referential `Json` type) turned out
  not to be one.

Building any of this will surface language gaps that don't show up in
toy programs — worth calling out now rather than discovering them one at
a time mid-implementation:

- ~~**No map/dictionary type.**~~ `Map<K, V>` and `Set<T>` are built-in
  types now, on a 32-way bitmap-compressed persistent hash trie
  (O(log32 n), value semantics like every other Ziman value, so `set` and
  `remove` return a new map and share the untouched structure) -- see the
  "[Resolved: `Map`/`Set` are now built-in types...]" entry below for how
  that was decided, and SYNTAX.md "Maps and sets" for the operations.
  Keys must support `==` and hashing, which is the `Hash` rule rather than
  a special case for maps.
  The "one collection type" question this entry posed did get answered,
  and not the way the entry assumed: there is no `{K: V}` type syntax.
  A map is written `Map<string, int> { "apple": 4 }`, with the type
  arguments always spelled out, because `{...}` alone could not say what
  an empty literal meant and the sequence brackets stay scoped to
  sequences -- the same reasoning that gave `Result<T, E>` angle brackets.
  Still unfinished underneath, and recorded in docs/DESIGN.md rather than
  here: the trie is not arena-routed, so a map crossing a thread is
  allocated the way everything was before that work.
- ~~**No optional/nullable type.**~~ `Option<T>` exists now (built on
  generics, not a hardcoded parametric type like `Result<T, E>` after
  all — see the "Generics" and "Collections module" entries below for
  how it was originally built, and the "Prelude" entry in docs/DESIGN.md
  for why it later moved out of `collections` into the prelude, needing
  no `use` anywhere). Still genuinely needed for a
  *direct* self-referential struct with no escape (`struct Node { next:
  Node }` is still unbuildable as-is; `struct Node { next: Option<Node>
  }` would be) — but a struct/enum that recurses *through an array*
  instead (`enum Json { ... JArray([Json]) ... }`, see the `json` core
  module entry below) never needed `Option<T>` for this at all, since
  an empty array (`[]`) is already a natural finite base case. The two
  looked like the same problem from here; they weren't.
- ~~**No recoverable error handling.**~~ `Result<T, E>` exists now — see
  "Recoverable error handling" under "Near-term, likely next" below for
  the full design and what's still not covered by it (see also: it
  turned out *not* to force the generics question the way this entry
  originally guessed — see that entry's update).
- ~~**Almost no string manipulation.**~~ `concat`/`str`/`format` plus now
  `len` (also accepts `string`), `substring`, `split`, `trim`,
  `to_upper`/`to_lower`, `parse_int`/`parse_float`, and `contains`
  (string-in-string, and array membership for scalar `[T]`) — see
  docs/SYNTAX.md "Builtins". ~~Still missing: `replace`, and a general
  `index_of`/`find` (deferred — see the `index_of` entry below on why it
  needs `Option<T>` or a Result-shaped decision first).~~ *Update 2026-10-16:
  both shipped and this clause had gone stale. `replace(s, old, new)`,
  `index_of(s, needle, from)` and `last_index_of(s, needle)` are builtins
  (docs/SYNTAX.md "Builtins"), and the Result-shaped decision the deferral was
  waiting on was settled by returning `-1` rather than an `Option<int>`.* Numeric builtins
  now also exist: `abs`/`min`/`max`/`floor`/`ceil`/`round`. Array
  `remove(arr, i)` exists, and `==`/`!=` on arrays is now allowed when
  the element type is scalar (structs, Results, and nested arrays still
  aren't comparable — see docs/SYNTAX.md "Arrays"/"Structs"). This closes
  out the "complete the built-in function set" pass.
- ~~**No concurrency story.**~~ Implemented: real OS threads via `spawn`
  and a thread-safe `Channel<T>` (docs/SYNTAX.md "Concurrency"), built on
  a single-writer/actor pattern rather than shared memory plus locks —
  one thread owns a piece of mutable state and is the only thing that
  ever touches it; everyone else reaches it by sending a message and
  waiting for a reply on a `Channel`. `Channel<T>` is a third hardcoded
  parametric type (after `[T]`/`Result<T, E>`), but native by necessity
  this time, not by choice — see docs/DESIGN.md's new entry for why that
  doesn't reopen the generics question the way `Option<T>`/`Map<K, V>`
  did. Surfaced by (and solves) the exact problem found writing
  `health-record-backend-ziman`'s layered example: pure functional
  state-threading works within one request's own call chain, but doesn't
  answer how *multiple concurrent* requests reach one shared,
  consistently-updated store. Still open, not yet decided:
  - ~~**Unbounded queue.** `send` never blocks in v0 — no backpressure/
    capacity limit, a real production concern (a runaway producer could
    exhaust memory) but not a correctness one. Revisit if a real program
    needs bounded backpressure.~~ *Update 2026-10-16: answered on 2026-09-24
    and this item never caught up -- R10's own entry records it. A channel is
    unbounded (`channel_new()`: `send` never blocks) or **bounded**
    (`channel_new(8)`: `send` blocks while full, which is the backpressure
    asked for here), with `try_send`/`try_receive`/`channel_len` alongside
    and `channel_bounded.zm` pinning it.*
  - **No thread-handle/join mechanism.** A spawned function must return
    `void`; its result, if any, can only come back through a `Channel`
    it's explicitly given. Revisit only if a concrete need for "wait for
    this thread to finish" shows up.
- **Generics: the "just hardcode it" strategy stopped scaling, and this
  entry's earlier prediction has flipped.** `Result<T, E>` shipped
  *without* general generics, using the same "hardcode one more
  parametric type into the compiler" trick arrays already used — a
  deliberate, cheaper alternative that solved that one problem well.
  But `Option<T>` and `Map<K, V>` would each mean repeating that same
  one-off C special-casing forever, in C, never in Ziman itself — the
  opposite of what this project is *for*. User-declared `enum` (tagged
  unions, docs/SYNTAX.md "Enums") shipped as half the fix: it's concrete/
  monomorphic today (`enum Shape { Circle(float) Point }`, not
  `enum Option<T> { Some(T) None }`), but combined with real type
  parameters it would let `Option<T>` — and anything Result/Option-shaped
  a user wants — be written as ordinary Ziman, not another C builtin.
  Generics themselves were the still-missing other half, tracked as its
  own design pass below (parser syntax for `<T>` on `struct`/`enum`/`fn`
  declarations, and a monomorphization-or-erasure runtime strategy).
  *Update 2026-10-16: **done, and this paragraph had gone stale** -- it still
  read "in progress, staged struct → enum → function; generic structs are
  done", which is the state of some weeks ago. All five stages shipped
  2026-10-10 (F39): generic structs, bounded type parameters, generic enums,
  cross-module generics and generic functions. `Option<T>` is now ordinary
  Ziman built on them rather than another C builtin, which is what this entry
  wanted. The one residue is stated in that entry: a generic function body can
  do nothing with an abstract `T` beyond `Eq`/`Hash`/`Ord`, and extending that
  was the `Row` bound, withdrawn 2026-10-09 (C18) on finding it cannot work the
  way `compare` does.*

## External review follow-ups (2026-09-23)

An outside compiler/PL-expert review (first exposure to the language; it
read the docs, checked them against the source, ran ~25 probes and
load-tested the sample backend) produced the items below. Full report with
reproductions: docs/reviews/pl-expert-review-2026-09-23.md. Its verdict in
one line: the design is coherent and unusually well documented, but
measured against the "AI-first" goal Ziman fixed the cheap, visible error
classes (implicit coercion, overloading) and left the ones that actually
dominate machine-written bugs, and the runtime cannot yet host a
long-running process. Each item is meant to be picked up on its own, in
roughly this order (cheapest and highest-value first); tick them off here
as they land.

**A. Silent-wrong-answer holes (fix first -- small, and they contradict the
language's own principles)**

- [x] **R1. `Result<T, E>` auto-wrap turns errors into successes when
  `T == E`.** *Done 2026-09-23.* `return "path must not be empty"` from a
  `Result<string, string>` function compiles to `Ok(...)` -- no
  diagnostic -- and that is the stdlib's most common Result shape
  (`io.read_file` included). Fix: refuse the bare-value auto-wrap when
  `T == E` and require `ok(...)`/`err(...)`; audit the stdlib for existing
  victims. Also worth a diagnostic: `Option<Option<T>>` quietly wraps a
  `None` into `Some(None)`. Also correct SYNTAX.md's claim that this
  mirrors Rust's `From` (Rust has no such implicit conversion; this is
  closer to a C++ converting constructor).
- [x] **R2. Integer overflow is silent, and is undefined behaviour in C.** *Done 2026-09-23: traps with a runtime error; `-fwrapv` and `-O2` added.*
  `9223372036854775807 + 1` wraps quietly, while `bigint as int` and
  `int as byte` saturate "so nothing silently wraps" -- the language
  contradicts itself. Immediate floor: add `-fwrapv` to the Makefile (and
  `-O2`, which is currently absent and worth 2-3x). Then decide the
  language rule (trap, saturate, or documented wrap) and apply it to `+`,
  `-`, `*`.
- [x] **R3. `for x in arr` vs `for i in arr` is one syntax with two
  meanings.** *Done 2026-09-23 as a narrow compile error (`for i in a { a[i] }`), keeping one keyword; the `in`/`of` split was considered and deferred.* Iterating an array binds the element; iterating a range binds
  the index; the only guard is a naming convention in SYNTAX.md. A model
  that writes Python's/Go's index loop gets silently wrong sums, not an
  error. Fix: a distinct form for index iteration (or a compile error when
  the binder of an array loop is used to index the same array).
- [x] **R4. No exhaustiveness checking for enums.** *Done 2026-09-23 as `switch` (one arm per variant, no default arm; enums and `Result`); full `match` stays deferred.* Adding a variant
  compiles cleanly everywhere and crashes at runtime in the consumers
  (the sample backend's `service_error_response` has exactly this
  shape). Full `match` stays deferred, but a `switch`-style statement over
  an enum that requires one arm per variant (desugaring to the existing
  `is_`/`unwrap_`) closes the gap with machinery that already exists. This
  is the highest-value static check for machine-generated code.

**B. Diagnostics (the error message is the interface an agent iterates
against)**

- [x] **R5. Special-case the mistakes models make most.** *Done 2026-09-23 for method calls, string/array `+` and `match`; the derive-error case is R6.* `p.to_string()`
  says "no module imported under the name 'p'" -- it should say Ziman has
  no methods and name the function; `"a" + "b"` says the operands must be
  "the same numeric type" and suggests a cast that cannot exist -- it
  should say `concat(a, b)`; `match` says "expected an expression" -- it
  should say there is no `match` and point at `Enum.is_X`/`unwrap_X`. A few
  lines each.
- [x] **R6. `#[derive(...)]` errors point into an invisible file.** *Done 2026-09-23: positions map to the declaration and the message names the derive; no per-field pinpointing.*
  Generated code has no source mapping, so any error reports `1:5` of
  nothing. Stamp every spliced token with the originating declaration's
  line/column and prefix the message with `in derive(Json) for 'Point':`.
- [x] **R7. Auto-import what a derive itself generated.** *Done 2026-09-23 (Json, Row, Properties); field types' own conversions stay explicit but their missing-function error now says what to do.* One
  `#[derive(Json)]` currently needs five hand-written `use json.*` lines
  the generator already knows it needs, and forgetting one produces the
  unhelpful error above. Inject them, or at least say which `use` is
  missing.
- [x] **R8. `--check` and `--json` diagnostics.** *Done 2026-09-23: `--check`, `--json`, `--emit-derived`, `--version`, `--help`.* The CLI takes exactly one
  argument. A type-check-only mode is a few lines (typecheck is already a
  separate phase); machine-readable diagnostics are DESIGN.md principle #5
  and are still unbuilt, even though they are the item most aligned with
  the AI-first goal. (`--emit-derived` to print generated code would help
  R6 too.)

**C. Runtime and memory**

- [x] **R9. Rewrite the memory-reclamation plan before implementing
  anything.** *Superseded 2026-09-23: neither plan was built. A tracing
  collector (Boehm) cut the load test from ~31 GB to ~0.47 GB, and on
  2026-09-24 became the default build (vendored, `make nogc` for the old one),
  with pause times measured; sockets/SQLite handles are still closed by hand.
  See DESIGN.md's "Memory reclamation" entries.* Measured: the sample backend's server reached ~31 GB RSS
  after ~1,500 requests (about 87 kB leaked per trivial request), so it is
  not merely "a real problem" -- the north-star application cannot stay up.
  And both proposed fixes have a design error: (a) the refcounting entry
  claims the value graph can never contain a cycle, but `Channel<T>` is
  mutable in place, so a struct holding a channel of its own type, sent into
  that channel, is a real cycle (reproduced) -- channels need weak edges or
  to be excluded from counting; (b) the per-thread arena entry does not fit
  a thread-per-*connection* server, where one thread serves thousands of
  keep-alive requests and would free nothing until disconnect -- the region
  has to be per request, which needs an escape rule for the buffered
  session and for anything sent over a channel. Also note the leak is
  currently what makes cross-thread `Value` sharing lock-free, so this is
  one project with the ownership design, not two. See the corrections
  added inline to the two entries below.
- [x] **R10. Server hygiene in the sample backend.** *Done 2026-09-23: `os.exit`/`write_stderr`/shutdown handler, `server.max-connections`, graceful drain; bounded channels landed 2026-09-24: `channel_new(capacity)`, blocking `send`, `try_send`/`try_receive`/`channel_len`.* Cap concurrency
  (`server.zm` spawns an unbounded thread per connection -- a DoS
  primitive on its own), and fix the config path that prints a friendly
  error and then crashes on `unwrap` anyway. Add a native `exit(code)`;
  there is also no signal handling, so no graceful shutdown.
  Backpressure is absent too: `send` never blocks and queues are
  unbounded.
- [x] **R23. A packed `[byte]` representation.** *Done 2026-09-24. A `[byte]`
  is now one real byte per element (`ArrayObj.bytes`) instead of a 16-byte
  `Value`; `tests/bench/bytes_bench.zm` (a 1 MB buffer: build, sum, sha256,
  concat, slice, compare, convert) went from 297 MB / 0.33 s to 66 MB / 0.23 s.
  Views (`slice`) work on both representations. See DESIGN.md "Packed byte
  arrays".* (Original problem: every element of a `[byte]` was a full
  interpreter `Value`, so a 1 KB buffer cost ~16 KB and every `net.read`,
  `bytes_concat` and `crypto` call copied and boxed accordingly.)

**D. Language expressiveness**

- [x] **R11. `Ord` bound and a generic `sort`.** *Done 2026-09-23: `Ord` bound, `compare`, `<` on strings, `#[derive(Ord)]`, `collections.sort`/`sort_by_keys`/`binary_search`.* Sorting is not just
  missing, it is inexpressible: no comparator (no function values) and no
  "comparable" bound. The nine hand-written `_compare` functions across
  `byte`/`uuid`/`time` are the symptom. `Eq` and `Hash` already prove the
  bound mechanism.
- [x] **R12. Widen `Hash`.** *Done 2026-09-23: structural and automatic (any `==`-comparable type with hashable leaves); `Uuid`, enums, structs, `Option`, `Result` are usable keys.* Map keys are limited to the seven scalars, so
  nothing can be indexed by `Uuid`, an enum, or a struct -- a real hole for
  an entity-id-keyed backend. Derive `Hash`/`Eq` for structs and enums
  whose fields are hashable.
- [x] **R13. Revisit "no closures".** *(Follow-up 2026-09-24: expression-bodied lambdas and parameter/return type inference from the expected function type, see SYNTAX.md "Function values".)* *Stage 1 done 2026-09-23: function types and references to named functions (no capture); `collections.map`/`filter`/`fold`/`any`/`all`/`find`/`for_each`/`sort_with`. `datasource.with_transaction` and `http.serve` (with an explicit context value) followed on 2026-09-24, and `spawn` accepts explicit type arguments. Stage 2 done 2026-09-24: lambdas `fn(x: int) -> int { ... }` capturing immutable locals by value (a `var` cannot be captured); see DESIGN.md "Closures".* The stated benefit ("libraries can't
  take opaque callback holes") is already delivered by no overloading, no
  methods and no inheritance; the cost is that `http` cannot be a
  framework, every app hand-writes a linear route chain (near-copy blocks,
  the riskiest shape for copy-paste mistakes at ~40 entities), and no
  generic `map`/`filter`/`sort`/`with_transaction` can exist. Decide
  explicitly, and record the decision either way.
- [x] **R14. Derives on generic types.** `#[derive(Ord)]` and `#[derive(Json)]`
  now work on generic structs and enums. Ord generates a generic
  `name_compare<A: Ord, ...>`. Json takes one converter *function value* per
  type parameter (`page_to_json<T>(v, t_to_json)`, `page_from_json<T>(j,
  t_from_json)`), with `int_/float_/bool_/string_ to/from_json` in `json`
  for the scalar arguments. A field like `Page<Point>` gets its converters
  filled in automatically; array/Option type arguments are rejected with a
  message. `Row` and `Properties` still reject generics.

- [x] **R21. `if` and `switch` as expressions.** *Done 2026-09-24: first as the whole right-hand side of `val`/`var`, an assignment or `return` (arms may leave), then anywhere an expression can appear -- an argument, an operand, an element -- provided no arm can leave. See SYNTAX.md "`if` and `switch` as values". Not done: an arm that leaves inside an expression.* (Not from the review --
  surfaced while implementing R4.) Both are statements today, so getting a
  value out of a branch means declaring a `var` first and assigning in each
  arm, or returning from every arm:
  ```
  var status = 500
  switch e {
      NotFound(m) { status = 404 }
      Constraint(m) { status = 409 }
      Connection(m) { status = 500 }
      Sql(m) { status = 500 }
  }
  ```
  Wanted: `val status = switch e { NotFound(m) { 404 } ... }` and
  `val label = if n > 0 { "positive" } else { "not" }`, with each arm's
  final expression as its value. Design questions to settle before
  building: the value of a block (a trailing expression, or a keyword such
  as `yield`); every arm must have the same type (and an `if` used as a
  value needs an `else`); how `return`/`break`/`?` inside an arm behave
  (they leave the enclosing function/loop, not the expression); whether it
  can appear in any expression position or only as a `val`/`var`
  initializer, a `return` value or an argument (compare the deliberately
  narrow rules for `?`); and how it interacts with the Option/Result
  auto-wrap. `switch` already type-checks by rewriting into an `if` chain,
  so the two should be designed together, and doing `if` first makes the
  `switch` case mostly free. Benefit: it removes the mutable-`var`-then-
  assign pattern, which is the shape most likely to leave a path
  unassigned.

**E. Standard library gaps for a backend language**

- [x] **R15. Missing pieces** (HTTP client, logging, filesystem and regex are all done -- regex is a native, linear-time engine without backreferences; see SYNTAX.md and DESIGN.md "Regular expressions"): an HTTP *client* (a Ziman service cannot
  call another); logging (levels, destination); filesystem beyond
  `read_file`/`write_file` (append, stat, delete, list; `write_file` is
  still fatal on failure); regex; sorting (R11); domain-typed map keys
  (R12). *Update 2026-10-08: "linear-time" is a promise about the
  subject string, not about the pattern. `regex.compile("()" * 6000)`
  then `is_match` segfaults at every stack size up to the 768 MB
  ceiling (A16) -- the thread-list construction recurses once per
  instruction carrying a VLA frame, and allocates `n_inst x ncap`
  beside it, so the engine is linear in what it scans and unbounded in
  what it compiles. The existing nesting-depth limit does not catch it
  because the pattern is wide, not nested. No backreferences still
  means no exponential backtracking; it does not mean a hostile pattern
  is safe.*
- [x] **R16. Naming convention.** *Done: the rule is written down (SYNTAX.md, "Naming conventions for module functions"). Surveying every bundled module found it was already followed -- every name imported bare is noun-prefixed, every qualified-use name is a short verb -- so the only rename was `httpclient.client_error_message` -> `error_message`.* `collections` uses type-prefixed names
  meant for bare import (`hashmap_get`); `http` uses bare names meant for
  qualified use (`header`, `response`), which then collide with
  application modules. Pick one convention per usage mode and document it.
- [x] **R22. More serialization formats, and maybe a `Serde` name.** *Done: `ndjson`, `csv`,
  `toml` and a strict `yaml` subset, each a pure-Ziman module converting text to and from a `Json`
  tree, so every `#[derive(Json)]` type works with all of them (docs/SYNTAX.md "Serialization
  formats"). `Json` gained `JInt` and Map-backed objects first (docs/DESIGN.md "JSON on Map").
  No `Serde` alias: `derive(Json)` is the data-model derive and a second derive name would only
  duplicate it -- revisit if that reads badly.*
  *XML followed as `xml`, and the "attribute/text convention" this line waited for turned out to
  be the wrong thing to look for: every such convention (`@attr` keys, text under `#text`) loses
  either the order between text and elements or the difference between an attribute and a child,
  so `xml` has a tree of its own -- `Document`/`Element`/`Node` -- and is the one format module
  that does not go through `Json`. It reads and writes; it has no DTD at all, which is what rules
  out entity-expansion and external-entity attacks, and `XmlLimits` caps size, depth and attribute
  count.*
  *Markdown followed as `markdown`, and it is the one format module whose reader cannot report
  what it does not understand: Markdown has no syntax errors, so an unsupported construct is a
  paragraph rather than a failure. `markdown.parse` does what Markdown prescribes and
  `markdown.parse_strict` returns an Err naming the line, which is the only way a caller learns
  it was handed something that would be misread. The writer is the part that earns its keep --
  escaping chosen per position, so nothing a caller puts in a string can change the structure of
  the output.* Still open: MessagePack/CBOR (binary; bytes have no `Json` form), YAML
  anchors/aliases, TOML dates as a real type, CSV nesting beyond one level.

**F. Tooling and documentation**

- [x] **R17. A formatter (or at least a lint) that makes `val` and `var`
  visually distinct** -- DESIGN.md already calls the one-character
  difference a silent-mutability typo hazard. *Lint done: `ziman --lint`
  flags a `var` that is never reassigned, in user code only (bundled modules
  and derived code are skipped); running it over the bundled library found
  three. `ziman --fmt` is the formatter (a text formatter: it keeps comments and line breaks, see DESIGN.md "The formatter"); `make fmt-check` keeps the library, backend and extra tests formatted.*
  *Update 2026-10-08: the formatter buffers a quadratic result in
  memory -- 32 KB of nested blocks formats to 256 MB of output at 9.7
  GB RSS, and the same code reached through the language server turns
  a 24 KB document into a 144 MB reply (A18). The quadratic output is
  inherent, since every line is indented by its depth; holding all of
  it before writing a byte is not. `make fmt-check` never sees this
  because real code is not nested 8,000 deep -- an editor and a fuzzer
  both are.*
  *Update 2026-10-09: bounded, and the 2026-10-08 note above was half
  wrong about why. Capping nesting depth (A18) did not remove the
  blowup, because the cost is O(lines x depth) and only depth was
  capped -- a document of ordinary depth and very many lines passed the
  check and still grew without limit. The output itself is now capped at
  16 MB as it is appended (A27), which is ~180x the largest file here, and
  the language server will not analyse a document over 4 MB. At depth 190,
  inside the nesting limit: 60,000 lines used to produce 45 MB of output
  at 235 MB RSS and is now refused at 70 MB. Through the language server,
  that same document's `textDocument/formatting` reply is now a 4,682-byte
  refusal. (The 24 KB / 144 MB case in the note above was already refused
  by A18's depth limit; what A27 adds is the ordinary-depth document, which
  that limit cannot see.) Streaming the output, which the note above implied
  was the fix, was not needed and was not built.*
- [x] **R18. Split README.md into sections.** *Done: README.md is now ~900 words (a taste, the language at a glance, a module table, tooling); the old wall of prose lives in docs/FEATURE-NOTES.md under ten headings.* It is ~3,000 words in
  essentially one paragraph, and it is the document most likely to be
  pasted into a model's context as "the language".
- [x] **R19. Fix documentation drift.** *Done: the three named items, plus a sweep that also fixed the "Eight core modules" paragraph (it claimed `Uuid` had no `Hash`), "no `match`"/"no Ord bound" statements in SYNTAX.md, the time/collections/http comments, and stale "no function values" claims (annotated in DESIGN.md, which is a dated record, rather than rewritten).* SYNTAX.md's `Result` section still
  says "no `enum`, no `match`"; DESIGN.md still lists multi-file modules
  under "deliberately deferred"; the `index_of` entry in SYNTAX.md's
  builtins list is corrupted (starts mid-signature) so the function cannot
  be discovered from it.
- [x] **R20. Reconcile the "AI-first" rationale with the real motives.** *Done: DESIGN.md "What the AI-first goal does and does not justify" states the real motive per restriction and the training-distribution cost of each divergence; the weakest diagnostics were fixed (`;`, `if`/`fn` in expression position, `.length`/`.size` on a value).*
  Non-nestable `?` is an interpreter constraint (it happens to be good
  style -- say so plainly); "no closures" is justified by a benefit other
  choices already give (R13). Also acknowledge the training-distribution
  cost of deliberate divergences (`Enum.unwrap_X(e)` for `match`,
  `arr = push(arr, x)` for `arr.push(x)`, `concat` for `+`): a model's
  priors are wrong on the most common operations, which makes R5's
  diagnostics matter more.

Things the review said to keep and copy (do not "fix"): block-scoped
`defer`, parser-enforced identifier casing, compile-checked `format`
literals, named arguments reusing struct-literal syntax, no wildcard
imports, non-shadowable globals, readable generated derive source, the
decision-log-with-rejected-alternatives habit, and generating CORELIB.md
from source.

## Near-term, likely next

- ~~**Recoverable error handling.**~~ Implemented: `Result<T, E>`, a
  second hardcoded parametric type (same trick as `[T]`, not general
  generics — see docs/DESIGN.md), manipulated only through `ok`/`err`/
  `is_ok`/`unwrap`/`unwrap_err` — no sum types, no `enum`, no `match`.
  `<>` brackets, not `[]` (`[]` stays scoped to sequences; `<>` is the
  cross-language convention for parameterizing a builtin type, and
  specifically matches Rust's own split between `Vec<T>` and `[T; N]`).
  `T`/`E` are fully arbitrary Ziman types, not fixed to `string` — lets
  a native function like `io.read_file` keep a simple `string` error
  while application code layers a richer one on top at the boundary.
  `ok(x)`/`err(e)` reuse the empty-array-literal "expected type from
  context" mechanism, extended to also cover a function's own return
  type (not just a `val`/`var` annotation), since `return ok(...)`/
  `return err(...)` is the dominant shape, not an edge case.
  `io.read_file` is migrated (`-> Result<string, string>`) as the
  flagship proof — a missing file is now something a program can
  recover from instead of crashing on unconditionally, with `unwrap`
  as the explicit, opt-in way back to a fatal error when that's what
  you actually want. Deliberately left out, not overlooked:
  - ~~**No `?`-style propagation operator.**~~ **Done as `try`, and F67 checked it:**
    `val s = try connect(host, port)` yields the `Ok` payload and returns the `Err` from the
    enclosing function, and `try` is used throughout `lib/`, so this bullet has described a gap
    the language does not have for some time. The shorthand was not designed speculatively; it
    was designed once the verbosity was real, which is what the bullet asked for. The original
    note: chaining several fallible calls means repeating `if is_ok(...) { ... } else { return
    err(...) }` at each step — real, Go-style verbosity, accepted for now rather than designing a
    shorthand speculatively.
  - **`io.write_file` isn't migrated** — still fatal on failure. Not a
    design gap, just not done; the exact same treatment `read_file` got
    would apply.
  - ~~**`==`/`!=` on `Result`**~~ Implemented: variant (`Ok`/`Err`) then
    payload — an `Ok` and an `Err` are never equal regardless of
    payload, matching two same-variant values are equal iff their
    payload is (reusing `type_supports_equality`, shared with arrays'
    and structs' own equality checks). Legal only when both `T` and `E`
    are (recursively) comparable, since the variant isn't known
    statically. Caught and fixed a real, previously-latent bug while
    verifying this: `type_name()`'s `Result<T, E>` formatting
    (ast.c) allocated one byte short and silently truncated the closing
    `>` in any error message that printed a Result type name — nothing
    had exercised that path before this feature's own error messages
    did.
  - ~~**`Option<T>`** (for the still-separate optional/nullable-type gap
    above) would very likely use this exact same trick — not designed,
    but the precedent now exists.~~ **Done, and the guess was right (F67 checked it):**
    `Option<T>` exists and uses exactly this mechanism — `peer: None` and
    `cert_file: Some("client.pem")` are written bare, with the type coming from context. The
    "still-separate optional/nullable-type gap above" has closed too.
  - ~~**An empty array literal isn't inferred as an ordinary enum variant
    constructor's argument**~~ **-- done 2026-10-11 (F66).** A variant's payload now goes through
    the same check a call argument gets, so every form that needs an expected type works there:
    the empty `[]`, an unannotated lambda, `ok`/`err`, `channel_new()`, an `if`/`switch` used as
    a value, and a bare value auto-wrapped to Option or Result -- all of which were equally
    refused, not just the array this bullet noticed. No new mechanism: the asymmetry was that one
    call site passed no expected type, and the general mechanism this bullet asked for already
    existed. The original note, which described it exactly:
    even though a struct literal field gets
    exactly this treatment (`Holder { items: [] }` works;
    `Json.JArray([])` doesn't, "cannot infer the element type of an
    empty array literal", even though `JArray`'s one field type is just
    as statically fixed as `items`'s). `ok(x)`/`err(e)` are
    special-cased past this (see above), but no general mechanism
    extends "the constructor's own declared field type" to variant
    construction the way struct literals already get it. Narrow, but a
    real asymmetry: found while simplifying `[Json]`-returning code that
    has to bind an empty array to a separately-annotated `val` first
    (`val no_rows: [Json] = []; return ok(Json.JArray(no_rows))`)
    instead of writing `return ok(Json.JArray([]))` inline.
- ~~**Structured/machine-parseable diagnostics** (`--json` error mode).~~
  Done 2026-09-23 (roadmap R8): `--json` prints each error as one JSON
  object on stderr. Still open: reporting *more than one* error per run
  (the compiler stops at the first), a `file` field (no error carries a file
  name yet), and warnings/notes as a separate severity.
- ~~**String concatenation.**~~ Decided and implemented: a `concat(a, b)`
  builtin (both arguments must already be `string` — no auto-stringify,
  keeps it from becoming implicit coercion by another name), plus a
  companion `str(x)` builtin to explicitly convert `int`/`float`/`bool`
  into their display string (rejects an already-`string` argument, so
  there's exactly one way to get a string from any given type). `+`
  itself stays numeric-only, per principle #2. Also added `format(fmt,
  args...) -> string`, requiring `fmt` to be a string literal so its `{}`
  placeholder count is checked against the argument list at compile time
  (see docs/SYNTAX.md) — the one variable-arity builtin in the language,
  by necessity, but still fully statically checked because the literal
  is visible at the call site.
- ~~**Escaping literal `{`/`}` in format strings.**~~ Already implemented, and this entry was
  simply stale: `{{` and `}}` write a literal brace, documented in docs/SYNTAX.md's own `format`
  section and suggested by the error message a stray `{` produces. Found 2026-10-05 while looking
  for something to work on, which is the argument for `make docs-check` existing at all -- three of
  this section's own open items turned out to be done, half-done or wrong.
- ~~**`for` loop.**~~ Implemented, three forms in one grammar production
  (docs/SYNTAX.md "For loops"): `for x in arr` (value), `for i in a..b`
  (index, over an exclusive-end int range), `for i, x in arr` (both,
  index-first — matches Go's `for i, v := range` / Python's
  `enumerate`, the one convention with overwhelming cross-language
  precedent). `a..b` is deliberately *not* a first-class `Range` value —
  it only parses directly after a `for`'s `in`, lowering straight to a
  counted loop with no allocation (rejected an earlier `indices(arr) ->
  [int]` builtin idea for exactly this reason: it would've materialized
  a whole array just to count). Two-binder form is array-only — a range
  has nothing to name besides the count itself, so `for i, j in 0..5` is
  a syntax error. Both binders are `val`-immutable and scoped to the
  loop body only, same as every other binding.
  ~~`break`/`continue`~~ Implemented: both target the nearest enclosing
  `while`/`for` only (no labels — small orthogonal core, revisit only if
  a real program needs multi-level early exit often enough to justify
  it), and using either outside a loop is a compile-time error that
  follows lexical nesting through `if`/blocks but never crosses a
  function boundary (a `break` inside a function called from a loop is
  still rejected on its own, in that function).
- ~~**Arrays / lists.**~~ Decided and implemented: one type, `[T]`, not
  two (see the "one collection type" discussion in this project's
  originating conversation) — no separate fixed-size vs. growable
  distinction, since nothing else in Ziman exposes a stack/heap split for
  that to mean anything yet. Value semantics, like everything else in the
  language (see docs/DESIGN.md's new array entry for the full reasoning
  and the ArrayObj-is-immutable implementation trick that keeps that
  cheap). Builtins: `len`, `push`, `insert`, `set`, `remove` (docs/SYNTAX.md).
  ~~`remove`/`pop`~~ Implemented as `remove(arr, i)`, symmetric with
  `insert`; no separate `pop` (indexing already covers "peek", so a
  combined peek+remove would just be a second way to do the same thing).
  ~~`==`/`!=` on arrays~~ Implemented for scalar-element `[T]` only
  (`int`/`float`/`bool`/`string`) — length then elementwise
  `value_equals`. Deliberately not generalized to "array of any
  comparable `T`" even now that structs have their own equality (see
  the Classes/structs entry below) — arrays of structs/Results/nested
  arrays stay a type error until something actually needs that,
  keeping today's array-equality feature scoped to what was asked for.
  ~~Iterating without manual indices~~ Implemented — see the `for` loop
  entry above; `for x in arr` (or `for i, x in arr` when the index is
  also needed) replaces the manual `var i = 0; while i < len(arr) { ...
  }` pattern.
- ~~**Classes vs. data+trait vs. plain data.**~~ Decided: plain, nominal,
  value-typed `struct`s only — no classes, no implementation inheritance,
  no methods/`impl` blocks. Considered Java-style classes and rejected
  them (virtual dispatch is the same "hidden control flow" problem
  principle #1 already rules out elsewhere); considered Rust's data+trait
  model but shipped only the "data" half, since Ziman has no generics yet
  and trait bounds without generics aren't very useful — see
  docs/DESIGN.md's new struct entry. Construction is named-fields-only,
  never positional (`Point { x: 1, y: 2 }`, not `Point(1, 2)`) to kill the
  "swapped x/y" bug class for structs with same-typed fields. Mutation
  follows arrays' value-semantics story exactly: no field assignment,
  `with p { x: 99 }` builds a new struct instead (a considered-and-rejected
  `..p` spread operator and a considered-and-rejected `new Point { ... }`
  construction keyword are both written up in DESIGN.md, with why they
  lost to the shipped alternatives).
  ~~`==`/`!=` on structs~~ Implemented: field-by-field, in declaration
  order, short-circuiting on the first mismatch (docs/SYNTAX.md
  "Structs") — legal only when every field's own type is itself
  comparable, checked recursively, so a struct nesting only comparable
  structs/scalars/scalar-arrays/comparable-`Result`s is fine but one
  with, say, an array-of-struct field (or a `Result<T, E>` whose `T`/`E`
  isn't itself comparable) is a type error naming the struct. A
  self-referential struct declaration (still uninstantiable,
  see the "no optional/nullable type" entry above) can't send the
  checker into unbounded recursion either — a depth guard in
  `type_supports_equality` (typecheck.c) treats a cycle as
  not-comparable, which can never actually matter for real data since
  no value of that shape exists yet. Still open, not yet decided:
  - **Trait/interface-style polymorphism** — deliberately not attempted
    yet; revisit only once a concrete program needs it and free functions
    plus nominal typing genuinely can't express it.
- ~~**User-declared `enum` (tagged unions).**~~ Implemented
  (docs/SYNTAX.md "Enums"): a variant holds zero or one payload value —
  never an inline tuple or inline struct-shaped payload, same "wrap a
  struct for more than one value" shape `Result<T, E>`'s `Ok`/`Err`
  already use. Construction and consumption are both enum-qualified
  (`Shape.Circle(5.0)`, `Shape.is_Circle(s)`, `Shape.unwrap_Circle(s)`),
  never bare, so two different enums may freely reuse the same variant
  name. `is_<Variant>`/`unwrap_<Variant>` are compiler-generated per
  enum, per variant, mirroring `Result`'s `is_ok`/`unwrap`/`unwrap_err`
  exactly. `==`/`!=` works when every payload-carrying variant's payload
  type is (recursively) comparable, reusing the same
  `type_supports_equality` machinery arrays/structs/Result already use.
  Was concrete (monomorphic) only when this shipped — see the "Generics"
  entry below, now landed through generic enums:
  `enum Option<T> { Some(T) None }` is expressible today.
  ~~**Cross-module (`use`-qualified) enum access.**~~ Implemented
  alongside the "Generics" entry's Stage 4 below (`mod.Shape.Circle(...)`
  works now, generic or not) — turned out not to need its own separate
  effort once cross-module generics needed the same second dot anyway.
  Still open, not yet decided:
  - **`match`/pattern matching — design pass done 2026-10-11 (F67), and the answer is "not
    yet", on a measurement rather than an argument.** Most of what this bullet asked for has
    arrived by other routes: `switch` is exhaustive with no default arm and *is* usable as a
    value in most positions, `if Some(x) = ...` binds, and a payload binder names its value.
    What a real `match` adds beyond that is nested patterns, guards, literal patterns,
    destructuring and or-patterns. **Nested patterns were measured and rejected: across `lib/`
    and the 13,000-line reference backend there are 304 `switch` sites, of which 4 immediately
    re-destructure** — the feature would flatten four places in about 25,000 lines of real
    Ziman. That is the deferral rule answering in the negative with evidence rather than a
    shrug. **Guards, destructuring and or-patterns** show no demand either, and each is a syntax
    a reader must learn. **Literal patterns are the honest open question**, because demand for
    them is structurally invisible to that measurement: an if-else chain on a string is not a
    failed `switch`, so nothing in the tree looks like a missing feature. Revisit on a writer's
    judgement, not on a grep. See docs/DESIGN.md's "Pattern matching, measured rather than
    argued" entry. The original note: the predicate-plus-unwrap style above is the only way to
    consume a variant in v0 — same choice `Result` already made.
- ~~**A real test harness beyond tests/run_tests.sh's stdout/substring
  matching** — fine for now, will need line/col assertions once
  diagnostics get richer.~~ **Done 2026-10-11 (F57).** The line/col assertions this asked for, as a
  third fixture kind: `name.diag` holds `<phase> <line>:<col>` and an optional message substring,
  one line per diagnostic in order, compared against `ziman --json` by `tools/check_diag.py`. No new
  compiler work was needed -- `--json` already emitted `phase`/`line`/`col`/`message` per
  diagnostic (principle 5), and the harness simply ignored it.

  **The gap was demonstrated, not argued.** A `.err` fixture asserts that a message appears
  somewhere in stderr and nothing about where it was reported, so a diagnostic naming the right
  problem in the wrong place passes it -- which is what F50 and F53 both turned out to be. Shifting
  one reported column by one in `src/parser.c`: the `.err` fixture **passed**, the `.diag` fixture
  failed with `col is 29, wanted 28`.

  Leaving the substring off asserts position alone, so a fixture about *where* an error lands no
  longer breaks when someone rewords the message -- the churn that made `diag_map_as_function` fail
  on a reworded but correct hint earlier the same day. A trailing `...` allows further diagnostics,
  which cascades need: one stray `let` produces five.
- ~~**"Did you mean...?" suggestions on an undefined name.**~~ Done 2026-09-28: edit-distance
  search (same algorithm and "strictly less than 3" cutoff as `properties.check_unknown`'s own
  suggestion) over the module's own functions/structs/enums and the callable builtins, on both
  "use of undefined variable" and "call to undefined function". Deliberately does not search other
  modules' exports, including ones not `use`d -- suggesting a name that still needs its own `use`
  line first would trade one error for a more confusing one; a qualified `module 'X' has no
  function 'Y'` is left for later. Two named special cases the benchmark's data pointed at
  directly, not left to the fuzzy search: a foreign declaration keyword (`let`/`const`/`mut`/
  `auto`) parsed as a bare identifier now says "Ziman has no 'let' -- declare with 'val' ... or
  'var' ..."; a hardcoded generic type called like a function (`map()`) now says "'Map' is a
  built-in generic type, not a function: construct one with 'Map<...> { ... }'". The general
  mechanism, with no special-casing needed, also caught `Ok(...)`/`Err(...)` one edit from the
  real lowercase `ok(...)`/`err(...)` construction. Re-run against the benchmark that motivated
  it (`docs/reviews/llm-benchmark-2026-09-28.md`'s own follow-up sections): 2 of the 3 targeted
  tasks move from "did not converge in 5 attempts" to converging (struct_distance in 4,
  safe_divide_result in 5). Re-validating the third (word_count_map) found the fuzzy search
  suggesting `hash` for `has` -- a real builtin, one edit away, and completely unrelated, a close
  name that is actively the wrong answer -- fixed with a third named special case (`has`/
  `has_key`/`contains_key`/...) pointing at the real `contains(m, key)`/`map_get(m, key)`. A final
  re-test still didn't converge, but for a budget/ordering reason, not a remaining diagnostic gap:
  the task needs two independent discoveries (map construction and membership), each fix worked
  correctly when the model reached it, and a model resolving one `--check` error at a time can
  exhaust 5 attempts on a two-discovery task depending which error its own strategy looks at
  first. `enum_shape_area`, `filter_double` and `json_roundtrip` remain outside this fix's reach on
  purpose (a wrapper-struct convention, a cross-module qualified name, and an unrelated vocabulary
  guess -- none an edit-distance-close name). Fixtures: `tests/lang/diag_let_keyword`,
  `diag_map_as_function`, `diag_undefined_function_suggestion`, `diag_map_membership`.
- **`#[deprecated]` and `#[experimental]` as interpreted attributes, not just
  prose.** ~~`#[deprecated]` on a *function*~~ Done 2026-10-05, in the shape this entry's own
  "at minimum" asked for: `#[deprecated]` / `#[deprecated("use X instead")]` on a function, with a
  cross-module call or value reference reported by `ziman --lint` (quoting the message when given),
  and same-module use deliberately silent. `lib/http/server.zm`'s `open_session_with_timeout` --
  the candidate named below -- now carries it. Two things the entry got wrong, both found by trying
  it: `#[deprecated("...")]` did **not** already parse (the attribute grammar accepted only
  identifier arguments, so a quoted message was a syntax error -- only the bare form parsed), and
  an attribute between a doc comment and its declaration made `tools/gen_lib_manual.py` drop that
  declaration's whole description from the manual, which had already silently cost
  `#[thread_key] ThreadLocal<T>` and prelude's five `#[thread_bound]` structs their documentation
  before this feature existed. ~~`#[experimental]` and its generated "Experimental" callout~~ Also done
  2026-10-05, together with the **module-level attribute** this entry called new grammar surface:
  `#![...]`, with the `!`, attaches to the whole file instead of the next declaration (the
  inner/outer split Rust draws) and must precede the first declaration. `#![experimental]` carries
  no compile-time or runtime behaviour by design -- it exists to be the single place the claim
  lives. `docs/CORELIB.md` and `docs/lib/*.md` now generate the callout from it, and the regenerated
  `CORELIB.md` came out byte-identical to the hand-written version, which is the proof the dedup is
  faithful; `make experimental-check` holds `README.md`'s own table to it, that one being
  hand-written and the fourth copy. `pg`, `mysql`, `toml` and `yaml` carry the attribute, and their
  header comments keep the module-specific *reason* while no longer repeating the label.
  ~~`#![deprecated]` is refused with a message saying why, since what it should warn about is still
  undecided.~~ **Done 2026-10-11 (F70): it warns at each `use` of the module.** That is the answer
  to the question the refusal named -- "every `use`, or every call into it" -- and the `use` line
  wins because it is the one place the dependency is written, and so the one line that dropping it
  means editing; warning at every call would bury the answer under repetition. Both import forms
  count, since each is a line that has to go. It lives in `loader.c`'s `resolve_named_imports`
  rather than in the type checker, for the reason the redundant-`as` lint already records there: a
  module's `use` list belongs to the loader and is **already gone** from the merged Module the type
  checker is handed. On the entry file it warns nowhere, since nothing imports an entry file --
  pinned as a fixture so it is not later mistaken for a gap. ~~Still open: `#[deprecated]` on a `struct`/`enum`/`val`~~ **Done for `struct` and `enum`
  2026-10-11 (F58):** naming either from another module -- a struct literal, an enum access -- is a
  `--lint` warning carrying the attribute's message, and same-module use stays silent, exactly as
  the function case already worked. **Deliberately not warned on a type annotation:** a function
  migrating away from a deprecated type still has to name it to accept one, so warning there would
  be loudest in the code doing the migration. ~~**`val` is still refused**~~ **-- done 2026-10-11 (F69), and
  without carrying anything on the binding.** F58's message said supporting it meant putting the
  deprecation on the `Binding`, which would have widened the interpreter's hottest allocation
  (one per binding per call) for a rare attribute. A side table keyed by name avoids that, and is
  sound on two guarantees rather than hopes: the loader merges every module into one and
  self-qualifies global-val names, so a name is unique program-wide, and a local can never shadow
  a global val, so an identifier matching one is that val. The read is caught at `EXPR_IDENT`,
  which its own comment already called "the one place a name is read". **All four declaration
  kinds now carry the attribute and none rejects it.** One limit recorded rather than discovered
  later: a read inside *another* module's global val initializer does not warn, because the
  user-code gate is set per function and global vals are checked before any function body.
  ~~**Still open and blocked on something that does not exist:** whether a deprecation should ever
  be a warning by default (needs "warnings/notes as a separate severity") or a hard error under a
  "deny" policy.~~ **Decided and done 2026-10-11 (F68): a deprecation warns on every compile.**
  That bullet was wrong about the blocker -- `ziman_warning` already existed and already emitted
  `{"severity":"warning","phase":"lint",...}`, so the severity was there; what was missing was that
  the deprecation checks shared a gate with the *style* lints and so only ran under `--lint`.
  Splitting the gate is the whole change. **Menderes chose "every compile"** over keeping it
  lint-only, on the argument that carries this item: the 2026-10-09 benchmark found diagnostics are
  what let a model self-correct, and a warning behind an opt-in command is invisible to anything
  that just runs the program. **A warning never changes the exit code**, so this cannot break a
  passing build; `--lint` still reports the same warnings and still exits 1, and the style findings
  stay behind the flag. **A deny policy was deliberately not added** -- a flag with no consumer,
  and nothing in the tree or the backend is asking to enforce a migration yet. One consequence
  worth recording: `.diag` fixtures required a *non-zero* exit, because when F57 added them every
  diagnostic was an error; a warning on a successful compile falsified that, so a `.diag` fixture
  now honours `name.exit` exactly as a `.out` fixture does. The original entry follows.

  F4 (2026-09-28) just labeled `pg`/`mysql`/`toml`/`yaml`
  "experimental" the only way available today: a sentence in each module's
  header comment, the README's module table and DESIGN.md, all hand-written,
  none of it checked by the compiler or visible to an editor. The grammar
  already has room for this — `#[...]` above a `pub fn`/`struct`/`enum`/`val`
  is a general, reusable mechanism (`src/parser.c`'s `parse_attributes`); any
  name not already interpreted (`derive`, `thread_bound`, `thread_key`,
  `test`) "is parsed ... but silently has no effect yet" by design, so
  `#[deprecated]`/`#[deprecated("use X instead")]` and `#[experimental]`
  already parse today with zero grammar work. What's missing is giving them
  meaning: at minimum, `ziman --lint` (or the type checker itself) flags a
  cross-module use of a `#[deprecated]` item, with the attribute's message if
  it gave one — same-module use is left alone, since the author obviously
  already knows. `open_session_with_timeout` (superseded by
  `open_session_with_timeouts`, kept only for its existing callers, see
  DESIGN.md's E7 entry) is a real, already-in-the-library candidate.
  `#[experimental]` is the softer case F4 actually needed: probably no
  warning by default, but a natural place for `tools/gen_lib_manual.py` and
  `tools/gen_corelib_reference.sh` to generate the "Experimental" callout
  automatically instead of it being hand-copied into each module's
  `describe()` entry and header comment separately, which is exactly the
  kind of drift-prone duplication `derive`'s own "can't silently drift from
  the code" argument was made for elsewhere in this project's own tooling.
  Not designed in detail: a **module-level** attribute (F4 flagged whole
  modules, not individual functions) is new grammar surface, since `#[...]`
  today always precedes one specific declaration, never stands alone at the
  top of a file — needs its own decision on syntax and on what a module-level
  `#[deprecated]` even warns about (every `use` of the module? every call
  into it?); whether a deprecation warning is a `--lint`-only opt-in or a
  compiler warning by default (needs "warnings/notes as a separate severity"
  from the `--json` entry above, itself still open); and whether
  `#[deprecated]` should ever become a hard error (a "deny" policy) once a
  removal is actually scheduled, or stay a warning forever.

- **Make `Map`/`Set` operations faster -- constant factors, not complexity.** Measured 2026-10-05
  against CPython 3.14's `dict`, 1,000,000 operations on a 100,000-entry container, in-program
  clocks, pinned (loop overhead ~41ns/iteration in Ziman and ~43ns in Python, so the figures are
  directly comparable):

  | operation | Python `dict` | Ziman `Map` | Ziman `MapBuilder` |
  | --- | --- | --- | --- |
  | get | 87ns | 2989ns (34x) | 1856ns (21x) |
  | set | 84ns | 1971ns (23x) | 850ns (10x) |
  | remove | 41ns | 5249ns (128x) | 170ns (4x) |
  | contains | 64ns | 254ns (4x) | 214ns (3x) |
  | len | 73ns | 89ns (1.2x) | 108ns (1.5x) |
  | snapshot of 100k | 1.2ms (`dict(d)`) | -- | 2.3ms (`mapb_to_map`) |

  **The complexities are already right** and this entry is not about changing them: `Map`/`Set` are a
  32-way HAMT capped at `MAX_DEPTH 12`, so a lookup is at most twelve hops for any map up to 32^12
  entries -- a constant, and with a better worst case than `dict`'s O(n) under adversarial collisions
  (O(k) among keys sharing 60 hash bits). Every gap above is a constant factor. `Set` needs no
  separate work: `SetBuilder` is `mapobj_set_mut` with a dummy value and `Set<T>` is a `MapObj`, so
  the same trie and the same fixes serve both.

  Ranked by what the measurements actually point at, each with its mechanism already identified:

  **Re-measured 2026-10-11 (F56): the "41x" above is really a quadratic, and the entry attributes it
  to the wrong map size.** The 6,829ms figure it reports "with a 100,000-entry map alive" reproduces
  at **200,000** entries, not 100,000; at 100,000 the same loop costs ~1,800ms. Measured across
  sizes on one machine in one sitting: 215ms with nothing alive, 1,787ms at 100k, 6,303ms at 200k,
  23,852ms at 400k -- four times the cost for twice the heap. So the shape to remember is not "a 41x
  toll on allocation" but "collection is quadratic in live heap size", which is objects x blocks and
  is exactly what (2)'s index removes.

  **Re-measured 2026-10-10 (F31), and (1) below is mis-ranked: the wrapper allocation is not the
  cost, and (2) is not merely "what makes that allocation expensive" -- it is the whole of it.** An
  `Option.Some` allocation on its own costs ~94-123ns. The same allocation with a 100,000-entry map
  alive costs **~6,800ns, a 41x slowdown of an allocation that has nothing to do with the map**, and
  `map_get` (6,424ns net) versus `map_get_or` (106ns net) is that same 60x, not nine tenths of a
  wrapper. So removing the `Option` would not win 9x; anything else allocating in that loop pays the
  same toll. `map_get_or` is already the non-allocating path, at 106ns against CPython's 87ns for
  `dict.get`. **And the one fix this entry suggested for (2) was tried and does nothing:** a
  one-entry last-block-found cache in `gc_lookup`, reverted after paired interleaved rounds showed
  set 231->216ms, get 565->570ms and the 41x case 6,782->6,821ms, all inside the run-to-run spread --
  marking's locality is not block-local, so the cached block misses. That makes four cheap fixes for
  (2) measured and reverted; what is left is the per-arena sorted block index, which is the only
  suggestion never tried, and it is not cheap.

  1. **`get` allocates an `Option` and `contains` does not, which costs ~9x.** Same trie descent,
     1856ns against 214ns (and 2989 against 254 for the immutable `Map`), where Python's own two are
     87 and 64. So roughly nine tenths of `get` is the wrapper allocation and the collection work it
     causes, not the lookup. `contains` at 3x Python is the honest measure of the trie itself. This is
     the largest and clearest win available.
  2. **`gc_lookup` finds a block by walking the block list linearly**, which is what makes that
     allocation so expensive: marking a 200k-entry map walks every leaf calling `gc_lookup`, and a
     heap grown that far has a hundred-odd blocks -- twenty million block visits per collection,
     `arena_block_content` 48% of the profile against `mapobj_get`'s 2.6%. Fix is a per-arena block
     index sorted by address and binary-searched, or a one-entry last-block-found cache since marking
     has real locality. Contained to `arena.c`/`gc_lookup.c`, and it speeds up everything that marks,
     not only maps. Four cheaper attempts at this are already ruled out by measurement -- see
     **tasks.md F31** before trying another. (*Corrected 2026-10-11: that pointer used to name a
     DESIGN.md entry, "MapBuilder vs CPython dict", which does not exist and never did; the record
     is F31's own entry.*)

     ***Done 2026-10-11 (F56): the per-arena sorted block index, and it is the whole of (2).***
     Measured paired and interleaved, baseline against indexed: the 1,000,000-allocation loop with a
     live map went **1,831-1,967ms -> 531-551ms at 100k entries, 6,472-6,527ms -> 1,189-1,263ms at
     200k, and 23,950-24,636ms -> 2,614-2,801ms at 400k**; `map_get` 184-234ms -> 66-78ms and
     `map_set` 108-117ms -> 63-66ms. An allocation with *nothing* alive is unchanged (221-251ms ->
     241-250ms), which is the control: a one-block arena's binary search is one comparison.
     **And the cost is no longer quadratic in live heap size** -- four times the time for twice the
     heap became roughly two, which is what taking the block count out of the per-object question
     means. The numbers this entry reported for (2) are corrected below.
  3. ~~**`MapNode` has no capacity field**, so every insert `realloc`s the node to exact fit and
     `memmove`s to open a slot: a node filling to 32 slots does ~32 reallocs where `dict` amortises
     by doubling. Growing capacity geometrically (4/8/16/32) is contained to `mapobj.c` and is the
     insert-side fix for the 10x.~~

     ***Built and measured 2026-10-15; it changes nothing, and the reason retires this item.***
     A `cap` field plus a doubling `node_reserve_mut` (from 1, not 4 -- these nodes are meant to be
     sparse, and a floor of 4 hands every single-entry node three dead slots) gave
     **281-287 ns/insert against the baseline's 284-288** on 2,000,000 inserts. The realloc *count*
     was real; its *cost* was not, because glibc grows a small chunk in place rather than copying,
     and the per-insert `memmove` that opens the slot is unchanged by capacity either way.

     **What the insert actually costs is collection pressure, measured by varying the live set:
     135 ns/insert building to 1,000 entries, 149 to 10,000, 291 to 100,000.** Same code path,
     more than twice the cost, purely because more is alive -- one `Leaf` is allocated per entry,
     and a tracing collector prices a short-lived allocation by the size of the live set. That is
     the identical mechanism already measured for `get` (191/236/378 ns at the same three sizes),
     so `MutableMap`'s insert gap and its lookup gap are one problem, not two.

     **And that strengthens the case for a flat table behind `MutableMap` specifically** -- the
     thing this entry's closing paragraph warns against starting with. `dict` stores its entries
     inline in a dense array, so an insert allocates nothing amortised; the trie allocates a `Leaf`
     per entry. Removing that allocation is the lever the measurement points at, and a flat table
     removes it by construction. Not a licence to skip measuring: the warning below still stands,
     and three "structurally obvious" changes measured worse than nothing on 2026-10-15 alone.
  4. **Every `mapb_*`/`setb_*` call validates its handle through `handleset_has`**, a hash probe paid
     per operation. Small, but it is on literally every builder call, and for `len` -- 108ns against
     the immutable `Map`'s 89ns for the same field read -- it is the entire cost.

  **Re-measured 2026-10-15, with a checked-in benchmark this time (`tests/bench/map_ops.zm` and
  `map_ops.py`).** The table at the top of this entry had no source and so could not be retaken,
  while three changes that bear on exactly this workload landed after it: (2)'s block index (F56),
  the scaled `ZIMAN_GC_TRIGGER_BYTES`, and `Value.type` as a pointer. Two runs, 100,000 entries,
  1,000,000 operations, loop overhead measured and subtracted:

  All figures are **nanoseconds per operation**, loop overhead subtracted, two runs shown as a
  range -- the same unit as the 2026-10-05 table above, which carries it on each cell instead.

  | operation | CPython 3.14 `dict` | Ziman `Map` | Ziman `MutableMap` |
  | --- | --- | --- | --- |
  | get (`Option`) | 12ns (`d[k]`) | 493-502ns | 533-562ns |
  | get_or | 30ns (`.get`) | 79-94ns | **70-81ns** (was 580-612) |
  | contains | 27ns | 178-253ns | 164-172ns |
  | len | 15ns | 8-31ns | 24-39ns |
  | set | 12ns | 1316-1335ns (persistent) | 349-395ns |
  | remove | 61ns (`del`+set) | 1467-1476ns (persistent) | 998-1014ns (incl. a set) |

  The cheap rows (`len`, `contains`) carry a wider error bar than the spread suggests: the clock is
  `clock.monotonic_millis`, so a row's cost is a millisecond count over exactly 1,000,000
  operations, and the subtracted loop overhead itself measured 63-81ns across runs. Read single
  digits there as "at parity", not as a figure.

  **Ziman's absolute costs are 2-6x better than the 2026-10-05 figures** -- `Map` get 2989 -> ~497,
  `Map` remove 5249 -> ~1471, `MutableMap` get 1856 -> ~553, `MutableMap` set 850 -> ~372 -- which
  is (2) and the trigger fix arriving. **The *multiples* in the old table cannot be carried
  forward**, because its CPython column was measured on an older Python and with a subtraction this
  benchmark had to fix: a first version of `map_ops.zm`/`.py` did not make each measured loop the
  baseline loop *plus* the operation, which reported `dict len` at **-5 ns/op** and `dict [k] = v`
  at 1 ns/op. Compare absolute Ziman columns across dates; treat any ratio as belonging to its own
  sitting.

  **One row was backwards, and fixing it is done.** `mutable_map_get_or` measured *slower* than the
  `Option`-allocating `mutable_map_get` it wrapped, because it was Ziman sugar that switched on that
  `Option` -- so the one call whose purpose is to avoid a wrapper allocated one anyway, while the
  immutable `Map`'s `map_get_or` (a builtin in `interpreter.c`) was 5-6x faster than its `map_get`.
  Now a native of the same shape: **580-612 -> 70-81 ns/op, about 7.5x**, and the two `get_or`s
  agree as they always should have. This is the word-count idiom in prelude.zm's own comment, run
  once per input word.

  **The builders are a flat hash table now (2026-10-15).** `src/maptable.c`, behind
  `MutableMap`/`MutableSet`; `Map`/`Set` keep the trie, which is right for a *value* whose `set`
  must leave the old map intact. Per the measurement above, the lever was the trie's per-entry
  `Leaf` allocation, and a dense entries array removes it. Against CPython 3.14 `dict`, 100,000
  entries: **set 335-346 -> 47-49 ns/op** (dict 14-15, so 18x -> 3.2x), **contains 71-78 -> 34-39**,
  **get_or 76-86 -> 45**, **remove+set 1107 -> 336-350**, and filling one map 741-762 -> **112-117
  ns/insert**, 6.5x. `shared` disappeared with the leaf sharing that needed it, `set_add` is one
  lookup instead of two, and insertion order comes from the dense array rather than a per-entry
  sequence number and a sort.

  **`Set`/`MutableSet` need no separate work, now measured rather than assumed (2026-10-15).**
  `MutableSet` already shares its storage with `MutableMap` -- the same `MutableMapBuf` over the
  same `MapTable`, the same content marker, every native through `mutable_map_get_buf` -- so the
  flat table applied to it unchanged, and `Set` keeps the trie for the same reason `Map` does.
  `map_ops` had no set rows at all, which meant a session's worth of optimisation went unverified
  on half the types; it has them now, with a matching `set` section on the CPython side:

  | operation | CPython `set` | Ziman | note |
  | --- | --- | --- | --- |
  | contains / `in` | 7ns | `Set` 59ns, `MutableSet` 24-36ns | tracks `Map`/`MutableMap` exactly |
  | len | 14ns | `Set` 19-20ns, `MutableSet` 41-52ns | as for maps |
  | add, already present | 15ns | `MutableSet` 12-25ns | short-circuits on the first lookup |
  | remove + add | 36ns | `MutableSet` 104-126ns | 2.9-3.5x |
  | insert, persistent | -- | `Set` 1595-1650ns | path copying, as `Map`'s own `set` |

  **And there is no `discard` for sets because there is nothing to fix:** `mutable_set_remove`
  already answers with a `bool`, which is exactly the shape `mutable_map_discard` had to be added to
  provide. The set side of the API had the cheaper design all along -- and `MutableSet remove+add`
  at 104-126ns against `MutableMap discard+set` at 111ns is the measurement that says so.

  **`mutable_map_discard`, added 2026-10-15 because the measurement asked for it.** `remove+set`
  was the one row still far out -- 8.9x `dict` where everything else had come down to 2-3x -- and
  the cause was not the removal. Isolated by comparing against `mutable_set_remove`, which answers
  the same question with a `bool` where `mutable_map_remove` answers it with an `Option<V>`: on a
  100,000-entry map, removing and re-inserting every key costs **356-357 ns/op through `remove`
  against 111-112 through the bool-returning path**, on the same table, the same probe and the same
  tombstone. About 240ns of that row was one `Option` allocation, and the removal itself is ~65ns,
  i.e. **2.9x `dict`'s 35-38ns for `del`+set -- in line with every other operation.**
  `mutable_map_discard(mb, k) -> bool` is that path, measured at **111 ns/op**. `remove` stays for
  when the old value is actually wanted, and says in its own doc comment what it costs.

  **The one regression, partly closed and honestly reported.** Creating and discarding *many*
  builders churns more allocator bytes than the trie did, because a table is two arrays and `free`
  is a no-op in a `make gc` build. Making the index array atomic (it is `int32_t`; it cannot hold a
  pointer), allocated fresh rather than `realloc`'d-then-overwritten, and sized by the load factor
  alone rather than a third larger took it from 122/259/320 to **125/230/263 ns/insert** at
  1,000/10,000/100,000 entries against the trie's 135/146/281 -- so the 1,000 and 100,000 cases are
  now at or better than the trie, and the 10,000 case is ~1.6x worse. Two further attempts are
  recorded in `maptable.c` as measured failures: fixed 1024-entry chunks (fixed it, and cost
  csv_extended 0.16s -> 6.8s by allocating 57 KB per small builder) and
  `GC_malloc_ignore_off_page` (removed the large-block warning, and gave up `GC_REALLOC`'s in-place
  growth, 262 -> 322-356).

  **Then cut the gap further, 2026-10-15, and found most of it is not the map.** Decomposed `get` on
  the same binary: the lookup is ~92ns (that is `get_or`), the `Option` allocation ~310ns, the
  `if Some(v) =` destructure ~35ns. Two fixes landed. The `Option` *type* was being rebuilt per
  call -- `type_enum("Option")` strdups the name and `type_ref` mallocs 64 bytes -- so every
  Option-returning native did three heap allocations where only the `EnumObj` carries per-call
  information; it is now one shared constant in `.rodata` (`type_option_ref`, src/ast.c), taking
  `get`'s allocation-only cost from 402 to 274-294 ns/op and `MutableMap get` from 533-562 to
  480-486. And `mutable_map_get_or` became a native, 580-612 -> 70-81 ns/op.

  **What remains of `get` is collection pressure, and it is measured, not assumed.** The same
  1,000,000 Option-allocating lookups cost **191 ns/op against a 100-entry live map, 236 at 10,000
  and 378 at 100,000** -- identical allocation and identical lookup, twice the cost purely because
  more is alive. That is the structural difference from `dict`: a tracing collector makes a
  short-lived allocation cost a function of the live set, while CPython's refcounting frees that
  `Option` immediately at constant cost. A per-operation allocation in a hot loop over a large
  container is the worst case for one and the best case for the other. So the lever is "do not
  allocate per operation" -- which is `get_or`, now at 64-119ns against `dict.get`'s 30ns -- and
  not "make the allocation cheaper", whose cost is somewhere else entirely. Removing the
  allocation from `get` itself means unboxing single-payload variants, which docs/MEMORY.md
  analysed and recommended against on its own measurements.

  **And one finding that is not about maps at all, filed as F140.** `contains` measured 182-220ns
  while the allocation-free `get_or` measured 87-119, for the same trie descent and no wrapper.
  The cause is that `exec_call` dispatches every builtin through a chain of **60 sequential
  `strcmp`s** on the callee name, and `contains` sits 44th where `map_get_or` sits 22nd. Hoisting
  it to 5th, changing nothing else, took it to 78-124 ns/op. Every builtin call in every program
  pays its own share of that, so it is worth more than anything left in this entry -- see F140,
  including why reordering the chain is *not* the fix.

  **Checked whether the int keys were flattering CPython, 2026-10-15: they are, and it does not
  matter.** `hash(i) == i` for a small int and the slot is `hash & mask`, so keys 0..n-1 land one
  per slot in a `dict` with no collisions and no hashing cost, while `map_hash` (mapobj.c) runs a
  murmur3 finalizer over the identity hash precisely so consecutive keys do not pile into one
  subtrie of a 32-way trie -- Ziman pays ~5 arithmetic ops the other side skips. String keys also
  cut the other way in one respect: a CPython `str` caches its hash on the object, where a Ziman
  string is a `{ptr,len,term}` view with nowhere to keep one, so FNV-1a runs per lookup. So
  `map_ops` now measures both, with the keys pre-built either way so no key construction is timed:

  | operation | int keys | string keys |
  | --- | --- | --- |
  | `Map` get_or vs `dict.get` | 70-72 / 27ns = **2.6-2.7x** | 88-90 / 36ns = **2.4-2.5x** |
  | `MutableMap` get_or | 76-86 / 27ns = **2.8-3.2x** | 93-94 / 36ns = **2.6x** |
  | `Map` contains vs `in` | 60 / 32ns = **1.9x** | 83-85 / 17ns = **4.9-5.0x** |
  | `MutableMap` contains | 71-78 / 32ns = **2.2-2.4x** | -- |
  | `MutableMap` set | 335-346 / 19ns = **18x** | 320-324 / 19ns = **17x** |

  The read ratios sit around 2-3x whichever key type is used. **So the remaining gap is a property
  of the structures and the interpreter, not of a flattering key distribution** -- worth knowing
  before anyone spends effort on hashing.

  **The `contains` figures above are corrected, and the correction is the lesson.** Every earlier
  `contains` number on this page and in F140 was measured inside
  `if contains(...) { acc = acc + 1 }` while the `get_or` rows were measured as plain expressions
  -- and that `if` wrapper costs **32-33 ns/op on its own**, measured directly. So `contains` was
  reported as ~20ns *slower* than `get_or` all day when it is in fact faster, which is what the
  code says it should be: one `mapobj_get` and a bool, against one `mapobj_get` plus a third
  argument expression. `map_ops` now keeps two baselines, one per loop shape, and each row
  subtracts the one matching it.

  Chasing that phantom produced a type-checker specialisation of `contains`/`len` by static
  argument kind (the `BINARY_SUBSCR_DICT` idea), which measured **no change at all** and was
  reverted -- it was built to fix something that was never there. The before/after figures in F140
  are unaffected, since those compared `contains` against itself in a fixed loop shape; only the
  cross-row comparison was invalid.

  ~~**What is left of this entry, then:** (3) `MapNode`'s missing capacity field, which is the named
  mechanism behind `MutableMap` set at ~372 against `dict`'s 12 and is still untouched; and (4)'s
  `handleset_has` toll, now visible as the `len` gap (8-31 for `Map` against 24-39 for
  `MutableMap`, the same field read) and still a few nanoseconds on every builder call.~~ (1) is
  closed by F31's correction and by the `get_or` fix above; (2) is done.

  *Update 2026-10-16: that sentence is struck because **all four are now closed**, and it had gone
  stale twice over -- once when (3) was retired and again when (4) was, each time by a note added
  underneath it rather than a correction to it. (3) was measured and retired in `070d922`: node
  growth is not the insert cost (281-287 against 284-288 ns/insert, since glibc grows small chunks
  in place), and `MutableMap`/`MutableSet` then left the trie for a flat table anyway (`308503b`),
  so the field it asks for is on a structure they no longer use. (4) is the attempt-and-decline
  recorded below. **What is actually left of this entry is one thing, and it is not in this list:**
  `Option` boxing on `get`/`remove`-shaped natives, which keeps those rows far out -- `get` at
  344-348ns against `dict`'s 12 is allocation-bound, not lookup-bound. The general fix is unboxing
  single-payload variants, analysed and declined in docs/MEMORY.md ("Design: unboxing single-payload
  enum variants, and the case against building it"); the pattern that works instead is an additive
  native per case, as `mutable_map_discard` was (356-357 → 111 ns/op, because ~240ns of
  `remove`+`set` was one `Option` allocation).*

  **Half of that residue is now closed (2026-10-16): `Option.None` is one shared `.rodata` constant,
  and every failed lookup is 3.0x faster** -- the miss path of `MutableMap get` goes from 251-277 to
  **85-94 ns/op**, which is within the `if Some(v) = ...` shape's own 32-33 ns of the allocation-free
  `get_or` at 56-64. Interleaved A/B, with `tests/bench/option_none.zm` checked in so it can be
  retaken. So on a miss the allocation is gone rather than reduced. A `Some` carries the
  value found and still allocates; a `None` carries nothing, so every one of them was a fresh
  104-byte `EnumObj` holding identical bytes. This is not the declined unboxing -- it needs no new
  representation, because the object it removes holds no information. See docs/MEMORY.md, "Measured:
  `Option.None` as one shared constant", for why sharing it is sound and for the generalisation left
  open (every payload-less variant, interned on its AST node, which the one profile taken so far
  says would buy 0.031%).*

  **(4) attempted and declined, 2026-10-16.** The replacement was built and measured --
  `src/handleslot.c`, an index+generation table whose validation is a bounds check, a load and a
  generation compare: **6.7-7.5ns for `handleset_has` against 1.0-1.1ns**. It is in the tree, inert,
  and should stay that way until someone agrees the cost below, which is recorded in full in
  `src/handleslot.h`. The blocker is rooting, not speed. Today the handle *is* the address, and an
  integer indistinguishable from a pointer is what keeps the object alive under a conservative
  collector; an index roots nothing, and a slot holding the pointer strongly would keep every handle
  ever made alive. A **weak** slot (`GC_general_register_disappearing_link`) does not work either,
  for two reasons found while rewiring the first caller: the collector would see every builder as
  unreachable the moment it was made, so the first collection takes all of them; and the lock-free
  lookup is unsound regardless, because `GC_stopped_mark` restarts the world before
  `GC_finish_collection` clears the links, so a lookup can hand back an already-condemned object.
  The sound shape is `{ addr, handle }` -- the address stays in a scanned field purely to root, the
  index validates -- which costs a Ziman-visible second field on `net.Listener`, `MutableString` and
  every other wrapper. A few nanoseconds does not buy that, in this of all files.

  **What that attempt did find was worth more than the nanoseconds.** Five of the ten `HandleSet`s
  never removed anything, so each entry outlived its object as a dangling pointer, and the four
  builder sets were unbounded: 16,000,000 short-lived `mutable_string_new()` calls left a **268 MB**
  table at ~16.8 bytes each. Peak RSS at 4,000,000 builders went **98.9 MB to 12.1 MB**, flat in
  builder count instead of linear, at no measurable time cost. See `tools/check_handle_sets.py`,
  which now requires every set to have a removal path and found a fifth leaking set on its first run.

  Needs nothing: `len` is already at parity, and the immutable `Map`'s `remove` at 128x is the worst
  row but is inherent to being persistent (path copy plus node collapse plus a fresh `MapObj` per
  removal) -- `MapBuilder.remove` at 4x is the one to use in a loop, which is what the builders are
  for. Do not start with (3) or a flat open-addressing table for the builder: twice in one day the
  structurally obvious change measured worse than doing nothing (the `Type` union, and three separate
  attempts at (2)), so measure each step.

## Larger, needs its own design pass

- **Relaxing the no-overloading rule: let a *declaration* carry the dispatch the arms hand-code.
  Shipped 2026-10-19 (F174-F184), and built on the same day (F185-F201).**
  Raised 2026-10-18, at the end of the collection-pairing pass (F149-F162), from the question
  "now that we have quite a number of overloaded functions, what do you think on relaxing the
  no-overloading rule?" The evidence below is the case as it was argued; **what was actually
  built, and where it stopped, is under "What shipped".** This entry predicted the opposite --
  "not started, and deliberately not started yet", on the grounds that this project writes a case
  down and lets it sit, which is what declined `..=` and the handle-validation redesign. The
  prediction was wrong, and the thing that made it wrong is worth keeping: reading
  `infer_call_type_args` dissolved the one objection that had looked fatal.

  **The rule to restate first, because it is not what the docs said.** Ziman does not lack
  argument-based overloading: it has **28 overloaded builtin names over 87 declarations** (`len`
  and `contains` nine each, `str` six, `snapshot`/`remove`/`mutable`/`concat`/`clear` four).
  What it lacks is overloading *available to a declaration*. Every one of those sets is resolved by
  a hand-written arm in `typecheck.c`, and `builtin fn` entries in `lib/prelude.zm` are
  documentation that the arms are free to contradict. Two paragraphs of this file asserted the
  absence of a feature the language has in 28 places; both are corrected above.

  **What the pass proved about the mechanism, since this is the whole argument.**

  *First-argument dispatch was sufficient every time* -- 28 names, not one needing more. And the
  single case where it was **not** sufficient, `to_set` against `to_mutable_set` (same argument
  types, differing only in what they return), is the case where the restriction gave the right
  answer: they stayed two names. That boundary is worth keeping, not relaxing.

  *Nine of the ten things that broke during the pass trace to the arms, not to overloading.*
  A diagnostic quoting `$mutable_array_lit`, a name that does not lex. A discarded `mutable(42)`
  accepted in silence, and `remove`'s mirror-image trap, both because the purity gate is keyed per
  *name* while purity is per *overload*. `concat`'s spread rule written twice and the two copies
  disagreeing, so `concat([1, 2])` answered nothing. The quadratic lint keyed per name, leaving two
  dead branches and advising a `MutableString` for an array. Eight named-argument call sites broken
  and `sort<int>([])` made unspellable, both because overloading is builtin-only and a builtin
  takes neither named arguments nor type arguments. A fixture repointed off `slice`. Only the
  `[string]` ambiguity in `concat` is a cost of overloading *itself*.

  *And the declarations drift because nothing makes them binding.* `str` documented "accepts any
  type" while its arm refused `string`; `print`'s recorded reason stayed false for an hour.
  `tools/check_builtins.py` exists only to catch that divergence.

  **The shape worth designing**, kept as narrow as the evidence supports:

  > Several `fn`/`native fn` declarations may share a name **when they differ in the first
  > parameter's type**, and the checker resolves from the declarations. Overload sets that overlap
  > on argument one are a compile error. No return-type dispatch, no ranking, no implicit
  > conversions -- F148's boundary exactly, which held.

  **That narrow shape is not what shipped**: the user chose resolution over *every* argument
  position instead, for the reason recorded below. Everything else in the box held.

  What that buys, all of it observed rather than imagined: the ten prefixed implementations under
  `reverse`/`filter`/`sort`/`sort_desc`/`sort_with` would simply *be* the overloads and F162's arm
  would disappear; an error would name a real function because one exists, dissolving the
  `array_filter` leak; purity, the discard gate and the lint become per-declaration, which is what
  `remove` and the `concat` lint needed; `check_builtins.py` becomes redundant; and library code
  gets the power the prelude has -- today `lib/graphics` cannot express what `lib/prelude.zm` does.

  **What it costs.** The `concat` `[string]` conflict is the warning: a `[string]` is both "the
  pieces of a string" and "one array of parts", and only having two *names* kept those apart. Once
  a type can belong to two sets in library code, an author can build that ambiguity, so the overlap
  rule above and an error that explains *why* a set is ambiguous are part of the feature, not
  polish. `--api builtins`, `src/lsp.c`, the VS Code grammar and the formatter all move from
  name to set -- and note those last two hand-kept lists went stale three times in one day
  (F151, F152, F160), which this would also fix by making them derivable.

  **What shipped (2026-10-19): `docs/design/full-overloading.md`, F174-F184.** The user chose
  **full** resolution -- every argument position, not the first only -- after the first-parameter
  design measured out at 43 of the 50 existing sets. The superseded document is kept beside it for
  that measurement. The cost objection raised against full resolution (that it is circular, since
  an overload must be chosen before a lambda or an `[]` can be typed) **did not survive contact
  with the code**: `infer_call_type_args` already walks the arguments skipping exactly the ones
  `expr_needs_expected_type` flags, so resolution is a filter over self-describing positions plus
  the syntactic *shape* of the rest -- no speculation, no backtracking, no constraint solver. The
  earlier note below described the first-parameter design and is superseded.

  Seven stages: the resolver running beside the arms (F176), declaration-site checking for
  overlapping sets (F177), the 28 paired names converted into real overload sets (F178-F180),
  purity and the discard gate read off the declaration rather than the name (F182), the last
  hand-kept list generated instead of maintained (F183), and the feature opened to library and
  program code (F184). Stage 4 came in **void** (F181 -- corrected by F185, which found the
  reason given was wrong), and stage 6 turned out three-quarters already done. `make check` now
  runs `tools/overload_sets.sh`, which reports **39 resolver-owned sets, 0 unresolvable**. For a
  converted set the declaration *is* the rule rather than documentation an arm may contradict --
  but `tools/check_builtins.py` was **not** made redundant as stage 6 predicted, because the
  polymorphic builtins are still dispatched and still need its guard; F183 withdrew that
  prediction.

  **Stage 8 -- folding the polymorphic builtins in too -- converted five arms of seventy and
  stopped on three constraints** (F186-F191). The "expects ..." sentence must be derivable from
  the declarations; a set gets one message, not one per arm; and, the general one:

  > An overload set resolves from arguments that describe themselves. A function whose job is to
  > accept *anything* receives exactly the expressions that do not.

  `abs`, `swap`, `slice`, `to_set` and `len` take collections and numbers and converted. `str`
  passed the first two tests, was converted, broke `str(switch ... )` in real code because a value
  block carries no type of its own, and was reverted. 65 arms remain and are **not** a backlog:
  the collection vocabulary is the natural home of overloading in this language and the universal
  verbs are not. The estimate for this stage ran ~50 -> ~3 -> 5 as each constraint was found by
  reading the code rather than counting names.

  **And it was used immediately.** The Map/Set pairing pass (F192-F201) rests on it entirely:
  one `filter` over all six collections, one `keys`/`values`/`items`/`get`/`get_or` over both Map
  worlds, the four set operations over both Set worlds, variadic `remove_all` and `add`. Names
  invisible to `--api builtins` went **20 -> 1** (`array_order_of`, unadvertised on purpose per
  F172). That is the "another row of the collection table wanting an overload" test below, met
  within a day of the feature landing.

  **Superseded note (first-parameter design):** Written
  after the collection pass closed, on the user's request, and it changes one thing in this entry:
  measured against the prelude as it stands, **the rule above covers 43 of the 50 sets, not all
  of them.** Five (`mutable_set_union`/`_intersect`/`_difference`/`_is_subset`,
  `mutable_map_set_all`) are distinguished by argument *two*, `concat` by an arity rule no
  signature can state, and `mutable` by a catch-all `(item: T)` arm that would need the
  specificity ranking this rule forbids. The document proposes resolving on argument one and then
  two, leaving `concat` and `mutable` as documented arms, and stages the work so that each step
  has an observable invariant. It does not argue for starting.

  **What was to watch, and still is.** The argument against was that a second `[string]`-style
  ambiguity -- one type belonging to two sets, giving two readings silently -- would be the
  failure mode this language most consistently refuses. It has not appeared. The declaration-site
  overlap check (F177) and the `overload_sets.sh` gate exist so that a second one is a compile
  error rather than a surprise, and `ovl_indistinguishable_at` is what makes that checkable.

  **Two loose ends left deliberately.** `#[when_unmatched("...")]` (F187) was built for stage 8's
  per-set hints, and stage 8 then stopped at five conversions, none of which needed it -- it has
  **no production user**, only a fixture, and is either owed one or owed a retirement.
  `effectful_despite_answering` (F200) is a one-name veto list carrying a written promise that a
  third case means writing the attribute rather than extending the list.

- **`lib/graphics`: a 2D drawing model, with document and image backends -- all five phases
  built (F141-F145).** One `Scene` renders two ways: to PDF (vector paths, gradients, the standard
  fourteen fonts, an embedded TrueType face carrying Unicode text, images with alpha) and to an
  image (anti-aliased fills under both winding rules, stroking with caps/joins/dashes, clip paths,
  cached glyph rendering). **Phase 6, a document layout layer, is now designed and not built**
  (`docs/graphics/phase-6-layout.md`, F146) -- and that document says plainly that it is a guess,
  because nothing has used the drawing model yet.
  Raised 2026-10-16 out of asking what reaching export parity with the Spring backend would take.
  Of the reference backend's ~13 export formats, seven are reachable with the modules Ziman already
  has (csv, flat-json, html, markdown, plain text, xml, vcard, all served by `csv`/`json`/`xml`/
  `markdown`/`mime`) and four are named exclusions (FHIR, C-CDA, HL7 v2, SMART Health Cards).
  **PDF is the only one Ziman cannot reach at all**, and a PDF writer on its own would be a
  single-purpose answer to a question that is really "render a document, and also render an image".

  **The design is written before the code**, one document per phase in `docs/graphics/`, with the
  architecture and the shared invariants in its `README.md`. Tracked as **F141 to F145** in
  docs/reviews/tasks.md:

  | phase | modules | effort | state |
  | --- | --- | --- | --- |
  | 1 | `geom`, `shape`, `paint`, `scene` -- value types, no rendering | M | built |
  | 2 | `pdfobj`, `pdf` -- PDF with the standard fourteen fonts | L | built |
  | 3 | `truetype`, `font` -- TrueType parsing, metrics, CID embedding | L | built |
  | 4 | `image` -- pixel buffer, PNG, PDF image XObjects | M | built |
  | 5 | `raster`, `stroker` -- scan-conversion, stroking, glyph rendering | L | built |

  **Why it is shaped like Java 2D, and the three places it is not.** Java 2D's contribution is that
  `Shape` bottoms out in a `PathIterator` of five segment types plus a winding rule, and that
  vocabulary is almost exactly PDF's content stream -- so one backend is a serialiser and the other
  a scan-converter over the same five cases. Not copied: `Paint` there is a pixel producer, so a
  custom paint forces rasterisation (which is why PDF out of Java 2D degrades to embedded bitmaps),
  and the paint set here is closed; `Graphics2D` is a mutable state machine, which fights a language
  whose values are immutable and whose `Mutable*` types are thread-bound, so the model is an
  immutable display list -- one scene renders to PDF *and* an image, and tests assert on structure
  rather than pixels; and `getOutline()` is the right escape hatch for a raster device and the wrong
  default for a document, so text stays a first-class operation.

  Cairo, Skia, Quartz 2D, Direct2D, Canvas 2D, `lyon`/`tiny-skia` and ReportLab each contributed
  something and the README says what. Two worth repeating here: cairo's pattern set is a closed
  six-case enum, which independently confirms the closed-`Paint` decision, while its sixth case is a
  callback pixel producer and its PDF backend rasterises fallback regions for anything it cannot
  express natively -- the no-silent-degradation invariant's failure mode, in a mature library, from
  one open escape hatch. And ReportLab points at what all five phases leave out.

  **What they leave out, and the likely sixth phase: document layout.** Every phase is a drawing
  API, and the motivating use case does not think in coordinates -- it thinks in headings,
  paragraphs, tables and page breaks. ReportLab has a canvas *and* Platypus on top, because reports
  written straight against a canvas become unmaintainable arithmetic. **Designed 2026-10-16** in
  `docs/graphics/phase-6-layout.md` and tracked as F146, with the original reservation kept at the
  top of it: a drawing model nothing has used is the wrong moment to design a layout model over it,
  and the export work will say what it needs. `pdf.render_pages` already takes a list of scenes for
  this. The document's own advice is to write one real export page against `scene` and `pdf` first
  and let what hurts become the requirements.

  **One prerequisite, found by checking:** `math` has no `atan2`, `asin` or `acos`, and arcs, round
  joins and decomposing a matrix all need the first. That is part of phase 1.

- **An interactive debugger: breakpoints, stepping, variable inspection.** Split out of F6 on
  2026-10-05 when the rest of that task closed (see docs/reviews/tasks.md), because it is the one
  part of it that is a project rather than a feature and should not sit behind a checkbox that looks
  almost finished.

  What already exists, and is why this is not urgent: a runtime error prints a stack trace with
  every frame's file, line and column; `--locals` adds each frame's variables and their values under
  it, innermost scope outwards; `--profile` says where the time went; `--coverage` says what never
  ran. Between them they answer "where did it fail", "with what values", "what is slow" and "what is
  untested" without an interactive session, which is most of what a debugger gets used for -- and
  they answer it in a CI run or someone else's bug report, where nothing can attach.

  What stepping would add that those cannot: watching state change *as* it changes, rather than
  seeing it once at the point of failure. That is the real gap, and it matters most for a bug whose
  failure point is far from its cause.

  Two shapes, and the choice is the design pass:
  - A **DAP server** (Debug Adapter Protocol), so VS Code and every other DAP client drives it. The
    LSP in src/lsp.c is the precedent for both the protocol plumbing and the scale -- this is that
    size of job, not smaller.
  - A **terminal REPL** over the interpreter loop: `break file:line`, `step`, `next`, `print x`.
    Much less protocol, no editor integration, and it needs the same core underneath either way.

  The core underneath is the same for both and is the actual work: a breakpoint check on the
  statement path (`exec_stmt` already carries every statement's line, and `--coverage` showed what a
  flag-guarded test there costs, which is nothing measurable), a way to suspend the interpreting
  thread and resume it, and enough of `--locals`' scope walk to answer an inspection request for an
  arbitrary frame rather than only the dying one. Deferred, not rejected: the evidence so far points
  elsewhere, since the LLM benchmark's own finding was that diagnostics -- not step-debugging --
  were what let a model correct itself.

- ~~**Grow the LLM-generation benchmark from 10 tasks to the 50 the review asked for.**~~
  **Done 2026-10-10; what replaces it is variance, not scope.**
  *Done 2026-10-10 (F26 wrote the 50 tasks; F27/F34/F36/F49 ran them).* All 50 tasks exist and have
  now been run at least once: round 1 is **0 of 50** across five independent batches with five
  different habit mixes, and round 2 talked every cold attempt on tasks 11-50 into compiling using
  nothing but the compiler's own output. **What this entry asked for is finished; what replaces it
  is variance, not scope** -- every per-task figure is n=1 (the "two independent fresh model
  instances" below means one per *round*, which is isolation, not replication), so nothing yet
  measures two runs of the same task. The 2026-10-10 wave is also the clearest case for reading the
  attempts rather than the score: it scored 10/10 in round 2 and still contained three real
  diagnostic defects (F50, F51). Also split out
  of F6 on 2026-10-05 rather than closed with it, because 10 tasks is a fifth of the asked-for scope
  and calling that done would misrepresent it. `tests/bench/llm_tasks/` holds the fixed task set
  (**50 tasks plus a README as of 2026-10-10** -- this sentence said "the fixed 10-task set" until
  2026-10-16, contradicting the note above it in its own entry)
  and docs/reviews/llm-benchmark-2026-09-28.md the method -- each task run cold with zero Ziman
  context, then iterated against the compiler alone for up to five fix attempts, two independent
  fresh model instances per task.

  Worth growing because this is the one measurement here with a track record of changing the
  language: its finding -- that diagnostics naming an exact fix let a model self-correct every time,
  while a bare "undefined function" did not -- produced the "did you mean" suggestions, and the
  re-validation caught a real bug in them (fuzzy search offering `hash` for `has`). A 10-task set
  cannot separate a real effect from which error a given run happened to look at first, which is
  exactly what the third task's inconclusive result came down to.

- ~~**Memory is never reclaimed while a program runs (in the default build)
  — fine for a short-lived script, a real problem for the North Star goal
  of an always-on backend.**~~ **The headline stopped being true on 2026-09-29, when the collector
  became the default build; the updates below have said so since, but the headline a reader scans
  did not (struck 2026-10-16).** *Update 2026-09-23: `make gc` builds
  `ziman-gc`, which routes every allocation through the Boehm collector;
  the two plans below (refcounting, per-thread arenas) were not built and
  are kept for their reasoning. See docs/DESIGN.md's "Memory
  reclamation" entry for what shipped, what it measured, and what is left.*
  *Update 2026-09-29: the collector is now the default build and does not
  leak (re-verified), but does not scale with concurrent clients -- both
  plans below are superseded by a corrected design, one independently
  collected heap per thread, with no dependency on any particular
  library's usage pattern (the per-thread-arena plan's mistake). See
  docs/MEMORY.md for the full diagnosis, the reasoning for the correction,
  and the design -- not started, tracked as F2's concurrency half
  (docs/reviews/tasks.md). **Superseded 2026-10-07: that task (F7) is closed, and not by
  this design.** The gc_heap migration was measured and abandoned -- a gc_heap object pays
  *both* collectors, and each half of the fix is the other's prerequisite -- while the
  scaling problem itself was fixed by removing allocation rather than by moving it: four
  sites (binding names, call argument arrays, a scratch array inside the mark phase, and
  loop scopes) took allocation calls on a server workload from 9.96M to 2.57M, after which
  throughput stops falling as clients are added and is flat from 8 to 64. See DESIGN.md's
  "The gc_heap migration, measured and abandoned" and "Three allocation sites, and what
  they did to F7". A reader following the design below is following the abandoned path.*
  *Update 2026-10-08: "does not leak", re-verified twice above, has
  one measured exception, and it is not in the collector.
  `src/iter.c:133` releases a libc `getline` buffer through
  `zgc.h`'s `free`, which is a no-op in this build, so Boehm never
  reclaims a block it did not allocate: 1.27 GB RSS against 28 MB
  under `ziman-nogc` -- the build whose own comment says nothing is
  ever freed is the one that holds (A17, plus three `strndup`
  siblings in `src/lsp.c`). The rule the macro hides is that memory
  libc allocated has to be freed by libc, which
  `src/native_http.c:15-27` already does with `push_macro`. So the
  claim to make is narrower than the one made here: nothing Ziman
  allocates leaks.* Every `Value` payload (`ArrayObj`/`StructObj`/
  `EnumObj`/`ResultObj`, every `string` buffer) is heap-allocated and
  never freed, on purpose — see docs/DESIGN.md's "single-writer/actor
  pattern" entry: *"this interpreter never frees an `ArrayObj`/
  `StructObj`/`EnumObj`/`ResultObj` once built ... the interpreter is a
  short-lived process, so the OS reclaims everything at exit."* That's
  also exactly why sharing a `Value` across a `Channel` needed zero new
  machinery: nothing frees the thing being pointed to, so a value
  crossing threads is just a safe pointer copy. The justification holds
  for `ziman program.zm` today — a CLI invocation that runs to
  completion and exits. It stops holding the moment a program is meant
  to run forever, which is the entire point of "write a slice of the
  health-record backend in Ziman."
  Concretely, not hypothetically: `health-record-backend-ziman`'s
  `repository.save()` calls `push(store.patients, p)`, and arrays are
  immutable — `push` always builds a **brand new backing array**,
  copying every existing patient's `Value` into it. The moment
  `run_patient_actor`'s loop reassigns `store = unwrap(result)`, the
  *previous* array becomes garbage — and is never freed. Across N
  total creates, that's roughly `1+2+...+N` = O(N²) worth of array
  backing buffers accumulated over the process's lifetime, on top of
  every `format`/`concat` call building a response line and every
  character `parse_json` appends one at a time while unescaping a
  string. None of this is specific to any one feature (a `s[i]` string-
  indexing operator, if added, would contribute the exact same way
  `substring` already does) — it's a property of the whole interpreter,
  surfaced by asking what happens once `main.zm`'s current "run a fixed
  handful of operations, then exit" demo shape is replaced by "run
  forever, serving requests, until killed."
  The fix Ziman's own existing design actually points toward is
  **reference counting, not a tracing GC** — and it fits with unusually
  little downside for this language specifically: every `Value` is
  immutable once built and every struct/array/enum literal is
  constructed strictly bottom-up (all of a value's fields must already
  exist before the value itself can be built), so **the value graph can
  never contain a cycle**. That's precisely the situation where
  refcounting has no meaningful weakness compared to a tracing
  collector (no cycle-collector ever needed) — cycles are the classic
  reason production languages avoid *pure* refcounting: Python is the
  textbook example, using refcounting as its primary mechanism but
  bolting on a second, generational cycle-detecting collector
  specifically because a mutable object graph can form a cycle (`a.next
  = b; b.prev = a` keeps both alive forever once nothing else points to
  either). Ziman's value semantics rule that case out by construction
  — nothing is ever mutated after being built, so nothing can ever be
  wired into a cycle — meaning refcounting alone, with no second
  mechanism bolted on, would actually be *sufficient* here in a way
  it isn't for a language with mutable references. The cost is real,
  though: every value-sharing site (`push`/`insert`/`set`, a struct
  literal or `with` copying a field, a channel `send`, an ordinary
  `val`/`var` binding or reassignment) needs an increment, and every
  scope exit or rebinding needs a decrement — a genuinely large,
  cross-cutting change, comparable in scope to the generics work, not
  something to fold into a smaller, unrelated feature.
  **Correction (external review, 2026-09-23; see R9):** the "can never
  contain a cycle" premise above is wrong. `Channel<T>` is mutable in
  place, so `struct Node { peers: Channel<Node> }` with the node sent into
  its own channel is a genuine cycle (reproduced). Pure refcounting needs
  channels to hold weak references or to be left out of the counted graph;
  do not implement against this entry as written.
  Not started, not designed in detail — needs its own pass (representation
  of the refcount itself, where increments/decrements actually need to be
  inserted given the interpreter's current "just copy the `Value` struct"
  sharing style, and whether `Channel<T>`'s cross-thread sharing needs
  atomic increments/decrements or can stay under the channel's existing
  mutex) before implementation starts.
- **A likely-better alternative to the refcounting entry above:
  per-thread arena allocation, freed in one shot when the thread's work
  is done — the same design Erlang/BEAM already uses for exactly this
  problem, and a more natural fit for Ziman's concurrency model than
  refcounting is.** The motivating shape: a real Ziman HTTP backend
  handles each incoming request on its own thread (`spawn`), doing a
  burst of allocation (parsed JSON, intermediate structs, formatted
  response strings) that's entirely garbage the moment the request
  finishes — exactly the pattern arena/region allocators exist for
  (Apache httpd's per-request pool, nginx's memory pools, Rust's
  `typed-arena`). Give each such thread its own arena; every allocation
  it makes comes from that arena; when the thread's function returns,
  free the *entire* arena in one bulk operation. No per-value
  bookkeeping at all — no refcounts to increment/decrement, and (unlike
  the refcounting entry above) no need to lean on the value graph being
  acyclic either, since nothing is being freed value-by-value in the
  first place.
  The one place this breaks if implemented naively: Ziman's actor
  model *is* threads passing values to each other over a `Channel`, and
  today that's a cheap pointer copy specifically because nothing is
  ever freed — see docs/DESIGN.md's "single-writer/actor pattern" entry
  (*"nothing frees the thing being pointed to, so a value crossing
  threads is just a safe pointer copy"*). A request thread that builds
  a `CreatePatientRequest` and sends it to the long-lived actor, which
  stores it in `store.patients` for the rest of the program's life,
  would leave the actor holding a pointer into the request thread's
  arena — a dangling pointer the moment that arena is freed, silent
  memory corruption rather than a leak. Erlang solves this the same way
  it solves the general problem: each process has its own *private*
  heap, and `send` between processes *copies* the message from the
  sender's heap into the receiver's, never shares a pointer across the
  boundary. Ziman would need the same fix — `send`/`receive` deep-copy
  the `Value` across the channel instead of copying the struct/pointer
  — before per-thread arenas are safe to bulk-free. Given values are
  already immutable, this copy is behaviorally invisible (no aliasing
  to preserve), just a real runtime cost paid once per channel
  operation rather than never.
  Not started, not designed in detail — needs its own pass (an arena
  allocator implementation, hooking every allocation site to it instead
  of bare `malloc`, the deep-copy-on-send/receive mechanism and exactly
  what "deep" means for each `Value` variant, and what owns/frees the
  entry module's own top-level allocations, which never run inside a
  `spawn`ed thread at all) before implementation starts. Preferred over
  the refcounting entry above specifically because it needs no per-
  value bookkeeping during normal execution and matches the actor model
  already in place, rather than being a general mechanism bolted in
  beside it — but the two aren't necessarily exclusive (a long-lived
  actor's own heap could still eventually want its own reclamation
  story once *it* accumulates garbage across many processed messages,
  which per-thread arenas alone don't address).
  **Correction (external review, 2026-09-23; see R9):** the granularity
  doesn't match the server that motivates this. `server.zm` is thread-per-
  *connection*, and with keep-alive one thread serves thousands of
  requests, so a per-thread arena frees nothing until the client
  disconnects. The region has to be per *request*, which needs an escape
  rule for state that deliberately outlives one (the buffered `Session`
  carried between requests for pipelining) and for values sent over a
  channel (copy-on-send).
  **Second correction (2026-09-29):** "per request" is itself the wrong
  generalization -- a request is a concept `server.zm` invented, not
  something every Ziman thread has (a `spawn`ed worker consuming a
  `Channel` forever, a batch job, a CLI script have no request to hang a
  reset point on). See docs/MEMORY.md for the corrected design: one
  independently-collected heap per thread, with no notion of "request"
  anywhere in it.
- **[Resolved: `Map`/`Set` are now built-in types on a persistent hash trie, O(log32 n)
  writes -- see docs/DESIGN.md "Map and Set". The analysis below is the history.]**
  **`HashMap`/`HashSet`'s `set`/`add`/`remove` are O(bucket_count), not
  O(1) — a genuine gap next to Java's `HashMap` (O(1) amortized `put`),
  and a trie-shaped bucket structure could close it without touching
  the language at all.** Surfaced comparing `collections.zm`'s
  just-added load-factor growth (see docs/DESIGN.md's "Collections
  module" entry on growth) against Java's `HashMap`: growth fixed
  `get`/`has`'s degradation into a linear scan past ~16 entries, but
  `set`/`remove` stay O(bucket_count) ≈ O(n) *every call*, growing or
  not — the same root cause as the memory-reclamation entry above.
  `set(m.buckets, idx, new_bucket)` must allocate and copy a whole new
  backing array, since Ziman's arrays are immutable value types with no
  in-place mutation anywhere in the language — Java's `HashMap` doesn't
  pay this cost because its bucket array is mutable, so `put` just
  links a node into one bucket's chain in place. This isn't a bug in
  how the module was written; it's the same cost `push`/`set` already
  have on any plain Ziman array, just newly visible because a hash
  table does one per operation instead of one per bulk rebuild.
  The fix doesn't need a language change, though — it needs a different
  *data structure* for the buckets, one built for exactly this
  constraint: a **Hash Array Mapped Trie (HAMT)**, the technique
  Clojure's and Scala's persistent (immutable-by-default) hash maps use
  for the identical reason. Instead of one flat array of `bucket_count`
  slots, indexed by the full hash, a HAMT is a shallow trie of small,
  fixed-arity nodes (32-wide is the usual choice), each level indexed by
  one chunk of the hash's bits. Updating one entry only requires
  allocating and copying *one small node* (O(arity) = O(1)) at each of
  O(log₃₂ n) levels on the path to it — everywhere else in the trie is
  untouched and safely shared by reference, since nothing is ever
  mutated. That turns `set`/`remove` into genuine O(log n) — for any
  realistic `n`, indistinguishable from O(1) in practice (a trie holding
  a billion entries is only 6 levels deep at arity 32) — achieved
  entirely within Ziman's existing "arrays are immutable, copy-on-write"
  semantics. No new interpreter machinery, no refcounting dependency:
  this is pure `.zm` source, the same way the current flat-bucket-array
  version is.
  Real cost, not a small tweak: a HAMT needs a node representation
  (probably a small tagged struct/enum distinguishing an internal node
  from a leaf/collision-list), a bitmap or dense-array scheme for
  "which of the 32 slots at this node are populated" (a naive always-32-
  wide node wastes a lot of space for a sparsely populated trie — real
  HAMT implementations pack this with a 32-bit presence bitmap, which
  needs bit-counting. *Update 2026-09-28: this is no longer an open
  question* -- the `bits` core module (DESIGN.md "A bits module") now
  gives `int_popcount`/`int_and`/`int_or`/`int_shift_left`/... directly, so
  a 32-bit presence bitmap and its popcount are a native operation, not an
  all-arithmetic substitute to invent, same as the `byte`/`uuid`/`number`
  modules once had to work around case-by-case), collision handling when
  two keys hash into the same leaf path all the way down, and a
  hash-splitting scheme (which chunk of `hash(key)`'s bits picks the slot
  at each level). `hashmap_keys`/`hashmap_values`/`hashmap_size` would need
  reimplementing against the new shape too (`size` stops being a trivial
  field read if the trie doesn't track a running count itself, which it
  should). Not started, not designed in detail — needs its own pass (node
  representation, collision strategy, whether `HashSet<T>` shares the same
  trie implementation
  parameterized differently or duplicates it the way `HashMap`/`HashSet`
  already do today) before implementation starts. The current flat-
  bucket-array version stays as the interim implementation either way —
  this is a strict improvement to swap in later, not a correctness fix.
- **Effects visible in function signatures** (which functions do I/O or
  mutate outside their own frame). Real value per DESIGN.md's principles,
  real design cost — needs its own doc before implementation starts. Now
  also entangled with the module system above: an effect story and a
  native-vs-pure-Ziman module boundary are naturally related questions
  (does calling into a native module always count as an effect?).
  *Update 2026-10-10: considered under the AI-first ranking and deliberately
  left here rather than promoted.* The gap is real and easy to show -- a pure
  `tidy(name: string) -> string` and a `tidy_and_log` that writes and deletes
  a file have **identical signatures**, so `--api`, the type checker and a
  reader all see the same thing. And the notation is in the training
  distribution: cold benchmark attempts wrote `/ {IO}`, `! {IO}` and `-> T !
  IO` unprompted. But it is **4 of 30 attempts, all in the first wave and none
  in the two since**, so this is a habit models carry rather than a thing they
  repeatedly fail on -- the AI-first case is weak and the case for it remains
  the design one above. The native-boundary question now has a number against
  it too: `fs` declares 21 natives and `os` 28, so "calling a native is an
  effect" makes nearly everything effectful and the annotation stops
  discriminating, while "it isn't" leaves `fs.write_text` invisible -- failing
  at the exact case that motivates the feature. Propagation is the other cost:
  every caller inherits transitively, and every generic function needs effect
  polymorphism.
- ~~**Modules / multi-file programs.**~~ Implemented (`src/loader.c`):
  - **One file = one module**, identified by its filesystem path — no
    separate `module Foo` declaration to keep in sync with the filename.
  - **`use path/to/mod`** (slash, not dot — a module path is filesystem-
    literal and deliberately never shares a token with `.field` access,
    which already means something else). Resolved **project-relative
    first**, against the directory containing the entry file passed to
    `ziman`, falling back to a bundled core module (see "Bundled core
    modules" below) if no project file by that name exists — `use
    collections` and `use io` both find one this way now, with zero
    project-local files.
  - **Every cross-module reference is qualified — `user.greet(u)`, never
    a bare `greet(u)` that happens to resolve to some import — unless
    explicitly imported by name (`use models/user.greet`, see "Per-name
    imports" below), which is still visible right there in the file's own
    `use` list, not silent.** Same "derivable from the line itself"
    reasoning as `concat`/`str` existing instead of overloading `+`.
    There is no wildcard import form (`use mod.*`). Implementation note:
    a qualified reference looks identical to plain field access (`p.x`)
    until the parser sees what follows the second identifier, so it's
    parsed speculatively and backed out of (restoring position) if it
    doesn't match either `IDENT "("` or an uppercase `TYPE_IDENT "{"` —
    confirmed not to disturb plain field access via the existing struct
    test suite passing unchanged.
  - **Name-collision aliasing reuses `as`** (`use admin/user as
    admin_user`) — new meaning for an existing keyword, never ambiguous
    with cast-`as` since they only appear in different grammatical
    positions.
  - **Exports require `pub`; default is private to the module** — same
    instinct as `val`/`var`: an important property is explicit at the
    declaration, not inferred from whether something outside happens to
    use it. Corollary, confirmed working (regression test
    `multifile_name_isolation`): struct/function names only need to be
    unique *within* their own module, not across the whole program,
    unlike today's single-file rule.
  - **No separate manifest file** (`Cargo.toml`/`package.json`-style) —
    that solves third-party package versioning, which doesn't exist yet
    (already a stated non-goal). The app's dependency graph is just the
    entry file's `use` statements, followed transitively; a module is
    loaded and parsed at most once (a diamond dependency shares one
    instance, confirmed via `multifile_diamond`), and a cycle is a clear
    compile-time error (`multifile_cycle`) instead of infinite recursion.
  - ~~**A native module is `native fn name(params) -> type` with no `{ }`
    body at all.**~~ Fully implemented, registration mechanism included
    (`src/native.c`, `src/native_io.c`): the type checker treats a
    `native fn`'s signature like any other function's, and at call time
    the interpreter looks up a C function pointer registered under the
    function's exact fully-qualified name (`io::read_file`) and calls it
    — falling back to the existing "no registered implementation" error
    only if nothing was ever registered for that name. Every native
    function is a plain C function of one fixed shape regardless of its
    declared Ziman signature: `Value fn(Value *args, int argc, int line,
    int col)` — see docs/DESIGN.md's new entry for why this is
    deliberately *not* a 1:1 mirror of the Ziman signature (inspired by
    JNI, which does the same for a harder reason — bridging C's
    primitives to Java's object model — that Ziman's uniform `Value`
    representation doesn't actually have). `line`/`col` let a native
    function raise a `ziman_error` that reads exactly like any other
    runtime error (e.g. `io.read_file` on a missing path).
  - **Implementation strategy, not a language-semantics decision:** the
    loader rewrites every module's own top-level names, and every
    cross-module reference, to `<module_path>::<name>` and merges
    everything into one flat Module *before* typecheck()/interpret() ever
    run — so neither needed to learn anything about multiple modules at
    all, only the loader did. Known cosmetic side effect: an error
    message or a printed struct shows the qualified name (e.g.
    `models/user::User`) rather than the plain declared one — harmless,
    a little ugly, not worth fixing until it actually bothers someone.
  - ~~**Today's built-in scalar/array machinery stays implicit for
    now.**~~ Half-resolved: **the "always-auto-imported native module"
    idea landed** (`src/prelude.h`/`prelude.c`) — a fixed, small native
    module whose declarations are merged bare into every program, no
    `use` required and never qualified, the one deliberate exception to
    "every cross-module reference is qualified." `parse_int`/
    `parse_float`/`parse_bigint`/`parse_decimal`/`floor`/`ceil`/`round`/
    `math.round`/`concat`/`substring`/`split`/`trim`/`to_upper`/
    `to_lower` moved there — every one of them has exactly one fixed
    signature, so `typecheck.c`/`interpreter.c` needed zero new dispatch
    code for them; the *existing*, fully generic native-fn machinery
    (already built for `io`) handles type-checking and calling them with
    no per-function special-casing at all. Verified as a genuinely
    zero-breaking-change internal restructuring: every existing test
    fixture calling these unqualified still passes unchanged.
    **What's still core, and why it has to stay that way**: `print`,
    `str`, `format`, `len`, `push`/`insert`/`set`, `contains`, `abs`,
    `min`/`max`, `hash` — every one of these needs to work under *one*
    unqualified name across *multiple* types (`abs(5)` and `abs(5.0)`
    and `abs(5n)` and `abs(5.0d)` all share the name `abs`). A native fn
    is exactly one fixed signature per declared name — a *native* has no
    argument-based overloading — so a truly polymorphic-by-name
    builtin can't move to the prelude without either generic native fn
    signatures (`native fn abs<T: Numeric>(x: T) -> T`, which doesn't
    exist and hasn't been designed) or overloading.
    **Updated 2026-10-18:** "Ziman has no argument-based overloading
    anywhere" is what this said, and it stopped being true. F148 gave
    the dispatched builtins `builtin fn` declarations, and the
    collection-pairing pass (F149-F162) grew them to **28 overloaded
    names over 87 declarations** — `len` and `contains` with nine each,
    `concat` and `remove` with four. What the language does not have is
    overloading *available to a declaration*: every one of those sets is
    resolved by a hand-written arm in `typecheck.c`. See "Relaxing the
    no-overloading rule" below, which is the entry this paragraph now
    points at rather than the assumption it used to state. This isn't a remaining "not gotten to it" gap the way the
    type-specific functions were — it's the actual, structural boundary
    of what the prelude mechanism can express.
    **Updated again 2026-10-19: that boundary moved.** Overloading
    available to a declaration shipped (F174-F184), and a `native fn`
    name may now carry several declarations — `remove` and `filter`
    already do. So the second of the two routes this paragraph named is
    no longer missing, and the sentence above describing the structural
    limit now describes a limit that was lifted. What that leaves is a
    *choice* rather than a wall: `abs` and `len` had their hand-written
    arms deleted and resolve from their declarations (F188, F190), but
    both are still `builtin fn` with a core implementation, because
    moving them to `native fn` buys nothing the resolver did not already
    give. `str`, `contains`, `clear` and `snapshot` have reasons of
    their own to stay core, and the reasons are measured, not assumed:
    `contains` carries twelve distinct diagnostics, `clear` and
    `snapshot` carry hints keyed on the type that was rejected, and
    `str` takes expressions that cannot describe themselves. Those are
    recorded under "Relaxing the no-overloading rule" below. The operators themselves
    (`+`/`-`/`==`/etc.) were never a candidate either way, for the same
    "no operator overloading" reason (see docs/DESIGN.md's "Numbers"
    entry) — those stay hardcoded in `eval_binary`/`check_binary`
    regardless of what happens to function-shaped builtins.
- ~~**Generics — in progress, now a five-stage plan.**~~ **All five stages are
  implemented (2026-10-10, F39): this heading outlived them.** Generic structs,
  bounded type parameters, generic enums, cross-module generics and generic
  functions all shipped and are struck through below. The only residue is the
  limit stated in this paragraph -- a generic function body can do nothing with
  an abstract `T` beyond `Eq`/`Hash`/`Ord` -- and extending *that* was the `Row`
  bound, which C18 withdrew on 2026-10-09 after finding it cannot work the way
  `compare` does. Design decided
  (docs/DESIGN.md's "Generics" entry): explicit type arguments everywhere
  (no inference), a new `TY_TYPEVAR` substituted on demand rather than
  monomorphized (`Value` is already runtime-type-erased, so there's no
  per-instantiation runtime representation to generate — only the type
  checker's substitution logic is new). The original "no bounds/
  constraints at all" decision has been narrowed, not reversed: still no
  general trait/interface system, but a single hardcoded `Eq` bound is
  being added specifically because `Map`/`Set` need it (see the
  "Collections module" entry below) — a generic function body still can't
  do anything else with an abstract `T`.
  - ~~**Stage 1: generic structs.**~~ Implemented (docs/SYNTAX.md
    "Generics"): `struct Box<T> { value: T }`, constructed as
    `Box<int> { value: 5 }`; field access, `==`, and `with` all
    substitute correctly through nested generics (`Box<[T]>`,
    `Wrapper<T> { inner: Box<T> }`) and arrays of a generic struct.
    Same-module only, like `enum`. Arity/genericity mismatches
    (`Box { ... }` with no type args, or type args on a non-generic
    struct) are compile-time errors, and a type parameter name colliding
    with a real struct/enum name is rejected at declaration time (it
    would otherwise silently shadow real references to that type inside
    the generic struct's own fields).
  - ~~**Stage 2: bounded type parameters (`<T: Eq>`).**~~ Implemented
    (docs/SYNTAX.md "Generics"): the one hardcoded bound, meaning
    "supports `==`," parses on a struct/enum/function's own type
    parameters (`struct HashMap<K: Eq, V> { ... }`) and is enforced at every
    concrete instantiation — `HashMap<Channel<int>, V>` is a compile-time
    error, since `Channel` doesn't support `==`, checked via the same
    `type_supports_equality` machinery arrays/structs/Result/enum already
    use. A bound name other than `Eq` (`<T: Ord>`) is also a compile-time
    error. Usable inside a generic declaration's own body since Stage 5
    landed (`fn are_equal<T: Eq>(a: T, b: T) -> bool { return a == b }`)
    — see that entry below for what building that half surfaced.
  - ~~**Stage 3: generic enums.**~~ Implemented (docs/SYNTAX.md
    "Generics"): `enum Option<T> { Some(T) None }`, constructed as
    `Option<int>.Some(5)`/`Option<int>.None` — the actual `Option<T>`
    motivation landing. Construction spells out type arguments like a
    generic struct; consumption (`Option.is_Some(a)`/
    `Option.unwrap_Some(a)`) deliberately never does, since the concrete
    instantiation is already fully known from the argument's own value —
    the type checker reads it off the argument's resolved type instead of
    requiring (redundant) restatement, and rejects
    `Option<int>.is_Some(a)` as a compile-time error for writing it
    anyway. `is_X`/`unwrap_X`/`==` all substitute through
    `decl->type_params`/the value's own `type_args` the same way struct
    fields do. A bound (`enum Option<T: Eq> { ... }`) works identically to
    a struct's.
  - ~~**Stage 4: cross-module generics.**~~ Implemented (docs/SYNTAX.md
    "Generics"): `collections.Box<int> { ... }`,
    `collections.Option<int>.Some(5)`, and a qualified type annotation
    (`val x: collections.Option<int>`) all work. This was never a
    generics-specific gap: the qualified-reference parser
    (`mod.Name`/`mod.Type { ... }`) just needed extending to also accept
    `<Args>`, and enum construction/consumption needed a second dot on
    top of that (`mod.EnumName.Variant`) that plain field access and
    module-function calls never did — building this also happened to
    close the pre-existing, generics-independent "cross-module enum
    access" gap noted under the `enum` entry above, and a related latent
    hole where a qualified *type annotation* (`val x: mod.Shape`) only
    ever resolved against a struct, never an enum. This is what actually
    lets a shared `collections` module (below) export anything generic.
  - ~~**Stage 5: generic functions.**~~ Implemented (docs/SYNTAX.md
    "Generics"): `fn identity<T>(x: T) -> T`, called as
    `identity<int>(5)` (cross-module too: `mod.identity<int>(5)`). Uses
    the one-token lookahead heuristic docs/DESIGN.md's "Generics" entry
    describes to disambiguate a generic call's `<` from a real
    comparison, which structs/enums didn't need (an uppercase name can
    never be a real variable, but a generic function's lowercase callee
    genuinely can appear on the left of `<`). This is also the stage
    where the `Eq` bound finally gets *exercised* inside a body, not just
    enforced at instantiation (`fn are_equal<T: Eq>(a: T, b: T) -> bool
    { return a == b }`) — surfaced two real bugs while wiring it up,
    both fixed and covered by regression fixtures:
    - A bounded type parameter used *as a type argument to another
      bounded generic type* in the enclosing declaration's own signature
      (`fn hashmap_get<K: Eq, V>(m: HashMap<K, V>, ...)` -- checking
      `HashMap<K, V>`'s own `K: Eq` bound against the caller's `K`, itself just a bare,
      unsubstituted type variable at that point) needs the same "which
      declaration's type parameters are in scope right now" context a
      function *body* needs for `==`, but during *declaration
      validation*, before any body is checked. Generalized the
      body-only tracking into one shared "type parameters of whichever
      generic declaration is currently being validated" variable, set
      around struct fields', enum variants', and function params/return
      types' well-formedness checks too, not just around a function's
      own body.
    - A generic function's own type parameter referenced *from inside its
      body* in a position other than a param/return type
      (`Option<V>.Some(...)`, a `val` annotation, a cast) was never
      reclassified from a provisional struct/enum reference into a type
      variable, since only a struct/enum *declaration*'s own field/
      variant types went through that step before. Fixed with a new
      parser-side walker (`reclassify_type_params_in_stmt`/`_in_expr`,
      mirroring loader.c's self_qualify_expr/self_qualify_stmt shape)
      that reclassifies every type-bearing spot in a generic function's
      parsed body, run once right after the body itself is parsed.
    Needed to write `hashmap_get`/`hashmap_set`/`hashset_add`/etc. once
    instead of once per concrete instantiation.
- ~~**Collections module (`Box`/`Option`/`HashMap`/`HashSet`) — the
  concrete payoff of the generics work above.**~~ Implemented
  (`lib/core/collections.zm` — see "Bundled core modules" below for why
  it lives there and not under `examples/`): a `collections` module,
  written in ordinary Ziman, no new native/C code, exporting `Box<T>`
  (later removed outright — nothing ever used it beyond being the first
  generic struct ever written, see docs/DESIGN.md's "Bundled core
  modules" entry), `Option<T>` (plus a small `option_unwrap_or<T>`
  helper — later moved into the prelude instead, see the "Prelude"
  entry in docs/DESIGN.md), `HashMap<K: Eq + Hash, V>`, and
  `HashSet<T: Eq + Hash>` (`HashSet`
  needs both bounds too, for the same O(1)-bucket reason as `HashMap`)
  as plain structs/enums plus free functions
  (`hashmap_new`/`hashmap_get`/`hashmap_set`/`hashmap_has`/
  `hashmap_remove`, `hashset_new`/`hashset_add`/`hashset_contains`/
  `hashset_remove`). `HashMap`/`HashSet` use real bucket-array storage
  (separate chaining, `hash(key) % bucket_count`), verified against both
  a small demo (`examples/collections/main.zm`, also
  `tests/lang/multifile_collections`) and a 50-entry stress fixture
  (`multifile_collections_stress`) confirming collisions within a
  bucket chain correctly. Bucket count was originally fixed at creation
  (16, no resizing/rehashing) — later given real load-factor-triggered
  growth instead, plus `hashmap_size`/`hashmap_keys`/`hashmap_values`
  (`hashset_size`/`hashset_items`), once "the hashmap implementation
  isn't great" surfaced the fixed bucket count as a real problem (chains
  degrading lookups to O(n) past ~16 entries) rather than an acceptable
  simplification — see docs/DESIGN.md's "Collections module" entry (the
  one titled about growth, not this one) for the full writeup, including
  why growing doesn't also make set/remove O(1). `Entry`/`buckets`/
  `count` are implementation details Ziman has no per-field privacy to
  actually hide (a caller *can* reach `m.buckets` directly) —
  documented as "don't," not enforced.
- ~~**Bundled core modules — closes the "every project keeps its own
  copy" gap noted above under native modules.**~~ Implemented
  (`src/corelib.h`/`corelib.c`): `collections` moved from
  `examples/collections/collections.zm` to `lib/core/collections.zm`,
  and the Makefile now generates `src/collections_source.h` from it (a
  `#define COLLECTIONS_SOURCE "..."`, one long escaped string literal —
  a `#define` rather than a `static const char *` since the latter
  isn't a constant expression and can't initialize `corelib.c`'s
  `CORE_MODULES` table) at build time, embedding it straight into the
  `ziman` binary. `loader.c`'s `load_module` tries a project-relative
  file first, same as always, and only falls back to
  `corelib_source_for(use_path)` if that file doesn't exist — a project
  file of the same name always wins, so this is strictly a fallback,
  not a shadowing/override mechanism. Deliberately narrower than the
  prelude (prelude.h): a core module is only parsed when a program
  actually `use`s it, and a whole-module `use collections` stays
  qualified (`collections.HashMap`, never a bare `HashMap`) exactly like
  any other module — a bare `HashMap` only becomes possible via a
  separate, later mechanism (per-name imports, below), never as a
  side effect of this one — this is a stdlib, not language-level
  machinery, so the usual `use`/`pub` boundary still applies. Every
  fixture that used to vendor its own copy of `collections.zm`
  (`multifile_collections`, `multifile_collections_stress`,
  `multifile_bigint_hashmap`, `multifile_decimal_hashmap`) had it
  deleted and now resolves through the fallback with no changes to the
  fixture's own code — the clearest evidence the mechanism is a drop-in
  replacement for vendoring, not just a demo of one.
  ~~`io.zm` (the `io` native module's signature file) is the other
  concrete case this mechanism resolves.~~ Done too, the same day: moved
  from `examples/native_io/io.zm` to `lib/core/io.zm`, one line added to
  `corelib.c`'s `CORE_MODULES` table and to the Makefile's
  `CORELIB_MODULES` list (which now generalizes the header-generation
  rule to a pattern rule, `src/%_source.h: lib/core/%.zm`, rather than
  one hardcoded rule per module). The three `native_io_*` test fixtures'
  vendored `io.zm` copies (all byte-identical to the original) were
  deleted the same way `collections.zm`'s vendored copies were, and all
  three still pass unchanged.
- ~~**Per-name imports (`use path.Name [as alias]`) — closes the
  "generic collection code repeats `collections.` *and* `<K, V>` on
  nearly every line" ergonomics gap the bundled `collections` module
  above surfaced.**~~ Implemented (see docs/DESIGN.md's entry for the
  full "why this and not type-argument inference" reasoning):
  `use collections.HashMap` then plain `HashMap<int, string>`, no
  `collections.` prefix — one name per import, no list form. Resolved
  and privacy-checked once at the `use` line (`resolve_named_imports`,
  loader.c), not re-resolved at every call site, and it's a compile-time
  error to import the same local name twice or to collide with
  something the importing module declares itself. An `as` alias's
  casing must match the imported name's own (checked at parse time,
  before the loader even knows what the name resolves to — `HashMap`
  starting uppercase is already enough to know an alias for it must too,
  since Ziman's grammar depends on that invariant regardless of what the
  name turns out to be). Only cuts the module-prefix repetition, not the
  explicit-type-argument one — `hashmap_new<int, string>()` still needs
  its type arguments spelled out; that's the half of the friction this
  was never meant to fix.
- ~~**`bigint`/`decimal` — arbitrary-precision integer and decimal.**~~
  Implemented (docs/SYNTAX.md "Numbers", docs/DESIGN.md's "bigint"/
  "decimal" entry): a third and fourth numeric type. `bigint` is
  sign-magnitude, modeled on Java's `BigInteger`, backed by
  `src/bignum.c`'s own arbitrary-precision arithmetic (add/sub/mul/
  truncating-divmod/compare, base-1e9 limbs), with full `int` parity:
  `+ - * / %`, unary `-`, `< > <= >= == !=`, `as` casts to/from
  `int`/`float`/`decimal` (lossy, saturating/truncating, never silent),
  `str`/`abs`/`min`/`max`/`hash`, `parse_bigint`. A literal needs an
  explicit `n` suffix (`123n`) — bare digits still always mean `int`.
  `decimal` is modeled on Java's `BigDecimal`, built directly on
  `bigint` (`src/decimal.c`: an unscaled magnitude plus a scale), with
  full `float` parity: `+`/`-`/compare align scales first and are exact;
  `*` needs no alignment and is exact; `/` doesn't always terminate, so
  it rounds half-up-away-from-zero to `max(a.scale, b.scale)`; `%` is an
  exact remainder; `==`/`hash` compare/hash numeric value ignoring
  trailing-zero scale differences (`1.0d == 1.00d` is `true`,
  deliberately diverging from Java's well-known `BigDecimal.equals()`
  gotcha where scale participates in `equals()` but not `compareTo()`);
  `math.round(d, scale)` is the `floor`/`ceil`/`round` equivalent (a
  separate name, not an overload — when this was written a native took
  exactly one signature per name; **since F184 it does not**, and
  `round(x: float) -> int` against `math.round(d: decimal, scale:
  int) -> decimal` differ in argument one, so merging them into one
  `round` set is now possible and simply has not been argued for. See
  "Relaxing the no-overloading rule"). A literal needs an explicit `d` suffix (`19.99d`,
  `20d`). Both compose with everything already built: a generic type
  argument (`Box<bigint>`/`Box<decimal>`), an array element, and a
  `HashMap` key/value (`bigint` satisfies both `Eq`/`Hash`) all verified
  working. `make test-bignum`/`make test-decimal` run each engine's own
  correctness suite (factorials up to `100!` cross-checked against the
  published exact value for `bigint`; scale-aligned arithmetic,
  half-up-away-from-zero division/round rounding both directions, and
  a 30-plus-digit multiply/divide round-trip for `decimal`) independent
  of the language-level tests/lang harness.
- ~~**`byte` — a fifth scalar type, motivated by wanting to represent
  raw octets (a UUID's bytes) without overloading `[int]` for it.**~~
  Implemented (docs/SYNTAX.md "Numbers", docs/DESIGN.md's "byte" entry):
  unsigned, 0-255, literal suffix `b` (`200b`), range-checked at parse
  time (`300b` is a compile-time error, not a silent wrap). Deliberately
  *not* a sixth numeric type on the `int`/`float`/`bigint`/`decimal`
  model: no `+ - * / %`, no unary `-`, no `< > <= >=`, no
  `abs`/`min`/`max` — nothing that needs a `byte` needs arithmetic on
  one, and Ziman having no operator overloading means adding those
  would force an arbitrary wrap-vs-saturate answer to a question
  nothing's actually asking. Supports `==`/`!=`, `hash`, `str`/`print`/
  `format`, and a saturating cast to/from `int` only (`300 as byte` is
  `255`, `-5 as byte` is `0`, matching `bignum_to_i64_saturating`'s
  "clamp, don't wrap" precedent; no cast to/from `float`/`bigint`/
  `decimal` — unused code otherwise). Composes with everything generics
  already built at essentially zero marginal cost: `byte` satisfies both
  `Eq` and `Hash` (just another case in the existing scalar-kind
  checks), so `HashMap<byte, V>`, `[byte]` equality, and `contains` on a
  `[byte]` all work with no new machinery. The actual motivating case
  was UUID (below) — this was step one, done in isolation so it could
  be tested (and get its own test fixtures) independent of UUID's own
  design.
- ~~**`random`/`uuid` core modules — the actual motivating case for
  `byte`.**~~ Implemented (docs/SYNTAX.md "Modules", docs/DESIGN.md's
  "random/uuid" entry): two new bundled core modules. `random` is one
  native function, `random.bytes(n: int) -> [byte]`, reading straight
  from `/dev/urandom` — Ziman's first source of genuine
  non-determinism (`clock`, below, is the second). `uuid` is pure Ziman
  on top of it: a `Uuid` struct (`bytes: [byte]`), `uuid_new()`
  generating an RFC 4122 v4 UUID, `uuid.format` formatting the
  canonical dashed-hex string. Sets the version/variant bits with pure
  integer arithmetic (`(b % 16) + 64`, `(b % 64) + 128`) instead of
  bitwise operators, which Ziman has none of (no `& | ^ << >>`) —
  confirms `byte`'s int-cast-but-no-bitwise-ops design was actually
  sufficient for the one thing that needed bit-level control. Also
  confirms a core module can depend on another core module
  (`uuid.zm`'s `use random.bytes`) with zero new loader logic,
  since the corelib fallback triggers on the `use` path string alone,
  never on where the *importing* module's own source came from.
  `Uuid` satisfies `Eq` — usable as a struct field
  compared with `==`, e.g. a `Patient.id: Uuid` replacing the
  `next_id: int` counter `health-record-backend-ziman` used to use —
  (*update 2026-09-23: it is also `Hash` and `Ord` now, so it is a usable
  `HashMap` key; see roadmap R11/R12*). ~~Not wired into
  `health-record-backend-ziman` yet~~ — done the same day:
  `Patient.id` is `uuid.Uuid` now, no more counter.
- ~~**`time`/`clock` core modules — seven `java.time`-shaped types
  (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`,
  `ZoneOffset`, `ZoneId`), closing `health-record-backend-ziman`'s last
  remaining documented gap (a real date type).**~~ Implemented
  (docs/SYNTAX.md "Date and time", docs/DESIGN.md's "time/clock"
  entry). `ZoneOffset` only at first; IANA named zones (`zoneid_of`, read from
  the system's zone files by the `zone` module) came later.
  `Instant`/`Duration` are `{seconds: int, nanos: int}` (matching both
  C's `struct timespec` and `java.time`'s own internal shape) after a
  `bigint`-nanoseconds design was tried and dropped once nanosecond
  precision was specifically wanted: plain `int` seconds have ~292
  *billion* years of range with no `bigint` needed at all, `int`
  nanoseconds-since-epoch would only have ~292. Calendar math
  (`LocalDate`'s epoch-day conversion) is Howard Hinnant's public-domain
  days-from-civil algorithm, translated directly into Ziman since it
  was already written against truncating integer division. One new
  native primitive, `clock.now_raw() -> [int]` (`[seconds, nanos]` from
  one atomic `clock_gettime()` call), the same "native returns a small
  scalar array, pure Ziman gives it a shape" split `random`/`uuid`
  already established. No `<`/`>` on any of the seven types — each gets
  an explicit `_compare(a, b) -> int` instead, same "don't reopen
  Ord/Cmp without a forcing case" posture as everywhere else. ~~Not
  wired into `health-record-backend-ziman` yet~~ — done the same day:
  `Patient.date_of_birth`/`created_at` are `time.LocalDate`/
  `time.Instant` now, closing that project's last documented gap.
  ~~No way to parse a date string back into a `LocalDate`~~ — resolved
  later, once `request.zm`'s own hand-rolled `parse_iso_date` surfaced
  it as a real, concrete need: every type got a `_parse` (the exact
  inverse of its `_to_string`) plus, for the calendar-shaped types and
  `Instant`, a general pattern-based `_format`/`_parse_pattern` pair —
  see docs/DESIGN.md's "time" formatting/parsing entry.
- ~~**`json` core module — a parser/serializer, motivated by
  `health-record-backend-ziman`'s "no JSON, no real request/response
  types" gap.**~~ Implemented (docs/SYNTAX.md "JSON", docs/DESIGN.md's
  "json" entry). Before writing it, verified directly (not assumed)
  that Ziman can represent arbitrary JSON at all, given an old ROADMAP
  note that a self-referential struct can't be constructed: `enum Json
  { JNull JBool(bool) JNumber(float) JString(string) JArray([Json])
  JObject([JField]) }` (`JField { key: string, value: Json }`) compiles
  and runs correctly, including genuinely nested values — the old
  limitation is about a *direct* self-reference with no escape
  (`struct Node { next: Node }`, truly infinite), not recursion through
  an array, which has a finite base case (`[]`). `JNumber` holds a
  `float` (matching virtually every other JSON implementation, not
  `decimal`/`bigint` — nothing needs exact precision for a JSON number
  specifically). `parse_json`/`json_to_string` are a real recursive-
  descent parser and serializer, not string-searching; string escapes
  cover `\" \\ \/ \n \t`, not `\r \b \f`/`\uXXXX` (Ziman's own string
  literal syntax can't write the first three, and `\uXXXX` needs a
  codepoint-to-UTF-8 encoder nothing here has). `json_object_get` is
  the one convenience lookup beyond parse/serialize. `Json` does not
  support `==`/`!=` (array equality needs a scalar element type,
  `Json` itself isn't one) — a real, known gap, left alone. Stops at
  the general engine — a schema-specific parser for one particular
  request shape (`parse_patient_create_request`-style) is expected to
  live in `health-record-backend-ziman` itself, not in this module.
- ~~**`docs/CORELIB.md` — a generated signature reference for the core
  modules, once the surface got big enough that hand-knowing or
  hand-listing them stopped being realistic.**~~
  Implemented (`tools/gen_corelib_reference.sh`, `make
  corelib-reference`; docs/DESIGN.md's "CORELIB.md" entry). Extracts
  every `pub struct`/`pub enum`/`pub fn`/`pub native fn` straight from
  `lib/core/*.zm`'s own source rather than hand-copying them — this
  project had already caught and fixed stale hand-written comments/docs
  repeatedly by this point, so a hand-maintained signature list was
  guaranteed to drift the same way eventually. Confirmed against source
  before considering it done: the generated file's total `pub` line
  count matched `grep -c '^pub ' lib/core/*.zm`'s count exactly. Not part
  of the default `make`/`make all` build (it's a doc, not a build
  input) — an opt-in target, same treatment `test-bignum`/`test-decimal`
  already get.
- **Compilation to bytecode or native code.** v0 is a tree-walking
  interpreter on purpose — revisit only once the language design itself
  has stabilized against real use, not before.

  *Update 2026-10-12 (F82): measured, and the answer is "not yet" on this entry's own criterion
  rather than on appetite.* **The gate here is language stabilisation, not speed**, and it is
  plainly unmet: 31 commits in the two days to 2026-10-12 changed the language or its library --
  `#[deprecated]` gained two declaration kinds and a module form, `ServeOptions` gained fields
  twice, the type checker's expected-type plumbing changed, and the *formatter's output* changed,
  which reformatted 12 files of the reference backend. A bytecode backend is a second consumer of
  every such decision; F66 is the sharpest case, where one call site passing no expected type was a
  six-form bug.

  **"Bytecode or native" is three different projects, and two of them are already answered.**
  *Startup* is **3.5ms** for a trivial program (process, prelude, type check), so there is nothing
  to win and a compile step to lose. *Throughput* is about **19M loop iterations per second** on a
  tree-walker that has already had its easy wins taken (F2 found the GC rather than `strcmp` was
  the cost, dispatch is cached, slots are pre-resolved) -- a bytecode VM buys 2-5x over a *naive*
  walker and less over this one, and no program is waiting on it: the reference backend serves
  about 1,600 rps at concurrency 8. *A deployable artifact* bytecode does not give you at all --
  you would still ship the interpreter plus a blob -- and plain `.zm` modules with no bytecode
  format are an **AI-first feature**, since a model can read the library.

  **Two arguments that look like they favour it and do not.** A faster interpreter would not speed
  the generation benchmark, whose cost is agent time. And it would not speed `make check`: the 957
  fixtures take **25 seconds** of a roughly twelve-minute gate, the rest being two C builds and the
  backend's 40 server-spawning suites. **The cost nobody had priced** is a second execution engine
  against two collector builds, every change having to stay green in all of them.

  **What would reopen it**, so this is a testable "not yet" rather than a standing no: a real
  program where Ziman's speed is the blocker; `make check` becoming interpreter-bound (it is off by
  a factor of about 25); or the language going quiet for a sustained stretch, which is what this
  entry asked for in the first place.
- **First-class functions / closures.** *Update 2026-09-23 (R13): stage 1
  shipped -- function types and references to named top-level functions, no
  capture; see docs/DESIGN.md's function-values entry. What follows is the
  original reasoning, kept for the record; capturing lambdas are still
  open.*
  *Update 2026-10-10 (F39): that "no capture" is out of date, and what is left
  is one specific thing rather than "closures".* **Capture by value shipped:**
  a lambda literal captures a `val` (`fn(n) { n * factor }`), and it may
  outlive the function that built it -- `fn multiplier(by: int) -> fn(int) ->
  int { return fn(n) { n * by } }` works, capturing a parameter. Verified by
  running each case rather than from this entry.
  **Dropped 2026-10-10 (F40): mutable state in a lambda is already both
  expressible and guarded, so there is nothing here worth a language feature.**
  Capture is by value and what gets copied is a *handle*, so a lambda holding a
  `MapBuilder`/`StringBuilder`/`ArrayBuilder` mutates shared state through it --
  a memoising closure with a live cache needs no change (fixture
  `closure_builder_capture`). The unsafe case is a compile error: a lambda that
  captured a builder carries `Type.func_captures_thread_bound`, so `spawn` and
  `send` refuse it even though its written type (`fn()`) says nothing about a
  builder (A14; fixture `diag_lambda_builder_crosses_thread`). And the
  accumulate shape that looks like it needs a mutable capture is what `fold`
  is for, with the accumulator in the signature instead of hidden in a capture.
  So the `var` rule costs a spelling, not a capability -- and it buys something:
  since captures are by value, allowing a `var` would leave `fn(n) { n * factor }`
  ambiguous about whether it sees later writes, while a handle puts shared
  mutable state at its declaration where it is visible.
  *Also corrected: `metrics.zm`'s header gives the capture rule as the reason
  its counters are native, and that overstates it -- the binding reason is that
  a builder cannot cross a `spawn` at all, so thread-shared state needs native
  atomics regardless.*
  **What remains, and it is a parser gap rather than a feature:** a function
  type cannot be written as a *type argument*, so `Channel<fn(int) -> int>` is
  inexpressible -- `f: fn(int) -> int` as a parameter and `val f: fn(int) -> int`
  as an annotation both work. The `send`-side thread-bound guard is therefore
  present but unreachable, written for a case the parser forbids; whoever fixes
  the parser should know that check is already waiting for it.
  **The original framing, kept for the record -- capturing a `var`**, so that a
  lambda could hold mutable state directly: the classic `fn counter() -> fn() -> int { var n = 0
  return fn() { n = n + 1 return n } }` is refused, with a message that says
  why -- captures are by value, so capturing a `var` would silently snapshot
  something the author expects to stay live. That is not academic:
  `lib/core/metrics.zm`'s own header gives it as the reason every counter,
  gauge and histogram in that module is native ("a Ziman lambda cannot mutate
  what it captures, so state shared across threads has to sit behind a handle
  the way `StringBuilder` and `Iter` do"). So the open question is narrower
  and sharper than "closures": should a lambda capture a `var` by reference,
  what owns the cell, and what does that mean across threads -- which is the
  "new runtime representation bundling code + captured environment" this entry
  anticipated, plus the thread-safety question the metrics module already
  answers in C.
  *Separately, and small: `multiplier(3)(5)` does not parse (`expected ')',
  found '('`) -- calling a returned function requires binding it first. A
  parser limitation, independent of capture.*
  Originally: function calls only
  resolve a bare identifier at parse time; no function values, no passing
  a function as an argument. Considered building this before `spawn`
  (whose natural long-run shape would take a closure) and deliberately
  didn't: nothing concrete forces the question yet. The more likely real
  forcing case is `net`/`http` routing (a table of path → handler
  function) once that module exists — and what that needs is probably
  smaller than full closures anyway: a plain reference to an
  already-declared function (closer to a C function pointer than a JS
  closure, since a top-level function has no enclosing scope to capture
  from) would likely suffice for a routing table, without the harder
  closure questions (capture by value or reference, a new runtime
  representation bundling code + captured environment). Worth deciding
  that distinction first, whenever the real need arrives.
- **String interpolation.** Once string concatenation is settled, decide
  whether interpolation is a second way to do the same thing (against
  principle #2) or genuinely a different need.
  *Update 2026-10-10 (F38): measured, and there is no AI-first case for it.*
  **Across all 30 cold benchmark attempts no interpolation form appears even
  once** -- `f"..."` 0/30, `"${x}"` 0/30, `"#{x}"` 0/30. What models reach for
  is an operator for concatenation -- **re-counted 2026-10-10 (F48) as `+`
  6/30 and `++` 8/30, 14 of 30 between them**, not the 16/8/24 first recorded
  here, which had counted `+` tokens rather than string concatenations (`i = i
  + 1` is in most of these files) -- and
  that collision is already handled the way this language says it should be:
  it fails at compile time with the sentence that fixes it, a message several
  benchmark runs called the best in the set. So the question this entry poses
  is unchanged by the evidence and the entry stays open on its own terms.
  Recorded because the opposite was assumed -- the 2026-10-10 priority ranking
  put interpolation in tier 1 on the reasoning that a model writes an
  f-string by reflex, which the data refutes.
- **A real package registry** (npm/crates.io-shaped, not Maven Central-
  shaped). Worth naming the distinction: Java's JARs are *compiled*
  artifacts, but Ziman modules are plain `.zm` source with no bytecode/
  compiled form at all — so npm and crates.io are the closer precedents
  regardless, since both are fundamentally source-distributing registries
  (crates.io hosts source tarballs; cargo compiles locally, same as
  Ziman's interpreter would just parse locally). Firmly sequenced after
  the module system itself is proven out with a real first-party
  standard library (nothing to register or resolve against yet) — and a
  much bigger undertaking than anything in the language itself once it's
  time:
  - **A manifest file** (`ziman.toml`/`ziman.json`-shaped) declaring a
    package's own name, version, and dependencies — exactly the thing
    the module system's design deliberately did *without*, since there
    was no third-party versioning to support yet. This is the point
    where that non-decision gets revisited, not before.
    *Update 2026-09-28:* `internal` visibility (docs/DESIGN.md's
    "`internal` visibility..." entry) now has a coarse, working notion of
    "package" — `same_package` (loader.c) is one boolean comparison,
    "both bundled corelib, or both this project" — specifically designed
    as the one seam a manifest file would redefine later (comparing
    manifest identities instead of that flag) without `internal` itself,
    its enforcement call sites, or any already-written `internal`
    declaration needing to change. Not a manifest file, not dependency
    resolution — just confirms the visibility half of "what package am I
    in" won't need reworking once this is built.
  - **A versioning + resolution scheme.** Leaning toward Cargo's model
    (one resolved dependency graph, a lockfile pinning exact versions)
    over npm's historical nested/duplicate-version tolerance — a single
    unambiguous resolution per build fits "one way to do things" better
    than allowing two versions of the same dependency to coexist.
  - **The registry itself is a real piece of internet infrastructure**,
    not a language feature: hosting, publish/fetch, namespacing,
    availability — comparable in scope to the rest of this project, not
    a small addition to it.
  - **Supply-chain trust is a real, not theoretical, concern** — npm
    specifically has a well-documented history of malicious/compromised
    packages. Whatever gets built should design around this from the
    start (lockfiles pinning exact resolved versions at minimum;
    namespacing and any review/signing story are open questions), not
    bolt it on after an incident.
  - **A concrete first candidate for what should actually move out of the bundled corelib once
    this exists: the `db` drivers, `mysql.zm` and `pg.zm` specifically** (2026-09-29). Each is a
    real wire-protocol implementation end to end (`pg.zm` 850 lines, `mysql.zm` 1,338 —
    "the largest externally-specified surface in the library," per the language review,
    §4.4), already marked experimental ahead of the rest of the core (see F4/docs/DESIGN.md's
    "Experimental modules" entry) precisely because they carry protocol/maintenance risk the
    rest of corelib doesn't. Today every `ziman` binary embeds them regardless of whether a
    given program ever opens a Postgres or MySQL connection, the same "one binary, no install
    step" tradeoff the whole corelib makes ("Bundled core modules" elsewhere in this file) --
    reasonable for `json`/`collections`/`regex`, much less so for two entire SQL wire protocols
    (SCRAM auth, wire-format encode/decode) a typical program never touches. Once packages are
    real, `mysql`/`pg` (and plausibly `sqlite`, for the same reason, though it links a system
    library rather than reimplementing a protocol) become ordinary published packages a project
    opts into with a dependency line, not permanently part of every binary; `db`/`datasource`
    (the driver-agnostic `Connection`/pooling layer) stay in the corelib proper, since every
    driver package would need them. `internal` visibility (docs/DESIGN.md's own entry) already
    treats `db`'s request-plumbing structs as shared *within* today's one bundled-corelib
    package; splitting drivers out means `same_package`'s definition (today "both bundled, or
    both not") has to grow to recognize "this driver package and the corelib it depends on"
    as cooperating packages too, not just "corelib vs. everything else" -- one more concrete
    thing the eventual manifest-based redefinition needs to handle, not a new problem.
- ~~**Attributes (`#[...]`) and a first `derive` on top of them, starting
  with `#[derive(Json)]`.**~~ Implemented (`src/derive.c`, docs/DESIGN.md's
  "Attributes and `#[derive(Json)]`" entry). Grew out of a "should Ziman
  have reflection" conversation, prompted directly by
  `health-record-backend-ziman`'s `patient/response.zm`/`request.zm`
  hand-writing the exact same field-by-field JSON mapping a real
  Jackson-based backend gets from annotations + runtime reflection.
  Rejected true reflection outright (needs a value of unknown type until
  runtime, cutting against every static-typing decision this language
  has made) in favor of *compile-time* derivation — a general `#[...]`
  attribute mechanism, with `derive(Json)` as its first consumer,
  generating both `<type>_to_json` and `<type>_from_json` for a struct
  or enum. Per-field/per-payload conversion follows a priority order
  (custom `_to_json`/`_from_json` first, then a `_to_string`/
  `_from_string` pair, then a plain scalar, then `Option<U>`/`[U]`
  recursion); deserialization is lenient (unknown JSON fields ignored);
  an enum derives an externally-tagged `{"type": ..., "value": ...}`
  shape (Rust serde's default), with `Option<T>` excluded from that
  tagging since it already has its own null/unwrapped handling in the
  per-field rules. Two things this v1 deliberately still leaves open,
  recorded so a future version doesn't rediscover them: field name
  mapping (`snake_case` -> `camelCase`) is fixed and unconfigurable, and
  there's no per-*field* annotation yet (e.g. overriding one field's
  JSON key, or a future `#[derive(Validate)]`-style per-field
  constraint).

- **Database access follow-ups.** `db`/`datasource`, `#[derive(Row)]`, `net`,
  `crypto`, a shared URL parser (now in `db`), and Postgres/MySQL drivers (`pg`/`mysql`)
  exist (docs/DESIGN.md's `db`/`datasource`, `net`, `crypto`/`pg` and `mysql`
  entries); SQLite, Postgres and MySQL are the three backends. Still open: (1) ~~TLS~~ **done for clients** (docs/DESIGN.md's TLS entry) -- ~~still
  open: server-side TLS (`listen_tls`, for an HTTPS server), client
  certificates, ALPN~~ -- **all three now done** (F64, F65);

  *Update 2026-10-11 (F64): two of those three were already or are now done.* **Server-side TLS
  has existed for some time** -- `net.listen_tls`, with its own TLS suite test, and `http`'s own
  docs tell you to hand `serve` a listener from it -- so this list outlived its first item.
  **Client certificates are now done on both sides**: `net.TlsServerOptions` (`client_ca_file`,
  `require_client_cert`, via `net.listen_tls`) makes a listener authenticate its clients,
  `net.TlsClientOptions` (`cert_file`, `key_file`, via `net.start_tls` /
  `net.connect_tls`) presents one, and `net.peer_certificate_subject` is the identity to
  authorize against. Two things the test transcript records because neither is obvious: a client
  whose certificate is refused still gets `Ok` from the handshake and only learns on the first read
  (TLS 1.3 validates the client certificate after its Finished), and requiring a certificate with
  no CA file to check it against is refused at `listen` rather than failing every connection with a
  complaint about the certificate. **Not done: mutual TLS through `http.client`**, which threads a
  lone `ca_file` positionally through six call layers; the right fix is to thread a
  `net.TlsClientOptions` there instead, which is mechanical but wide enough to be its own task.

  *Update 2026-10-11 (F65): ALPN closes this list.* `TlsServerOptions.alpn` and
  `TlsClientOptions.alpn` offer protocol lists and `net.negotiated_protocol` reports what was
  agreed -- the signature the HTTP/2 entry below guessed, down to the name. The listener's order is
  its preference, which is the choice RFC 7301 leaves to the server, and the test pins that by
  having the two sides disagree on purpose. A client that offers ALPN and shares nothing with the
  listener is **refused** with a fatal alert rather than served a protocol it did not ask for. (2) ~~scoped cleanup~~ **done as `defer`** (docs/DESIGN.md's `defer` entry).
  What remains of it: a block form (`defer { ... }`), the deferred call's
  discarded `Result` (a failing cleanup is invisible), fatal errors
  skipping defers, and no true `with_transaction(fn)` (no closures); (3) ~~connection pool~~ **done as v1**
  (docs/DESIGN.md's pool entry) -- still wanted, and deliberately left for
  a better implementation: ~~reconnect and an acquire timeout~~ (done: a
  connection-class failure replaces the connection, `datasource.acquire_timeout`,
  `datasource.discard`, and a `SELECT 1` health check of long-idle connections on borrow,
  `datasource.open_pool_with`), min/max sizing and
  idle timeouts, resetting a connection on release, and real pooling over
  in-memory SQLite (shared-cache); (4)
  ~~**schema management/migrations**~~ (done: the `migrate` module, a
  directory of `V<n>__<name>.sql` files applied once each with checksums, with
  per-engine subdirectories for SQL that differs; no down migrations); (5) per-field `#[derive(Row)]` annotations
  (column-name override, primary key, generated columns) -- the same
  deferred per-field-annotation gap `#[derive(Json)]` documents; (6)
  Postgres gaps: MD5 authentication, prepared-statement reuse, COPY,
  LISTEN/NOTIFY, IPv6 literals in the URL, SASLprep, and decoding native
  `timestamptz`/`date`/`uuid`/`numeric` columns into their Ziman types
  rather than through `Text`; (7) **performance** on big result sets --
  `[byte]` is an array of interpreter values, so a packed byte-buffer
  type would help the driver as much as `net`; (8) MySQL gaps: the RSA
  public-key exchange (so caching_sha2's first login works without TLS),
  the compressed protocol, LOCAL INFILE, client certificates, IPv6
  literals, multiple result sets (only the last is kept), prepared-
  statement reuse, and decoding DECIMAL/DATE/DATETIME/TIME/JSON columns
  into their Ziman types rather than through `Text`.
- **`net` follow-ups.** ~~A packed byte-buffer type (`[byte]` is an array
  of interpreter values -- wasteful for bulk data)~~ **done, and this entry
  had gone stale: `[byte]`, `[bool]`, `[int]` and `[float]` are stored packed
  -- one real byte/byte/int64/double per element rather than an 80-byte boxed
  value (docs/SYNTAX.md "Arrays"). Measured 2026-10-10 rather than trusting
  the doc: two million `[byte]` elements cost 40.9 MB against 540.2 MB for two
  million boxed one-field structs, 13x, so there is nothing left to pack.**
  ~~Unix-domain sockets
  (local Postgres often uses one)~~ **done 2026-10-10: `net.connect_unix(path)`
  and `net.listen_unix(path)`, with the `Socket` behaving identically afterwards.
  The two differences from a TCP listener are reported rather than hidden -- the
  107-byte path limit, and that binding an existing path is refused because a
  stale socket and a live one are indistinguishable from there.** UDP, and a
  connect timeout beyond the OS default, remain. ~~An `http` module (server first, pure Ziman on `net`)~~
  **done as a toolkit** (docs/DESIGN.md's `http` entry); still open there:
  ~~TLS (`listen_tls`), chunked request bodies~~ **done**, HTTP/2 (its own entry below), ~~multipart,
  compression, `Date`/`Server` headers~~, and -- once Ziman has function
  values -- a real `serve(listener, handler)` and route table on top of
  the same matcher.

  *Update 2026-10-11 (F61): three of those four are settled, and only one needed code.* `Date` was
  already on every response, and `Server` is a decided **no** -- the library does not say what it is,
  because that only tells an attacker what to look for, and the backend's `headers_test` asserts the
  absence. Response **compression** is now `server.compressed`, **off unless asked for**
  (`ServeOptions.compress_responses`): compressing a response that holds a secret beside
  attacker-influenced text leaks it through the length (BREACH), and an API whose login answer carries
  a JWT is that shape. 13,200 bytes to 117 on the wire when enabled. **Multipart was left for F62** (below). Adding one
  `ServeOptions` field broke eight call sites, so `serve_options()` now supplies the defaults and
  `with server.serve_options() { .. }` is the way to build them; that was the last such break.

  *Update 2026-10-11 (F62): the fourth is done too, so this list is closed.* `server.multipart(req)`
  reads a `multipart/form-data` body into `Part` values -- bytes, not text, because a part may be a
  file. I argued against building it on the grounds that nothing consumes one yet; Menderes decided
  to build it, and file upload being on the roadmap makes a parser worth having before its first
  caller. `multipart_boundary` is deliberately a separate function so a handler can answer **415**
  for a request that is not multipart at all while every rejection of a body is a **400**, and
  `Part.filename` is documented as the client's claim rather than a path, because it is the one
  field here that invites a traversal. What remains under this entry is HTTP/2 alone.

- ~~**A `Row` bound, generalizing `Eq`/`Hash`/`Ord`'s mechanism to a fourth case**~~ **-- design
  pass done 2026-10-09 (C18), and the answer is no, with F1's decision to add it withdrawn.**
  `Ord`'s mechanism is runtime dispatch on a value's own type and generics are erased, so
  `to_params: fn(E) -> [SqlValue]` fits it and `from_row: fn(Row) -> Result<E, DbError>` cannot:
  `E` is only in the return type, so at the call there is no value to dispatch on. Covering one of
  a derive pair's two functions under a bound named after the pair is worse than covering neither.
  Two further findings, both checked rather than argued: struct literals have no field defaults, so
  nothing could "disappear" from a literal without a second feature; and F1's safety argument is
  already covered by the type checker, which rejects the cross-wiring it worried about. What is
  left is eighteen lines of boilerplate in one file of one sibling repo. Full reasoning, and the
  three things that would reverse it, in docs/DESIGN.md "The `Row` bound, and why `from_row` cannot
  work the way `compare` does". Original entry below, unchanged, since it is what the decision was
  taken against (see
  docs/DESIGN.md "The traits/interfaces stance", F1). `Crud<E, Req, Res>` (E9,
  `health-record-backend-ziman`'s `service/crud.zm`) needs `from_row: fn(Row) ->
  Result<E, DbError>` and `to_params: fn(E) -> [SqlValue]` as explicit function-value
  fields on every literal, because there is no way to say "`E` already has a
  `#[derive(Row)]` pair" as a bound, the way `<T: Ord>` already says "`T` already has
  a `_compare`." A `Row` bound (`fn crud_find_by_id_and_patient_id<E: Row, Req,
  Res>(...)`) would resolve `<snake_type>_from_row`/`<snake_type>_to_params` the same
  way `compare<T: Ord>` already resolves `<snake_type>_compare`
  (`find_compare_function` in the type checker), and the two fields would drop out of
  every `Crud` literal -- nine of them today, growing as more resources migrate.
  Not started, not designed in detail -- needs its own pass: extending the bound
  grammar/checker to a fourth name (`Eq`/`Hash`/`Ord`/`Row`), teaching it a two-
  function signature pair rather than the one-function shape every existing bound
  has, deciding whether `#[derive(Row)]` alone satisfies it or a hand-written pair
  matching the naming convention also counts (`Ord` accepts both today), and a
  fixture proving `Crud<E: Row, Req, Res>` resolves the right pair for two different
  `E`s in the same file -- the exact copy-paste-mismatch class this bound exists to
  rule out at compile time, not just tidy up.

- **`Cell<T>`: one primitive that makes the mutable layer open to library code -- done 2026-10-14 as
  `Mutable<T>` (F108); the worked actor example followed on 2026-10-16 and closed the entry.** Raised 2026-10-13 by
  Menderes, out of planning to use Ziman on other projects -- UI applications among them -- where the
  objection is structural rather than about any one type: *"every time we hit a wall we have to change ziman
  itself to add mutable types instead of implementing one with ziman."* That is correct today, and it is the
  reason to do this; it is not a complaint about the five that exist.

  **What is already open, which narrows the problem a long way.** `#[thread_bound]` is a user-available
  attribute, not an internal marker: a struct written in Ziman carries it and gets the compile-time refusals
  (not a `Channel` element, not a `spawn` argument, not a module-level `val`), and those refusals are
  transitive -- verified through a struct field, an `Option<ArrayBuilder<int>>` and an `[ArrayBuilder<int>]`,
  on both boundaries, with a lambda that captured one refused where it is written. So the safety story a new
  mutable type would need is already there and already generalises. What is closed is exactly one thing:
  **there is no way to make a field change.**

  **And that wall is lower than it looks.** A mutable type is expressible in Ziman *today*, with a
  one-element `ArrayBuilder` standing in for a cell -- a `Counter` struct whose `ab_get`/`ab_set` pair gives
  bump-and-read, copied by value while the state behind the handle is shared, usable from a closure, and
  still refused across a thread. It works; it is simply unnameable and nobody would guess the API. The gap
  is a name and an honest signature, not a capability.

  **The proposal.** One new native generic, `Cell<T>`, `#[thread_bound]`, with `cell_new(value)`,
  `cell_get(c)` and `cell_set(c, value)`. Three functions, one type. Everything open-ended then composes in
  library code rather than in C: a ring buffer is a struct over `MutArray` plus two `Cell<int>`s; an LRU is
  `MutMap` plus `MutArray` plus a cell; a UI signal is a `Cell<T>` plus a `MutArray` of subscribers; a node
  with mutable children is a struct holding a `MutArray<Node>`, and the tracing collector already handles the
  cycles a parent pointer makes. The language stops growing each time a new mutable shape is wanted.

  **What this is not, and should not become: mutable struct fields.** A struct is a value and is copied, so a
  mutable field means mutating a copy and wondering why the original did not change -- the aliasing confusion
  the language has avoided everywhere else, and `with` already covers update-as-a-value. `Cell` keeps the
  distinction visible: the struct stays a value, the cell is plainly a handle. No methods are needed either.

  **A decision this has to make, found while checking the above and worth fixing either way.** A struct
  holding a handle is **comparable and hashable today**, by accident rather than by decision: the handle is
  an `int` field, so `Counter { slot: ab_new<int>() } == Counter { slot: ab_new<int>() }` is `false` (two
  empty builders, unequal), `hash` of one is stable within a run, and such a struct is accepted as a `Map`
  key. That is identity equality wearing value equality's clothes, keyed on a pointer value that means
  nothing across runs -- exactly where a UI framework's memoisation key or node comparison would go wrong.
  `Cell` should be **neither comparable nor hashable**, as `Channel` already is not, so a struct containing
  one is refused as a `Map` key and `==` on it is a type error rather than a surprise; and the four builders
  should be changed to match, which is a small breaking change nothing in this repository or the reference
  backend relies on (checked).

  **Cost, honestly.** A mutable type built in Ziman is a function call per operation through a handle, where
  `MutArray<int>` is a packed buffer -- so the native collections stay as the fast paths and `Cell` is for
  shapes, not for hot loops.

  **Do it when a real program asks.** The same test every other decision here takes: not before something
  concrete wants a mutable type the five do not cover. A UI project is the likely first caller, and the
  cheapest thing to do first is the one named at the end of the mutability discussion -- a worked actor
  example (a counter or small cache owned by a spawned loop, talked to over a `Channel`), which would show
  whether process-lifetime mutable state needs anything new at all.

  *Update 2026-10-13: designed in full the same day, and renamed. The type is `Mutable<T>` and the
  functions are `mutable_new`/`mutable_get`/`mutable_set`, plus a fourth, `mutable_update(c, f)`.
  `Cell` was dropped because the companion decision renaming the builders makes "every `Mutable*`
  type is thread-bound" a rule, and `Cell` would have been a third type outside it after
  `StringBuilder` and `Iter`; `Mutable<T>` is instead the root that `MutableArray`/`MutableMap`/
  `MutableSet` specialise -- which is also what the `MutArray`/`MutMap` spellings above became. The
  equality and hashing decision this entry raised stands and is now joined by two more found while
  checking it: a handle prints a raw address, and `#[derive(Json)]` does not refuse a handle field.
  Everything else here -- the objection, the transitive `#[thread_bound]` story, the refusal of
  mutable struct fields, the cost, and "do it when a real program asks" -- is unchanged. See
  docs/DESIGN.md, "`Mutable<T>`: the root of the family, and why there is no `MutableInt`" and "The
  builders become mutable collections".*

  *Update 2026-10-16: the worked actor example is written --
  `tests/lang/actor_owns_mutable_state.zm` -- and its answer is that
  process-lifetime mutable state needs nothing new. One actor owns a `MutableMap`, a `Mutable<int>`
  and a `MutableString` at once and serves four threads making a hundred requests over one
  `Channel`, with no lock, because one thread is the only thing that touches them; the state is
  created inside the actor, so the `#[thread_bound]` refusals never arise -- they are what makes
  this shape the one that works. Writing it did find one gap, now fixed: `send` gave its value
  argument no type context, so `send(replies, Some(n))` was unwritable even with `replies` declared
  `Channel<Option<int>>`, which is the natural shape for a cache `get`. See docs/DESIGN.md's
  "Cost, and when" for both that and the one subtlety worth copying (each client needs its own
  reply channel).*

  *Update 2026-10-14: built, F108. So "do it when a real program asks" below was not the test it
  ended up taking -- the whole mutable-collection rename (F103/F104) made `Mutable<T>` the root of a
  family rather than a fifth member of one, and at that point it cost three natives and one ordinary
  Ziman function, because `#[thread_bound]`, the non-comparability (F107) and the `Mutable*` naming
  check (F106) all applied to it without a line of new code. The worked actor example named at the
  end of this entry is still not written.*

- ~~**Refactor the `collections` module: seven duplicate spellings, one family split across two
  namespaces, and one name that can never move.**~~ **Done 2026-10-14 (F120): the module is deleted
  and the whole family is `array_*`.** Raised 2026-10-14 by Menderes, out of asking
  whether `collections.sort*` should simply be deleted now that `array_sort*` exists.

  *Update 2026-10-16: raised on 2026-10-14 and overtaken the same day -- F120 stage 3 landed at
  00:17 -- with no note added, so everything below read as an open question that was already
  settled. **And it was settled in the opposite direction to the one this entry weighs.** The entry
  asks "where does the sort family live", concludes the answer needs a trigger (promote
  `is_sorted`/`binary_search`/... once something sorts a `MutableArray` and then searches it), and
  says that until then the seven wrappers should stay and the 167 call sites be left alone. F120
  instead deleted `lib/core/collections.zm` outright and moved the call sites (stage 2), so arrays
  now have the shape `Map` and `Set` always had: one bare `array_*` family and no module. Nothing
  was promoted into the flat global namespace, which is the cost this entry was trying to avoid --
  `array_find`, `array_sort_with`, `array_frequencies` and the rest all carry the prefix, verified
  by running them. Both "hard constraints" dissolved with the module: `collections.join` never
  needed to be able to move, because it became `array_join`. The removal also dragged along five of
  the compiler's own suggestion strings, eight comments and two fixtures, recorded in `65591bf`.
  The reasoning below is kept for why promoting names into a flat namespace is expensive, which is
  still the live argument whenever a new bare name is proposed.*

  **The state.** Seven functions exist under two names: `map`, `filter`, `flat_map`, `reverse`,
  `sort`, `sort_desc` and `sort_with` are bare `array_*` in `lib/prelude.zm` *and* one-line
  `collections` wrappers over them (`pub fn sort_with<T>(items, cmp) { return
  array_sort_with<T>(items, cmp) }`). **167 call sites** across both repositories use the
  `collections` spelling -- `sort` 72, `map` 55, `sort_with` 13, `sort_desc` 9, `filter` 9,
  `reverse` 6, `flat_map` 3 -- while the bare ones had zero until F97 documented them. The other
  24 `collections` functions have no bare counterpart at all.

  **This is the rule working, not a leftover.** F97 promotes a function only when the other side
  already has one: a `map_*`/`set_*` twin, or a `mutable_*` twin. The three bare sorts are exactly
  the three with a mutable twin (`mutable_array_sort`/`_desc`/`_with`). So the duplication is the
  *cost* of the rule rather than an unfinished migration, which is why "just delete the wrappers"
  is not obviously right.

  **Deleting them splits the sort family.** It has eleven members and three are bare: remove the
  wrappers and `array_sort(xs)` is bare while `collections.sort_by_keys(xs, ks)`,
  `sort_indices`, `is_sorted`, `binary_search`, `lower_bound` and `upper_bound` still need the
  import. A reader then has to remember which sort lives where, which is worse than a duplicate
  name that always works.

  **Two hard constraints, both found while looking.** `collections.join(items, sep)` can **never**
  be promoted: the bare `join` is already the *path* join over strings, with different semantics
  entirely, so "everything becomes bare" is not a reachable end state. And promoting the rest would
  take `find`, `first`, `last`, `count`, `any`, `all`, `is_sorted` into a flat global namespace
  where every name added is a name a user can no longer have -- the reason F97 declined to promote
  more than it had to.

  **So the question is not "remove or keep" but where the sort family lives**, and there is a path
  that closes it without a decision: the deferred `is_sorted`/`binary_search`/`lower_bound`/
  `upper_bound` group gets promoted by the existing rule the moment it has mutable twins, and its
  trigger is a program that sorts a `MutableArray` and then searches it (checked 2026-10-14: has
  not fired). After that the split is gone and deleting the seven wrappers is tidying rather than a
  trade. Until then the documented answer stands: write `array_sort_with`, and leave the 167
  existing call sites alone.

  **What a refactor should also look at while it is in there**, since these are the same question:
  whether `collections` should be two modules (the array *algorithms* against the handful of
  map/set helpers like `frequencies`, `index_by` and `group_by` that return collections keyed by
  something); and whether `order_of`, which is private, wants to be the one public primitive the
  sort family is written over, the way `sort_positions` already is for the natives.

  *Update 2026-10-19: both loose ends above are now closed, and the trigger this entry named has
  fired. The first module question went with the module in F120. The deferred
  `is_sorted`/`binary_search`/`lower_bound`/`upper_bound` group -- which this entry says gets
  promoted "the moment it has mutable twins", checked 2026-10-14 as not yet fired -- got them in
  F167, so the split the entry worried about is gone: all four take either world under their bare
  names. And `order_of` **is** the one primitive the sort family is written over, as the last
  paragraph wondered, by subtraction rather than by promotion: F169 deleted
  `array_sort_indices`/`_desc`, the two one-line wrappers that were its only other spelling, after
  F166 left them with no caller anywhere outside `lib/prelude.zm`. It stays `array_`-prefixed and
  undocumented-as-API; nothing was lost, since the wrappers were `order_of(keys, false)` and
  `order_of(keys, true)`.*

- ~~**Interval notation for ranges: `[a..b]`, `(a..b)`, `[a..b)`, `(a..b]`.**~~ **Declined
  2026-10-16 by Menderes, after reviewing the evidence below.** `a..b` stays the one range form, and
  `..=` is not taken either; the reasoning is in the update note under this paragraph. Raised
  2026-10-12 by Menderes. ~~**Undecided, and may well be declined**~~ -- recorded so that both the
  option and its price are on the record rather than rediscovered.

  *Update 2026-10-16: **declined**, and the decision recorded here rather than left open, because an
  entry that reads as pending work has its own cost -- this file twice sent a reader to recommend
  already-finished items on the day this was decided.*

  ***The argument that settled it is one this entry had as a sub-point rather than the whole case:
  the proposal's value and its structural cost are the same forms.*** The pitch is that all four
  combinations become expressible. But the nesting-safe subset is only `[a..b]` and `(a..b)` --
  inclusive-both and exclusive-both -- while the ends anyone actually reaches for are exclusive
  (`a..b`, which exists) and **inclusive-end**, i.e. `a..b]`, a *mixed* form. So the useful half of
  the feature is exactly the half that breaks the delimiter-matching invariant the formatter's
  continuation indent (F72), the TextMate grammar and every editor's bracket matcher rely on. Drop
  the mixed forms to keep that invariant and nothing of value is left to ship.

  **Declining also keeps `[a..b]` in reserve, which is worth more than this feature.** This entry
  listed "slicing landing as `xs[a..b]`" as a *reopen* trigger; it cuts the other way. `xs[0..2]` is
  a clean `expected ']', found '..'` today (verified), slicing exists only as the function
  `slice<T>(a, start, end)`, and subscript slicing is a far more commonly wanted feature -- Python,
  Rust and Go all have it. Spending `[a..b]` on a range *value* forecloses it permanently, so
  keeping it free is a decision in its own right and not a deferral.

  **`..=` is declined too, which is the part that needed a reason rather than a cost.** It is cheap,
  nesting-safe, leaves `[` and `(` alone, matches a real latent habit (1 of 50 cold attempts reached
  for it) and the existing diagnostic already names the concept. Against that: it puts **two
  spellings on the single most common construct in the language**, and the count below is now **260**
  `for ... in a..b` sites in `lib`/`tests`/`examples` (277 counting docs) -- grown from the 235
  recorded here on 2026-10-12. Every counted loop would become a choice between `0..n + 1` and
  `0..=n`, and a model choosing between near-synonyms is the failure mode the AI-first goal exists to
  avoid. No cold run has ever *needed* an inclusive range; the measured failure was four runs
  **avoiding** ranges to dodge an off-by-one, which is hesitation, and hesitation is what F93's
  diagnostics already address. The current message is doing that work well: "a range is written
  `a..b` and stops before `b`, so an inclusive end is written `a..b + 1`".

  **The reopen trigger, tightened.** The one signal that would reopen either half is a run that
  writes an off-by-one `+ 1` and **gets it wrong** -- a defect, not a hesitation. The avoidance
  already measured is explicitly *not* that signal, and should not be read as evidence the feature is
  needed; mistaking the two is what would reopen this on no new information. Slicing landing as
  `xs[a..b]` no longer reopens anything: it is now the reason `[a..b]` stays unspent. The idea is that each end's bracket states its own
  inclusivity, so a reader never has to know a convention: `[` and `]` include the endpoint, `(` and
  `)` exclude it, and all four combinations are expressible. That is a real argument and the same one
  that favours Kotlin's `until` over a bare `..`: self-documenting beats diagnosed.

  **What exists, and why this is not urgent.** `a..b` is exclusive of `b` and always has been;
  `docs/SYNTAX.md` has said so in those words throughout, and F93 put the semantics into three
  diagnostics -- including the stray-semicolon message that 30 of 50 cold attempts hit, which is the
  widest-reach sentence in the compiler. `..=` is refused with a message naming the exclusive end.
  Critically, **the measured failure was confusion, not absence**: across rounds 1-4 and the variance
  wave, four runs avoided ranges rather than risk an off-by-one, and *not one ever needed an inclusive
  range*. So `[a..b)` would respell something that works, and `[a..b]` would add a form nobody has yet
  reached for.

  **What the design pass has to weigh, all of it measured rather than supposed:**
  - **`[` already means two things** -- array literal and index -- and `[a..b]` collides with a third
    the language should keep in reserve: `xs[0..2]` is a clean `expected ']', found '..'` today, and
    is the natural spelling for a slice, which Ziman has only as the function `slice<T>`. Spending
    `[a..b]` on a range *value* forecloses that.
  - **`(` already means grouping and an argument list**, so `(a..b)` reads as a parenthesised
    expression to both a parser and a person.
  - **`[a..b)` and `(a..b]` do not nest.** Delimiter matching is an invariant the formatter's
    continuation indent (F72) and the TextMate grammar both rely on, and every editor's bracket
    matcher assumes it.
  - **The generation cost is the largest of any syntax change yet proposed.** No cold run has ever
    written interval notation; agents write `0..n`, and `..=` appeared in 1 of 50 attempts. Ranges sit
    in the most common construct in the language -- **235 `for ... in a..b` sites** across `lib/`, the
    examples, the benchmark references, the tests and the reference backend -- so this would make the
    training reflex fail on essentially every counted loop. Compare the `variadic` keyword decision,
    whose blast radius is a handful of declarations.
  - **Migration** is those 235 sites plus the grammar, the formatter, the pinned-message fixtures and
    `SYNTAX.md`.

  **Cheaper answers if inclusivity turns out to be the real want**, in order of cost: accept `..=`
  (matches the one measured habit, matches Rust, and F93's message already names the concept, so the
  change is small), or a word -- `a to b` -- if self-documentation is the goal. Both leave `[` and `(`
  alone.

  **Reopen on any of:** a run that writes an off-by-one `+ 1` and gets it wrong; a run that reaches for
  an inclusive end and cannot express it; or slicing landing as `xs[a..b]`, which would settle
  `[a..b]`'s fate by taking the syntax. Absent one of those, the F93 diagnostics are the cheaper half
  of this idea and are already shipped.

- **Locale support: a `Locale` and the functions that would have to take one.** Raised 2026-10-23
  ("we may need Locale support"), which is the trigger F44 had been waiting for -- that entry
  sequenced indexed placeholders "behind a decision about localisation that nobody has asked for".
  Somebody has now asked.

  **Nothing is designed yet. What this entry is for is the surface**, because the cost of locale
  support is not one module -- it is that a dozen existing functions currently answer a
  locale-independent question and would have to grow a locale-dependent one beside it. Measured on
  2026-10-23 rather than assumed:

  > **(Closed 2026-10-23.)** `to_upper(s, l)` / `to_lower(s, l)`, with the one-argument form using
  > `locale.default()`. `towupper_l` gives *simple* case mapping, so `ß` still does not become `SS`
  > -- one code point in, one out is the POSIX interface's shape. The Turkish dotted/dotless `i`,
  > which is the mapping that actually bites, is correct.

  **1. Case mapping is ASCII-only, and silently so.** `to_upper("héllo straße İ")` is
  `HéLLO STRAßE İ` -- the `é` unchanged, the `ß` unchanged where German wants `SS`, the dotted
  capital `İ` unchanged where Turkish wants a dotless lowercase. `SYNTAX.md` says "ASCII only" and
  the implementation agrees; what is missing is any way to ask for more.

  > **(Closed 2026-10-23.)** `equals_ignore_case(a, b, l)`. The trap this item named is gone:
  > `equals_ignore_case("héllo", "HÉLLO", en)` is true. The ASCII form is unchanged and is what
  > protocol code wants -- 58 sites in `lib/` now pass `locale.root()` to say so.

  **2. `equals_ignore_case` is ASCII-only too, which is the sharpest trap of the four.**
  `equals_ignore_case("hello", "HELLO")` is true and `equals_ignore_case("héllo", "HÉLLO")` is
  **false**. A caller testing user input against a constant gets the right answer for English and
  the wrong one for everything else, with no error and no hint.

  > **(Closed 2026-10-23, and this item's own prediction held exactly.)** `compare(a, b, l)` over
  > `strcoll_l`, a *separate* comparator reached with `sort_with` -- it could not replace
  > `compare<T: Ord>`, for the reason given below. German sorts `ärger` beside `apple` and Swedish
  > after `Zulu`, measured. `compare` is a builtin, so its arm in `src/typecheck.c` runs before an
  > overload set would be consulted; the three-argument form is checked there and dispatched in
  > `src/interpreter.c`.

  **3. String order is code points, which is nobody's collation.**
  `sort(["zebra", "ärger", "apple", "Zulu"])` is `[Zulu, apple, zebra, ärger]`: capitals before
  lowercase because ASCII puts them there, and `ärger` *after* `zebra` because U+00E4 > U+007A.
  German collation puts `ärger` next to `apple`, Swedish puts it after `z` -- so the current answer
  is not a default that happens to suit one locale, it is one that suits none. This is the hardest
  piece: `compare<T: Ord>` is a single total order used by `sort`, `Map` and `Set`, and a
  locale-aware order cannot replace it (a `Map`'s key order must not depend on a locale) -- it has
  to be a *separate* comparator passed to `sort_with`.

  > **(Partly closed 2026-10-23.)** `number.number_format_for(l)` reads the host's decimal point
  > and grouping separator, so `1234567.89` renders `1.234.567,89` under `tr_TR` and
  > `1 234 567,89` under `sv_SE` (and ungrouped under `ar_SA`, which groups with nothing).
  > **`str(x)` is deliberately not locale-aware and will not become so** -- it is what a wire
  > format, a SQL value and a PDF object number are written with. What is still open is the parsing
  > half: `parse_float` accepts neither `1234,56` nor `1,234.56`, which is right for a wire format
  > and wrong for a form field, and a locale-aware `parse` has no home yet.

  **4. Numbers are the C locale.** `str(1234567.89)` is `1234567.89`: no grouping separator, and no
  way to ask for one. `parse_float` rejects both `1234,56` and `1,234.56`, which is correct for a
  wire format and wrong for a form field.

  **5. Dates have no names at all.** `time.zm`'s own header: "there is no locale, month name or
  weekday support". `datetime_format_of` takes a pattern, so the hole is shaped like a pattern
  letter that does not exist rather than a missing module.

  > **(Half closed on 2026-10-23, then closed the same day.)** `MMM`/`MMMM` landed first in English
  > only, which is what this item's shape predicted -- filling the pattern letters was the easy
  > half. Then `EEE`/`EEEE` joined them and all four read the host's locale data through
  > `locale.month_name` / `locale.day_name`, so `EEEE, dd MMMM yyyy` renders
  > `Cuma, 23 Ekim 2026` under `tr_TR` and `金曜日, 23 10月 2026` under `ja_JP`. A compiled pattern
  > stays locale-free and the locale goes to the call that renders.
  >
  > **What this item did not cover and is still absent:** era, quarter and week-of-year fields,
  > 12-hour time (`hh`/`a`), and a localised *ordering* of the fields -- the formatter renders the
  > pattern it is given, so a caller still writes `dd MMMM yyyy` or `MMMM dd, yyyy` themselves.
  > That last one is the part a real CLDR-backed formatter would answer and POSIX cannot.

  **6. No message catalogue, and `format_dynamic` -- the function F44 kept as the landing site for
  one -- was deleted on 2026-10-22** as unused. That is the one piece this entry makes *more*
  expensive than F44 assumed: a catalogue needs a runtime template, `format` refuses a computed
  first argument by design, and the function that bridged that is gone. It was 40 lines over a
  scanner `format` and `printf` still share, so restoring it is cheap -- but it has to be restored,
  and indexed placeholders (`{0}`, `{1}`), which F44 costed, would go into it.

  **Two Unicode questions sit underneath all of this and are not locale-dependent, so they could be
  settled first and separately.** `len` counts code points as of 2026-10-23, not graphemes: a flag
  or a skin-tone emoji counts as several. And nothing normalises -- precomposed `é` (U+00E9) has
  `len` 1, decomposed `e` + U+0301 has `len` 2, and `==` says they differ, so two strings that
  render identically are unequal. Any locale story that compares user input needs an answer to the
  second one; a `Locale` does not help with it.

  **The shape to decide, not decided here.** Whether a `Locale` is an explicit argument
  (`to_upper(s, locale)`), an ambient value, or a module (`locale.to_upper`); whether the data is
  bundled (CLDR is tens of megabytes and this interpreter embeds its whole standard library as C
  string constants) or read from the host (ICU, or the platform's own); and whether the first slice is just **case
  mapping plus month and weekday names**, which covers most of what a backend actually renders, with
  collation and number formatting deferred.

  > **Correction (2026-10-23, same day):** this entry first said a host library would be "a
  > dependency this project has otherwise avoided". That is false and was not checked before being
  > written. `make/c.mk` links `-lsqlite3 -lssl -lcrypto -lcrypt -lz` unconditionally, so there are
  > already four, none of them optional; ICU would be a fifth of a kind already accepted rather than
  > a first of a new kind. On this machine it is present (icu-uc 77.1) with 31.9 MB of data in
  > `libicudata`, which is the real figure to weigh -- against bundling, where the comparison is to
  > the interpreter embedding its whole standard library as C string constants today.

  **Not started. The next step is a decision about data, not code**: bundled, host-provided, or a
  deliberately tiny table for a handful of languages. Everything above is reachable from either
  answer, and the measurement is here so the next pass does not have to re-take it.

## Explicitly out of scope for now (see DESIGN.md "Non-goals")

- Tree-native/structural source of truth — Ziman's source of truth is
  text, on purpose, to keep the project bounded.
- A package manager or build system beyond the Makefile — see the "real
  package registry" entry above for the shape this would eventually take
  and why it's sequenced so far out.

- **`TreeMap<K, V>` / `TreeSet<T>` (sorted collections) -- postponed.** `Map`/`Set` iterate in
  insertion order; a sorted map would add ordered iteration, `first`/`last`, floor/ceiling
  lookups and range queries (e.g. records by date range), keyed by `Ord` instead of `Hash`.
  The natural design is a persistent balanced tree (red-black or AVL, path-copying, O(log n)
  writes, values stay immutable), as its own built-in type with the same surface (`set`,
  `insert`, `remove`, `contains`, `len`, `for`) plus the ordered operations. Not started:
  `sort(map_keys(m))` covers "give me it sorted" today, and nothing needs range queries yet.
  Revisit when a feature does.
- **`Map`/`Set` helper functions.** Done: `map_get_or`, `map_merge`, `map_filter`, `map_remove_all`,
  `map_from_arrays`, `set_union`, `set_intersect`, `set_difference`, `set_is_subset`, `to_set`,
  `set_insert_all`, `set_filter`, and now `map_map_values`, `set_map`, `map_entries` (a `MapEntry<K, V>`
  struct instead of a tuple type), `collections.frequencies` and `collections.index_by` (docs/SYNTAX.md "Maps
  and sets"). Nothing open here; `TreeMap`/`TreeSet` stay postponed (above).

- **Lazy iteration -- first step done:** `Iter<T>` and `fs.lines(path)`; `for` walks an `Iter` one item at a
  time and releases it on every way out (docs/SYNTAX.md "For loops", docs/DESIGN.md "Lazy iteration").
  Also done: `fs.chunks`, `fs.walk`, `datasource.rows` (SQLite, Postgres and MySQL cursors), `ndjson.records`,
  `csv.records`, `net.lines`, `client.stream`, and `iter_from` for sources written in Ziman. Still to do: the
  stream pipeline below on top. Reading the source of a pipeline lazily needs no pipe operator; chaining stages does.
- **Lazy streams (a Java-`Stream`-style pipeline) -- decided direction, not started.** Wanted: a
  lazy `Stream<T>` where intermediate steps only record what to do and nothing is computed until a
  terminal operation runs, so a pipeline can short-circuit (`first`, `any`), work on large or
  infinite sources (`iterate`, `generate`, `range`) and avoid intermediate arrays. Sketch:
  - **The type:** a built-in `Stream<T>` holding a source plus a chain of steps (filter, map,
    take, skip, ...), consumed once by a terminal step. Sources: an array, a `Set`, a `Map`'s
    keys/values/entries, `range(a, b)`, `iterate(seed, f)`.
  - **Intermediate ops (lazy):** `filter`, `map`, `flat_map`, `take`, `skip`, `take_while`,
    `distinct`, `sorted`, `peek`. **Terminals (run the pipeline):** `to_array`, `to_set`,
    `count`, `sum`, `min`/`max`, `fold`, `first`, `any`/`all`/`find`, `for_each`, `join`,
    `group_by` (a `Map<K, [T]>`), `to_map(key_fn, value_fn)`.
  - **What has to exist first / be decided:**
    - Type inference for the higher-order builtins: `map`'s result type comes from the lambda's
      inferred return, as `map_get`/the Map helpers already infer from their arguments. No
      general generic-call inference; these stay builtins.
    - Fluent syntax, so a pipeline reads left to right: method-call sugar (`x.f(a)` = `f(x, a)`)
      or a pipe (`x |> f(a)`). Undecided; method sugar suits both LLM output and existing
      `module.function` reading, but must resolve a local named like a module.
    - `Stream<T>` is single-use: consuming it twice is a compile or runtime error. Decide which.
    - `sorted`/`distinct` are barrier operations (they must see every item), which is fine but
      means they are not lazy per item.
  - **Not planned:** parallel streams, and primitive-specialised streams (`IntStream`); Ziman's
    packed scalar arrays already cover the memory side.
  - Until then, the eager array helpers in `collections` (`sort`, `map`, `filter`, `fold`, `any`, ...)
    are the stopgap.

- **A manual for the bundled libraries -- first version done** (`docs/lib/`, generated by `tools/gen_lib_manual.py`
  from each module's header comment and per-function comments; `make corelib-reference` regenerates it, `make
  lib-manual-check` fails on drift; runnable examples in `docs/lib/examples/<module>_example.zm` are run by `make
  test`). Examples exist for every module except `pg` and `mysql` (they need a live server); the
  still need one, and the header comments of `mysql`/`pg` are protocol notes more than a guide. Original plan: docs/CORELIB.md lists every `pub` signature (generated),
  and SYNTAX.md has a section per module, but there is no place that says how to *use* a module: what it is for,
  a worked example per function group, the error cases and how to handle them, and how the modules fit together.
  Wanted: one page per module (`docs/lib/<name>.md`) plus an index, each with a purpose paragraph, a runnable
  example, the functions grouped by task with their behaviour and errors spelled out, and the known limits.
  - **Kept true by tests:** the examples should be real programs run by the suite (as `readme_example.zm` is), or
    generated from fixtures, so the manual cannot drift from the code.
  - **Comments as the source:** each `pub` item's own comment becomes its manual entry, so the per-file review
    of `lib/` (file comment separate from function comments, as now done for `clock.zm`) feeds this directly.
    `tools/gen_corelib_reference.sh` could extract those comments next to the signatures.
  - **Order of work:** finish the per-file review of `lib/`; write the index and the shared conventions (errors
    as `Result`, naming, `use` forms); then a page per module, starting with the ones people reach for first
    (`json`, `http`/`httpclient`, `db`/`datasource`, `time`, `fs`, `collections`, then the format modules).

- **Platform support -- Linux only so far; nothing else has been tried.** The interpreter, the natives and
  the test scripts were written and run on Linux only, and the docs said nothing about platforms until the
  README gained a "Platforms" note. In order, cheapest first:
  - **macOS and the BSDs:** the code uses POSIX interfaces (`pthread`, BSD sockets, `poll`, `dirent`,
    `lstat`, `getline`, `clock_gettime`, `signal`), so a build probably works with small fixes (a
    `_GNU_SOURCE`-only function here and there, `-lm`/`-lpthread` differences, the vendored collector's
    platform flags). The first step is a CI job that builds and runs `make test` on macOS, so the README can
    say what is actually true.
    *Update 2026-10-08: a cheaper first step exists, and the C review named it -- **build with clang here**,
    which is not installed and so was the one gap that review could not close. A second front end is the
    cheapest way to find remaining UB in 34,000 lines, and it tests the same thing a macOS job does without
    the job: `cc` on macOS *is* clang. That review's survey of the POSIX and glibc dependencies found "would
    not compile elsewhere" to be the dominant category and **no** case of "would compile and be wrong" --
    `mallopt` is behind `__GLIBC__`, the `/proc` and cgroup reads in `stack.c:144-157` fall back for
    "unreadable (not Linux)", and `getline`/`strndup`/`pthread_attr_setstacksize` are POSIX rather than glibc-
    only. The two places where another compiler could be right and this one wrong are both filed: the signed
    `<<` at `native_bits.c:96`, which works only by a documented GCC extension (A21), and the `double` punning
    in `native_metrics.c` (A19), which is what clang is most likely to surface.*
  - **Windows through a POSIX layer (MSYS2 or Cygwin):** build there and record what breaks. Expect the
    `Makefile` and the bash test scripts to work as they are and the socket and signal code to need attention.
  - **A native Windows build:** a thin portability layer in C for threads (Windows threads, or a pthreads
    port), sockets (Winsock: startup, `closesocket`, `WSAPoll`), directory access and the shutdown handler,
    plus a build that does not need `make` and bash, and TLS/zlib/SQLite from a package manager. A few days of
    work, and worth doing only if Windows users appear; WSL and containers cover them until then.
  - **Zone data everywhere:** `zone` needs the system's zone files, which Windows and minimal containers
    lack. A compact copy of the IANA rules bundled in the binary would be the fallback (100 to 400 KB, and it
    has to be refreshed when the rules change). `UTC` and fixed offsets already work without any files.

- **A Ziman website -- not started; when there is one, it should carry the library map.** The manual
  (`docs/lib/`) is generated Markdown that reads fine on GitHub, but a site would want the module pages
  rendered, the runnable examples, and the interactive dependency map. `tools/gen_lib_map.py` already writes
  that map as one self-contained HTML page (layers, colours by folder, click a module to trace what it uses
  and what uses it; light and dark themes), read from the `use` lines so it cannot drift. It is not built by
  `make corelib-reference` and is not committed, so a site build would run it.

- **Content types on routes -- typed endpoints and `[byte]` bodies done, two follow-ups.** `mime` (media types: parse, write,
  wildcard matching, type by file extension) is in; `http` has `content_type`, `accepts` (wildcard ranges; `q=0`
  refuses), 406/415, and request and response bodies as `[byte]`; `server` has typed endpoints (`Endpoint`, `Decoder`,
  `Encoder` built with `decoder`/`encoder` over bytes or `text_decoder`/`text_encoder` over text, `handler`, `path_handler`,
  `body_handler` in `public_` and `secure_` forms, `Denied`, `dispatch`), where a handler takes the decoded request and
  returns the resource or the application's own error, and the library checks `Content-Type` (415), negotiates `Accept`
  (406), decodes (400) and encodes with the declared status. The health-record backend runs on it. ~~Still to do: ranking by
  `q` weights (today the first encoder the client allows wins, in the endpoint's own order)~~; and ~~encoders a derive emits~~
  **done 2026-10-10 (F43): `server.json_decoder`, `json_encoder` and `json_list_encoder` turn a
  `#[derive(Json)]` pair into the `Decoder`/`Encoder` an endpoint wants, one call per direction.
  Promoted out of the reference backend rather than invented -- it had written the same three
  itself, and its own comment says why: "every endpoint below used to spell these out, 105 times
  between them", 105 places for the media type or the JSON writing to drift apart. Any second JSON
  service would have written them again, which is what makes them the library's job.** The original
  note: encoders a derive emits
  (`#[derive(Json)]` already gives the function; an `Encoder` for JSON, YAML or CSV from one declaration would shorten the
  table).

  *Update 2026-10-11 (F63): `q` ranking is done, so both follow-ups under this entry are closed.*
  `http.quality(headers, media)` gives the weight as a number, `http.best_media` picks the best of
  what a handler can write, and the typed endpoints negotiate with it -- so an endpoint offering
  `text/plain` and `text/csv` answers `Accept: text/csv, text/plain;q=0.5` with **CSV**, where the
  endpoint's own listing order used to decide regardless of what the client asked. That order now
  breaks ties only, which keeps it as the server's preference among equals. The second half was not
  on the entry at all: the **most specific** matching range decides (`type/subtype`, `type/*`,
  `*/*`), per RFC 9110, so `*/*, text/*;q=0` is a refusal rather than a wildcard rescue -- and that
  rule *subsumes* the exact-match `q=0` case the old two-pass code handled specially, which is why
  removing it regresses behaviour that shipped before the ranking existed. Streaming bodies (an `InputStream`/`OutputStream` analogue: a request body read in blocks, a response written
  as it is produced) are not planned until an endpoint needs them.

- **HTTP/2 -- not started; worth doing when something needs to serve browsers or gRPC directly.** `server` and `client`
  speak HTTP/1.1 (and 1.0), `server` in pure Ziman on `net`, over TLS from the system OpenSSL. Behind a reverse
  proxy that terminates HTTP/2 and talks HTTP/1.1 upstream (the usual deployment, and the health-record backend's) nothing is
  missing, so this waits for a direct-exposure or gRPC need. What it takes, in order:
  1. ~~**ALPN in `net`.** The server side offers `h2` and `http/1.1` on `listen_tls` (`SSL_CTX_set_alpn_select_cb`) and the
     accepted socket says which was chosen (something like `net.negotiated_protocol(sock) -> Option<string>`); the client side
     offers a list on `net.connect_tls`. Small, and useful on its own.~~ **Done 2026-10-11 (F65)**, and this entry guessed
     its own answer: `net.negotiated_protocol(sock) -> Option<string>` is the name. The lists are
     `TlsServerOptions.alpn` and `TlsClientOptions.alpn` rather than parameters on `listen_tls` and `net.connect_tls`,
     since those two keep their signatures and the options structs are where new TLS settings now go. So **step 1 of HTTP/2
     is no longer in the way of step 2** -- what remains here is framing and HPACK. Cleartext HTTP/2 would be
     prior-knowledge only: the `Upgrade: h2c` route was dropped from the standard (RFC 9113) and is not planned.
  2. **Framing and HPACK, and the bit operations they need.** A 9-byte frame header (24-bit length, type, flags, 31-bit stream
     id), `DATA`/`HEADERS`/`CONTINUATION`/`SETTINGS`/`PING`/`GOAWAY`/`RST_STREAM`/`WINDOW_UPDATE` (`PRIORITY` read and
     ignored), and HPACK: the static and dynamic tables, prefix-coded integers and Huffman decoding. All of it is bit work.
     *Update 2026-09-28: the first of the three options below is now done independently of HTTP/2* -- the `bits` core
     module (`byte_and`/`int_and`/`bigint_and`/... plus `shift_left`/`shift_right`/`popcount`, see DESIGN.md "A bits
     module") opens exactly this to everyone, not just this feature. Remaining choice for HPACK/framing specifically:
     HPACK as natives in C (encode and decode over `[byte]` and header arrays, roughly 500 lines) with framing left in
     Ziman using the now-existing `bits` primitives; or linking nghttp2 (a new system dependency and a much larger surface
     than the rest of the natives, in exchange for a conformant implementation). The first keeps the current "system
     libraries plus one vendored collector" footprint.
  3. **A server that multiplexes.** `serve` is a thread per connection with a slot channel bounding them. HTTP/2 carries many
     concurrent streams on one connection, so a connection gets a reader thread that parses frames and hands each finished
     request to a handler thread for that stream, and every write goes through one writer (an actor thread reached over a
     channel, which `channel_close`, `receive_timeout` and `select2` now make workable). `Request`/`Response` stay as they are,
     so `Endpoint`, `dispatch` and every handler are untouched, and `SETTINGS_MAX_CONCURRENT_STREAMS` reuses the slot idea per
     connection. Per-stream and per-connection flow-control windows are new state to keep; a body is still buffered whole
     (up to 1 MiB), so `DATA` frames just accumulate. Response trailers, which gRPC needs and HTTP/1.1 chunked responses never
     had here, are a separate addition.
  4. **The hardening again, plus HTTP/2's own.** Everything D4 added applies per stream (the bounded head and body, the
     whole-request deadline, refusing a header value with a control character), and HTTP/2 brings its own floods to cap:
     rapid reset (`RST_STREAM` after `HEADERS`, CVE-2023-44487), `CONTINUATION` floods, `SETTINGS` and `PING` floods, an HPACK
     dynamic table sized by the peer, and the header list size. These are the reason to be careful about this being a
     conformant implementation and not a quick one: the h2spec suite is the reference to run it against.
  5. **The client.** `client` would negotiate `h2` over TLS and multiplex requests to one host on one connection; its
     idle-connection pool (`net.idle_put`/`idle_take`, keyed by host, one request per connection) is HTTP/1-shaped and would
     need a per-connection stream table instead.
  Not planned: server push (removed by the browsers that made it worth having), the priority tree (replaced by RFC 9218), and
  HTTP/3 (QUIC is its own UDP transport, not a framing layer on `net`'s TCP sockets, and OpenSSL only gained QUIC in 3.5). Testing has two halves: curl (`--http2`) against the server, and a
  frame-level test client written in Ziman, which doubles as the start of the client half.

- **`ziman test` and a `test` assertion module -- done; a project-root marker -- not started.** `#[test]` on a
  zero-parameter function (or `#[test(cases: NAME)]` on a one-parameter one, run once per element of `NAME`'s
  array -- `NAME` a val or a zero-parameter function, in the same file) marks it for `ziman test file.zm
  [file2.zm ...]` to run, each inside its own per-thread runtime-error boundary so one failing test doesn't stop
  the run; a `Result<unit, E>` return lets a test body `try` a fallible call. `lib/core/test.zm` is the small
  `test` module: `assert`, `assert_eq`/`assert_ne`, `assert_lt`/`assert_le`/`assert_gt`/`assert_ge`,
  `assert_some`/`assert_none`, `assert_ok`/`assert_err`, all built on `unwrap` (no new interpreter primitive
  needed for the assertions themselves). What's still not started: a project root marker (a `ziman.toml`, or
  just an empty file) whose directory is where `use` paths start instead of the entry file's own directory, so a
  test file under a separate `test/` tree could `use` a module under `src/` by the same path a sibling file
  would. Not yet needed in practice -- `use` already resolves relative to the entry file, which is enough for a
  test file that lives anywhere and `use`s its subject by a relative path (see
  `health-record-backend-ziman`'s `src/test/ziman/`, reserved for exactly this, alongside `src/main/ziman/`) --
  but a marker would still let the language server, `ziman --check` and `ziman --fmt` find a project's root
  without hunting for `main.zm`.
  *Update 2026-10-10 (F33): done, as a search path rather than a new root.* `ziman.toml` marks a
  project's root and its directory joins the `use` search path, tried after the importing file's own
  directory and before `ZIMAN_MODULE_PATH`. **Deliberately not a second resolution scheme:** this
  entry was written before `ZIMAN_MODULE_PATH` existed, and that variable already solves the
  test-tree case, so making the marker change the *root* would have been two ways to do one thing.
  One mechanism, two ways to point it at a directory -- a variable for a harness, a file in the tree
  for everyone else. Its contents are not read; the name is taken now so a later manifest lands in
  the file that already marks the root. The search stops after eight levels, so a stray marker in a
  home directory cannot claim every file beneath it. `tests/run_module_path.sh` (in `make check`)
  covers it, and is a script rather than a fixture because the entry file has to sit *below* the
  root, which the fixture runner's `name/main.zm` layout cannot express.

- **`format`/`str` follow-ups -- done (D2); one idea left.** `str`/`print`/`format` display a `float` as the
  shortest round-trip text; `format` has `{:.N}` precision (float and decimal) and `{{`/`}}` escapes;
  `format_dynamic(template, args)` is `format` for a runtime template; float exponent literals (`1e3`,
  `1.5e-10`) lex; there is a `math` module and `math.pow`/`math.floor`/`math.ceil`. Not done, and not
  needed yet: indexed placeholders (`{0}`, `{1}`) so a translation can reorder its arguments -- the case that
  would justify them is a message catalog whose languages put the pieces in different orders; `format_dynamic`
  would take them first.
  *Checked 2026-10-10 (F44) and still not needed, with the trigger further off than this entry
  implies.* **No message catalogue exists** in this library or in the reference backend -- the only
  mentions of one are `format_dynamic`'s own doc comment and `time.zm` saying it has **no locale,
  month-name or weekday support at all**. So the justifying case is not merely absent: the
  groundwork for it is absent too, and indexed placeholders would be among the *last* pieces an
  i18n story needed rather than the first. **And there is no AI-first case either:** `{0}`-style
  placeholders appear in **0 of 30** cold benchmark attempts. Sequenced behind a decision about
  localisation that nobody has asked for; `format_dynamic` remains where they would land.
  *(2026-10-22: `format_dynamic` was deleted, so they have nowhere to land and would arrive with
  it. This entry's own finding is why that was cheap -- the justifying case was absent and so was
  its groundwork -- and it is also why the deletion is reversible: the function was 40 lines over a
  scanner that `format` and `printf` still share.)*

  *(2026-10-23: localisation was asked for -- "we may need Locale support" -- so the trigger this
  entry sequenced behind has fired. See "Locale support" under "Larger, needs its own design pass"
  for the measured surface; indexed placeholders are item 6 there, and restoring `format_dynamic`
  is a precondition for them rather than a separate question.)*

- **`serve`: per-client connection cap -- done (2026-10-07).** `header_timeout_ms` (E7) frees a slot from a client that dawdles within seconds, but a
  flood from one address can still fill all `max_connections` slots. A cap per client address (`max_connections_per_client`, default 0 = off, since
  behind a reverse proxy every connection has the proxy's address) needs `net.peer_address(sock)` (`getpeername`) and a counter shared by every
  connection thread -- a native handle holding a mutex-guarded table, as `StringBuilder` and `Iter` hold theirs. Until then a firewall or the proxy in front does it.
  *Done 2026-10-07, as `ServeOptions.max_connections_per_client` (0 = off, the default, for the reason above). Both things this entry was waiting
  for had landed with B10: `net.peer_address(sock)`, and the "native handle holding a mutex-guarded table" it described, which `server.rate_allow`
  already was. The cap needed a different counter from the throttle even so -- a rate limiter counts events in a window and never counts down,
  while a connection cap goes up on accept and down on close -- so `conn_acquire`/`conn_release` are their own table. Decided in the accept loop
  before a slot is taken and before a thread exists, since a cap is only worth having if refusing costs less than admitting, and a refusal closes
  the socket without a reply (answering a flooding client from the accept loop would let one slow read stall every other accept). On a full table a
  connection is admitted and untracked, which is the one failure here that loses nothing already promised: refusing would deny service because a
  table is busy, and evicting a live count would silently raise someone's cap.*

- **health-record-backend-ziman: per-IP login throttle -- done (2026-10-07, B10).**
  E5 added the per-account lockout (`app.security.max-login-attempts`/`max-mfa-attempts`); B10 added the per-IP half the dev review also asked
  for (D item 12, S3). Both halves of what this entry said it needed landed with it: `net.peer_address(sock)` now exists, and the
  forwarded-for question is answered by `server.forwarded_for(req, trusted_hops)`, which counts proxies you own from the right of
  `X-Forwarded-For` and is never read unless configured -- so a client that is not behind your proxy cannot pick its own identity.
  `app.security.max-auth-attempts-per-address`/`auth-attempt-window-seconds` (100 per 60s, measured: 20 broke three of the backend's own
  suites) throttle `/api/auth/*` in `routes.route`, before any handler runs, because registration's cost begins before it knows who is asking.

- **health-record-backend-ziman: migrate the Python test suite to Ziman -- not started, revisit once Ziman is stable enough.**
  `src/test/other/*_test.py` (about 800 checks) is Python for two reasons, not just convenience, and both would need to still hold after a
  move: Ziman had no test framework at all when the suite was written (the language-design review's "no `assert`, no test runner" finding,
  since addressed for new code by `ziman test`/`lib/core/test.zm`, but never retrofitted here), and several checks are deliberately verified
  against an *independent* implementation in a different language (a foreign HMAC over the JWT, a hand-written TOTP/HOTP) so that a bug in
  `lib/core/crypto.zm`/`otp.zm` cannot validate itself by checking its own output. A rewrite in Ziman would need to keep that independence
  somehow (a second, deliberately separate implementation of HMAC/TOTP inside the same suite, or leaving just those checks in Python even
  after the rest moves), and would need `ziman test` to drive a real subprocess about as easily as Python's `subprocess`/`urllib.request`/
  `sqlite3` do today: spawn `serve.zm`, wait on `/health`, make real HTTP requests, and inspect the SQLite/Postgres rows directly afterward.
  Not worth doing while the interpreter and its library are both still moving targets -- the point of the exercise is confidence in Ziman,
  and a self-hosted test suite is weaker evidence of that than an external one until Ziman itself needs less looking-over.

- **Metrics and telemetry -- items 1 and 2 done (2026-10-07, F10); 3 to 5 not started.** Ziman has `log`, `clock` and
  threads but no way to count things across threads: a lambda cannot mutate what it captures, so shared state has to
  live in a native handle, as `StringBuilder` and `Iter` do. Planned, in order:
  1. **A `metrics` core module**: `counter`, `gauge` and `histogram` (fixed buckets), each with an optional
     `Map<string, string>` of labels, thread-safe, and `metrics.render()` writing the Prometheus text format. A cap
     on the distinct label sets per metric (documented), because a label that carries a user id would otherwise
     grow without bound.
  2. **`server` records its own metrics**: requests by method, route pattern and status, request latency, open
     connections, bytes in and out, and serves them from a `/metrics` endpoint an application mounts with one line.
     `dispatch` already knows the matched pattern, which is the low-cardinality label to use (never the raw path).
  3. ~~**Runtime metrics through the same module**: GC heap size and collections, live threads, channel depths.~~
     **Done 2026-10-10 (F32), except channel depths.** `metrics.record_runtime()` samples four
     gauges -- `ziman_gc_heap_bytes`, `ziman_gc_collections`, `ziman_threads`,
     `ziman_open_descriptors` -- and `server.metrics_response` calls it, so a scrape carries them
     without every application remembering to ask, the way Prometheus' own client libraries ship
     process and GC collectors. Gauges rather than counters even for the monotonic collection count,
     because a counter in this module is something *we* add to and these are read out of the
     runtime. The two builds answer differently and the difference is documented rather than hidden:
     the heap is the whole process's mapped heap under the default build and the *calling thread's*
     reserved bytes under `nogc`, where the heap is per thread, and the collection count is -1 there
     because that build keeps none (-1 rather than 0, which would read as "none have happened").
     **Channel depths are deliberately left out:** a useful depth needs per-channel identity, which
     means a label per channel, and an unbounded label set is exactly what item 1's 128-series cap
     exists to prevent -- `channel_len` already answers for a channel you hold.
  4. ~~**Trace context**~~ **-- the propagation half done 2026-10-10 (F42); the OTLP exporter remains
     the deferred part.** `http.traceparent_of(req)` reads the incoming trace or restarts it,
     `http.traceparent_header_for(p)` is the child header to send onward (same trace, new span,
     sampling inherited), and `p.trace_id` goes to `log.with_request_id` so every line of a request
     carries it. The four rules worth enforcing are: all-zero trace or span ids invalid though
     well-formed hex, version `ff` reserved, an upper-case field rejected rather than lower-cased,
     and undefined flag bits ignored -- while a *future* version parses when the rest does, since
     refusing one breaks against a future sender for nothing. Originally:
     **Trace context**: W3C `traceparent` read by `server`, passed on by `client`, and written into `log` lines, so a
     request can be followed across services; then, if wanted, an OpenTelemetry (OTLP over HTTP/JSON) exporter on top
     of `client` and `json`, which needs a background thread to batch.
  5. ~~**`log`**: an optional JSON line format carrying a request id.~~ **Done 2026-10-10 (F41):**
     `log.logger(..., json: true)` writes one JSON object per line, and `with_request_id` stamps a
     logger the way `with_prefix` does, so a handler sets it once per request. Level lowercased and
     prefix trimmed, since the text format's padding is presentation; both `prefix` and
     `request_id` omitted when empty rather than `null`. The investigation note below is what
     shaped it.
     *Investigated 2026-10-10, not yet built -- the one design decision it needs is settled, so the
     next attempt should not re-derive it.* **Escape through the `json` module, not by hand.**
     `log`'s existing `single_line` escapes control bytes as `\xNN`, which is not valid JSON, and a
     hand-rolled JSON escaper in `log.zm` would reproduce exactly what A8 fixed: invalid UTF-8
     flowing into a document that other parsers reject. `json.json_to_string(Json.JString(s))`
     quotes and escapes correctly (verified: `he said \"hi\"`, `a\nb\tc`) and substitutes U+FFFD for
     invalid sequences. There is no import cycle -- `json` imports only `number.float_to_text` --
     and the cost is that every program using `log` then loads `json`, which is the right trade
     against emitting malformed JSON. Remaining shape: a `json: bool` on `Logger` set through
     `logger(...)`'s `Option` parameters like `timestamps`, a request id carried the way `prefix`
     is (with a `with_request_id` returning a copy, mirroring `with_prefix`), the id omitted rather
     than `null` when absent, and `prefix` as its own field rather than glued to the message, since
     a field is the point of a structured line.
  Out of scope: an agent or collector of our own; the module only exposes numbers for one to scrape.
  *Update 2026-10-07 (F10): 1 and 2 are done. `lib/core/metrics.zm` over `src/native_metrics.c` has counters, gauges and histograms with
  `Map<string, string>` labels, a documented 128-series cap per metric whose drops are themselves rendered, and `render()` in the Prometheus
  text format; `server` records requests by method, matched pattern and status, a latency histogram, open connections and bytes in and out,
  with `server.metrics_handler` to mount `/metrics` in one line. Two measurements shaped it and are worth knowing before extending it: a mutex
  per metric made throughput **fall** as threads were added, so an existing series is updated lock-free with a compare-exchange on the double's
  bits; and building a label key cost ~4.5us against ~0.25us for the update, so `metrics.series(...)` resolves a label set once into a handle
  (20x faster). Items 3 (runtime metrics), 4 (trace context) and 5 (JSON log lines) remain.*
  *Update 2026-10-08: the lock-free update that measurement chose has a race -- the compare-exchange seeds itself with a plain non-atomic
  load (`src/native_metrics.c:91`), reported twice by TSan under eight threads -- and the same three functions pun a `double` through a
  pointer cast in a build carrying no `-fno-strict-aliasing`. One fix closes both (A19). The measurement that chose the design stands; its
  implementation is what needs the two lines. Also worth knowing before trusting any clean concurrency measurement of the server:
  `server.metrics` is off by default, so a load test that does not turn it on never executes this code at all -- which is exactly how a
  clean 6,037-line TSan run of the backend missed the race.*

- **Accounts and tokens for backends -- password hashing and JWT done; the request pipeline next.** `crypto` has bcrypt
  (through libxcrypt, so `$2a$`/`$2b$` hashes written by other systems verify) and Argon2id (OpenSSL), with
  `password_hash`, `password_verify` and `password_needs_rehash`; `jwt` signs and verifies HS256/384/512 tokens. The
  health-record backend registers and logs in with them. Still to do, in order: the authenticate step in `server` is done (`secure_*` builders: `who` runs first and hands `run` its caller); still to do are
  ~~interceptors for CORS~~ (**done** -- see below), ~~request logging as an interceptor~~ (**done 2026-10-11, F71, and not as an interceptor**: `ServeOptions.on_request` is a field holding one function value, called once per finished exchange with a `RequestLog` of method, target, status, `duration_ms`, `response_bytes` and `peer`. A wrapper around `dispatch` -- what "interceptor" meant here -- would have to return a closure and allocate one per request, which is the same reason the CORS pair is two plain functions rather than a wrapper (F59); and a wrapper could not see what `serve` knows anyway: the peer, the bytes actually written after `compressed` had its say, and when the read finished. `log_requests` stays as the zero-config default line, and the two are independent), an `otp` module (HOTP/TOTP and base32) for MFA; OAuth2 login
  (Google, Microsoft) on `client`; and RS256/ES256 for tokens verified with a public key, which needs public-key
  operations in `crypto`.

  *Update 2026-10-11 (F59): CORS is done, and not in the shape this entry asked for.* `server` offers
  `cors_policy`/`cors_preflight`/`cors_apply` -- two functions sitting either side of the dispatch rather than
  one wrapper around it, because a wrapper would have to return a closure and allocate one per request. The
  work left in it was not the shape but the proving: the library's CORS said it was "generalised from the
  reference backend's own `route_cors`" while the backend went on running its own copy, so nothing exercised
  the library version. Migrating the backend onto it (52 lines out, 16 in) found **`Vary: Origin` missing from
  the whole CORS section** -- and `http_server_cors`, which prints every header of every case, had recorded
  that omission as its expected output. A test that locks a mistake in, not one that misses it. Fixed, and the
  backend now keeps the library exercised. Request logging as an interceptor is still open;
  `ServeOptions.log_requests` covers the server-level case.
