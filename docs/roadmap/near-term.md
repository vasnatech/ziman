# Near-term, likely next

Part of the [roadmap](README.md) -- see its preamble for what an entry's status means and how it goes stale.


- ~~**Recoverable error handling.**~~ Implemented: `Result<T, E>`, a
  second hardcoded parametric type (same trick as `[T]`, not general
  generics — see docs/design/journal/), manipulated only through `ok`/`err`/
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
  (see docs/syntax/) — the one variable-arity builtin in the language,
  by necessity, but still fully statically checked because the literal
  is visible at the call site.
- ~~**Escaping literal `{`/`}` in format strings.**~~ Already implemented, and this entry was
  simply stale: `{{` and `}}` write a literal brace, documented in docs/syntax/'s own `format`
  section and suggested by the error message a stray `{` produces. Found 2026-10-05 while looking
  for something to work on, which is the argument for `make docs-check` existing at all -- three of
  this section's own open items turned out to be done, half-done or wrong.
- ~~**`for` loop.**~~ Implemented, three forms in one grammar production
  (docs/syntax/ "For loops"): `for x in arr` (value), `for i in a..b`
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
  language (see docs/design/journal/'s new array entry for the full reasoning
  and the ArrayObj-is-immutable implementation trick that keeps that
  cheap). Builtins: `len`, `push`, `insert`, `set`, `remove` (docs/syntax/).
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
  docs/design/journal/'s new struct entry. Construction is named-fields-only,
  never positional (`Point { x: 1, y: 2 }`, not `Point(1, 2)`) to kill the
  "swapped x/y" bug class for structs with same-typed fields. Mutation
  follows arrays' value-semantics story exactly: no field assignment,
  `with p { x: 99 }` builds a new struct instead (a considered-and-rejected
  `..p` spread operator and a considered-and-rejected `new Point { ... }`
  construction keyword are both written up in docs/design/journal/, with why they
  lost to the shipped alternatives).
  ~~`==`/`!=` on structs~~ Implemented: field-by-field, in declaration
  order, short-circuiting on the first mismatch (docs/syntax/
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
  (docs/syntax/ "Enums"): a variant holds zero or one payload value —
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
    judgement, not on a grep. See docs/design/journal/'s "Pattern matching, measured rather than
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
  header comment, the README's module table and docs/design/journal/, all hand-written,
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
  docs/design/journal/'s E7 entry) is a real, already-in-the-library candidate.
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

- ~~**Cache derived output, so `--lsp` stops re-running every generator on every keystroke.**~~
  **DONE 2026-10-30**, the same day it was filed. `src/generators.c` memoises the text
  `ziman_generate_in_ziman` returns -- text, not AST, because a re-parsed module's declarations are
  fresh pointers and cached nodes would reference stale ones.

  **The key is the whole of what a generator can see**: the module's source hash (which covers the
  declaration, its attributes, and the `use` list the per-module attribute specs come from), the
  declaration's name (so a hash collision alone cannot serve one declaration's code for another's),
  the target, and for a project's own generator that generator's own source hash, so editing a
  derive invalidates everything it produced. A bundled generator contributes 0, its source being in
  the binary.

  | | before | after |
  | --- | --- | --- |
  | `--lsp` reparse, the `serve.zm` graph (294 modules) | 1,847 ms | **807 ms** |
  | `--lsp` reparse, the `main.zm` graph (141 modules) | 395 ms | **198 ms** |

  Steady over a 30-edit session, which is the second thing this needed: the first version only ever
  appended, and since every keystroke gives the edited module a new hash, the table grew for the
  life of the server and its scan slowed with it. An entry is now *replaced* when the same
  declaration of the same file is generated again, capping the table at one entry per (file,
  declaration, target) -- the size of the project rather than the length of the session.

  **The risk a cache carries here is a silent wrong compile, so it is tested as such**: the LSP
  suite edits a `#[derive(Json)]`d struct's field and asserts the diagnostic follows, then reverts
  and asserts it regenerates. `--emit-derived serve.zm` is byte-identical at 34,801 lines, which is
  the property the cache spends and which this repository already checks on every compiler change.

  **It does newly depend on generators being pure**, which nothing enforces --
  docs/design/user-generators.md §8 now records that as the strongest argument for its option (3),
  restricting what a generator may reach.

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
     docs/design/journal/ entry, "MapBuilder vs CPython dict", which does not exist and never did; the record
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
  underneath it rather than a correction to it. (3) was measured and retired in `7c8cb93`: node
  growth is not the insert cost (281-287 against 284-288 ns/insert, since glibc grows small chunks
  in place), and `MutableMap`/`MutableSet` then left the trie for a flat table anyway (`b3fc91c`),
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

