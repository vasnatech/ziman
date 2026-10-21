#!/usr/bin/env python3
"""Fails when a builtin the interpreter dispatches has no `builtin fn` declaration and no reason.

The compiler already enforces the other direction: a `builtin fn` naming something the interpreter
does not dispatch is a type error (typecheck.c). What it cannot enforce is the *absence* of a
declaration, which is the failure that started F148 -- a builtin with no signature anywhere is one
`--api builtins` can only name, an editor cannot complete, and a reader cannot find. That cost
three corrections in one session.

So every dispatched name must either be declared in lib/prelude/ or appear below with the reason
it cannot be. "Not yet done" is a reason; the point is that it is written down rather than silent.

It also checks two more tables for the same kind of rot.

`renamed_builder_fn`'s old-name hints each say "'x' is now 'y'", and nothing was holding `y` to
being a name that still exists -- so F195 and F200 renamed four of the targets out from under it
and the compiler went on telling people to call `mutable_map_get`, which had not existed for a day.
A hint naming something that is not there is worse than no hint: it reads as authoritative and
costs a search.

`PURE_BUILTINS` is the list of names whose discarded result is an error. Here a stale entry is not
the bug but the evidence of one: F195-F201 took the prefix off fourteen of them and each rename
left the old name in place and never added the new one, so the list named fourteen names that no
longer resolve while thirteen that did went unlisted -- and `union(s1, s2)` in statement position
was accepted with its whole output thrown away. Every entry must now be a name the interpreter
dispatches or a name a rewrite records in `rewritten_from`, so half a rename cannot pass again.
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


def prelude_declarations():
    """Every name declared anywhere in lib/prelude/, advertised or not.

    Not `--api builtins`: an `internal` declaration is a perfectly good rename target
    (`mutable_set_add` is one) and is deliberately absent from that list.
    """
    names = set()
    decl = re.compile(r"^(?:pub |internal )?(?:native fn|builtin fn|fn|struct|enum|val)\s+(\w+)", re.M)
    for path in sorted((ROOT / "lib" / "prelude").glob("*.zm")):
        names |= set(decl.findall(path.read_text()))
    if not names:
        raise SystemExit("check_builtins.py: no declarations found in lib/prelude/ -- the layout moved")
    return names


def rename_hints():
    """`src/typecheck.c`'s renamed_builder_fn table, read rather than copied."""
    src = TYPECHECK_C.read_text()
    m = re.search(r"static const char \*renamed_builder_fn\(.*?\{(.*?)\{NULL, NULL\}\};", src, re.S)
    if m is None:
        raise SystemExit("check_builtins.py: renamed_builder_fn not found in src/typecheck.c -- it was "
                         "renamed or moved, and this tool reads it rather than copying it")
    return re.findall(r'\{"([^"]+)",\s*"([^"]+)"\}', m.group(1))


def pure_builtins():
    """`src/typecheck.c`'s PURE_BUILTINS, read rather than copied."""
    src = TYPECHECK_C.read_text()
    m = re.search(r"static const char \*const PURE_BUILTINS\[\] = \{(.*?)NULL\};", src, re.S)
    if m is None:
        raise SystemExit("check_builtins.py: PURE_BUILTINS not found in src/typecheck.c -- it was "
                         "renamed or moved, and this tool reads it rather than copying it")
    return re.findall(r'"([^"]+)"', m.group(1))


def rewrite_sources():
    """The names `check_stmt`/`check_call` record in `rewritten_from`.

    PURE_BUILTINS is matched against `authored_callee`, which answers `rewritten_from` when the
    checker has replaced the callee -- so `mutable` is a legitimate entry even though no builtin
    is dispatched under that name.
    """
    return set(re.findall(r'rewritten_from\s*=\s*"([^"]+)"', TYPECHECK_C.read_text()))


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

    # Every PURE_BUILTINS entry must be a name that can actually reach the dropped-result gate.
    # F212 found fourteen that could not: the Map/Set pairing (F195-F201) renamed `set_union` to
    # `union` and so on, and each rename left the old name here and never added the new one -- so
    # the list named fourteen names that no longer resolve while thirteen that do went unlisted,
    # and `union(s1, s2)` was accepted with its result thrown away. A stale entry is harmless on
    # its own; it is the *evidence* of the missing one, which is why this checks it.
    for name in sorted(set(pure_builtins()) - (declared | undeclared | rewrite_sources())):
        problems.append(f"PURE_BUILTINS lists '{name}', which the interpreter dispatches no builtin of "
                        f"and no rewrite records in `rewritten_from` -- it can never reach the "
                        f"dropped-result gate, so either the name moved and its replacement is missing "
                        f"from the list, or the entry should go")

    # Every old-name hint must point at a name that exists.
    hints = rename_hints()
    available = prelude_declarations() | declared | undeclared
    for was, now in hints:
        if now not in available:
            problems.append(f"renamed_builder_fn says '{was}' is now '{now}', and there is no '{now}' -- "
                            f"the hint outlived the name it points at")
        if was in available:
            problems.append(f"renamed_builder_fn calls '{was}' an old name, but something is declared "
                            f"'{was}' today -- the hint would fire on a name that works")

    if problems:
        for p in problems:
            print(f"check_builtins.py: {p}", file=sys.stderr)
        return 1
    print(f"check_builtins.py: {len(declared)} builtins declared, "
          f"{len(reasons)} with a recorded reason they cannot be, "
          f"{len(hints)} rename hints all pointing at names that exist, "
          f"{len(pure_builtins())} pure builtins all reachable by the dropped-result gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
