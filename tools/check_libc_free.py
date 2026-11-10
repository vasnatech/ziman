#!/usr/bin/env python3
"""Catches memory crossing the allocator boundary src/zgc.h draws, in either direction.

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
was the one that did not leak (docs/reviews/tasks/ A17).

Nothing in the compiler can see this: both spellings type-check, both run, and the leak shows up
only as RSS in a long-lived process, which is the North Star goal. So the rule is checked here
instead: a file that calls one of these allocators must also reach the real `free` -- the
`libc_free` helper in src/iter.c is the idiom, and `#pragma push_macro("free")` the mechanism (see
src/native_http.c:15-27, which uses the same suspension to read malloc.h).

**The second rule is the mirror of the first, and it is the worse of the two.** A block the
collector allocated, released by a *library* through the real `free`, is not a leak -- it is heap
corruption, because the pointer was never libc's to free. The direction is easy to miss: the first
rule's note says malloc, calloc, realloc and strdup are deliberately absent because "their blocks
belong to the collector and the no-op free is the right thing for them", which is true for every
block the interpreter keeps and false the moment one is handed to a library that frees it itself.
`src/native_window.c` did exactly that: `XCreateImage` takes ownership of the pixel buffer it is
given and `XDestroyImage` releases it with libc's `free`, so a buffer from the redirected `malloc`
aborted the process with `free(): invalid size` on every window close (docs/reviews/tasks/ F269).
It was found by pressing a key at a real window, not by reading the file -- whose own header already
stated the rule it was breaking. So: a file that calls one of these ownership-taking library
functions must reach the real malloc, by the same push_macro idiom.

Run directly, or via `make libc-free-check` (and `make check`). Exits 1 listing each file on either
side; exits 0 otherwise.
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

# Library functions that take ownership of a buffer they are handed and release it with libc's own
# free. Their argument must therefore come from libc's own malloc, which in this build `malloc` is
# not. Keyed by the function, with what frees it, because the message is only useful if it says so.
TAKES_OWNERSHIP = {
    "XCreateImage": "XDestroyImage frees the data pointer it was given, with libc's free",
}

# How a file proves it can reach the real malloc. Same idiom, other half.
REACHES_REAL_MALLOC = ["libc_malloc", 'push_macro("malloc")']


def main():
    problems = []
    owners = []
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "*.c"))):
        with open(path) as f:
            text = f.read()
        # comments are not code: a file may well discuss getline without calling it
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)

        taken = [f for f in TAKES_OWNERSHIP if re.search(r"\b" + f + r"\s*\(", code)]
        if taken and not any(marker in code for marker in REACHES_REAL_MALLOC):
            owners.append((os.path.relpath(path, ROOT), taken))

        used = [a for a in ALLOCATORS if re.search(r"\b" + a + r"\s*\(", code)]
        if not used:
            continue
        if any(marker in code for marker in REACHES_REAL_FREE):
            continue
        problems.append((os.path.relpath(path, ROOT), used))

    for path, taken in owners:
        for one in taken:
            print(
                f"{path}: calls {one} but never reaches the real malloc -- {TAKES_OWNERSHIP[one]}, "
                f"and in this build `malloc` is the collector's, so that is heap corruption rather "
                f"than a leak (see this script's own docstring, or src/native_window.c's libc_malloc)",
                file=sys.stderr,
            )

    for path, used in problems:
        print(
            f"{path}: calls {', '.join(used)} but never reaches the real free -- "
            f"the collector build's `free` is a no-op, so that block is never reclaimed "
            f"(see this script's own docstring, or src/iter.c's libc_free)",
            file=sys.stderr,
        )
    if problems or owners:
        if problems:
            print(f"check_libc_free.py: {len(problems)} file(s) release libc memory through a no-op free", file=sys.stderr)
        if owners:
            print(f"check_libc_free.py: {len(owners)} file(s) hand collector memory to a library that frees it", file=sys.stderr)
        return 1
    print("check_libc_free.py: allocators match on both sides of the collector boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