- **Colour Ziman as Ziman, outside the editor.** The TextMate grammar
  (`editors/vscode/syntaxes/ziman.tmLanguage.json`, 15 patterns over 14 scope groups) serves VS
  Code and nothing else. Everywhere else the language renders as plain text: GitHub highlights by
  a name Linguist knows, so both the 78 ` ```ziman ` fences in the docs and every `.zm` file in
  the file view are colourless, and so is Ziman code shown in a terminal.

  **Borrowing another language's grammar was measured and declined (2026-10-28).** Over the 46,000
  lines of `lib/` and the examples, Rust would colour 70.9% of keyword tokens, Kotlin 70.7%, Swift
  58.5% -- Rust's blind spots being `val` (3,184 uses), `var` (1,013) and `try` (1,001). Rejected
  not on the numbers but on the label: tagging a fence ` ```rust `, or mapping `*.zm` to Rust in
  `.gitattributes`, says Ziman is a language it is not. The `.gitattributes` form would at least
  change no source -- it only picks the grammar GitHub renders with -- and is parked with this
  entry rather than ruled out forever.

  **What to build is a tokenizer of our own**: `tools/gen_highlight.py`, emitting `<span>` classes
  from Ziman's own 25 keywords, strings, text blocks, numbers, comments, attributes, type names
  and builtins, run at publish time the way `gen_lib_map.py` already is. That colours ziman.dev
  honestly, with no JavaScript and no CDN, and replaces the hand-written `<b>` tags on the one
  example the page shows today -- which do not scale to the 30 examples and can drift from the
  syntax.

  **Linguist itself is the real fix and is out of reach**: it wants the grammar (we have one) plus
  the language in use across hundreds of repositories (we have one). Revisit if that changes.

- ~~**Declaration attributes are an open set that nothing can read.**~~ **Half done 2026-10-30
  (F259): the set is closed.** Option (1) below, which cost no migration -- nothing in this
  repository or the sample backend carried an uninterpreted one, checked rather than assumed.
  **Two things were silent, not one**: a bare `#[foo_bar]` fell off the end of the `if`/`else if`
  chain, and `#[drive(Json)]` reached the branch that reads a derive target list, was parsed, and
  was discarded -- the misspelling of the most-used attribute in the language, compiling to a
  struct that derived nothing. The branch is gated on `is_derive` now and both reach one refusal.

  **Option (2) is open and should probably stay that way** -- the reason it was wanted no longer
  holds. It said `meta.Decl` carries no attribute list, so nothing can *read* a declaration
  attribute, and that this is what a project needs "to put its own attribute on a declaration
  rather than on a field". **F253 covers that**: a derive target takes named, typed arguments, so a
  project writes

  ```ziman
  #[derive(Csv(table: "customers", header: true))]
  struct Customer { id: int  name: string }
  ```

  and reads them with `meta.decl_arg(d, "table")`. Tested 2026-10-30, with a generator that is not
  the compiler's: it works.

  What a *separate* declaration attribute would still buy is narrow, and worth writing down rather
  than building on faith:

  * **Data read by a generator other than the one named.** `#[derive(Json, Csv)]` with a shared
    `#[table(name: "x")]` says it once where target arguments say it twice. One repeated argument
    is a weak case for a new mechanism, a new position in `AttrSpec`, a second deferred check and
    a `Decl.attrs` to reify.
  * **Reading the compiler's own nine** -- a generator propagating `#[deprecated]` into what it
    writes. Plausible, speculative, and nothing asks for it.

  So the shape of a decision here is "wait for a use", not "build the plumbing". Option (3) --
  reopening the set *through* the generator -- is what either case would need, and it is the same
  per-module keying §5a wants, so it costs nothing to leave until one of them is real.
 (Raised by the user 2026-10-29, right after F246 closed the field-attribute half: "can I
  just add a random annotation in ziman like `#[foo_bar] struct User`?" The answer is yes, and that
  is the entry.)

  The three attribute positions have three different rules today, and only one of them was
  designed:

  * **A field attribute is a closed table.** `src/parser.c`'s `FIELD_ATTRS` holds the ten names a
    field may carry, and anything else is a syntax error naming the whole list. Each is reified into
    `meta.Field.attrs`, so a generator reads it.
  * **A module attribute is closed too**: `experimental`, `deprecated`, `repository`, `generator`,
    and an unknown one is refused with those four named.
  * **A declaration attribute is open.** Nine names are interpreted — `derive` and `thread_bound` on
    a type, `test`, `deprecated`, `when_unmatched`, `at_least_one`, `effectful` and
    `compiler_resolved` on a function — and **every other name parses, means nothing, and says
    nothing**. `#[foo_bar]` and `#[foo_bar(Baz)]` both compile silently. `--lint` does not mention
    them either.

  **And nothing can read one even on purpose.** `meta.Decl` carries `arg`, `name`, `snake_name`,
  `type_params` and `body`, and no attribute list, so a generator cannot see `#[foo_bar]` at all;
  `#[derive(Reflect)]` on a struct carrying one prints its *field* attributes and no trace of the
  declaration's. So the open set is not a user-extension point that happens to be undocumented —
  it is a hole that cannot be reached from either side.

  **This is the `#[deprecated]` shape exactly**, which is the argument already written into
  `FIELD_ATTRS`' own comment: it parsed and did nothing for years, and an attribute the compiler
  accepts while nothing reads it is indistinguishable from one that works — the silence is the same
  either way. F245 and F246 both chose the table for that reason. The declaration position is the
  last one that did not get the choice.

  Three answers, and they are not exclusive:

  1. **Close the set**, as the other two positions are closed: refuse an unknown declaration
     attribute, listing the nine. Cheapest, and it is the only one that makes a typo
     (`#[drive(Json)]`) an error rather than a struct that quietly derives nothing. The cost is that
     a program carrying a tolerated `#[foo_bar]` today stops compiling, and `#[derive]`'s own
     comment would need to say where a new name is added.
  2. **Reify them**, giving `meta.Decl` an `attrs: [meta.Attr]` filled exactly as `meta.Field`'s is.
     That makes a declaration attribute as useful as a field attribute — `#[table(schema: "audit")]`
     would stop having to be `#[derive(Entity("..."))]`'s positional argument — and it is the piece
     `docs/design/user-generators.md` needs if a project's own generator is ever to read an
     attribute the compiler has no opinion about.
  3. **Both, in that order**: close the set, then open it again *through* the generator, so a name
     is known either because the compiler interprets it or because a loaded generator declares it.
     That is the shape F243's "by name, not by derive" settled on for a different question, and it
     is the only answer that lets a project add an annotation without a compiler change.

  What to settle before building: whether (1) alone is worth a breaking change on its own, and
  whether (2) should wait for user-written generators, which is what would actually consume it.
  Nothing in the North Star carries an uninterpreted declaration attribute, so the migration cost
  of (1) is currently zero — which argues for doing it before that stops being true.

