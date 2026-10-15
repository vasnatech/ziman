# LLM-generation benchmark (F6, P2-7, review §6 risk 4)

**50 tasks since 2026-10-09 (F26).** Tasks 01-10 are the original set the 2026-09-28 report ran;
11-50 were added to reach the scope the review asked for, because ten tasks cannot separate a real
effect from which error a given run happened to hit first.

A fixed task set to measure, empirically, the claim DESIGN.md makes throughout ("cheap for an
LLM to generate correctly") instead of only asserting it. The review named this gap directly:
"'Cheap for an LLM to generate' is asserted, not measured. No experiment in the repo measures
generation accuracy."

## Method

Each task is a `.md` file: a prompt written the way an ordinary developer would ask an LLM to
write a small program, with no Ziman-specific hints, and an expected stdout to grade against.
Two rounds:

1. **Cold generation.** A fresh model instance (no memory of any other run, no Ziman
   documentation shown -- only that the target language is called "Ziman") writes its best
   attempt for each task and saves it, without compiling or running it. This measures the
   language's *training-distribution cost* directly (see DESIGN.md's "What the AI-first goal
   does and does not justify"): what a model's untouched habits produce when told to write in a
   language it has seen little or nothing of.
2. **Iterate against the compiler, given nothing but its own diagnostics.** For every task that
   failed round 1, a second fresh model instance gets the original attempt, the task prompt
   again, and the `ziman` binary itself -- nothing else about the language -- and is told to run
   `ziman --check`, read whatever it says, fix the file, and repeat until it compiles or it has
   made 5 attempts. (The first round-1 run showed several files fail on more than one distinct
   habit at once -- a semicolon *and* `let` *and* `match`/`=>` in the same file -- so capping the
   retry at exactly one look, the original plan, would have undercounted a diagnostic chain that
   actually resolves a file in three or four short hops.) This is the sharper, more direct test
   of the language's own stated mitigation for the training-distribution cost: "a model's habit
   should fail at compile time with the sentence that fixes it" (same DESIGN.md entry). A flat
   "does it work with the docs in context" number would mostly just measure how good the model is
   at reading a spec; this measures whether Ziman's specific diagnostics, followed the way a
   model would actually follow them, are the thing doing the work.

Grading is `ziman --check` (does it compile), then running it and diffing stdout against the
task's expected output (allowing for stated flexibility, e.g. task 06's exact wording).

## How tasks 11-50 were chosen, and the one rule that mattered

**Written blind to Ziman's diagnostics.** The whole point of round 2 is to measure whether this
language's error messages are what carries a model to a working program, so a task set chosen by
looking at the diagnostics would be marking its own homework -- it would measure the messages that
already exist, and the 2026-09-28 report's own finding (that a diagnostic naming an exact fix is
what let a model self-correct) is exactly the kind of result that would be manufactured by it. None
of the 40 was picked from an error message, a `tests/lang/diag_*` case, or a known habit
collision.

**The line drawn instead.** Tasks were chosen from what ordinary small programs *do* -- strings,
lists, maps, records, variants, errors, higher-order functions, recursion, files, dates, JSON, text
processing, numerics -- and then checked against the *library* to be sure each one is expressible,
which is a different thing from checking it against the diagnostics. A task that fails because
`fs` has no function for it would measure a library gap, not a generation habit; a task that fails
because a model wrote `let` would measure exactly what this benchmark is for. The 10 original tasks
lean toward feature showcases (`Result`, an enum, a map, a higher-order function, JSON); the 40 new
ones deliberately do not, so the set as a whole looks more like what people actually ask for.

**Every expected stdout was verified, and the proof is deliberately not in this repository.** Each
of the 40 has a reference solution that was written and run at authoring time, and all 40 produce
their task's expected output exactly. Those solutions are **not committed**, and that is the point:
a round-1 run is an agent with this repository in front of it, and a worked answer sitting next to
the prompt would quietly turn a cold-generation measurement into a reading-comprehension one. To
re-verify, write solutions outside the tree and diff each against the fenced block in its task
file.

**A task must say what to print, not only what to compute.** The 2026-10-09 holdout lost 3 of 10
to this: the programs were right and said so in a sentence (`found 7 at index 3`, `lines: 3`,
`ok: 4`) where the expected block was a bare number. A human would label it too, so the fault is
the prompt's. Task 08 had the pattern from the start -- "prints **just** the count" -- and tasks 22,
37 and 42 have been brought into line with it. When a new task's answer is a bare value, say so in
the prompt; when any shape is acceptable, say that in the `## Expected stdout` heading *before* the
run, never after seeing a result.

**What this does not make comparable.** The 2026-09-28 numbers (0/10 cold, 4/10 after compiler
iteration) are for tasks 01-10. A 50-task run produces a new baseline, not a continuation of that
one -- the per-number comparison to keep is within a single run.

## Results

See the dated reports: `docs/reviews/llm-benchmark-2026-09-28.md` (tasks 01-10) and
`docs/reviews/llm-benchmark-2026-10-09.md` (tasks 11-50, in four waves). **As of 2026-10-10 all 50
tasks have been run at least once**, one instance each; round 1 is 0 of 50 across five independent
batches. A second instance per task is still unrun, and is the remaining methodological gap. Re-run by
hand (spawn a fresh agent per task per round, save its output under a scratch directory, grade
with the `ziman` binary) -- there is no automated runner, since driving an actual model
generation is not a shell script's job. The task files themselves are the fixed, reusable part.
