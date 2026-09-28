# LLM-generation benchmark (F6, P2-7, review §6 risk 4)

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

## Results

See the dated report this run produced: `docs/reviews/llm-benchmark-2026-09-28.md`. Re-run by
hand (spawn a fresh agent per task per round, save its output under a scratch directory, grade
with the `ziman` binary) -- there is no automated runner, since driving an actual model
generation is not a shell script's job. The task files themselves are the fixed, reusable part.
