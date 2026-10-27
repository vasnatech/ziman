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

---

# Round 2, same day: the compiler alone

Fifty fresh agents, one per task, allowed `ziman --check` on files they wrote and nothing else — no
repository, no docs, no `--lsp`, no `--api`, no running the program. Prompt in
`docs/reviews/prompts/generation-benchmark.prompt.md`. Attempts and per-run logs in
`docs/reviews/benchmark/runs/2026-10-12-round2/`.

## The numbers

**50 of 50 compiled, up from 0 of 50 cold. 39 of 50 correct.**

| | |
|---|---|
| compiled | **50/50** (round 1: 0/50) |
| correct, strict byte equality | **39/50** |
| edits to the answer file | **1 edit in 48 of 50 runs**, 2 in the other two |
| compiler invocations | **1,401 total; median 12, mean 28, min 4, max 264** |

**The edit budget is the wrong instrument and this settles it.** F27 said so from 10 runs; at n=50
the ratio is stark — 52 edits against 1,401 compiler invocations, a factor of 27. Every run but two
produced a first-try answer file, because discovery happens in throwaway probes that cost no edits.
A five-edit budget would have passed all fifty while hiding a 264-invocation run.

## The 11 failures, all of them accounted for

Five are the benchmark's fault, not the model's. Four are one language defect. Two are another.

**Five are grading artifacts, and the method rule that would have prevented them was mine to apply.**
F27's third correction says *pre-register each task's grading flexibility*. This wave did not, so
these five are scored as failures although each satisfies its prompt as written:

| task | printed | expected | what the prompt pinned |
|---|---|---|---|
| 46-fold-average | `mean: 18.0` / `values above mean: 2` | `18.0` / `2` | "prints it, then prints how many" — no format |
| 17-bigint-factorial | `25! = 155112100433...` | `155112100433...` | "prints the factorial of 25 exactly" |
| 43-error-enum-message | `No user was found.` | `no such user` | "a function turning each into a sentence" |
| 09-parse-and-add | `error: x is not a number` | `error: not a number: x` | `error: <message>`, message text free |
| 44-try-chain | `value: 42` | `value is 42` | "the formatted value", format free |

The correct reading of these five is **44 of 50 by intent, 39 of 50 by byte equality**, and the honest
statement is that this set's `expected.txt` files are stricter than its prompts.

**Fixed and verified the same day (F85).** All fifty prompts were swept for the same ambiguity, not
just the five that happened to fail -- the other thirteen candidates a word-match flags turned out to
be properly pinned (`total: N`, `average: N`, `exit: N`, `problems: N`, `both: ...`), so the five
were the whole of it. Each now pins its wording without revealing any computed value: `17` says
"print only the number", `46` "only the two numbers, with no labels", and `09`, `43`, `44` give their
message texts. **Re-running those five under the same round-2 conditions scored 5 of 5 correct**,
which is what makes "grading artifact" a finding rather than an excuse: the attempts were already
right, and the instrument was reading them wrong. Expected outputs were not touched, and
`run.sh reference` still reports 50/50, so the only thing that moved is the prompt.

## The finding: two ways to write a compile-clean program that is silently wrong

Both defects produce exit 0 from `--check` and wrong output at run time, which is precisely what an
agent restricted to `--check` cannot detect. Six of the fifty programs — **12%** — were certified by
the compiler and wrong for a language reason.

**1. `${...}` in a string literal is accepted and printed literally (tasks 18, 49).** Verified
directly:

```
print("hello ${name}, n=${n}")   -->  --check exits 0
                                  -->  prints  hello ${name}, n=${n}
```

`docs/SYNTAX.md:21` states "No interpolation in v0", so this is the designed behaviour with no
diagnostic attached. The consequence is worse than a missing feature: **two independent round-2
agents concluded from `--check` that interpolation works**, one reporting "`\"${expr}\"` interpolation
works and stringifies int/bool". Round 1 put an interpolation form in 6 of 50 cold attempts. A
lexical warning on `${` inside a string literal would convert a silent wrong answer into a one-line
fix, and costs nothing else.

**2. `push(arr, item)` as a bare statement compiles, and does nothing (tasks 14, 36, 40, 41).**
Arrays are immutable values, so `push` returns a new array; a statement-position call discards it.
Verified directly:

