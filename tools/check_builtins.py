#!/usr/bin/env python3
"""Fails when a builtin the interpreter dispatches has no `builtin fn` declaration and no reason.

The compiler already enforces the other direction: a `builtin fn` naming something the interpreter
does not dispatch is a type error (typecheck.c). What it cannot enforce is the *absence* of a
declaration, which is the failure that started F148 -- a builtin with no signature anywhere is one
`--api builtins` can only name, an editor cannot complete, and a reader cannot find. That cost
three corrections in one session.

So every dispatched name must either be declared in lib/prelude.zm or appear below with the reason
it cannot be. "Not yet done" is a reason; the point is that it is written down rather than silent.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Why a name has no `builtin fn` declaration. Two kinds cannot have one at all; the third is
# F148 stage 2, which needs `builtin fn` to accept an overload set.
UNDECLARABLE = {
    # The rule reads more than types, so no signature states it.
    "print": "accepts any type",
    "printf": "accepts any type",
    "str": "accepts any type",
    "format": "validates its template string against the argument list",
    "error_message": "accepts any type with a `<type>_message` function -- a module lookup, not a type test",
    # Not callable names: the parser emits these for `Map<K, V> { ... }` and `if Some(x) = v`.
    "$map_lit": "parser desugaring target",
    "$set_lit": "parser desugaring target",
    "$is_variant": "parser desugaring target",
    "$unwrap_variant": "parser desugaring target",
}

NEEDS_OVERLOADS = {
    # Several unrelated shapes each -- one declaration per shape, which `builtin fn` does not yet
    # take. Counts are what the type-checker arm accepts today.
    "len": "string, [T], Map, Set, the four builders, Mutable<T>",
    "contains": "the same nine shapes, each with a second parameter",
    "snapshot": "the four builders, each to its immutable counterpart",
    "clear": "the four builders and Mutable<T>",
    "abs": "int, float, bigint, decimal",
    "unwrap": "Result<T, E> and Option<T>",
    "push": "an array (and, for its siblings, a Map or Set)",
    "insert": "an array, a Map or a Set",
    "set": "an array, a Map or a Set",
    "remove": "an array, a Map or a Set",
    "mutable_array_from": "an array or a MutableArray (`either_world`)",
    "mutable_map_from": "a Map or a MutableMap",
    "mutable_set_from": "a Set or a MutableSet",
    "mutable_set_from_array": "an array or a MutableArray",
    "mutable_array_push_all": "second argument is an array or a MutableArray",
    "mutable_map_set_all": "second argument is a Map or a MutableMap",
    "mutable_set_union": "second argument is a Set or a MutableSet",
    "mutable_set_intersect": "second argument is a Set or a MutableSet",
    "mutable_set_difference": "second argument is a Set or a MutableSet",
    "mutable_set_is_subset": "second argument is a Set or a MutableSet",
}


def dispatched():
    """Every name `ziman --api builtins` knows, declared or not."""
    out = subprocess.run([str(ROOT / "ziman"), "--api", "builtins"],
                         capture_output=True, text=True, check=True).stdout
    declared = set(re.findall(r"^builtin fn (\w+)", out, re.M))
    tail = out.split("also builtin", 1)
    undeclared = set()
    if len(tail) == 2:
        for line in tail[1].split("built-in types")[0].split("\n"):
            if line.startswith("  ") and line.strip():
                undeclared.add(line.strip())
    return declared, undeclared


def main():
    declared, undeclared = dispatched()
    excused = set(UNDECLARABLE) | set(NEEDS_OVERLOADS)
    problems = []

    for name in sorted(undeclared - excused):
        problems.append(f"'{name}' is dispatched by the interpreter, has no `builtin fn` declaration in "
                        f"lib/prelude.zm, and no reason recorded in {pathlib.Path(__file__).name}")
    # A name both declared and excused means the list has gone stale the other way.
    for name in sorted(declared & excused):
        problems.append(f"'{name}' is declared `builtin fn` but is still listed as undeclarable or "
                        f"needing overloads -- drop it from {pathlib.Path(__file__).name}")
    # And a reason for a name that is not dispatched at all is a reason for nothing.
    for name in sorted(excused - (declared | undeclared)):
        problems.append(f"'{name}' has a reason recorded but the interpreter dispatches no builtin of "
                        f"that name -- drop it from {pathlib.Path(__file__).name}")

    if problems:
        for p in problems:
            print(f"check_builtins.py: {p}", file=sys.stderr)
        return 1
    print(f"check_builtins.py: {len(declared)} builtins declared, "
          f"{len(UNDECLARABLE)} undeclarable by nature, {len(NEEDS_OVERLOADS)} awaiting overload sets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
