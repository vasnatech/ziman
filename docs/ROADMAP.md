# Ziman — Roadmap

Not commitments, not ordered — ideas and known gaps to pull from as v0
proves out. See docs/DESIGN.md for the principles these should be judged
against before adding any of them.

## Near-term, likely next

- **Structured/machine-parseable diagnostics** (`--json` error mode).
  Central to the "errors are data" principle in DESIGN.md; deferred out of
  v0 only to get a working language faster.
- **String concatenation.** Deliberately left out of v0 because `+` on
  strings would be operator-overloading-by-type, which DESIGN.md's
  principle #2 argues against. Options: a `concat(a, b)` builtin, or a
  dedicated `++` operator that's numeric-`+`'s explicit opposite (never
  ambiguous which one a given expression means). Needs a decision, not
  just an implementation.
- **`for` loop** over integer ranges (`for i in 0..10 { ... }`) — `while`
  covers everything but is more verbose and more error-prone for the
  extremely common counted-loop case.
- **Arrays / lists.** No collection type exists yet at all.
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
