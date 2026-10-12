#!/usr/bin/env bash
# Scores a set of attempts against the benchmark tasks.
#
# Usage: docs/reviews/benchmark/run.sh <attempts-dir> [ziman-binary]
#
# <attempts-dir> holds one file per task named after the task directory, e.g.
# `04-fizzbuzz.zm`. A task with no attempt counts as a miss, which is deliberate: a run that
# silently skipped half the set would otherwise score the same as one that passed it.
#
# Two numbers come out, and they are different questions. "compiles" is `ziman --check`: did the
# model produce Ziman the compiler accepts. "correct" is the program's stdout against the task's
# own expected.txt: did it produce the answer asked for. The 2026-09/10 waves found these diverge
# sharply -- a cold model's output compiled 0 times out of 50 -- so reporting one without the other
# hides the finding.
set -uo pipefail
# Both arguments are resolved against the caller's directory *before* this moves to its own, so a
# relative path like `./ziman` or `attempts/` means what the caller meant by it. Getting this wrong
# is the first thing that happened when this script was used from the repository root.
attempts=$(realpath "${1:?usage: run.sh <attempts-dir> [ziman-binary]}")
BIN=$(realpath "${2:-$(dirname "$0")/../../../ziman}" 2>/dev/null || echo "${2:-}")
cd "$(dirname "$0")"
[ -x "$BIN" ] || { echo "no ziman binary at ${2:-<repo root>/ziman}" >&2; exit 2; }
[ -d "$attempts" ] || { echo "no such attempts directory: $attempts" >&2; exit 2; }

compiles=0 correct=0 total=0 missing=0
for dir in tasks/*/; do
  name=$(basename "$dir")
  total=$((total + 1))
  attempt="$attempts/$name.zm"
  if [ ! -f "$attempt" ]; then
    printf 'MISSING  %s\n' "$name"
    missing=$((missing + 1))
    continue
  fi
  if ! "$BIN" --check "$attempt" >/dev/null 2>&1; then
    printf 'NOCOMPILE %s\n' "$name"
    continue
  fi
  compiles=$((compiles + 1))
  got=$(timeout 30 "$BIN" "$attempt" 2>/dev/null)
  if [ "$got" = "$(cat "$dir/expected.txt")" ]; then
    printf 'CORRECT  %s\n' "$name"
    correct=$((correct + 1))
  else
    printf 'WRONG    %s\n' "$name"
  fi
done
echo "----"
echo "$total tasks: $compiles compiled, $correct correct, $missing missing"
