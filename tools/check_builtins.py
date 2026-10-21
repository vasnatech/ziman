#!/usr/bin/env python3
"""Fails when a builtin the interpreter dispatches has no `builtin fn` declaration and no reason.

The compiler already enforces the other direction: a `builtin fn` naming something the interpreter
does not dispatch is a type error (typecheck.c). What it cannot enforce is the *absence* of a
declaration, which is the failure that started F148 -- a builtin with no signature anywhere is one
`--api builtins` can only name, an editor cannot complete, and a reader cannot find. That cost
three corrections in one session.

So every dispatched name must either be declared in lib/prelude/ or appear below with the reason
it cannot be. "Not yet done" is a reason; the point is that it is written down rather than silent.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The reasons are **not kept here.** `src/typecheck.c`'s `BUILTIN_NO_SIGNATURE` is the single
# source -- the same file the dispatch table lives in -- and this reads them out of it, the way
# tools/check_not_yet.py reads the dispatch table rather than repeating it. Its header states the
# principle: "a new builtin should not need an edit in this file to be known about". A second copy
# here is a list that goes stale, which is the failure F148 exists to stop.
TYPECHECK_C = ROOT / "src" / "typecheck.c"


def recorded_reasons():
    """name -> reason, from typecheck.c's BUILTIN_NO_SIGNATURE table."""
    src = TYPECHECK_C.read_text()
    m = re.search(r"BUILTIN_NO_SIGNATURE\[\] = \{(.*?)^\};", src, re.S | re.M)
    if m is None:
        raise SystemExit("check_builtins.py: BUILTIN_NO_SIGNATURE not found in src/typecheck.c -- "
                         "it was renamed or moved, and this tool reads it rather than copying it")
    return dict(re.findall(r'\{"([^"]+)",\s*"((?:[^"\\]|\\.)*)"\}', m.group(1)))


def dispatched():
    """Every name `ziman --api builtins` knows, declared or not."""
    out = subprocess.run([str(ROOT / "ziman"), "--api", "builtins"],
                         capture_output=True, text=True, check=True).stdout
    declared = set(re.findall(r"^builtin fn (\w+)", out, re.M))
    tail = out.split("also builtin", 1)
    undeclared = set()
    if len(tail) == 2:
        for line in tail[1].split("built-in types")[0].split("\n"):
            # Each line is "  <name>   <reason>", so the name is the first field -- taking the
            # whole stripped line made every name "print            accepts any type" the moment
            # the reason was added beside it.
            if line.startswith("  ") and line.strip():
                undeclared.add(line.split()[0])
    return declared, undeclared


def main():
    declared, undeclared = dispatched()
    reasons = recorded_reasons()
    problems = []

    for name in sorted(undeclared - set(reasons)):
        problems.append(f"'{name}' is dispatched by the interpreter, has no `builtin fn` declaration in "
                        f"lib/prelude/, and no reason in src/typecheck.c's BUILTIN_NO_SIGNATURE")
    # A reason for a name that is also declared means one of the two has gone stale.
    for name in sorted(declared & set(reasons)):
        problems.append(f"'{name}' is declared `builtin fn` *and* has a reason in BUILTIN_NO_SIGNATURE "
                        f"saying it cannot be -- drop one of them")
    # And a reason for a name that is not dispatched at all is a reason for nothing.
    for name in sorted(set(reasons) - (declared | undeclared)):
        problems.append(f"'{name}' has a reason in BUILTIN_NO_SIGNATURE but the interpreter dispatches no "
                        f"builtin of that name -- drop it")

    if problems:
        for p in problems:
            print(f"check_builtins.py: {p}", file=sys.stderr)
        return 1
    print(f"check_builtins.py: {len(declared)} builtins declared, "
          f"{len(reasons)} with a recorded reason they cannot be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
