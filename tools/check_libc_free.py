#!/usr/bin/env python3
"""Catches memory libc allocated being released through the collector build's no-op `free`.

`src/zgc.h` is force-included into every translation unit of the default build and redirects
malloc, calloc, realloc and strdup to the Boehm collector, then defines `free` as
`((void)(p))` -- correct for everything the interpreter allocates, since nothing it allocates is
ever released by hand.

It is wrong for a block the collector never handed out. libc has its own allocating functions that
zgc.h does not redirect, `getline` and `strndup` among them, and the collector cannot reclaim what
it did not allocate -- so such a block, released through that macro, stays for the life of the
process. `src/iter.c:133` did exactly that with getline's line buffer: 20,000 iterations over a
file of 60 KB lines cost **1.23 GB** of RSS under `ziman` against 12 MB after the fix, and 27 MB
under `ziman-nogc`, whose `free` is real. The build whose own comment says "nothing is ever freed"
was the one that did not leak (docs/reviews/tasks.md A17).

Nothing in the compiler can see this: both spellings type-check, both run, and the leak shows up
only as RSS in a long-lived process, which is the North Star goal. So the rule is checked here
instead: a file that calls one of these allocators must also reach the real `free` -- the
`libc_free` helper in src/iter.c is the idiom, and `#pragma push_macro("free")` the mechanism (see
src/native_http.c:15-27, which uses the same suspension to read malloc.h).

Run directly, or via `make libc-free-check` (and `make check`). Exits 1 listing each file that
allocates with libc and never reaches a real free; exits 0 otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# libc functions that return a heap block zgc.h does NOT redirect. malloc/calloc/realloc/strdup are
# deliberately absent: those ARE redirected, so their blocks belong to the collector and the no-op
# free is the right thing for them.
ALLOCATORS = ["getline", "getdelim", "strndup", "asprintf", "vasprintf"]

# How a file proves it can reach the real free: either it has the helper, or it suspends the macro.
REACHES_REAL_FREE = ["libc_free", 'push_macro("free")']


def main():
    problems = []
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "*.c"))):
        with open(path) as f:
            text = f.read()
        # comments are not code: a file may well discuss getline without calling it
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        used = [a for a in ALLOCATORS if re.search(r"\b" + a + r"\s*\(", code)]
        if not used:
            continue
        if any(marker in code for marker in REACHES_REAL_FREE):
            continue
        problems.append((os.path.relpath(path, ROOT), used))

    for path, used in problems:
        print(
            f"{path}: calls {', '.join(used)} but never reaches the real free -- "
            f"the collector build's `free` is a no-op, so that block is never reclaimed "
            f"(see this script's own docstring, or src/iter.c's libc_free)",
            file=sys.stderr,
        )
    if problems:
        print(f"check_libc_free.py: {len(problems)} file(s) release libc memory through a no-op free", file=sys.stderr)
        return 1
    print("check_libc_free.py: every libc allocation reaches a real free")
    return 0


if __name__ == "__main__":
    sys.exit(main())
