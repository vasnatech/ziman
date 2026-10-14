#!/usr/bin/env python3
"""Catches a `HandleSet` declared without its initialiser.

`HANDLE_SET_INIT` fills 64 shard mutexes with `PTHREAD_MUTEX_INITIALIZER`. A declaration that
leaves it off gets zero-initialised static storage instead, and POSIX does not define what locking
such a mutex does. `src/native_process.c` shipped that way and worked, because glibc's own
initialiser for a default mutex happens to be 40 bytes of zero -- measured, not assumed -- which is
an implementation detail of one libc and not a promise (docs/reviews/tasks.md A45).

So the bug is invisible by construction: it cannot fail a test on the machine that has it. Only a
check like this one keeps the next one out, which is the same argument as check_xalloc.py's.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
# A file-scope declaration: `static HandleSet g_name = HANDLE_SET_INIT;` is the only right shape.
DECL = re.compile(r"^\s*(?:static\s+)?HandleSet\s+(\w+)\s*(=\s*HANDLE_SET_INIT\s*)?;", re.M)

bad = []
total = 0
for path in sorted(SRC.glob("*.c")):
    for m in DECL.finditer(path.read_text(encoding="utf-8")):
        total += 1
        if m.group(2) is None:
            line = path.read_text(encoding="utf-8")[: m.start()].count("\n") + 1
            bad.append(f"{path.relative_to(SRC.parent)}:{line}: HandleSet '{m.group(1)}' has no = HANDLE_SET_INIT")

if bad:
    print("check_handle_sets.py: a HandleSet without HANDLE_SET_INIT leaves 64 mutexes undefined by POSIX", file=sys.stderr)
    for b in bad:
        print("  " + b, file=sys.stderr)
    sys.exit(1)
print(f"check_handle_sets.py: all {total} HandleSet declarations carry HANDLE_SET_INIT")
