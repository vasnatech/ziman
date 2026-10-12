# Generation benchmark, 2026-10-12: round 1 on the committed task set

The first wave against the task set committed in F83, and **the first number this instrument has**.
It does not continue the 2026-09-28 or 2026-10-09 figures: those were measured on a different set
that was lost with a cleared working directory, which is why this one is in the repository. The
famous "0 of 50 cold" belongs to that set. Treat what follows as a baseline starting from nothing.

| | |
|---|---|
| Date | 2026-10-12 |
| Tasks | all 50, `docs/reviews/benchmark/tasks/` |
| Instances | 1 per task (variance unmeasured) |
| Round | 1 only -- cold, zero context. Round 2 not yet run |
| Agents | one fresh general-purpose agent per task, Claude Opus 5, 3 tool calls each |
| Prompt | `docs/reviews/prompts/generation-benchmark.prompt.md`, verbatim, identical across all 50 |
| Attempts | `docs/reviews/benchmark/runs/2026-10-12-round1/` |
| Instrument self-check | `run.sh reference` → 50 compiled, 50 correct, before and after |

## The numbers

**0 of 50 compiled. 0 of 50 correct. 0 missing. 0 of 50 flagged by the contamination screen.**

The screen (`docs/reviews/benchmark/screen.sh`) greps for library spellings that exist nowhere but
Ziman -- `fs.read_text`, `mapb_`, `set_items`, `collections.sort_with` and friends -- deliberately
not generic keywords like `val`, `switch` or `try`, which a cold model reaches for by accident. All
fifty attempts are clean, so this is training-distribution output and not reading.

## What a cold model thinks Ziman is

Counted as the number of the 50 attempts containing each form, not occurrences.

| guess | attempts | right? |
|---|---|---|
| `fn` for a function | **50/50** | **yes** |
| `let` | **45/50** | no (`val`/`var`) |
| `print(` | **37/50** | **yes** |
| semicolon-terminated statements | **32/50** | no |
| `mut` | 23/50 | no |
| `-> T` return type | 23/50 | yes, when the type is not unit |
| capitalised primitives (`Int`, `String`, ...) | 19/50 | no (lowercase `int`, `string`) |
| a method call, `x.f()` | 17/50 | no (functions take their receiver) |
| `println(` | 14/50 | no (`print`) |
| `match` | 6/50 | no (`switch`) |
| `=>` arms | 6/50 | no |
| string interpolation (`f"`, `${}`, `#{}`) | 6/50 | no |
| `import` | 4/50 | no (`use`) |
| `..=` inclusive range | 1/50 | no |
| `use` | 0/50 | — |
| `++` for concatenation | 0/50 | — |

`fn` at 50 of 50 and `print` at 37 of 50 is the AI-first surface claim holding where it is cheapest
to hold: the two most frequent tokens in any program are already what a model reaches for. Every
row below those two is the training-distribution cost the language says diagnostics have to pay
down, and `let` at 45 of 50 is the single largest line item.

**Two habits the earlier set never saw, and they are the most common wrong ones here**: `mut`
(23/50) and capitalised primitives (19/50). Both already have good messages. **And `++`, which the
F48 re-count made the headline concatenation habit, appears zero times** -- further evidence that
the operator-for-concatenation argument was weaker than its first count suggested.

## The finding: the semicolon is a gate in front of every other diagnostic

**30 of the 50 first errors are the same message**, and in **21 of those it is on line 2**:

```
lexical error: unexpected character ';' -- Ziman statements end at a newline;
loops are 'for i in 0..n' and 'while cond'
```

That message is good -- it names the rule and two shapes that follow from it. The problem is
structural, not textual: a cold attempt dies on its first semicolon, usually in the first
statement, so **round 1 at n=1 measures the semicolon message and almost nothing else**. The `let`
habit is in 45 attempts and the `let` diagnostic fired as the first error **once**, because the
semicolon on the same line lexes first. (The `val`/`var` message fired twice in all: once for `let`,
once for `mut`.) Three consequences, and the third is the actionable one:

1. A cold first-error histogram is **not** a ranking of which diagnostics matter. It is a histogram
   of which habit happens to lex earliest.
2. Round 2 is where this instrument earns its keep, because the second compile is the first one that
   can reach a type error.
3. The habit table above, which does not depend on error order, is the durable product of round 1.

## 43 of 50 first errors teach; these 7 do not

The compiler names the Ziman way in 43 of the 50. Those are the messages that work. The remaining
seven are bare parse failures, each from a real habit, and this is the list a wave exists to produce:

| task | the line as written | first error | the habit with no message |
|---|---|---|---|
| 04-fizzbuzz | `for i in 1..=20 {` | `expected an expression, found '='` | `..=` for an inclusive range |
| 09-parse-and-add | `'0' => 0,` | `unexpected character '''` | a character literal |
| 13-regex-extract | `return c >= '0' && c <= '9';` | `unexpected character '''` | a character literal (again) |
| 29-named-and-optional | `fn label(text: String, width: Int? = nil)` | `expected ')', found '?'` | `T?` for optional, and `nil` |
| 31-spawn-and-collect | `let ch = chan[int]()` | `expected an expression, found 'int'` | `[T]` for a generic argument |
| 48-map-of-lists-invert | `fn sort_strings(items) {` | `expected ':', found ')'` | an untyped parameter |
| 49-struct-update-with | `host: String,` | `expected a field name, found ','` | comma-separated struct fields |

Two of the seven are the same habit (a character literal, 2 attempts), so it is six distinct gaps.
Ranked by what a message would save: the **untyped parameter** and the **comma-separated struct
field** are the two that a model cannot guess its way out of from the current text, and both are
one-line habits in extremely common positions. `T?`/`nil` and `[T]` are each a one-line redirect to
a spelling that exists. `..=` is rare here (1/50) but sits in the most common loop form in the
language. A character literal needs a decision first -- what Ziman offers instead -- not a message.

## What this establishes, and what it does not

It establishes what a model writes cold against these fifty tasks, with the attempts kept so the
claims above can be re-read, and it gives the committed instrument its first baseline: **0/50**.

It does not establish variance (n=1), what the compiler does when allowed to answer back (round 2
is unrun), that Ziman is easy for people, that these fifty tasks represent backend work in
proportion, or anything at all about the pre-F83 numbers. The set's author was not neutral, which
the benchmark README states and which this result cannot correct for.

**One correction to my own method, found while writing this up.** The first habit table was built
with a `\w`-based regex inside a shell function that did not expand as intended, and it reported
`x.f()` method calls as **0 of 50**. The real figure is **17 of 50** -- the regex, not the finding,
was the artefact. The table above is rebuilt with explicit `[A-Za-z_]` classes, and the lesson is
the one this project keeps relearning: a count is a measurement and needs checking against a case
you can see by hand before it is published.

**A hazard worth naming for whoever extends `fmt-check`.** The attempts in
`runs/2026-10-12-round1/` are broken Ziman by design -- none of them parses. `fmt-check` finds
`.zm` files under `docs/reviews/benchmark/reference` specifically, not under the benchmark
directory, so they are outside every gate today. Widening that `find` to the benchmark directory
would fail the build on files whose whole purpose is to be wrong.
