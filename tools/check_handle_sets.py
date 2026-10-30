#!/usr/bin/env python3
"""Two things a `HandleSet` cannot be trusted to get right on its own.

The first: a `HandleSet` declared without its initialiser.

`HANDLE_SET_INIT` fills 64 shard mutexes with `PTHREAD_MUTEX_INITIALIZER`. A declaration that
leaves it off gets zero-initialised static storage instead, and POSIX does not define what locking
such a mutex does. `src/native_process.c` shipped that way and worked, because glibc's own
initialiser for a default mutex happens to be 40 bytes of zero -- measured, not assumed -- which is
an implementation detail of one libc and not a promise (docs/reviews/tasks/ A45).

So the bug is invisible by construction: it cannot fail a test on the machine that has it. Only a
check like this one keeps the next one out, which is the same argument as check_xalloc.py's.

The second: a `HandleSet` nothing ever removes from. A set is a validation table and not a root
(src/handles.c), so an entry whose object has been collected is a dangling pointer kept for the life
of the process -- and because `handleset_add` is all it takes to make a working handle, a set can be
complete, correct and leaking without anything failing. The four builder sets in src/prelude.c
shipped that way: measured before it was fixed, 16,000,000 short-lived `mutable_string_new()` calls
left a 268 MB table, one permanent 16.8-byte entry per builder ever created, growing linearly and
without bound. Fixing it *lowered* peak RSS at 4,000,000 builders from 91 MB to 12 MB and was
slightly faster, because a table that never grows is also a table that never gets copied.

Same argument as the first check, then: the leak is invisible unless someone thinks to watch RSS
while churning millions of handles. This rule asks only that every set have *a* way out -- a
`handleset_remove` naming it, or its address handed to something that registers a finalizer -- not
that the way out is correct, which no regex can tell.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
# A file-scope declaration: `static HandleSet g_name = HANDLE_SET_INIT;` is the only right shape.
DECL = re.compile(r"^\s*(?:static\s+)?HandleSet\s+(\w+)\s*(=\s*HANDLE_SET_INIT\s*)?;", re.M)
# `handleset_add(&g_name` / `handleset_remove(&g_name`, and the sets handed to a finalizer-
# registering helper by address. The last of those is how src/prelude.c's builders are removed:
# one shared finalizer takes the set as its `client_data`, so no `handleset_remove(&g_map_builders`
# is ever written literally.
USE = re.compile(r"handleset_(add|remove)\s*\(\s*&(\w+)")
VIA_FINALIZER = re.compile(r"(?:finalizer|_drop)\w*\s*\([^;]*&(\w+)")

sources = {path: path.read_text(encoding="utf-8") for path in sorted(SRC.glob("*.c"))}

# Keyed by (file, name), not by name: three different sets are called `g_live` (native_net.c,
# native_sqlite.c, native_process.c), and keying on the name alone both under-counts them and lets
# one file's removal vouch for another file's set. Every one of these is `static`, so a set and the
# code that adds to or removes from it are always in the same file.
uninitialised = []
declared = {}  # (file, name) -> "file:line"
added, removed = set(), set()
for path, text in sources.items():
    where_file = str(path.relative_to(SRC.parent))
    for m in DECL.finditer(text):
        line = text[: m.start()].count("\n") + 1
        declared[(where_file, m.group(1))] = f"{where_file}:{line}"
        if m.group(2) is None:
            uninitialised.append(f"{where_file}:{line}: HandleSet '{m.group(1)}' has no = HANDLE_SET_INIT")
    for kind, name in USE.findall(text):
        (added if kind == "add" else removed).add((where_file, name))
    for name in VIA_FINALIZER.findall(text):
        removed.add((where_file, name))

# Only sets something adds to: a declared-but-unused set holds nothing and leaks nothing.
unremovable = sorted(added - removed)

if uninitialised:
    print("check_handle_sets.py: a HandleSet without HANDLE_SET_INIT leaves 64 mutexes undefined by POSIX", file=sys.stderr)
    for b in uninitialised:
        print("  " + b, file=sys.stderr)
if unremovable:
    print("check_handle_sets.py: a HandleSet nothing removes from grows for the life of the process", file=sys.stderr)
    for key in unremovable:
        print(f"  {declared.get(key, key[0])}: '{key[1]}' is added to but never removed from -- give it a"
              " finalizer that takes the dead object out (see builder_drop_finalizer in src/prelude.c)",
              file=sys.stderr)
if uninitialised or unremovable:
    sys.exit(1)
print(f"check_handle_sets.py: all {len(declared)} HandleSet declarations carry HANDLE_SET_INIT"
      f"; all {len(added)} that are added to have a removal path")
