# LLM-generation benchmark, 2026-10-09 pilot (F27)

A pilot of the 50-task set F26 built, not the full run: **10 of the 40 new tasks, one model
instance per task per round.** Scoped deliberately, to validate the pipeline and the contamination
screen before spending 200 agent runs. Read the numbers as a first signal and not as the 50-task
baseline, which F27 remains open for.

The ten, chosen to span the categories rather than at random: 11 reverse_string, 15 split_csv_line,
18 sort_numbers, 23 invert_map, 28 big_factorial, 30 fizzbuzz, 31 sort_records_by_field,
36 parse_int_error, 41 write_then_read_file, 45 build_json_from_struct.

## Round 1: cold generation, zero context

**0 of 10 compiled.** All ten passed the contamination screen (no Ziman-specific spelling appears in
any attempt), so this is training-distribution output rather than reading. That reproduces the
2026-09-28 result of 0/10 on ten entirely different, ordinary tasks -- the earlier set leaned toward
this language's own features, and this one deliberately does not.

**What a cold model guesses Ziman is**, counted across the ten attempts:

| guess | attempts | right? |
|---|---|---|
| `fn` for a function | 9 | **yes** |
| `print` | 10 | **yes** |
| `let` | 7 | no (`val`/`var`) |
| `-> Unit` | 7 | no (omit it) |
| `match` | 5 | no (`switch`) |
| `=>` arms | 5 | no |
| `--` comments | 5 | no (`//`) |
| effect row `/ {IO}` | 4 | no |
| `end` terminators | 2 | no |
| `import` | 2 | no |
| semicolons | 1 | no |

The two most frequent tokens in any program, `fn` and `print`, are already what a model reaches for
9 and 10 times out of 10. That is the AI-first claim holding at the surface. Everything below it is
the training-distribution cost the language says diagnostics have to pay down.

## Round 2: iterate against the compiler alone, up to 5 edits

**7 of 10 produced a correct program.** Three ran out of the five-edit budget without compiling
(18 sort_numbers, 23 invert_map, 28 big_factorial). Two of the seven are graded under their task
file's stated flexibility: 36 printed `not a number: abc` where the file allows "any clear failure
indication", and 45 printed `{"x": 3, "y": 4}`, the same JSON document as the expected
`{"x":3,"y":4}`.

| task | outcome | edits | where it got stuck |
|---|---|---|---|
| 11 reverse_string | correct | 3 | no `reverse` builtin, and no suggestion of what to build it from |
| 15 split_csv_line | correct | 5 | comment syntax, `= do ... end`, `import` |
| 18 sort_numbers | **no compile** | 5 | 2 of 5 edits spent guessing comment syntax and generic type syntax |
| 23 invert_map | **no compile** | 5 | `=>` has no diagnostic; `Map.from_entries`; map iteration |
| 28 big_factorial | **no compile** | 5 | the primitive type names; one edit short of clean |
| 30 fizzbuzz | correct | 4 | 13 probe compiles to find `int`/`string` |
| 31 sort_records_by_field | correct | 1 | `struct` keyword and `[T]` by guessing |
| 36 parse_int_error | correct | 4 | ~15 probe compiles for the primitives |
| 41 write_then_read_file | correct | 1 | 8 probes to find `fs.write_text` |
| 45 build_json_from_struct | correct | 4 | `struct` keyword, the primitives |

The 2026-09-28 run was 0/10 cold and 4/10 after iteration on tasks 01-10. This is 0/10 and 7/10 on
ten different and more ordinary tasks. The two numbers are **not** a before-and-after: different
tasks, one instance rather than two, and the caveats below. What they agree on is the shape --
nothing compiles cold, and most things compile once the compiler is allowed to talk back.

## The finding: diagnostics are excellent at the expression level and absent at the type level

Five of the ten runs converged on this independently, in their own words, and every message below
was then reproduced by hand. One agent put it best: *"the type-level vocabulary needed guessing,
while every expression- and statement-level mistake was diagnosed with the fix spelled out."*

**What the compiler does well** -- each of these was named by multiple runs as decisive, and each
names the fix rather than the token:

- `'+' doesn't join strings ... use concat(a, b), or format("{}{}", a, b) for several pieces;
  convert a number with str(n)` -- three correct APIs in one line.
