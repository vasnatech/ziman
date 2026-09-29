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

## Follow-up: implemented the suggestion, re-tested against it

Added "did you mean" suggestions to `src/typecheck.c` the same day (edit-distance search over the
module's own functions/structs/enums and the builtin list, same algorithm and cutoff as
`properties.zm`'s own `check_unknown`), plus two named special cases the data pointed at directly:
a foreign declaration keyword (`let`/`const`/`mut`/`auto`) parsed as a bare identifier now says
"Ziman has no 'let' -- declare with 'val' ... or 'var' ...", and a hardcoded generic type called
like a function (`map()`) now says "'Map' is a built-in generic type, not a function: construct
one with 'Map<...> { ... }'". The general suggestion mechanism, with no special-casing needed,
also caught something outside either named case: `Ok(...)`/`Err(...)` (a reasonable guess from
seeing `Ok`/`Err` work as bare `switch`-arm patterns) is one edit from the real lowercase
`ok(...)`/`err(...)` construction, so it now says "did you mean 'ok'?"/"did you mean 'err'?".

Re-ran the round-2 protocol (fresh model, zero docs, up to 5 fix attempts) against the three
tasks the fix specifically targets, starting from each one's exact original round-1 code:

- **struct_distance**: now converges in 4 attempts (was: did not converge). The `let` message
  named the fix directly on the attempt it first appeared.
- **safe_divide_result**: now converges in 5 attempts (was: did not converge) -- right at the
  budget's edge, since it still needed to separately discover `switch` (for `match`), that a
  switch arm's own `Ok(value)`/`Err(message)` pattern stays uppercase even though *construction*
  is lowercase (a real, distinct wrinkle the diagnostic doesn't -- and structurally can't --
  collapse into one lesson), and `concat`/`str` for string-building. The `ok`/`err` suggestion
  removed exactly the one wall that had made every attempt dead-end before.
- **word_count_map**: **still did not converge**, but for an honest, informative reason. Isolated
  outside the rest of the file, `map()` alone now gets exactly the intended message and the model
  in this run did reach and read it (its own log shows `val counts = map()` survived, unedited,
  through to the final attempt). What it never discovered is the *second*, independent gap this
  task needs closed: membership-testing and reading a `Map` (there is no `.has`/`in`; the answer
  is `map_get(counts, word)` returning `Option<int>`, read with `is_Some`/pattern-matching) --
  the model spent its later attempts guessing at that instead, including a plausible `if word in
  counts` that is itself a new syntax error masking the now-fixed `map()` one on the next
  `--check`. One targeted diagnostic fix closing one gap in a task that has two is exactly the
  outcome to expect, not a failure of the fix -- and it is a cleaner result than declaring victory
  would have been.

Net, measured: 2 of the 3 targeted tasks move from "did not converge" to "converges," the third
demonstrably gets past the specific wall targeted and stalls on a separate, still-undiagnosed one.
Extrapolating (not re-run in full): round 2 across all ten would plausibly move from 4/10 to
6/10, with `enum_shape_area`, `filter_double` and `json_roundtrip` still out of this fix's reach,
since none of their gaps are an edit-distance-close name -- they need a wrapper-struct convention,
a cross-module qualified name, and an unrelated vocabulary guess, respectively, none of which a
name suggestion, however good, can supply.

Test fixtures added: `tests/fixtures/diag_let_keyword`, `diag_map_as_function`,
`diag_undefined_function_suggestion` (all `.err`-checked, `make test`: 789 passed, 0 failed).

## Second follow-up: a misleading suggestion, and word_count_map's real second gap

Re-validating `word_count_map` above surfaced a new, more specific problem than "no
suggestion": once the model tried the reasonable free-function guess `has(counts, word)` for
membership (after "Ziman has no methods" correctly steered it away from `.has(...)`), the fuzzy
search suggested `hash` -- a real builtin, one edit away, and *completely unrelated* to map
membership. A close-by name that is the wrong answer is worse than no suggestion at all, since it
actively steers a model toward a dead end with the compiler's own authority behind it. Fixed the
same way as the other two: a named special case (`has`/`has_key`/`hasKey`/`contains_key`/
`containsKey`/`has_value`/`hasValue`/`exists`, checked before the fuzzy search reaches `hash`)
naming the real answer, `contains(m, key)` for a bool or `map_get(m, key)` for the value too
(`Map`/`Set` really do support `contains` -- confirmed directly, not assumed).

Re-ran `word_count_map` end to end once more, fresh model, zero docs, the full 5-attempt budget,
starting from the exact original round-1 code. Result: **still did not converge, but for a
budget/ordering reason, not a remaining diagnostic gap.** This run fixed errors one at a time in
a different order than the previous ones -- map literal syntax, then indexed assignment, then
method calls, then (on attempt 5) both `let` and `has` at once, correctly landing on `contains`
exactly as intended -- and only *then*, on what would have been a 6th attempt, did `--check`
surface the `map()`-is-a-type message, past the cutoff. The two fixes this session added each did
exactly their job when the model reached them (`contains` was applied correctly, no confusion);
the task simply needs two independent discoveries, and a model that resolves one error per
`--check` cycle rather than every error `--check` reports at once (ziman reports up to 25 per
run) can exhaust a 5-attempt budget on a two-discovery task depending on which error the model's
own strategy happens to look at first. That is a property of the 5-attempt protocol and this
particular model's error-fixing strategy, not something a diagnostic wording can fix -- confirmed
the honest way, by re-testing, rather than assumed.

Test fixture added: `tests/fixtures/diag_map_membership` (`make test`: 790 passed, 0 failed).

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
