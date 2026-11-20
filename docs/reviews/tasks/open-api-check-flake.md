# Open: `api-check` fails intermittently under `make`, never on its own

Observed 2026-11-20, twice, in otherwise clean runs of `make check-fast`. **Not fixed, and not
diagnosed.** Written down so the next person does not start from nothing, and because a gate that
goes red at random is worse than one that goes red: it trains everyone to re-run until green, which
is exactly how a real failure gets waved through.

## What was seen

Two failures, each a *different* assertion in `tests/run_api.sh`:

| Run | Assertion | Reported output |
|---|---|---|
| 1 | `fn mutable_array_new` missing from `--api builtins` | 18186 bytes, 344 lines |
| 2 | `Io(string)` missing from `--api net` | 18232 bytes, 292 lines |

Run on its own immediately afterwards, each passed: 41 passed, 0 failed. `./ziman --api net` is
**293 lines and contains `Io(string)`**, stably.

**One line short, in two unrelated modules.** That is the shape of the thing: not a wrong answer,
a short one.

## What has been ruled out

- **The script.** `tests/run_api.sh ./ziman` run eight times in a row: **0 failures**.
- **Parallel make.** `MAKEFLAGS` is empty; no `-j` anywhere in the Makefile.
- **A shared temp path.** The script uses `mktemp -d` and removes it on exit.
- **`test-nogc` disturbing the binary.** Before and after `make test-nogc`, `./ziman` has the same
  md5 and `--api net` the same 293 lines.

## Where to look next

The difference that remains is *everything else `make check-fast` does before `api-check`*: the
full `run_tests.sh`, `test-nogc`, `unit-tests`. Something in that sequence leaves the process or
the filesystem in a state where one line of `--api` output goes missing. A loop running
`make check-fast` and keeping every failing log would give more than guessing does -- the two
samples here are the whole evidence.

Worth noting that **this gate had never run in a green gate before 2026-11-20**: it sits behind
`docs-check`, where make halts whenever the regenerated docs are uncommitted, so it was invisible
until that check started passing. Its flakiness may be old.
