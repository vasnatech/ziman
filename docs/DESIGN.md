# Ziman — Design

Ziman ("language" in Kurdish) is a small, statically-typed language designed
to be cheap for an LLM to generate correctly and cheap for a human to read
without running it. It is not modeled on C — C is only this project's
*implementation* host (the interpreter is written in C; see README for why).
Ziman's own syntax and semantics are designed from scratch: it is not
designed to be tree-native/structural either — it's a normal parsed text
language, but every syntax and semantics decision is chosen to remove a
class of error that shows up often in LLM-generated and human-written code
alike, borrowing individual ideas (explicit mutability, no coercion) from
several languages rather than inheriting any one of them wholesale.

## Principles

1. **No hidden control flow.** What a line does must be derivable from that
   line plus its declared types and imports — never from action-at-a-distance
   (no operator overloading, no implicit conversions, no macros that rewrite
   meaning invisibly).
2. **One way to do a thing.** No syntactic synonyms. One loop construct per
   shape of loop, one function-definition form, one string-formatting
   mechanism. Every accreted "second way" in an existing language is a
   documented source of both human bugs and LLM training-distribution
   ambiguity — Ziman doesn't add a second way without removing the first.
3. **Explicit over implicit.** Mutability, type coercion, and effects are
   opt-in and visible at the declaration site, not inferred from usage.
4. **No semantic whitespace.** Blocks are delimited by `{}`, never by
   indentation. Indentation-as-syntax (Python) is a disproportionate source
   of LLM diff/generation errors — a model that drops or adds one space
   silently changes program structure. Statement boundaries are newlines,
   which is unambiguous (see SYNTAX.md) and not the same failure mode.
5. **Errors are data, not just prose.** The compiler's diagnostics are
   structured (machine-parseable) and render as clear text for humans from
   the same underlying record. This lets an LLM (or a human's editor)
   consume a compile error programmatically without scraping a string.
   (Planned for v0.2 — v0.1 just needs errors to carry line/column and a
   stable message shape.)
6. **Small orthogonal core.** Prefer a handful of composable constructs over
   many special-cased ones. Anything not in the core is a library function,
   not new syntax.

## Concrete decisions for v0

- **File extension:** `.zm`
- **Typing:** static, with local type inference for `let` bindings.
  Function signatures always declare parameter and return types explicitly
  — a function's type is a contract that shouldn't require reading its body
  (helps both a human skimming a diff and a model generating a call site).
- **Mutability:** `val` declares an immutable binding, `var` declares a
  mutable one (Kotlin/Scala-style) — two distinct keywords rather than a
  keyword+modifier, so `grep -n "var "` alone finds every mutable binding
  in a file. Mutability is exactly the kind of thing that should be visible
  at the declaration, not inferred from whether an assignment shows up
  later in the function. Trade-off worth naming: `val`/`var` differ by one
  character, which is a one-keystroke typo that silently flips mutability —
  accepted for v0 for the terseness and precedent; a future formatter/linter
  should visually distinguish them (e.g. color, lint warning on the
  near-miss) rather than the language changing the keywords.
- **No implicit coercion.** `int` and `float` don't silently mix; use an
  explicit `as` cast. This is one of the most common silent-bug classes in
  C-family languages and a common LLM mistake (assuming numeric types
  unify the way Python's do).
- **Statement termination:** newline-terminated, no semicolons. A statement
  continues onto the next line only while a paren/bracket/brace is still
  open or the line ends in a binary operator — a purely lexical rule (like
  Go's automatic semicolon insertion), not an indentation rule. This is not
  the same failure mode as Python's block-structure-by-whitespace: dropping
  a newline can't silently change which block a statement belongs to,
  because blocks are still braced.
- **Naming convention:** `snake_case` for values and functions, `PascalCase`
  for types. One convention, enforced by the parser being the arbiter of
  style is out of scope for v0 — this is documented convention only, not
  yet a linted rule.
- **No `return`-less last-expression returns in v0.** Every function that
  returns a value uses an explicit `return`. (Reconsider post-v0: implicit
  last-expression return is common in AI-generated functional-style code
  and reads fine, but it's a second way to return a value — worth revisiting
  once the core is stable, not before.)
- **Arrays (`[T]`) are value types, not reference types.** Assigning or
  passing an array always yields an independent value — never an alias two
  names can mutate through. This is the same principle #1 reasoning as
  everywhere else: a reference-semantics collection lets a function
  silently mutate the caller's data through a parameter, which isn't
  derivable from the call site without reading the function's body, and
  Ziman has no effects-in-signatures (yet) to make that visible. The
  corollary: `push`/`insert`/`set` never mutate their `arr` argument, they
  always return a new array — one mutation pattern (`arr = builtin(arr,
  ...)`), no exceptions, no indexed-assignment statement. `val` on an
  array is therefore a *stronger* guarantee than `val`/`final` on a
  collection in most mainstream languages (Kotlin, Java): the whole value
  is frozen, not just which object the name points to, because there's no
  "same object, mutated in place" concept for arrays at all.
  Implementation note, not a language-semantics one: this doesn't cost the
  performance you'd expect from "always copies." An ArrayObj is immutable
  once built (see `src/value.h`), so a plain Value struct copy (shared
  pointer) is always safe — exactly like Ziman's strings, which get away
  with the same trick for the same reason. The O(n) cost of a "copy" only
  actually happens inside `push`/`insert`/`set` themselves, building the
  one new buffer they return.

## Deliberately deferred (not v0)

- Effects visible in function signatures (which functions do I/O or mutate
  outside their own frame) — real value, real design cost, tracked in
  ROADMAP.md.
- Structured/machine-parseable compiler diagnostics as a first-class output
  mode (`--json` errors).
- Generics / parametric polymorphism.
- Modules / imports beyond a single file.
- A package manager or build system beyond a Makefile.

## Non-goals

- Ziman is not trying to be a tree-native/structural-editing language (see
  the earlier discussion in this project's originating conversation for
  why that's a much larger, more novel effort). Source of truth is text.
- Not chasing raw performance in v0 — it's a tree-walking interpreter.
  Compilation to bytecode or native code is a later milestone if the
  language design proves out.