- `Ziman has no 'match' -- use 'switch value { Variant(name) { ... } ... }' ... (one arm per
  variant, no default arm)` -- resolved in a single edit, every time it appeared.
- `an array element cannot be assigned: arrays are values, not mutable containers -- write
  'arr = set(arr, i, value)'` -- called "exemplary, copy-pasteable".
- `Ziman has no methods -- if 'n' is a value, call a function on it instead, e.g. str(n)` -- names
  the fix *and* teaches the design rule in one sentence. This is the standard the rest should meet.
- `this Result<int, fs::FsError> is discarded ... bind it ('val _ = ...'), or use 'try'` -- which
  also leaked the function's exact signature, and so taught the type name as a side effect.
- `Ziman has no 'let' -- declare with 'val' ... or 'var'`, and `did you mean 'print'?` for
  `println`. Both were added after earlier benchmark rounds, and both still earn their keep.
- **C19's map-literal message, written the same day as this run**, was described by a cold agent as
  *"the single most useful message of the run -- fully self-contained fix, including the
  empty-literal rationale"*. That is one of this report's questions answered: at least one of those
  four fixes demonstrably works on a model that has never seen this repository.

**The one that actively misleads, and the pilot's most valuable result.** The primitive type names
are not discoverable, and the rule that is supposed to help points the wrong way:

```
fn f(n: u64)     ->  "a type name must start with an uppercase letter"
fn f(n: U64)     ->  "unknown struct or enum type 'U64'"      (no suggestion)
fn f(n: Int)     ->  "unknown struct or enum type 'Int'"      (no suggestion)
fn f(s: String)  ->  "unknown struct or enum type 'String'"   (no suggestion)
```

The answer is lowercase `int` and `string`. A reader writes `i64`, is told to capitalise,
capitalises, and is told the capitalised name does not exist -- a two-error dead end whose first
step is advice away from the answer. The rule is true of *user-defined* types only, and the message
does not say so. The phrase "struct or enum" compounds it, since the thing being sought is neither.
Measured cost: one run spent 13 probe compiles sweeping `I64, Int64, Integer, Num, Nat, U64, i64,
int` and `str, string, Str, Text`; another spent ~15; a third lost two of its five edits here; task
28 failed for this reason alone, one edit short. **This was the single largest consumer of the
budget across the whole pilot.**

**The other gaps, in descending order of measured cost:**

1. **`module 'fs' has no function 'write'` offers no suggestion**, though the compiler holds the
   module's whole function list at that point. One run brute-forced eight names before
   `write_text`. `write` -> `write_text` is well within edit distance of the existing "did you
   mean" machinery.
2. **`-> Unit` frames a deletion as a missing type.** `Unit`, `Void`, `None`, `()` and `void` are
   all rejected; the answer is to omit the return clause entirely, which no message says. Seven of
   ten cold attempts wrote a unit return type, so this is the most *frequent* habit in the set.
3. **Comment syntax has no diagnostic.** `-- comment` gives `expected 'fn', found '-'`. Five of ten
   cold attempts used `--`, and two runs lost edits guessing `//`.
4. **`=>` has no diagnostic** and degrades into a misleading assignment error, or `expected '{',
   found '='` in a lambda. Five of ten cold attempts used it.
5. **Generic and list type syntax is unguided.** `List(Person)`, `List[Person]`, `Map[K, V]` all
   give a bare `expected ')'` or `expected '<'`, with no hint that an array is `[T]`.
6. **The `struct` keyword is unguided.** `type Point = { ... }` gives `expected 'fn', found
   identifier`, which says what is not allowed but not what is.
7. **Effect annotations.** `-> T / {E}` gives `expected '{', found '/'`, from which a reader cannot
   tell whether the syntax is wrong or the feature absent. Four of ten cold attempts wrote one.
8. **Index assignment on a `Map` reports an array-worded message**, leaving open whether maps have
   their own `set`.

## What this does and does not establish

**Does:** nothing compiles cold, on ordinary tasks as much as on feature-shaped ones; the
expression-level diagnostics carry a model most of the way unaided; and the type-level vocabulary is
where the budget actually goes. The ranking above is measured, not guessed, which is what F26's
warning about writing tasks blind was protecting.

