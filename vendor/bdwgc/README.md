# Vendored Boehm-Demers-Weiser garbage collector

The collector Ziman links (see `src/zgc.h` and docs/DESIGN.md, "Memory
reclamation"), copied from https://github.com/ivmai/bdwgc at commit `2547e33`,
trimmed to what `extra/gc.c` (bdwgc's single-file build) includes: the `*.c`
sources, `include/`, `extra/gc.c` and the license (MIT-style; keep `LICENSE`
with any copy).

It is built by the Makefile with the options Ziman needs (`ALL_INTERIOR_POINTERS`,
`GC_THREADS`, parallel marking); a system libgc is deliberately not used, since a
distribution build may lack them. To update, run `tools/vendor_bdwgc.sh <commit>`.
