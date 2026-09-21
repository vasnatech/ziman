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
  basic file I/O, and — since `io.zm`'s signature file isn't found by any
  bundled-stdlib search path yet (see below) — every project that wants
  it has to keep its own copy alongside its entry file for now.
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
  worth it. `json` (encode/decode) is flagged
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
  be built, since there's no base case without nullability). Very likely
  the same "hardcode one more parametric type" trick `Result<T, E>` used
  (see below) would work here too, as `Option<T>` — not designed yet,
  but the precedent now exists.
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
  needs `Option<T>` or a Result-shaped decision first). Numeric
  builtins now also exist: `abs`/`min`/`max`/`floor`/`ceil`/`round`. Array
  (`remove`, `==`/`!=`) builtins are the last item in this same
  "complete the built-in function set" pass.
- **No concurrency story.** Serving more than one request without
  blocking everything needs at least a primitive answer (even a
  native-thread-per-request module would need its own native module for
  threads) — not designed at all yet.
- **Generics are still an open question — but weaker than this entry
  originally argued.** The prediction here was that `Result<T, E>` and
  `Map<K, V>` would force the decision. `Result<T, E>` shipped (see
  below) *without* general generics, using the same "hardcode one more
  parametric type into the compiler" trick arrays already used — not a
  workaround, a deliberate, cheaper alternative that turned out to solve
  the whole problem. `Map<K, V>` would very likely take the identical
  path. So general user-definable generics may never actually get
  forced by anything currently on this list — revisit only if a *third*
  case shows up that genuinely doesn't fit "just hardcode it," not
  preemptively.

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
  - **`==`/`!=` on `Result`** is a type error, same open gap arrays and
    structs already have.
  - **`Option<T>`** (for the still-separate optional/nullable-type gap
    above) would very likely use this exact same trick — not designed,
    but the precedent now exists.
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
- ~~**Modules / multi-file programs.**~~ Implemented (`src/loader.c`):
  - **One file = one module**, identified by its filesystem path — no
    separate `module Foo` declaration to keep in sync with the filename.
  - **`use path/to/mod`** (slash, not dot — a module path is filesystem-
    literal and deliberately never shares a token with `.field` access,
    which already means something else). Resolved **project-relative
    only for now** — against the directory containing the entry file
    passed to `ziman`. There's no bundled standard library to fall back
    to yet (nothing under "everything above the native floor" below has
    been written), so that half of the design (`use io` finding a
    bundled module the same way it'd find a local one) isn't built —
    only the local-project half is.
  - **Every cross-module reference is qualified — `user.greet(u)`, never
    a bare `greet(u)` that happens to resolve to some import.** Same
    "derivable from the line itself" reasoning as `concat`/`str` existing
    instead of overloading `+`. There is no unqualified/wildcard import
    form. Implementation note: a qualified reference looks identical to
    plain field access (`p.x`) until the parser sees what follows the
    second identifier, so it's parsed speculatively and backed out of
    (restoring position) if it doesn't match either `IDENT "("` or an
    uppercase `TYPE_IDENT "{"` — confirmed not to disturb plain field
    access via the existing struct test suite passing unchanged.
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
  - **Today's built-in scalar/array machinery stays implicit for now** —
    `int`/`float`/`bool`/`string`/`[T]` and their functions (`print`,
    `concat`, `str`, `format`, `len`, `push`, `insert`, `set`) remain
    unqualified, no `use` required, exactly as they work today. Nothing
    else gets this treatment — `io`, `net`, `http`, `db`, and any
    user-defined module all go through explicit `use` + qualified calls.
    Whether to eventually formalize the builtins themselves as an
    always-auto-imported native module (a real "prelude," Rust-style) is
    an explicit non-decision — revisit only if keeping them special-cased
    forever starts costing something concrete, not on principle alone.
- **Generics.** Back to "not clear yet whether Ziman wants them" — see
  the North Star section's updated note. `Result<T, E>` shipped without
  needing them; `Map<K, V>` would very likely take the same path. Stay
  monomorphic-by-design until something concrete can't be solved that
  way, not before.
- **Compilation to bytecode or native code.** v0 is a tree-walking
  interpreter on purpose — revisit only once the language design itself
  has stabilized against real use, not before.
- **First-class functions / closures.** Currently function calls only
  resolve a bare identifier at parse time; no function values, no passing
  a function as an argument.
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

## Explicitly out of scope for now (see DESIGN.md "Non-goals")

- Tree-native/structural source of truth — Ziman's source of truth is
  text, on purpose, to keep the project bounded.
- A package manager or build system beyond the Makefile — see the "real
  package registry" entry above for the shape this would eventually take
  and why it's sequenced so far out.