**Does not:** this is one instance per task, so a per-task outcome is one sample. It is 10 of 50
tasks. And three honest irregularities, recorded rather than smoothed:

- **Task 11's pass is assisted.** That agent used `ziman --lsp` completion to discover names --
  compiler output, but considerably more than a diagnostic. The loophole was closed for the four
  reruns, so the first six and the last four ran under slightly different rules.
- **Four tasks were interrupted mid-run by a session rate limit** and rerun from their cold
  attempts. They were not counted as failures, because the method did not fail them.
- **The five-edit budget understates the real cost.** Several runs did their discovery in throwaway
  probe files and then edited the working file once or twice, so "1 edit" can mean a dozen compiles.
  The next run should count compiler invocations, not edits -- the method's own metric is the wrong
  one, which this pilot is how we found out.

One defect in the task set itself: task 45's expected output did not say whitespace may vary, so a
semantically identical JSON document needed a hand judgement. Fixed in the task file.

## Validation: the three failures re-run against the fixed compiler (same day, F28)

The five messages F28 added were chosen from the ranking above. The honest test of a diagnostic
change is not that it reads better, so the three tasks that ran out of budget were re-run from
**the same cold attempts**, with the **same five-edit budget**, against the fixed compiler. All
three now compile, and all three produce the right answer.

| task | before F28 | after F28 | what carried it |
|---|---|---|---|
| 18 sort_numbers | no compile, 5 edits | **correct**, 4 edits | the `--` comment message, and `unknown type 'Int' -- did you mean 'int'?` |
| 23 invert_map | no compile, 5 edits | **correct**, 2 edits | the `Unit`, `--`, `let`, `match` and map-literal messages, which it said "carried the whole rewrite" |
| 28 big_factorial | no compile, 5 edits | **correct**, 1 edit | the lowercase-types list, which handed it `bigint` and made its hand-rolled bignum arithmetic unnecessary |

**Round 2 therefore moves from 7/10 to 10/10** on this pilot's ten tasks. Read it as what it is:
the same ten tasks that produced the ranking, so this measures whether the fixes address what was
measured, not whether they generalise. The 40 remaining tasks are where that question gets answered,
and F27 stays open for them.

Task 28 is the one worth keeping. It had failed *one edit short*, and the message that fixed it did
not just name a type -- it listed the built-ins, which revealed `bigint` and collapsed a
hand-written decimal-digit multiplication into a three-line loop. A diagnostic that names the
neighbourhood, not only the token, can change what a program *is*.

**Two more gaps surfaced by the validation itself**, both fixed the same day:

- **`bigint` literals.** `var x: bigint = 1` said only *cannot initialize 'x' of declared type
  bigint with a value of type int*, and `bigint(1)` was rejected too, so there was no path to `1n`
  but guessing. The mismatch message now names the suffix (`1n`, `1d`, `1b`).
- **Reading a map.** `get(m, k)` suggested `set` -- the *mutator*, the wrong direction -- and one
  agent then brute-forced six names (`lookup`, `at`, `value_at`, `fetch`, `find`) before reaching
  `map_get`. Indexing was the other dead end: `m["k"]` said only *cannot index into
  Map<string, string> (not an array)*. Both now name `map_get(m, key)` and its `Option<V>` result,
  and the set case names `contains`.

**What the pilot says about the method, having now run it twice.** The five-edit budget is the wrong
unit: these runs used 10, 15 and 10 compiler invocations respectively while making 4, 2 and 1 edits,
because discovery happens in throwaway probe files. Count invocations. And a run that ends in
"compiles" is not a run that ends in "correct" -- two of the ten compile and print the right thing
only under their task file's stated flexibility, which is why that flexibility has to be written
down before the run rather than adjudicated after it.

## The holdout: 10 untouched tasks against the fixed compiler (F27)

The pilot's round-2 figure of 10/10 was measured on the same ten tasks whose errors produced the
fixes, which is fitting and evaluating on one sample. This is the holdout that was missing: **ten of
the thirty tasks nothing in this project had looked at**, chosen to match the pilot's category
spread (strings, splitting, arrays, maps, numerics, control flow, records, errors, file IO, JSON) so
the comparison would isolate generalisation rather than difficulty.

