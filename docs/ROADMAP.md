# Ziman — Roadmap

Not commitments, not ordered — ideas and known gaps to pull from as v0
proves out. See docs/DESIGN.md for the principles these should be judged
against before adding any of them.

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
  worth it. ~~`json` (encode/decode) is flagged separately below since
  it hits a real language gap, not just missing library code.~~
  Implemented — see the `json` core module entry further below; the
  suspected language gap (a self-referential `Json` type) turned out
  not to be one.

Building any of this will surface language gaps that don't show up in
toy programs — worth calling out now rather than discovering them one at
a time mid-implementation:

- **No map/dictionary type.** Only `[T]` exists. A backend needs
  key-value lookups constantly (headers, query params, JSON objects,
  config). Same "one collection type" question arrays already went
  through, but for `{K: V}`-shaped data instead of a sequence.
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
  collector (Boehm) is available as `make gc` and cut the load test from
  ~31 GB to ~0.47 GB; still open: making it the default (it is a build-time
  dependency), pause times, sockets/SQLite handles that are still closed by
  hand. See DESIGN.md's "Memory reclamation" entry.* Measured: the sample backend's server reached ~31 GB RSS
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
- [x] **R10. Server hygiene in the sample backend.** *Done 2026-09-23: `os.exit`/`write_stderr`/shutdown handler, `server.max-connections`, graceful drain; bounded channels (backpressure) are still open.* Cap concurrency
  (`server.zm` spawns an unbounded thread per connection -- a DoS
  primitive on its own), and fix the config path that prints a friendly
  error and then crashes on `unwrap` anyway. Add a native `exit(code)`;
  there is also no signal handling, so no graceful shutdown.
  Backpressure is absent too: `send` never blocks and queues are
  unbounded.

**D. Language expressiveness**

- [x] **R11. `Ord` bound and a generic `sort`.** *Done 2026-09-23: `Ord` bound, `compare`, `<` on strings, `#[derive(Ord)]`, `collections.sort`/`sort_by_keys`/`binary_search`.* Sorting is not just
  missing, it is inexpressible: no comparator (no function values) and no
  "comparable" bound. The nine hand-written `_compare` functions across
  `byte`/`uuid`/`time` are the symptom. `Eq` and `Hash` already prove the
  bound mechanism.
- [ ] **R12. Widen `Hash`.** Map keys are limited to the seven scalars, so
  nothing can be indexed by `Uuid`, an enum, or a struct -- a real hole for
  an entity-id-keyed backend. Derive `Hash`/`Eq` for structs and enums
  whose fields are hashable.