```
var out: [string] = []
push(out, "a")        // accepted, no diagnostic
push(out, "b")        // accepted, no diagnostic
print(len(out))       // 0
out = push(out, "c")
print(len(out))       // 1
```

All four remaining failures contain exactly that line shape, and the symptom is an empty or
zero-length result (`36` and `40` print nothing at all; `41` prints `problems: 0`). **The fix needs
no new mechanism, because the compiler already does this for `Result`:**

```
this Result<int, fs.FsError> is discarded, silently dropping a possible error --
bind it ('val _ = ...' to discard on purpose), or use 'try' to propagate the error
```

That rule just does not extend to a discarded non-`Result` return from a pure function. Extending it
to any discarded return value whose function has no effects would have caught 4 of the 11 failures
here, and several agents explicitly reported believing `push` mutates in place.

## What the compiler taught well

The prescriptive messages repeatedly took an agent from no knowledge of Ziman to a first-try answer,
and the same handful did nearly all the work. Named by most runs, roughly in order of value:

- `Ziman has no 'let' -- declare with 'val' ... or 'var' ...` — cited by almost every run as the one
  message that mattered; several called it "essentially documentation".
- `'+' doesn't join strings ... use concat(a, b), or format("{}{}", a, b) ...; convert a number with str(n)`
  — hands over three library names in one line.
- `Ziman has no 'match' -- use 'switch value { Variant(name) { ... } ... }' ... no default arm`
  — gives the whole replacement grammar.
- `Ziman has no methods ... call a function on it instead, e.g. chars(s, ...)` — teaches the calling
  convention from one mistake.
- `unknown type 'Int' -- did you mean 'int'? (the built-in types are lowercase: ...)` — leaks the
  entire primitive list, repeatedly reported as the biggest single teaching moment.
- The `|` message, which enumerates `||`, `bits.int_or` after `use bits`, `enum`, and denies `|>`.

## What it taught badly: name discovery is the whole remaining cost

The invocation distribution is bimodal. Tasks needing only syntax cost 4-12 invocations; tasks
needing library names cost **264 (37-run-a-command), 167 (32-iterator-take), 131 (15-sha256-hex),
109 (12-json-build), 64 (01-word-frequency)** — and in each case the agent reported that most of
those were pure name-fishing through deliberate misspellings to harvest "did you mean" lists.
Reported limits of that channel, which is the only discovery channel there is:

- **No way to list a module's members.** `module 'json' has no function 'X'` names nothing that does
  exist. 12-json-build spent roughly 80 of its 109 invocations reverse-engineering `json` this way.
- **Suggestions fire only at edit distance ~1**, so `int_shl` never reaches `int_shift_left`, and
  `recv` never reaches `receive`.
- **Type names get no suggestions at all** — `json.Jso` never hints `Json`; the type surfaced only
  from an argument-mismatch message.
- **Derive-generated functions are absent from the suggestion index**, exactly where a hint is most
  needed: `to_json(u)` got nothing while `user_to_json` existed.
- **Signatures are never shown**; arity, parameter names, types and return type each need a separate
  deliberately-wrong probe.

This is the `--api` case, now with a number attached: **a module listing would have removed most of
735 invocations across five tasks.** It is the same conclusion the lost 2026-10-09 wave reached,
reproduced on a committed instrument.

**Correction, found while acting on the above and before anyone relied on it: `ziman --api` already
exists.** F34 built it and F37 added `--api builtins`; `./ziman --api fs` prints the module's `pub`
types and signatures today, and `make api-check` gates it. The paragraph above originally called it
unbuilt tier-1 work, which was wrong -- inherited from a stale note in my own memory and not checked
against the binary, the exact failure this project has recorded a dozen times (see tasks.md F67,
F81). What round 2 measured is therefore **not** a missing feature but a protocol: the run prompt
forbids `--api` on purpose, so that the diagnostics are measured alone.

That makes the real open question a different and better one. The cost is real and `--api` answers
it, so what is unmeasured is **whether a cold model reaches for `--api` at all** when nothing tells
it the flag exists. The earlier wave's note that one run used `--lsp` "because no CLI equivalent
exists" is stale for the same reason. The next measurement is a **round 3 on this committed
instrument with `--api` allowed and advertised, and a fourth arm where it is allowed but not
mentioned** -- the second is the one that says whether discoverability or the feature is the gap.

