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
  - ~~**Stage 2: bounded type parameters (`<T: Eq>`).**~~ Half
    implemented (docs/SYNTAX.md "Generics"): the one hardcoded bound,
    meaning "supports `==`," parses on a struct's own type parameters
    (`struct Map<K: Eq, V> { ... }`) and is enforced at every concrete
    instantiation — `Map<Channel<int>, V>` is a compile-time error, since
    `Channel` doesn't support `==`, checked via the same
    `type_supports_equality` machinery arrays/structs/Result/enum already
    use. A bound name other than `Eq` (`<T: Ord>`) is also a compile-time
    error. **Still not usable inside a generic declaration's own body**
    (a bounded `T`'s `==` doesn't type-check anywhere yet), since nothing
    generic has a body to test it in until generic functions exist —
    that half necessarily lands together with Stage 5 below, where a
    real function body (`fn map_get<K: Eq, V>(m, key) { ... key == k
    ... }`) finally gives it somewhere to be exercised. Building the
    instantiation-side enforcement now, ahead of the body-side check,
    means the guarantee "a bounded `T` is always comparable" will already
    hold by the time Stage 5 needs to rely on it.
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
  - **Stage 5: generic functions.** Not started — `fn identity<T>(x: T)
    -> T`, called as `identity<int>(5)`. Needs the one-token lookahead
    heuristic docs/DESIGN.md's "Generics" entry describes to disambiguate
    a generic call's `<` from a real comparison, which structs didn't
    need (an uppercase struct/type name can never be a real variable, but
    a generic function's lowercase callee genuinely can appear on the
    left of `<`). Needed to write `map_get`/`map_set`/`set_add`/etc. once
    instead of once per concrete instantiation.
- **Collections module (`Box`/`Option`/`Map`/`Set`) — the concrete payoff
  of the generics work above, not started.** A `collections` module,
  written in ordinary Ziman (no new native/C code), exporting `Box<T>`,
  `Option<T>`, `Map<K: Eq, V>`, and `Set<T: Eq>` (`Set` needs `Eq` too, to
  dedupe on insert) as plain structs/enums plus free functions
  (`map_get`/`map_set`/`map_has`/`map_remove`, `set_add`/`set_contains`/
  `set_remove`), depending on every stage of the generics entry above.
  **`Map.get` must be O(1), not the O(n) linear scan a naive
  parallel-arrays-plus-`Eq`-loop implementation would give** — this needs
  real hash-bucket storage, which means a second bound, `Hash` (parallel
  to `Eq`), so the type checker can prove a key type supports hashing the
  same way `Eq` proves it supports `==`. Not yet decided: whether `Hash`
  is hardcoded to the same scalar types `Eq`/array-`==` already special-
  case (int/string/bool/float — matching the existing "no derived
  equality/hashing for an arbitrary struct" pattern throughout the
  language), which would keep `Map`/`Set` keys scalar-only in v0, or
  whether it's worth a more general per-type hash story later. Revisit
  when Stage 5 above is done and this module actually gets built.
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

## Explicitly out of scope for now (see DESIGN.md "Non-goals")

- Tree-native/structural source of truth — Ziman's source of truth is
  text, on purpose, to keep the project bounded.
- A package manager or build system beyond the Makefile — see the "real
  package registry" entry above for the shape this would eventually take
  and why it's sequenced so far out.
