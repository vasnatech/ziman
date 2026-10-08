#!/usr/bin/env python3
"""Catches a new allocation that does not check for failure.

`src/xalloc.h` holds four wrappers -- ziman_xmalloc, ziman_xcalloc, ziman_xrealloc,
ziman_xstrdup -- which report and exit rather than returning NULL. Every `src/*.c` call site goes
through them, because before 2026-10-08 there were 13 NULL checks across 283 malloc/calloc sites
and 3 across 80 reallocs, and the worst of them was `src/lsp.c`'s `sb_grow`: a
`p = realloc(p, n)` that lost the old pointer *and* then wrote through the NULL, in the one process
a user keeps open all day (docs/reviews/tasks.md A20).

A bare `malloc` compiles and works, so nothing but a check like this one keeps the next one out.
A file with a genuine reason to handle NULL itself says so with an `xalloc-exempt:` marker comment
and the reason -- `src/regex.c` is the one that has one, since its buffers are the only ones here
sized by user input and it can report the failure as a Ziman runtime error instead of exiting.

Run directly, or via `make xalloc-check` (and `make check`). Exits 1 listing each file and the
count; exits 0 otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BARE = ("malloc", "calloc", "realloc", "strdup")
# The collector's own allocators, which the wrappers do not own but which fail the same way: a
# call to either must be wrapped in ziman_xcheck(...). GC_MALLOC_ATOMIC is reached through
# src/value.h's zalloc_atomic, which wraps it once for everybody.
COLLECTOR = ("GC_MALLOC_ATOMIC", "GC_MALLOC_UNCOLLECTABLE")
# xalloc.c is the implementation; allocsites.c is the profile build's counters, which the wrappers
# call by name.
IMPLEMENTATION = {"src/xalloc.c", "src/allocsites.c"}


def main():
    problems = []
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "*.c"))):
        rel = os.path.relpath(path, ROOT)
        if rel in IMPLEMENTATION:
            continue
        with open(path) as f:
            text = f.read()
        if "xalloc-exempt:" in text:
            continue
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)
        hits = re.findall(r"\b(" + "|".join(BARE) + r")\s*\(", code)
        # a collector allocator counts only when it is not already inside ziman_xcheck(...)
        for name in COLLECTOR:
            hits += [name for m in re.finditer(r"\b" + name + r"\s*\(", code)
                     if not code[:m.start()].rstrip().endswith("ziman_xcheck(")]
        if hits:
            problems.append((rel, len(hits), sorted(set(hits))))

    for rel, n, which in problems:
        print(
            f"{rel}: {n} bare call(s) to {', '.join(which)} -- use ziman_xmalloc/ziman_xcalloc/"
            f"ziman_xrealloc/ziman_xstrdup from src/xalloc.h, or add an `xalloc-exempt:` comment "
            f"saying what this file does with a NULL instead",
            file=sys.stderr,
        )
    if problems:
        print(f"check_xalloc.py: {len(problems)} file(s) allocate without checking", file=sys.stderr)
        return 1
    print("check_xalloc.py: every allocation site checks for failure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