## Smaller defects worth fixing, each reported independently

- **Cascade noise after a rejected binding.** One `let` fans out into "assignment to undefined
  variable" plus "use of undefined variable" per mention — 5 to 9 errors from one cause, reported by
  at least eight runs. The declaration error should suppress its own follow-ons.
- **A self-suggestion:** `call to undefined function 'StringBuilder' -- did you mean 'StringBuilder'?`
  (the name exists as a type, not a function).
- **A false parenthetical:** `'str' expects an int, float, ... found Foo (it's already a string)` —
  untrue for a struct.
- **A misleading message:** a *trailing comma* in an all-named call reports "a call's arguments must
  be either all positional or all named, not a mix". It cost the only extra edit in the wave.
- **`..` inclusivity is undiscoverable.** `..=` is a bare syntax error (round 1 finding), and nothing
  reveals whether `..` includes its upper bound; four runs avoided ranges entirely and used `while`
  rather than risk an off-by-one. That is a correctness hazard hiding behind a style choice.

## What round 2 establishes, and what it does not

It establishes that **the diagnostics carry a cold model from 0 to 50 of 50 compiling**, that the
cost is name discovery rather than syntax, and that `--check` passing says nothing about behaviour:
12% of the certified programs were wrong for a language reason, in two shapes, both fixable.

It does not establish variance (still n=1), that these fifty tasks are representative, or anything
about the pre-F83 numbers. And the 39/50 correctness figure is **not** a clean measure of model
capability, because five of its eleven failures are the task set's strictness rather than the
model's error — which is the next thing to fix in the instrument.

---

# Round 3, same day: what `--api` is worth, and whether a model finds it

Round 2's cost finding -- 1,401 compiler invocations, nearly all name-fishing -- was measured with
`--api` **forbidden by the protocol**. `--api` exists (F34, F37), so the open question was never
whether to build it but what it is worth and whether a cold model reaches for it. This round answers
both, and the second answer is not the one the plan assumed.

**Design: paired, not another full wave.** Twelve tasks spanning round 2's invocation distribution
(264 down to 4), with the two cheapest as controls, run in two conditions. Round 2's own numbers for
the same twelve are the third condition, already measured, which makes every comparison within-task
rather than across samples. 24 runs instead of 100.

- **Arm A** -- `--api` named in the prompt, with its three forms, and explicitly encouraged.
- **Arm B** -- `--api` never mentioned. The rules permit running the binary "with whatever arguments
  it accepts" and forbid only `--lsp`, `--fmt`, `--lint` and executing the program. Nothing points
  at `--api`; `--help` is reachable, exactly as it is for a developer.

Attempts and per-run logs: `docs/reviews/benchmark/runs/2026-10-12-round3a/` and `-round3b/`.

## The numbers

| task | round 2 (no `--api`) | 3a (advertised) | 3b (not mentioned) |
|---|---|---|---|
| 37-run-a-command | 264 | 8 | 6 |
| 32-iterator-take | 167 | 6 | 6 |
| 15-sha256-hex | 131 | 3 | 17 |
| 12-json-build | 109 | 12 | 5 |
| 01-word-frequency | 64 | 6 | 5 |
| 35-sqlite-crud | 44 | 8 | 8 |
| 11-json-read-fields | 38 | 10 | 5 |
| 49-struct-update-with | 23 | 32 | 18 |
| 28-derive-ord-sort | 22 | 16 | 5 |
| 38-bits-flags | 22 | 6 | 7 |
| 46-fold-average | 9 | 8 | 4 |
| 05-binary-search | 4 | 4 | 7 |
| **total** | **897** | **119** | **93** |

Correctness: **arm A 12 of 12**, arm B 10 of 12, against round 2's 39 of 50 strict on the full set.

## Finding 1: the cost is a protocol artefact, not a gap in the compiler

**897 invocations become 119.** On the heaviest task, 264 become 8. That is the measured worth of a
module listing, and it is the same conclusion the lost 2026-10-09 wave reached, now reproduced on the
committed instrument with a paired design.

The honest restatement of round 2's headline: *a model restricted to `--check` burns roughly ten
times the compiler calls, and nothing in `--check`'s output points at `--api`.* The library surface
was never undiscoverable; it was fenced off by the measurement.

