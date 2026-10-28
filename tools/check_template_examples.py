#!/usr/bin/env python3
"""Checks that the worked example above each `template.must_parse(` is text the generator emits.

A generator's template says what the *shape* of its output is; the comment above it says what the
output actually looks like for one declaration, which is the half a reader wants first. That
comment is a copy of generated text, and a copy of generated text goes stale -- the same failure
F168 found in `src/lsp.c`'s hand-kept builtin list and F212 found in two name lists that asserted
nothing at all. So it is checked rather than trusted.

The rule, deliberately loose about *which* fixture: every run of `//` lines indented five spaces,
directly above a `template.must_parse(`, must appear verbatim in some `tests/lang/*.out`. The
fixtures already pin each generator's output byte for byte, so "appears in one of them" is the
same claim as "this is really what it writes" without a hand-kept pairing of comment to fixture --
which would be one more list to go wrong.

Run directly, or via `make docs-check`. Exits 1 naming every example that is not generated text.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLE = re.compile(r"^//( {5})(.*)$")


def examples_in(path):
    """Each (line number, text) example block directly above a `template.must_parse(`."""
    with open(path) as f:
        lines = f.read().split("\n")
    out = []
    for i, line in enumerate(lines):
        if "template.must_parse(" not in line:
            continue
        # Walk up over the comment block, collecting the last contiguous run of indented lines.
        block, j = [], i - 1
        while j >= 0 and lines[j].startswith("//"):
            block.append(lines[j])
            j -= 1
        block.reverse()
        run, started = [], False
        for c in block:
            m = EXAMPLE.match(c)
            if m:
                run.append(m.group(2))
                started = True
            elif started and c.strip() == "//":
                run.append("")
            elif started:
                break
        while run and run[-1] == "":
            run.pop()
        if run:
            out.append((i + 1, "\n".join(run)))
    return out


def main():
    expected = ""
    for out_file in glob.glob(os.path.join(ROOT, "tests", "lang", "*.out")):
        with open(out_file) as f:
            expected += f.read() + "\n"
    checked, bad = 0, 0
    for path in sorted(glob.glob(os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True)):
        for line_no, text in examples_in(path):
            checked += 1
            if text in expected:
                continue
            bad += 1
            rel = os.path.relpath(path, ROOT)
            print("%s:%d: the example above this template is not text any fixture shows being "
                  "generated -- regenerate it from tests/lang/*.out" % (rel, line_no), file=sys.stderr)
            print("  first line: %r" % text.split("\n")[0], file=sys.stderr)
    if bad:
        return 1
    print("check_template_examples.py: %d worked example(s), each one generated text" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