All three method corrections the pilot earned were applied from the start, which is the other thing
being tested here: compiler **invocations** counted rather than edits, `--lsp` forbidden in the
prompt, and each task's grading flexibility checked before the run rather than adjudicated after
(the check found nothing to fix, which is the correction working).

### Round 1: 0 of 10 compiled

Third independent replication of that figure -- 2026-09-28, the pilot, and now this -- across 30
distinct tasks. All ten passed the contamination screen.

**But the two batches are not drawn from the same habit distribution, and the comparison is weaker
for it.** This is the pilot's mix against the holdout's:

| habit | pilot | holdout |
|---|---|---|
| `--` comments | 5/10 | **10/10** |
| `do ... end` bodies | 2/10 | **10/10** |
| `import` | 2/10 | **9/10** |
| `let` | 7/10 | 7/10 |
| effect row `/ {IO}` | 4/10 | **0/10** |
| `=>` | 5/10 | 2/10 |
| semicolons | 1/10 | 0/10 |

The pilot's attempts were a Rust/ML/Haskell mix; the holdout's were almost uniformly
Haskell/Idris -- `--` was the *first* error in all ten. So this batch is a strong test of three
things F28 added (the comment message, `import`, `= do ... end`) and barely tests the effect-row
message at all. The holdout's round-2 number stands on its own; the delta against the pilot's
pre-fix 7/10 is suggestive and not more than that, which is a limit of the design rather than of
the result.

### Round 2: 10 of 10 compiled

**Every one of the ten reached a compiling program**, from cold attempts written in a dialect Ziman
is not, using nothing but `--check` and its own throwaway probe files. That is the generalisation
evidence the pilot could not give: these tasks' errors played no part in choosing the nine
diagnostics C19 and F28 added, and the compiler still talked all ten home.

| task | compiles | edits | compiler invocations |
|---|---|---|---|
| 13 palindrome_check | yes | 1 | 10 |
| 16 join_with_dashes | yes | 2 | 12 |
| 22 binary_search | yes | 1 | 9 |
| 24 group_by_first_letter | yes | 1 | 12 |
| 27 primes_under_30 | yes | 1 | 7 |
| 29 average_two_decimals | yes | 1 | 8 |
| 33 aggregate_by_group | yes | 1 | 14 |
| 37 chain_fallible | yes | 1 | 25 |
| 42 count_lines_in_file | yes | 1 | 11 |
| 44 parse_json_field | yes | 1 | 16 |

**Correct output: 7 of 10**, and the three misses are a defect in the task set rather than in the
language: 22 printed `found 7 at index 3`, 42 printed `lines: 3`, 37 printed `ok: 4`. Each computed
the right answer and labelled it, against an expected block holding a bare number -- and the prompts
never said to print only the value. A person would label it too. Task 08 had the pattern from the
start ("prints *just* the count"); 22, 37 and 42 have been brought into line, and the README now
states the rule. **Scored as failures here regardless**, because the flexibility was not
pre-registered and the whole point of pre-registering is that it costs something when the result
arrives.

**The edit/invocation columns settle the method question the pilot raised.** Nine of ten runs made a
single edit to the working file, while invoking the compiler between 7 and 25 times (median 11.5).
Discovery happens in throwaway probes, so counting edits understates the real cost by about an order
of magnitude. Count invocations.

### What generalised, and what this still does not say

On the measure that is actually about the language -- *can the compiler talk a cold attempt into
compiling* -- the holdout matches the pilot's post-fix 10/10 and beats its pre-fix 7/10, on tasks
whose errors never shaped a single message. Agents named the same messages as decisive, unprompted
and repeatedly: `--` to `//`, `import` to `use` (with "builtins need no use", which several said
saved probes), `let` to `val`/`var`, the lowercase built-in type list, "Ziman has no methods", the
discarded-`Result` message, and `'+' doesn't join strings`. Five of those seven were written on
2026-10-09.

What it does not say: the holdout's habit mix was far more uniformly Haskell than the pilot's, so
the fixes being exercised were disproportionately the comment, `import` and `do ... end` messages.
"F28 generalises" is supported for those; the effect-row message was not tested at all here, having
appeared in 0 of 10 cold attempts. One instance per task remains one sample.
