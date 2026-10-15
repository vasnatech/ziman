# Benchmarks that actually fill the per-thread gc_heap

Every other benchmark in `tests/bench/` leaves the per-thread `GcHeap` arena nearly empty --
`scalar_arrays_bench` peaks at **384 bytes in 4 objects**, because what it allocates heavily
(string bytes from `concat`, `Value[]` buffers, decimal digits) lands on Boehm's heap, not in the
arena. That makes those benchmarks blind to the one cost this interpreter's two-collector
architecture actually has: a gc_heap object is marked and swept by Ziman's own collector *and* sits
inside the range Boehm conservatively re-scans as a root on every one of its collections.

These four put a large **live** set in the arena, so that cost is visible at all. Read them with
`docs/MEMORY.md`, whose "the conservative arena root push is the only shape that fits" entry is
what they were written for.

| benchmark | what it fills the arena with | why it is here |
| --- | --- | --- |
| `decimal_churn.zm` | 300k live `Row` structs; ~268 MB pushed to Boehm per run over 12 collections | the only one where the arena is both large and ~95% unskippable (nothing in it is free or atomic) -- the case any change to the root push has to win on |
| `structs_live.zm` | 200k live `Row` structs (394k live `StructObj`s) whose `fields` arrays are on Boehm's heap | the cross-heap reference that forces the conservative scan |
| `map_live.zm` | a 150k-entry map (390k live `Leaf`s) under a Boehm-heap `MapNode` chain | the same cross-heap shape through a different type |
| `strings_live.zm` | ~300k reclaimed objects against a few hundred bytes live | the opposite extreme: an arena that is almost all free chunks |

Each is a plain `./ziman tests/bench/gcheap/<name>.zm` and runs in well under a second. Two
environment variables are worth knowing when measuring with them:

- `GC_PRINT_STATS=1` -- Boehm's own collection count and pause times. Note that **Boehm collects far
  less often than Ziman's collector does**: `strings_live` is 75 Ziman collections against 3 of
  Boehm's. Anything on the Boehm path is rarer than it looks.
- `./ziman-allocprofile <file>` (`make ziman-allocprofile`) -- the live set in the per-thread heap
  broken down by kind, plus mark/sweep split. This is what shows the arena is empty on the older
  benchmarks and full on these.
