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
  docs/SYNTAX.md "Builtins". Still missing: `replace`, and a general
  `index_of`/`find` (deferred — see the `index_of` entry below on why it
  needs `Option<T>` or a Result-shaped decision first). Numeric builtins
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
  - **Unbounded queue.** `send` never blocks in v0 — no backpressure/
    capacity limit, a real production concern (a runaway producer could
    exhaust memory) but not a correctness one. Revisit if a real program
    needs bounded backpressure.
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
  Generics themselves are the still-missing other half, tracked as its
  own design pass below (parser syntax for `<T>` on `struct`/`enum`/`fn`
  declarations, and a monomorphization-or-erasure runtime strategy) — in
  progress, staged struct → enum → function; generic structs are done
  (see the "Larger, needs its own design pass" entry below for the
  current state).

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
  duplicate it -- revisit if that reads badly.* Still open: MessagePack/CBOR (binary; bytes have
  no `Json` form), XML (needs an attribute/text convention), YAML anchors/aliases, TOML dates as
  a real type, CSV nesting beyond one level.

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
  - **No `?`-style propagation operator.** Chaining several fallible
    calls means repeating `if is_ok(...) { ... } else { return
    err(...) }` at each step — real, Go-style verbosity, accepted for
    now rather than designing a shorthand speculatively.
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
  - **`Option<T>`** (for the still-separate optional/nullable-type gap
    above) would very likely use this exact same trick — not designed,
    but the precedent now exists.
  - **An empty array literal isn't inferred as an ordinary enum variant
    constructor's argument**, even though a struct literal field gets
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
  - **`match`/pattern matching.** The predicate-plus-unwrap style above
    is the only way to consume a variant in v0 — same choice `Result`
    already made. A real `match` expression (exhaustiveness checking,
    binding syntax) is its own design pass, deliberately not attempted
    alongside the enum type itself.
- **A real test harness beyond tests/run_tests.sh's stdout/substring
  matching** — fine for now, will need line/col assertions once
  diagnostics get richer.
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
  guess -- none an edit-distance-close name). Fixtures: `tests/fixtures/diag_let_keyword`,
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
  `#![deprecated]` is refused with a message saying why, since what it should warn about is still
  undecided. Still open: `#[deprecated]` on a `struct`/`enum`/`val` (rejected with a clear message
  today rather than silently ignored, since nothing reads it there); whether a deprecation should
  ever be a warning by default (still needs "warnings/notes as a separate severity") or a hard error
  under a "deny" policy. The original entry follows.

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
     not only maps. Three cheaper attempts at this are already ruled out by measurement -- see
     docs/DESIGN.md's "MapBuilder vs CPython dict" entry before trying another.
  3. **`MapNode` has no capacity field**, so every insert `realloc`s the node to exact fit and
     `memmove`s to open a slot: a node filling to 32 slots does ~32 reallocs where `dict` amortises
     by doubling. Growing capacity geometrically (4/8/16/32) is contained to `mapobj.c` and is the
     insert-side fix for the 10x.
  4. **Every `mapb_*`/`setb_*` call validates its handle through `handleset_has`**, a hash probe paid
     per operation. Small, but it is on literally every builder call, and for `len` -- 108ns against
     the immutable `Map`'s 89ns for the same field read -- it is the entire cost.

  Needs nothing: `len` is already at parity, and the immutable `Map`'s `remove` at 128x is the worst
  row but is inherent to being persistent (path copy plus node collapse plus a fresh `MapObj` per
  removal) -- `MapBuilder.remove` at 4x is the one to use in a loop, which is what the builders are
  for. Do not start with (3) or a flat open-addressing table for the builder: twice in one day the
  structurally obvious change measured worse than doing nothing (the `Type` union, and three separate
  attempts at (2)), so measure each step.

## Larger, needs its own design pass

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

- **Grow the LLM-generation benchmark from 10 tasks to the 50 the review asked for.** Also split out
  of F6 on 2026-10-05 rather than closed with it, because 10 tasks is a fifth of the asked-for scope
  and calling that done would misrepresent it. `tests/bench/llm_tasks/` holds the fixed 10-task set
  and docs/reviews/llm-benchmark-2026-09-28.md the method -- each task run cold with zero Ziman
  context, then iterated against the compiler alone for up to five fix attempts, two independent
  fresh model instances per task.

  Worth growing because this is the one measurement here with a track record of changing the
  language: its finding -- that diagnostics naming an exact fix let a model self-correct every time,
  while a bare "undefined function" did not -- produced the "did you mean" suggestions, and the
  re-validation caught a real bug in them (fuzzy search offering `hash` for `has`). A 10-task set
  cannot separate a real effect from which error a given run happened to look at first, which is
  exactly what the third task's inconclusive result came down to.