- [ ] **R13. Revisit "no closures".** The stated benefit ("libraries can't
  take opaque callback holes") is already delivered by no overloading, no
  methods and no inheritance; the cost is that `http` cannot be a
  framework, every app hand-writes a linear route chain (near-copy blocks,
  the riskiest shape for copy-paste mistakes at ~40 entities), and no
  generic `map`/`filter`/`sort`/`with_transaction` can exist. Decide
  explicitly, and record the decision either way.
- [ ] **R14. Derives on generic types.** `#[derive(Json)]` rejects generic
  structs, which already forced the sample backend to type a field as raw
  `Json` and lose the type.

- [ ] **R21. `if` and `switch` as expressions.** (Not from the review --
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

- [ ] **R15. Missing pieces:** an HTTP *client* (a Ziman service cannot
  call another); logging (levels, destination); filesystem beyond
  `read_file`/`write_file` (append, stat, delete, list; `write_file` is
  still fatal on failure); regex; sorting (R11); domain-typed map keys
  (R12).
- [ ] **R16. Naming convention.** `collections` uses type-prefixed names
  meant for bare import (`hashmap_get`); `http` uses bare names meant for
  qualified use (`header`, `response`), which then collide with
  application modules. Pick one convention per usage mode and document it.

**F. Tooling and documentation**

- [ ] **R17. A formatter (or at least a lint) that makes `val` and `var`
  visually distinct** -- DESIGN.md already calls the one-character
  difference a silent-mutability typo hazard.
- [ ] **R18. Split README.md into sections.** It is ~3,000 words in
  essentially one paragraph, and it is the document most likely to be
  pasted into a model's context as "the language".
- [ ] **R19. Fix documentation drift.** SYNTAX.md's `Result` section still
  says "no `enum`, no `match`"; DESIGN.md still lists multi-file modules
  under "deliberately deferred"; the `index_of` entry in SYNTAX.md's
  builtins list is corrupted (starts mid-signature) so the function cannot
  be discovered from it.
- [ ] **R20. Reconcile the "AI-first" rationale with the real motives.**
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
- **Escaping literal `{`/`}` in format strings.** `format` has no escape
  syntax yet — a literal brace can't appear in a format string at all.
  Low priority until someone actually needs one.
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

## Larger, needs its own design pass

- **Memory is never reclaimed while a program runs (in the default build)
  — fine for a short-lived script, a real problem for the North Star goal
  of an always-on backend.** *Update 2026-09-23: `make gc` builds
  `ziman-gc`, which routes every allocation through the Boehm collector;
  the two plans below (refcounting, per-thread arenas) were not built and
  are kept for their reasoning. See docs/DESIGN.md's "Memory
  reclamation" entry for what shipped, what it measured, and what is left.* Every `Value` payload (`ArrayObj`/`StructObj`/
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
- **`HashMap`/`HashSet`'s `set`/`add`/`remove` are O(bucket_count), not
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
  needs bit-counting; Ziman has no bitwise operators, so this specifically
  would need either adding them or finding an all-arithmetic substitute,
  same open question the `byte`/`uuid`/`number` modules already hit and
  worked around case-by-case), collision handling when two keys hash
  into the same leaf path all the way down, and a hash-splitting scheme
  (which chunk of `hash(key)`'s bits picks the slot at each level).
  `hashmap_keys`/`hashmap_values`/`hashmap_size` would need reimplementing
  against the new shape too (`size` stops being a trivial field read if
  the trie doesn't track a running count itself, which it should).
  Not started, not designed in detail — needs its own pass (node
  representation, the bitwise-operator question specifically, collision
  strategy, whether `HashSet<T>` shares the same trie implementation
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
- **Generics — in progress, now a five-stage plan.** Design decided
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
  `Uuid` satisfies `Eq` but not `Hash` (see the `byte` entry's "no
  derived hashing for a struct" note) — usable as a struct field
  compared with `==`, e.g. a `Patient.id: Uuid` replacing the
  `next_id: int` counter `health-record-backend-ziman` used to use —
  not yet usable as a `HashMap` key. ~~Not wired into
  `health-record-backend-ziman` yet~~ — done the same day:
  `Patient.id` is `uuid.Uuid` now, no more counter.
- ~~**`time`/`clock` core modules — seven `java.time`-shaped types
  (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`,
  `ZoneOffset`, `ZoneId`), closing `health-record-backend-ziman`'s last
  remaining documented gap (a real date type).**~~ Implemented
  (docs/SYNTAX.md "Date and time", docs/DESIGN.md's "time/clock"
  entry). `ZoneOffset` only, deliberately — no IANA-style `ZoneId` with
  DST rules, a fundamentally different (data-maintenance, not
  code-volume) problem, ruled out up front rather than attempted.
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
- **First-class functions / closures.** Currently function calls only
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

- **Database access follow-ups.** `db`/`sql`, `#[derive(Row)]`, `net`,
  `crypto`, a shared `dburl`, and Postgres/MySQL drivers (`pg`/`mysql`)
  exist (docs/DESIGN.md's `db`/`sql`, `net`, `crypto`/`pg` and `mysql`
  entries); SQLite, Postgres and MySQL are the three backends. Still open: (1) ~~TLS~~ **done for clients** (docs/DESIGN.md's TLS entry) -- still
  open: server-side TLS (`listen_tls`, for an HTTPS server), client
  certificates, ALPN; (2) ~~scoped cleanup~~ **done as `defer`** (docs/DESIGN.md's `defer` entry).
  What remains of it: a block form (`defer { ... }`), the deferred call's
  discarded `Result` (a failing cleanup is invisible), fatal errors
  skipping defers, and no true `with_transaction(fn)` (no closures); (3) ~~connection pool~~ **done as v1**
  (docs/DESIGN.md's pool entry) -- still wanted, and deliberately left for
  a better implementation: health checks and reconnect (a dead pooled
  connection is never replaced), an acquire timeout, min/max sizing and
  idle timeouts, resetting a connection on release, and real pooling over
  in-memory SQLite (shared-cache); (4)
  **schema management/migrations**, currently a plain `create_schema`
  script per repository; (5) per-field `#[derive(Row)]` annotations
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
- **`net` follow-ups.** A packed byte-buffer type (`[byte]` is an array
  of interpreter values -- wasteful for bulk data), Unix-domain sockets
  (local Postgres often uses one), UDP, and a connect timeout beyond the
  OS default. ~~An `http` module (server first, pure Ziman on `net`)~~
  **done as a toolkit** (docs/DESIGN.md's `http` entry); still open there:
  TLS (`listen_tls`), chunked request bodies, HTTP/2, multipart,
  compression, `Date`/`Server` headers, and -- once Ziman has function
  values -- a real `serve(listener, handler)` and route table on top of
  the same matcher.

## Explicitly out of scope for now (see DESIGN.md "Non-goals")

- Tree-native/structural source of truth — Ziman's source of truth is
  text, on purpose, to keep the project bounded.
- A package manager or build system beyond the Makefile — see the "real
  package registry" entry above for the shape this would eventually take
  and why it's sequenced so far out.
