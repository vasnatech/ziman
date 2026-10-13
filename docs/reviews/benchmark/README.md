# Generation benchmark: 50 tasks

Fifty small programming tasks, each with a prompt, the output a correct answer must produce, and a
reference solution kept well away from the prompt. The question they exist to answer is the one
this language is built around: **what does a model that has never seen Ziman actually produce, and
what does the compiler tell it when that is wrong?**

| | |
|---|---|
| Authored | 2026-10-12, by Claude (Opus 5), in-session |
| Tasks | 50, numbered `01`..`50` |
| Verified | every `expected.txt` is the real stdout of the matching `reference/*.zm`, captured by running it |
| Self-check | `run.sh reference` scores 50 compiled, 50 correct |

## Read this before using it as a measurement

**This is a new instrument. It does not continue the earlier numbers.** The 2026-09-28 and
2026-10-09 benchmark reports in the directory above were run against a different, earlier task set
which is **not in this repository and was not recoverable** -- it lived in a cleared working
directory, which is the whole reason this one exists and is committed. So the famous "0 of 50 cold"
figure belongs to that set. **Nothing here may be compared with it.** Treat results from these
tasks as a new baseline starting at zero.

**The author is not neutral, and that is a real limitation.** These were written by a model that had
spent a long session inside Ziman's internals -- fixing the formatter, the type checker, the editor
grammar and the language server. Someone who knows where the sharp edges are cannot help but choose
tasks that sit near or away from them, so a headline number from this set partly measures the task
selection. Two deliberate counterweights: every task is phrased as a **realistic requirement** ("count
word frequencies", "validate a registration", "page a list") rather than as a probe of a known
failure mode, and coverage was driven by **library surface** -- strings, arrays, maps and sets,
sorting, structs, enums, `Option`, `Result`, JSON, CSV, TOML, YAML, regex, crypto, dates, files,
SQLite, processes, concurrency, iterators, bigint, decimal, bits -- rather than by difficulty. It is
still an author-biased set and a genuinely independent one would be worth more.

**Authoring found friction, which is itself a result.** Writing the fifty reference solutions took
several wrong turns that a reader should know about, because a cold model will hit the same ones:
sorting is `collections.sort_with` and not a `sort_by` (three attempts to find); `map_keys` returns a
`Set` that needs `set_items` before it can be sorted, which was discoverable only by grepping
`lib/core/metrics.zm`; a `switch` used as a value needs an annotation when an arm yields an empty
array literal; `switch` is exhaustive with **no** default arm, so `_` is not a wildcard; and
`number.format_number` takes a pattern through `parse_pattern` rather than a struct literal. None of
these is a defect. All of them are what `ziman --api` exists for.

## Layout

```
tasks/NN-slug/prompt.txt     the task, exactly as a model should receive it
tasks/NN-slug/expected.txt   stdout a correct answer prints, byte for byte
reference/NN-slug.zm         a working answer -- NOT to be shown to anyone being measured
run.sh                       scores a directory of attempts
```

**`reference/` is separate from `tasks/` on purpose.** An agent must be handed the *contents of
`prompt.txt`* and nothing else. Pointing one at a task directory, or at this directory, hands it the
answer and the run is worthless. The split makes that an explicit mistake rather than an easy
accident.

**Hand the prompt over by path or by file, never by retyping it.** On 2026-10-10 a ten-task wave was
discarded because four prompts had been hand-transcribed and diverged from their source
(docs/reviews/tasks.md F49) -- and discarding only the four would have left a tainted comparison, so
all ten were re-run. The prompts are files for this reason.

## Running it

```
docs/reviews/benchmark/run.sh <attempts-dir> [ziman-binary]
```

`<attempts-dir>` holds one file per task named after the task directory -- `04-fizzbuzz.zm`. A task
with no attempt counts as **missing** rather than being skipped quietly, so a half-finished run
cannot score like a complete one.

**The grader is byte-exact, and the tasks' prose is not**, so whoever is being measured has to be
told that: the run prompt carries one line, "print exactly what the task asks for and nothing else".
Without it a program that prints `Total: 25.50` for a task asking it to "print the total" scores as
wrong, which is the instrument's fault and not the model's -- seven tasks across F85 and F90 were
scored that way before the line existed.

Two numbers come out and they answer different questions. **compiles** is `ziman --check`: did the
model write Ziman the compiler accepts. **correct** is stdout against `expected.txt`: did it answer
the question. The earlier waves found these diverge sharply, so neither number means much alone.

To confirm the instrument itself still holds:

```
docs/reviews/benchmark/run.sh docs/reviews/benchmark/reference
```

That must report 50 compiled and 50 correct. If it does not, a language or library change has moved
under the task set, and **the tasks are what need revisiting, not the result** -- an `expected.txt`
is a record of what Ziman did on 2026-10-12, not a specification of what it should do.

## What a run does and does not establish

It establishes what a model produces cold, and whether the diagnostics move it toward an answer when
it iterates. It does **not** establish that Ziman is easy to learn for people, that these fifty tasks
represent backend work in proportion, or that a score here transfers to a program of real size -- the
reference backend is 13,000 lines and nothing in this set is longer than forty.

Three tasks touch the world outside the process (`21` writes a file, `35` an SQLite database, `37`
runs `echo`), all under `/tmp` with fixed paths, and `31` starts threads. Nothing uses the network,
the clock or a random source, so every expected output is deterministic.
