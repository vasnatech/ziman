# The generation-benchmark run prompt

The prompt handed to each agent in a benchmark wave, and the method around it. It lives here, next
to the review prompts, because **a wave whose prompt is not written down cannot be repeated** --
2026-10-10 discarded a ten-task batch over four hand-transcribed task prompts (`tasks.md` F49), and
2026-10-12 found the whole earlier task set unrecoverable (F82, F83). The prompt is as much of the
instrument as `run.sh` is.

## Round 1: cold generation

One fresh agent per task. It gets the task text and nothing else -- no repository, no compiler, no
documentation -- so what it writes is training-distribution output. Verbatim, with `<task>` the
task directory's name and `<staging>` a directory holding **only** prompt copies:

```
You are writing a program in **Ziman**, a programming language for which you have been given no documentation.

Read exactly one file — your task:
`<staging>/prompts/<task>.txt`

Rules. These exist because this is a measurement of what a model produces cold, and breaking one invalidates the run:
- Read **no** other file. Do not search the filesystem. Do not look for Ziman source code, documentation, examples, tests, a standard library, or any other task. Do not read anything under any `ziman` directory.
- Do not run any compiler or interpreter. Do not run a `ziman` binary. Do not use Bash at all.
- Guess the language from what the task needs. Write the program you believe is correct — no placeholders, no TODOs, no comments hedging about uncertainty, no alternative versions.
- Write your single best attempt, complete and self-contained, to:
  `<staging>/attempts/round1/<task>.zm`
- Do not ask questions; no one can answer them.

When the file is written, reply with one line: the task name, and nothing else.
```

**Why a staging directory and not the task directory.** `tasks/NN-slug/` holds `expected.txt` beside
`prompt.txt`, so an agent pointed at the task directory can read the answer. Each prompt is `cp`'d
to `<staging>/prompts/<task>.txt` and every copy verified with `cmp` against its source -- copied,
never retyped, which is F49's lesson mechanised.

**The screen, not the fence.** The rules are detection, not prevention: `screen.sh` greps every
attempt for library spellings that exist nowhere but Ziman (`fs.read_text`, `mapb_`, `set_items`,
`collections.sort_with`, ...), deliberately **not** generic keywords like `val`, `switch` or `try`,
which a cold model reaches for by accident. A flagged attempt means that run was not cold.

## Round 2: iterate against the compiler alone

Same task, a fresh agent, allowed `ziman --check` and nothing else. Four rules the pilot earned
(`tasks.md` F27, F28), each of which was found by getting it wrong first:

- **Count compiler invocations, not edits.** Runs made 1-4 edits while making 10-15 compiler calls,
  because discovery happens in throwaway probe files. An edit budget understates the cost by an
  order of magnitude.
- **Forbid `--lsp`.** One run used completion to list names. That is compiler output, but it is far
  more than a diagnostic, and a pass obtained that way is marked assisted.
- **Pre-register each task's grading flexibility** before the run, not after a judgement call.
- **The repository stays closed in round 2 as well.** `--api` is a separate, third condition; mixing
  it in measures the flag rather than the diagnostics.

## What a wave is for

The score is not the product. **The habit table and the first-error texts are.** F50 and F51 both
came out of reading attempt files from a wave that scored 10 of 10 -- a result that on its own says
"nothing to fix" while three real diagnostic defects sat in the attempts.