## Finding 2: discoverability is not the gap -- 12 of 12 found it unprompted

Every arm-B run ran `ziman --help`, found `--api`, and used it without being told it existed. **Arm B
is cheaper than arm A** (93 against 119) because those agents went to `--help` *first* and then read
exactly the one module they needed, where several arm-A agents probed first and consulted `--api`
later. The fourth arm this report originally proposed -- "allowed but not mentioned" as a separate
discoverability test -- was answered by arm B itself, and the premise behind proposing it was wrong.

Two tasks got *worse* with `--api` available, and both are the control working: **05-binary-search**
(4 to 7) needs no library at all, so the `--help`/`--api` detour is pure overhead, and
**49-struct-update-with** (23, 32, 18) is a *syntax* feature -- prefix `with` -- that no module
listing can reveal. `--api` buys nothing where the answer is not a name.

## Finding 3, new and unfixed: a signature is not a behaviour

**Two of arm B's three errors trace to one cause, and it is the limit of `--api` rather than of the
diagnostics.** A listing gives names, types and arity -- not what a function prints.

- **46-fold-average is wrong** because `number.float_to_text(18.0)` prints `18`, not `18.0`. The
  agent chose the function off its signature and could not check its output, because the protocol
  forbids running the program.
- **15-sha256-hex is right in spite of the same doubt**, and said so: it found `number.bytes_to_hex`
  and *declined to use it*, hand-rolling a hex encoder instead, "because the exact output format of
  `bytes_to_hex` (possible `0x` prefix or separators) could not be verified without executing the
  program". The uncertainty cost correctness in one task and 14 extra invocations in the other.

`--api` prints signatures only; the modules' own doc comments are right there in the source and do
not appear in its output. Putting the one-line doc comment next to each signature is the obvious fix
and would have decided both cases.

## Finding 4: a third interpolation syntax, and today's fix does not catch it

F86 made `"${name}"` warn when the braces hold a binding in scope. Arm B's 49 invented a **fourth**
form and concluded from `--check` that it works, reporting "String interpolation is
`\"{expr.field}\"`" -- the brace-only shape, no dollar. Verified directly:

```
print("{cfg.host}:{cfg.port}")   -->  --check exits 0, warns nothing
                                  -->  prints  {cfg.host}:{cfg.port}
```

That attempt is one of arm B's two wrong answers. So three agents across two rounds have invented
three different interpolation syntaxes (`${}`, `${a.b}`, `{a.b}`), and the committed rule covers the
first two. **The brace-only form is a harder rule and needs its own corpus sweep**, because `{}` is
Ziman's own format placeholder: any such warning has to exempt a literal that is the format argument
of `format`/`printf`, where braces are not only legal but required. That is the next piece of work,
and it came out of this round rather than the last.

## What round 3 establishes, and what it does not

It establishes the worth of `--api` within-task (897 to 119), that a cold model finds it from
`--help` without being told (12 of 12), that it buys nothing for syntax features, and that its
signature-only output leaves a semantic gap that cost one of twelve tasks its correctness.

It does not establish anything at n=1 per cell about variance, and twelve tasks chosen *because*
round 2 found them expensive is a biased sample for the 10x figure -- deliberately so, since the
point was to measure the ceiling on the tasks where name discovery dominates. The two controls are
in the table for exactly that reason, and they are where the effect reverses.

---

# Variance: a second instance, against a pinned round-2 compiler

Round 2 ran at n=1, and by the time variance was worth measuring the compiler had changed under it --
F86 and F88 made two silently-wrong programs into diagnostics, F90 a third message. A second instance
against today's binary would have mixed variance with those fixes, so **the round-2 compiler was
rebuilt from its own commit** (`7d9baa7`) in a worktree and verified to behave as it did: it accepts a
discarded `push` silently, emits no interpolation warning, and self-checks 50/50 on the reference
solutions. Both the attempts and the scoring use that binary.

**45 of the 50 tasks, not all 50.** F85 amended five prompts, so those five cannot be compared across
instances; `round2b/` is already their second instance under the new wording. The other 45 prompts
were taken from the pinned worktree and `cmp`-verified identical, and exactly five differed, which is
the check that the exclusion is right rather than assumed.

