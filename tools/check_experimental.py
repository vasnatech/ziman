#!/usr/bin/env python3
"""Catches README.md's "experimental" markers drifting from the modules' own `#![experimental]`.

A module says once, in its own source, that it is experimental -- the module-level
`#![experimental]` attribute. docs/CORELIB.md and docs/lib/*.md are *generated* from that
(tools/gen_corelib_reference.sh, tools/gen_lib_manual.py), so they cannot disagree with it. The
README's module table is hand-written and marks an experimental module with a `\\*` after its name,
which can: a module gaining or losing the attribute leaves that table saying the opposite, silently,
exactly the way docs/ROADMAP.md's own entry was still claiming `format` had no brace escape long
after it grew one.

So this compares the two sets and fails when they differ. It is the cheap half of the problem --
the expensive half, generating the README table outright, is not worth it for one footnote.

Run directly, or via `make experimental-check` (and `make docs-check` with the other doc guards).
Exits 1 listing each disagreement; exits 0 otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def modules_with_attribute():
    found = set()
    for path in glob.glob(os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True):
        with open(path) as f:
            if any(line.strip() == "#![experimental]" for line in f):
                found.add(os.path.basename(path)[: -len(".zm")])
    return found


def modules_starred_in_readme():
    """Every `\\`name\\`\\*` in the README's module table -- a backticked module name immediately
    followed by an escaped asterisk, which is how that table marks one experimental."""
    with open(os.path.join(ROOT, "README.md")) as f:
        text = f.read()
    return set(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`\\\*", text))


def main():
    attributed = modules_with_attribute()
    starred = modules_starred_in_readme()
    problems = []
    for name in sorted(attributed - starred):
        problems.append(
            "lib/**/%s.zm has `#![experimental]` but README.md's module table does not mark "
            "`%s` with a `\\*`" % (name, name)
        )
    for name in sorted(starred - attributed):
        problems.append(
            "README.md marks `%s` experimental with a `\\*`, but lib/**/%s.zm has no "
            "`#![experimental]`" % (name, name)
        )
    if problems:
        for p in problems:
            print("check_experimental.py: %s" % p, file=sys.stderr)
        sys.exit(1)
    print(
        "check_experimental.py: README.md agrees with #![experimental] (%d module(s): %s)"
        % (len(attributed), ", ".join(sorted(attributed)) or "none")
    )


if __name__ == "__main__":
    main()
