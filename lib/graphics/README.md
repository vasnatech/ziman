# `lib/graphics`

A device-independent 2D drawing model, and backends that render it: PDF now, raster images next.

**No code here yet.** The design is written first, one document per phase, under
`docs/graphics/` -- start with its `README.md`, which carries the architecture and the invariants
every phase shares.

| phase | modules | document |
| --- | --- | --- |
| 1 | `geom`, `shape`, `paint`, `scene` | `docs/graphics/phase-1-geometry-and-scene.md` |
| 2 | `pdfobj`, `pdf` | `docs/graphics/phase-2-pdf-backend.md` |
| 3 | `truetype`, `font` | `docs/graphics/phase-3-fonts.md` |
| 4 | `image` | `docs/graphics/phase-4-images.md` |
| 5 | `raster` | `docs/graphics/phase-5-raster-backend.md` |

A sixth phase is likely but not designed: a **document layout layer** (flows, paragraph wrapping,
tables, pagination) over phases 1-3. `docs/graphics/README.md` says why it is deliberately not
folded into the five -- a drawing model nothing has used yet is the wrong moment to design a layout
model on top of it.

Module names are flat however they are nested here, because that is how the Makefile's `vpath`
resolves a bundled module -- `lib/graphics/geom.zm` is `use geom`, exactly as `lib/time/time.zm` is
`use time`. Each file below therefore claims one global module name.