## The aggregate is stable; the individual task is not

| | round 2 | second instance |
|---|---|---|
| compiled | 45/45 | **45/45** |
| correct | 39/45 | **37/45** |
| invocations, total | 1,338 | **1,194** |
| invocations, median | 12 | **13** |
| invocations, max | 264 | **194** |

**Per-task agreement is the real finding: 35 right in both, 4 wrong in both, and 6 of 45 flipped
(13%).** Two tasks round 2 got wrong came out right, and four it got right came out wrong:

| task | round 2 | instance 2 | |
|---|---|---|---|
| 14-base64-roundtrip | wrong | right | the discarded `push` did not recur |
| 40-matrix-transpose | wrong | right | ditto -- and its log shows why: the *no-methods* message handed this run `xs = push(xs, item)` |
| 22-url-encode | right | wrong | |
| 26-defer-order | right | wrong | |
| 45-set-operations | right | wrong | its log states "`push(v, x)` mutates a `var` array in place" -- the same false belief, in a task that did not hit it in round 2 |
| 48-map-of-lists-invert | right | wrong | |

So **a single instance of this benchmark predicts the headline to about ±2 of 45 and an individual
task's outcome only about 87% of the time.** Any claim of the form "this task fails" needs n>1; a
claim of the form "roughly six of these fail" is safe at n=1. That is the honest licence for every
per-task statement made earlier in this report, and the four tasks wrong in both instances
(18-money-decimal, 36-yaml-list, 41-validate-collect-errors, 49-struct-update-with) are the ones that
earned their findings twice over.

**Cost varies much less than the per-task outcome.** The per-task ratio has a median of exactly 1.00
and 41 of 45 tasks land within 2x of their first instance. The tail moves most: `12-json-build` fell
109 to 33, `01-word-frequency` 64 to 26, while `10-palindrome` rose 6 to 18 and `32-iterator-take`
167 to 194. The 10x `--api` effect measured in round 3 is an order of magnitude larger than this
spread, so that finding survives the variance; a 20% difference in invocations would not have.

## What the second instance found that the first did not

Two of the three defects this report already names **recurred in tasks that had not shown them**,
which is stronger evidence than the original instance gave:

- **The discarded `push`**: `45-set-operations` wrote down "`push(v, x)` mutates a `var` array in
  place" and was wrong for it, where in round 2 that task was correct and four *other* tasks carried
  the bug. The mistake is a property of the language's surface, not of particular tasks.
- **Interpolation**: `49-struct-update-with` and `21-file-write-read` both recorded `"${x}"` as
  working. Across rounds 2, 3 and this instance, **four independent agents reached that belief**, in
  three different spellings.

And one new, verified defect, from the run that cost the most (`32-iterator-take`, 194 invocations):
**the no-methods message invents a function that does not exist.** `a.iter()` is answered with "call a
function on it instead, e.g. `iter(a, ...)`" -- there is no `iter`. That run concluded `Iter<T>` is
"visible in the type system and unreachable from source", while `iter_of_array` exists and round 3's
arm A found it immediately through `--api builtins`.

**One claim in the first write-up of this was wrong, and the fix is better for it.** I wrote that the
message "already looks the name up", on the evidence that `a.len()` prints `len(a)` with the right
arity. It does not: `len` is one of ten **hand-written special cases** that F28 added for exactly this
reason -- naming a function that does not exist costs the reader a round -- and the fallback's own
comment says the pass "has no scope to look the name up in". So the defect was real and my account of
the mechanism was not. Fixed in F92 by giving it a scope to look in: the **parsed prelude**, which is
the source `--api builtins` prints from and, as `main.c` records there, the only honest answer to
"does this exist" (`BUILTINS_NO_NAMED_ARGS` is the subset taking no named arguments, so it would
report `chars` as absent).

## What variance establishes, and what it does not

It establishes that this instrument's aggregate is reproducible (45/45 compiled twice, 39 vs 37 of 45
correct) while its per-task verdicts are not (13% flip), that cost is stable in the median and noisy
in the tail, and that the two language defects recur across instances in different tasks.

It does not establish variance for the five amended tasks, anything at n>2, or that the flip rate is
13% rather than some nearby number -- 6 flips out of 45 is itself a small count, and a third instance
could move it by several points.
