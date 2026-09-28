# LLM-generation benchmark, 2026-09-28 (F6, P2-7, review §6 risk 4)

The language-design review named this gap directly: "'Cheap for an LLM to generate' is
asserted, not measured. No experiment in the repo measures generation accuracy." This is that
experiment. Method and the fixed task set are in `tests/bench/llm_tasks/`; this file is the
dated report a run of it produced.

Ten tasks, spanning basics (hello world, a sum-of-evens loop, recursion), the type system
(structs, enums with `switch`), the two features that are the language's own core pitch (`Result`
for error handling, `#[derive(Json)]`), and two collection/higher-order tasks (filter+map,
a word-count `Map`). Each ran two rounds, both fully isolated (a fresh model instance per agent,
no memory of any other run, no access to any Ziman documentation, source, or web search).

## Round 1: cold generation, zero context

A model was told only that the target language is called "Ziman" and given the task in plain
English -- no syntax hints, no example, nothing else. Single attempt, not run or checked by the
model itself.

**Result: 0 of 10 compiled.** Every attempt reached for mainstream-language habits: C-style
semicolons (7 of 10 files), `let`/`let mut` for bindings (6 of 10), `match`/`=>` (2 of 10),
method-chaining (`.filter(...).map(...)`, 1 of 10), Go-style `map[string]int{}` and `type X
struct {}` (2 of 10), Rust-style `|`-prefixed sum-type variants (1 of 10), and a plain `import`
statement (1 of 10). None of this is a surprise -- it is exactly the "training-distribution
cost" DESIGN.md's "What the AI-first goal does and does not justify" entry already names --
but it is the first time the actual failure modes were catalogued across more than the ten
manually-constructed probes that entry itself lists, and at 0/10 the baseline this project's own
"cheap to generate" claim has to clear is not "mostly works," it is "works after at least one
real correction."

## Round 2: iterate against the compiler alone, up to 5 fix attempts

A second, independently fresh model instance got the original attempt, the task again, and the
`ziman` binary -- nothing else about the language, explicitly no documentation -- and was told to
run `--check`, read whatever it says, fix the file, and repeat. This is the direct test of
DESIGN.md's stated mitigation: "the mitigation is the diagnostic: a model's habit should fail at
compile time with the sentence that fixes it."

**Result: 4 of 10 converged to a clean compile with exactly correct output** (hello_world in 2
attempts, sum_evens in 3, fibonacci in 6, apply_twice in 3). **6 of 10 did not converge within 5
attempts**: struct_distance, enum_shape_area, safe_divide_result, filter_double, word_count_map,
json_roundtrip.

The split between the two groups is not random -- it falls cleanly along one line: **grammar
habits that differ from mainstream syntax get a diagnostic that names the fix; vocabulary the
model has to guess (an exact builtin/module function name, or a hardcoded type's construction
syntax) does not, because "X does not exist" is all a compiler can honestly say about a name it
has never seen.**

