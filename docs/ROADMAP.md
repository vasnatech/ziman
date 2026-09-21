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

- **A module system is the load-bearing prerequisite.** Everything below
  (files, networking, a database client) gets organized as a module, not
  bolted directly onto the interpreter one builtin at a time the way
  arrays'/structs' builtins were. Still to design: declaration/import
  syntax, how a module exports vs. keeps something private, how the app
  (entry point) declares which modules it depends on, and namespacing for
  calling into another module (`io.read_file(...)`-shaped, presumably).
- **Only a minimal, fixed set of "native modules" touch C** — the actual
  syscalls (raw socket read/write, raw file descriptor I/O, maybe raw
  process/env access) and nothing more. A native module still presents
  ordinary typed Ziman function signatures to its callers, who shouldn't
  need to know or care whether a given module is native or pure Ziman.
  This isn't a new idea so much as formalizing what already exists:
  `print`/`concat`/`str`/`format`/`len`/`push`/`insert`/`set` are already,
  conceptually, a native module that's implicitly imported into every
  program today — the module system should make that explicit rather than
  special-cased in the type checker forever.
- **Everything above the native floor is real Ziman code.** `io`/`fs` on
  top of native file-descriptor primitives; `net` (raw sockets) and then
  `http` (protocol parsing, serving, client) on top of native socket
  primitives; a database module either as pure Ziman speaking a DB's wire
  protocol directly over `net`, or as a native module wrapping an existing
  C client library (e.g. libpq/sqlite) if hand-rolling the wire protocol
  turns out not to be worth it. `json` (encode/decode) is flagged
  separately below since it hits a real language gap, not just missing
  library code.

Building any of this will surface language gaps that don't show up in
toy programs — worth calling out now rather than discovering them one at
a time mid-implementation:

- **No map/dictionary type.** Only `[T]` exists. A backend needs
  key-value lookups constantly (headers, query params, JSON objects,
  config). Same "one collection type" question arrays already went
  through, but for `{K: V}`-shaped data instead of a sequence.
- **No optional/nullable type.** Needed for DB nulls, missing/optional
  JSON fields, and — noted back when structs shipped — for a
  self-referential struct (`struct Node { next: Node }`) to be
  constructible at all (right now it type-checks but can never actually
  be built, since there's no base case without nullability).
- **No recoverable error handling.** Every runtime error today is
  `ziman_error()` → `exit(1)` — fine for a short-lived script, not for a
  server that can't crash the whole process because one request had bad
  input. Needs some `Result`/error-value story, which likely also forces
  the generics question below.
- **Almost no string manipulation.** `concat`/`str`/`format` exist;
  substring, split, trim, find, replace, and parsing a string into a
  number (the reverse of `str`) don't. A backend parses untrusted text
  constantly (request bodies, headers, query strings).
- **No concurrency story.** Serving more than one request without
  blocking everything needs at least a primitive answer (even a
  native-thread-per-request module would need its own native module for
  threads) — not designed at all yet.
- **Generics are still an open question** (see below) and several of the
  above (a real `Map<K, V>`, a `Result<T, E>`) get much more awkward
  without them — this is likely the item that forces that decision, not
  something to resolve independently of it.

## Near-term, likely next

- **Structured/machine-parseable diagnostics** (`--json` error mode).
  Central to the "errors are data" principle in DESIGN.md; deferred out of
  v0 only to get a working language faster.
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
- **`for` loop** over integer ranges (`for i in 0..10 { ... }`) — `while`
  covers everything but is more verbose and more error-prone for the
  extremely common counted-loop case.
- ~~**Arrays / lists.**~~ Decided and implemented: one type, `[T]`, not
  two (see the "one collection type" discussion in this project's
  originating conversation) — no separate fixed-size vs. growable
  distinction, since nothing else in Ziman exposes a stack/heap split for
  that to mean anything yet. Value semantics, like everything else in the
  language (see docs/DESIGN.md's new array entry for the full reasoning
  and the ArrayObj-is-immutable implementation trick that keeps that
  cheap). Builtins: `len`, `push`, `insert`, `set` (docs/SYNTAX.md). Still
  open, not yet decided:
  - **`remove`/`pop`** — symmetric with `push`/`insert`, no design
    question blocking it, just not built yet.
  - **`==`/`!=` on arrays** — currently a type error. Would need a
    recursive `value_equals` for `TY_ARRAY`; not hard, just hasn't been
    asked for yet.
  - **Iterating without manual indices** — right now every array walk is
    `var i = 0; while i < len(arr) { ... }`. A `for x in arr { ... }`
    form (or extending the still-hypothetical `for` loop above to cover
    both integer ranges and arrays) would remove the manual-index
    boilerplate, but see the `for`-loop item above — neither exists yet.
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
  lost to the shipped alternatives). Still open, not yet decided:
  - **`==`/`!=` on structs** — currently a type error, same reasoning gap
    as arrays' still-open equality question above.
  - **Trait/interface-style polymorphism** — deliberately not attempted
    yet; revisit only once a concrete program needs it and free functions
    plus nominal typing genuinely can't express it.
- **A real test harness beyond tests/run_tests.sh's stdout/substring
  matching** — fine for now, will need line/col assertions once
  diagnostics get richer.

## Larger, needs its own design pass

- **Effects visible in function signatures** (which functions do I/O or
  mutate outside their own frame). Real value per DESIGN.md's principles,
  real design cost — needs its own doc before implementation starts. Now
  also entangled with the module system above: an effect story and a
  native-vs-pure-Ziman module boundary are naturally related questions
  (does calling into a native module always count as an effect?).
- **Modules / multi-file programs.** Promoted from "someday" to the
  actual load-bearing next design piece — see the North Star section
  above for the concrete plan (module system → a minimal native-module
  boundary → io/net/http/db as pure-Ziman modules on top).
- **Generics.** Was "not clear yet whether Ziman wants them"; the North
  Star section above makes the case that a real `Map<K, V>` and a
  `Result<T, E>` (for recoverable error handling) both want them badly
  enough that this is likely to get forced, not chosen independently.
- **Compilation to bytecode or native code.** v0 is a tree-walking
  interpreter on purpose — revisit only once the language design itself
  has stabilized against real use, not before.
- **First-class functions / closures.** Currently function calls only
  resolve a bare identifier at parse time; no function values, no passing
  a function as an argument.
- **String interpolation.** Once string concatenation is settled, decide
  whether interpolation is a second way to do the same thing (against
  principle #2) or genuinely a different need.

## Explicitly out of scope for now (see DESIGN.md "Non-goals")

- Tree-native/structural source of truth — Ziman's source of truth is
  text, on purpose, to keep the project bounded.
- A package manager or build system beyond the Makefile.
