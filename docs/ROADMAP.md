# Ziman — Roadmap

Not commitments, not ordered — ideas and known gaps to pull from as v0
proves out. See docs/DESIGN.md for the principles these should be judged
against before adding any of them.

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
  real design cost — needs its own doc before implementation starts.
- **Modules / multi-file programs.** Everything is one file today.
- **Generics.** Not clear yet whether Ziman wants them or wants to stay
  monomorphic-by-design; revisit once real programs are being written and
  the pain (or lack of it) is concrete.
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