- ~~**`--lint` cannot read a library module, so `lib/`'s own findings are invisible.**~~ **DONE
  2026-10-30, and this entry was wrong the day it was written.** `tools/lint_lib.py` already did
  it and `make lint` already ran it: it generates a throwaway entry importing every module and
  points `ZIMAN_MODULE_PATH` at the sources, so they load from disk with real `source_file` paths.
  The "shape 2" it proposed -- linting every non-corelib module of whatever program is given --
  was already the behaviour too, because the lint gate is per function on `source_file`, not per
  entry. The promised `lib/db/eql.zm` `val d` had been fixed as well. The original text is gone
  rather than kept: unlike the other corrections made this day, its argument was not the reason
  for any work, because the thing it asked for already existed.

  The real work was the gap between findings existing and findings being *gated*, which is now
  closed: **43 findings -> 0**, and `lint-lib-check` runs in `check-fast`.

  * **Two populations had never been linted at all.** `lib/graphics` was missing from the tool's
    module list (12 modules), and the prelude cannot be imported, so no generated entry reaches
    it -- `--lint-bundled` is new for that, reporting against each part's `origin_file`. It found
    three discards that should have been the bare `_`.
  * **One of the 43 was the lint's own bug.** A binding used only as the receiver of a *called*
    field was reported as never read, because that call path synthesises its own base reference
    and never hands it to `check_expr`, where a read is marked. `--lint` was telling people to
    delete bindings their program calls through -- four in `lib/db/datasource.zm`, whose
    neighbouring `c.dialect` was correctly silent, which is what made it look like a rule about
    enum binders. Pinned by `tests/lang/lint_called_field_is_a_read`.
  * **29 were real and mechanical**: unused enum payloads now `_`, two `var`s that never change,
    a `snapshot` before a `to_array` that reads the set directly, one dead `val`.
  * **Two were real quadratics, fixed.** `expr.zm`'s string lexer built its literal with `concat`
    in a loop over the caller's input; `memdb.zm`'s foreign-key update copied the whole child
    table once per affected row, and now collects the rewritten rows by index and applies them in
    one pass -- `tests/lang/memdb_foreign_keys` cross-checks that path against real SQLite and its
    output is byte-identical.
  * **7 are marked `// lint-ok: <reason>`**, the shape `/* xalloc-ok: */` already uses in the C,
    with the reason required and a bare marker refused. Each is bounded by something that is not
    input: a datetime format pattern, a foreign key's column count, the SET clauses of one
    UPDATE, the `@template`s in one sheet, a configured header limit.