- **Memory is never reclaimed while a program runs (in the default build)
  — fine for a short-lived script, a real problem for the North Star goal
  of an always-on backend.** *Update 2026-09-23: `make gc` builds
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
    `decimal_round`/`concat`/`substring`/`split`/`trim`/`to_upper`/
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
    is exactly one fixed signature per declared name — Ziman has no
    argument-based overloading anywhere — so a truly polymorphic-by-name
    builtin can't move to the prelude without either generic native fn
    signatures (`native fn abs<T: Numeric>(x: T) -> T`, which doesn't
    exist and hasn't been designed) or overloading (deliberately not
    wanted). This isn't a remaining "not gotten to it" gap the way the
    type-specific functions were — it's the actual, structural boundary
    of what the prelude mechanism can express. The operators themselves
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
  `tests/fixtures/multifile_collections`) and a 50-entry stress fixture
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
  `decimal_round(d, scale)` is the `floor`/`ceil`/`round` equivalent (a
  separate name, not an overload — Ziman has no argument-based
  overloading). A literal needs an explicit `d` suffix (`19.99d`,
  `20d`). Both compose with everything already built: a generic type
  argument (`Box<bigint>`/`Box<decimal>`), an array element, and a
  `HashMap` key/value (`bigint` satisfies both `Eq`/`Hash`) all verified
  working. `make test-bignum`/`make test-decimal` run each engine's own
  correctness suite (factorials up to `100!` cross-checked against the
  published exact value for `bigint`; scale-aligned arithmetic,
  half-up-away-from-zero division/round rounding both directions, and
  a 30-plus-digit multiply/divide round-trip for `decimal`) independent
  of the language-level tests/fixtures harness.
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
  native function, `random_bytes(n: int) -> [byte]`, reading straight
  from `/dev/urandom` — Ziman's first source of genuine
  non-determinism (`clock`, below, is the second). `uuid` is pure Ziman
  on top of it: a `Uuid` struct (`bytes: [byte]`), `uuid_new()`
  generating an RFC 4122 v4 UUID, `uuid_to_string` formatting the
  canonical dashed-hex string. Sets the version/variant bits with pure
  integer arithmetic (`(b % 16) + 64`, `(b % 64) + 128`) instead of
  bitwise operators, which Ziman has none of (no `& | ^ << >>`) —
  confirms `byte`'s int-cast-but-no-bitwise-ops design was actually
  sufficient for the one thing that needed bit-level control. Also
  confirms a core module can depend on another core module
  (`uuid.zm`'s `use random.random_bytes`) with zero new loader logic,
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
  is `+` (16/30) and `++` (8/30) for concatenation, 24 of 30 between them, and
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
  entries); SQLite, Postgres and MySQL are the three backends. Still open: (1) ~~TLS~~ **done for clients** (docs/DESIGN.md's TLS entry) -- still
  open: server-side TLS (`listen_tls`, for an HTTPS server), client
  certificates, ALPN; (2) ~~scoped cleanup~~ **done as `defer`** (docs/DESIGN.md's `defer` entry).
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
  ~~TLS (`listen_tls`), chunked request bodies~~ **done**, HTTP/2 (its own entry below), multipart,
  compression, `Date`/`Server` headers, and -- once Ziman has function
  values -- a real `serve(listener, handler)` and route table on top of
  the same matcher.

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
  `map_from_arrays`, `set_union`, `set_intersect`, `set_difference`, `set_is_subset`, `set_from_array`,
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
  (406), decodes (400) and encodes with the declared status. The health-record backend runs on it. Still to do: ranking by
  `q` weights (today the first encoder the client allows wins, in the endpoint's own order); and ~~encoders a derive emits~~
  **done 2026-10-10 (F43): `server.json_decoder`, `json_encoder` and `json_list_encoder` turn a
  `#[derive(Json)]` pair into the `Decoder`/`Encoder` an endpoint wants, one call per direction.
  Promoted out of the reference backend rather than invented -- it had written the same three
  itself, and its own comment says why: "every endpoint below used to spell these out, 105 times
  between them", 105 places for the media type or the JSON writing to drift apart. Any second JSON
  service would have written them again, which is what makes them the library's job.** The original
  note: encoders a derive emits
  (`#[derive(Json)]` already gives the function; an `Encoder` for JSON, YAML or CSV from one declaration would shorten the
  table). Streaming bodies (an `InputStream`/`OutputStream` analogue: a request body read in blocks, a response written
  as it is produced) are not planned until an endpoint needs them.

- **HTTP/2 -- not started; worth doing when something needs to serve browsers or gRPC directly.** `server` and `client`
  speak HTTP/1.1 (and 1.0), `server` in pure Ziman on `net`, over TLS from the system OpenSSL, with no ALPN. Behind a reverse
  proxy that terminates HTTP/2 and talks HTTP/1.1 upstream (the usual deployment, and the health-record backend's) nothing is
  missing, so this waits for a direct-exposure or gRPC need. What it takes, in order:
  1. **ALPN in `net`.** The server side offers `h2` and `http/1.1` on `listen_tls` (`SSL_CTX_set_alpn_select_cb`) and the
     accepted socket says which was chosen (something like `net.negotiated_protocol(sock) -> Option<string>`); the client side
     offers a list on `connect_tls_with`. Small, and useful on its own. Cleartext HTTP/2 would be prior-knowledge only: the
     `Upgrade: h2c` route was dropped from the standard (RFC 9113) and is not planned.
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
  `1.5e-10`) lex; there is a `math` module and `decimal_pow`/`decimal_floor`/`decimal_ceil`. Not done, and not
  needed yet: indexed placeholders (`{0}`, `{1}`) so a translation can reorder its arguments -- the case that
  would justify them is a message catalog whose languages put the pieces in different orders; `format_dynamic`
  would take them first.

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
  interceptors for CORS and request logging, as a wrapper around `dispatch`, an `otp` module (HOTP/TOTP and base32) for MFA; OAuth2 login
  (Google, Microsoft) on `client`; and RS256/ES256 for tokens verified with a public key, which needs public-key
  operations in `crypto`.
