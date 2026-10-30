#!/usr/bin/env python3
"""Checks that a design doc's `Status:` line is not contradicted by its own body.

**Why this exists: three documents drifted the same way.** The work gets recorded where it
happened -- a staging step struck through, a decision closed, a section headed "Built
2026-10-30" -- and the `Status:` line at the top, which is the one thing a reader sees first, is
never touched. `docs/design/expressions.md` said "design, not built" for a day after all five of
its steps shipped; `meta.md` and `user-generators.md` said the same with their own bodies
recording otherwise. Nothing caught any of it, because `check_not_yet.py` reads `lib/` and
docs/roadmap/ and never looks at a design doc's header.

The rule is a **contradiction inside one file**, which is the only thing a script can honestly
decide here -- it cannot know whether a document describes something built:

1. every `docs/design/*.md` has a `Status:` line, and
2. a status claiming nothing is built must not sit above a dated "this shipped" marker.

A marker is this repository's own convention: a bolded `DONE`/`CLOSED`/`Built`/`Arrived`/`Partly
built` followed by a date, or a section heading saying built/shipped/done with one. The date is
required on purpose -- `**Built as one parameter, not two.**` is a sentence about a design and
`**CLOSED: kept, both.**` is a decision that needed no code, and neither is evidence that anything
shipped.

Run directly, or via `make docs-check`. Exits 1 naming every contradiction.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Two spellings, both already in use: a `Status:` line, and a `| Status | .. |` row in the summary
# table `full-overloading.md` and its superseded sibling open with. A gate that knew only the first
# reported those two as missing a status they plainly have.
STATUS = re.compile(r"^(?:Status:|\| *Status *\|)\s*(.*?)\s*\|?\s*$", re.M)
# A status that claims nothing is built. Deliberately a short list of this repo's own phrasings
# rather than a guess at English: a wrong match here fails a healthy document.
UNBUILT = re.compile(r"not built|design only|nothing below is built|not implemented", re.I)
MARKERS = [
    re.compile(r"\*\*(?:DONE|CLOSED|Built|Arrived|Partly built),? 20\d\d-\d\d-\d\d", re.I),
    re.compile(r"^#+ .*\b(?:built|shipped|done)\b.*20\d\d-\d\d-\d\d", re.I | re.M),
]


def main():
    bad = 0
    checked = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "design", "*.md"))):
        rel = os.path.relpath(path, ROOT)
        with open(path) as f:
            text = f.read()
        found = STATUS.search(text)
        if found is None:
            print("%s: no 'Status:' line -- every design doc says whether it is built, so that a "
                  "reader knows before reading and so that the check below has something to check"
                  % rel, file=sys.stderr)
            bad += 1
            continue
        checked += 1
        status = found.group(1)
        if not UNBUILT.search(status):
            continue
        for marker in MARKERS:
            hit = marker.search(text)
            if hit is None:
                continue
            print("%s: the status says %r, and the body says %r -- one of the two is stale"
                  % (rel, status.strip()[:60], hit.group(0).strip()[:70]), file=sys.stderr)
            bad += 1
            break
    if bad:
        return 1
    print("check_design_status.py: %d design doc(s), each status agreeing with its own body" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
