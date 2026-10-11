#!/usr/bin/env python3
"""Compares a fixture's expected diagnostics against what `ziman --json` actually reported.

Usage: check_diag.py <expected.diag> <actual-json-file>
Exit 0 when every expectation holds, 1 otherwise with the mismatch on stdout.

The point of this over the `.err` fixtures' substring match is **position**: a `.err` file can say
that a message appeared somewhere in stderr, but not that it was reported at the right line and
column. A diagnostic that names the right problem several tokens away from it is a real defect --
tasks.md F50 and F53 are both exactly that -- and nothing in 452 substring fixtures could catch
one.

Each non-empty, non-`#` line of the expected file is one diagnostic, in the order the compiler
reports them:

    syntax 10:28 Ziman has no methods

`<phase> <line>:<col>` then an optional message substring. Leaving the substring off asserts the
position and says nothing about the wording, which is the whole reason a fixture about *where* an
error is reported should not break when someone rewords it. A column of `*` asserts the line only,
for the few cases where a column is genuinely an implementation detail.

A final line of `...` means "and possibly more after these", for a program whose first mistake
cascades. Without it the count must match exactly, which is what catches an extra diagnostic
nobody meant to add.
"""
import json
import sys


def parse_expected(path):
    out = []
    allow_more = False
    for raw in open(path):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.strip() == "...":
            allow_more = True
            continue
        parts = line.split(None, 2)
        if len(parts) < 2 or ":" not in parts[1]:
            sys.exit(f"check_diag.py: {path}: cannot read '{line}' -- want '<phase> <line>:<col> [substring]'")
        phase = parts[0]
        where = parts[1].split(":", 1)
        want_line = where[0]
        want_col = where[1]
        needle = parts[2] if len(parts) > 2 else ""
        out.append((phase, want_line, want_col, needle))
    return out, allow_more


def parse_actual(path):
    out = []
    for raw in open(path):
        raw = raw.strip()
        if not raw.startswith("{"):
            continue  # a non-JSON line (a panic, a trace) is reported by the caller, not parsed here
        try:
            out.append(json.loads(raw))
        except json.JSONDecodeError as e:
            sys.exit(f"check_diag.py: {path}: not JSON: {e}: {raw[:120]}")
    return out


def describe(d):
    return f'{d.get("phase")} {d.get("line")}:{d.get("col")} {d.get("message", "")[:90]}'


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check_diag.py <expected.diag> <actual-json-file>")
    expected, allow_more = parse_expected(sys.argv[1])
    actual = parse_actual(sys.argv[2])
    problems = []

    if not allow_more and len(actual) != len(expected):
        problems.append(f"expected {len(expected)} diagnostic(s), got {len(actual)}")
    elif len(actual) < len(expected):
        problems.append(f"expected at least {len(expected)} diagnostic(s), got {len(actual)}")

    for i, (phase, want_line, want_col, needle) in enumerate(expected):
        if i >= len(actual):
            problems.append(f"#{i + 1}: missing -- wanted {phase} {want_line}:{want_col} {needle}")
            continue
        got = actual[i]
        if got.get("phase") != phase:
            problems.append(f'#{i + 1}: phase is {got.get("phase")}, wanted {phase}')
        if str(got.get("line")) != want_line:
            problems.append(f'#{i + 1}: line is {got.get("line")}, wanted {want_line}')
        if want_col != "*" and str(got.get("col")) != want_col:
            problems.append(f'#{i + 1}: col is {got.get("col")}, wanted {want_col}')
        if needle and needle not in got.get("message", ""):
            problems.append(f'#{i + 1}: message does not contain {needle!r}')

    if problems:
        for p in problems:
            print(f"  {p}")
        print("  --- diagnostics actually reported ---")
        for i, d in enumerate(actual):
            print(f"  #{i + 1}: {describe(d)}")
        return 1
    return 0


sys.exit(main())
