#!/usr/bin/env bash
# Builds the Boehm-Demers-Weiser collector as a static library for `make gc`:
#   third_party/bdwgc/libgc.a  (headers in third_party/bdwgc/include)
# A system libgc is deliberately not used: Ziman needs it built with
# ALL_INTERIOR_POINTERS (string slices are interior pointers -- without it a
# live slice into a large buffer is collected) and GC_THREADS, which a
# distribution build may or may not have, so it is always built from a pinned
# source tree.
# Needs git, a C compiler and ar; no autotools or cmake (bdwgc's single-file
# build, extra/gc.c, with the compiler's own atomics).
set -euo pipefail
cd "$(dirname "$0")/.."

REPO=https://github.com/ivmai/bdwgc.git
COMMIT=2547e33
DIR=third_party/bdwgc

if [ ! -d "$DIR/.git" ]; then
    mkdir -p third_party
    git clone --quiet "$REPO" "$DIR"
fi
git -C "$DIR" checkout --quiet "$COMMIT"

cc -O2 -DNDEBUG -I"$DIR/include" \
    -DGC_THREADS -DGC_BUILTIN_ATOMIC -DTHREAD_LOCAL_ALLOC -DPARALLEL_MARK \
    -DUSE_MMAP -DALL_INTERIOR_POINTERS -D_GNU_SOURCE -pthread \
    -c "$DIR/extra/gc.c" -o "$DIR/gc.o"
rm -f "$DIR/libgc.a"
ar rcs "$DIR/libgc.a" "$DIR/gc.o"
echo "built $DIR/libgc.a ($COMMIT)"