**What self-corrected reliably, often on the very next attempt:**
- **Semicolons.** Every file that had one got the exact diagnostic ("Ziman statements end at a
  newline...") and every model removed them correctly, first try, no exceptions across all 10
  files that had them.
- **`match` → `switch`.** The compiler's own error for `match` names the replacement syntax
  outright (`"Ziman has no 'match' -- use 'switch value { Variant(name) { ... } ... }'"`) and the
  one model that hit it applied the suggested form immediately and correctly.
- **Method calls.** `"Ziman has no methods... call a function on it instead, e.g. show(w, ...)"`
  worked exactly as designed in both tasks that tried `.filter(...)`/`.has(...)`-style calls.

**What was a genuine coin flip: the declaration keyword.** `let`/`let mut` produces "use of
undefined variable 'let'" and "assignment to undefined variable 'x'" -- true statements, but
they frame the problem as a name-resolution failure, never suggesting `val`/`var`. Three models
found `var` by elimination (trying bare assignment, then `Type x = ...`, then `x: Type = ...`,
each rejected, before landing on `var`); one sidestepped the whole question by rewriting
`fibonacci` as pure recursion with no local variables at all, reaching correct output without
ever learning the real answer; two genuinely got stuck -- `struct_distance` tried `let`, bare
assignment, `Type name = expr`, and `name: Type = expr`, and never tried `var`, despite it being
one guess away. This is the one habit where a small diagnostic change (a fuzzy "did you mean
'var'?" against the two real binder keywords) would likely have moved most of these from a
multi-attempt hunt to attempt 1 -- see the follow-up below.

**What did not recover at all: construction syntax for anything with dedicated vocabulary.**
- **`Result` construction (`safe_divide_result`).** The model correctly discovered and applied
  `switch`/`match` semantics, but never found that `Result` is built with the lowercase builtins
  `ok(x)`/`err(e)`, not the uppercase `Ok(...)`/`Err(...)` it (reasonably) inferred from seeing
  `Ok`/`Err` work as bare `switch`-arm patterns. It tried `Ok(...)`, `Result.Ok(...)`,
  `Result::Ok(...)`, and `Result<int, string>::Ok(...)` -- all rejected as "undefined function" or
  "expected an expression, found 'Result'" -- and never got a hint toward the actual answer. This
  is the language's own central pitch (no exceptions, `Result` instead) and it was completely
  opaque to a model working from diagnostics alone.
- **`Map<K, V>` construction (`word_count_map`).** Tried `map[string]int{}`, `map{}`, `map()`,
  method-style `.set`/`.has`, and free-function `set`/`has` -- none close to the real
  `Map<string, int> { "k": v }` generic-type-literal syntax. "call to undefined function 'map'"
  gave no path toward the capitalized generic type.
- **Enum variants with more than one payload value (`enum_shape_area`).** The model never
  discovered that a variant holds at most one payload and several values need a wrapping struct
  (`Rectangle(RectDims)`, not `Rectangle(float, float)`); it cycled through comma-separated
  positional payloads and tuple-index field access (`rect.0`), each a plausible guess from other
  languages, each rejected without the error ever pointing at "wrap it in a struct."
- **Exact function names (`filter_double`, `json_roundtrip`).** `collections.filter`/`push`-based
  idioms and `json_to_string`/`parse_json` are never guessed from `map`/`filter`/`encode`/
  `decode`/`stringify` being rejected one at a time as "undefined function" -- correctly true,
  unhelpful in isolation.

## The actionable finding

DESIGN.md's stated mitigation for the training-distribution cost -- "a model's habit should fail
at compile time with the sentence that fixes it" -- holds up well for *grammar* (a token the
parser has never seen) and holds up **not at all** for *vocabulary* (an identifier the type
checker has never seen). Those are different failure classes needing different fixes: a grammar
habit needs a special-cased diagnostic per habit (already done for several, per ROADMAP R5); a
vocabulary miss needs a **"did you mean"** suggestion -- fuzzy-matching an undefined name against
the visible builtins and imported modules' own public names, the same idea `properties`'
`check_unknown` already implements for a config key (`"suggesting the closest known key"`).
`use of undefined variable 'let'` -> `did you mean 'var'?` and `call to undefined function 'map'`
-> `did you mean 'Map<K, V> { ... }'?` are both this same mechanism, and per this benchmark it is
the single highest-leverage diagnostic improvement available: it would likely have converged 4 to
6 more of the 10 tasks in round 2 with no other change. Tracked as a ROADMAP follow-up.

## What this does and does not establish

Ten tasks is a start, not the review's suggested "fixed 50-task benchmark" -- a real gap, noted
rather than overstated. It also tests one point on a spectrum (zero context, then diagnostics
alone): it says nothing about how a model does with the actual docs in context, which is the
realistic condition for anyone actually writing Ziman today, and is a natural next run of this
same fixed task set. What it does establish, with real evidence instead of an assertion: Ziman's
"cheap to generate" story is currently carried entirely by its diagnostics recovering from a bad
first guess, that recovery is excellent for syntax and non-existent for vocabulary, and the fix
for the vocabulary half is a small, well-understood, already-precedented feature (fuzzy name
suggestions) rather than a documentation problem.
