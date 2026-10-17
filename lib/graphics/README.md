# `lib/graphics`

A device-independent 2D drawing model, and backends that render it: PDF now, raster images next.

**Phases 1 to 4 are built; phase 5 is designed and not started.** The design is written first, one
document per phase, under `docs/graphics/` -- start with its `README.md`, which carries the
architecture and the invariants every phase shares. A built phase's document ends with an
*As built* section recording where the code went a different way and why.

| phase | modules | state | document |
| --- | --- | --- | --- |
| 1 | `geom`, `shape`, `paint`, `scene` | built | `docs/graphics/phase-1-geometry-and-scene.md` |
| 2 | `pdfobj`, `pdf` | built | `docs/graphics/phase-2-pdf-backend.md` |
| 3 | `truetype`, `font` | built | `docs/graphics/phase-3-fonts.md` |
| 4 | `image` | built | `docs/graphics/phase-4-images.md` |
| 5 | `raster` | designed | `docs/graphics/phase-5-raster-backend.md` |

So a PDF today can carry vector paths, gradients, the standard fourteen fonts, an embedded TrueType
face with Unicode text, and images with alpha. What it cannot do is produce a *raster* image of a
scene, which is phase 5.

A sixth phase is likely but not designed: a **document layout layer** (flows, paragraph wrapping,
tables, pagination) over phases 1-3. `docs/graphics/README.md` says why it is deliberately not
folded into the five -- a drawing model nothing has used yet is the wrong moment to design a layout
model on top of it.

Module names are flat however they are nested here, because that is how the Makefile's `vpath`
resolves a bundled module -- `lib/graphics/geom.zm` is `use geom`, exactly as `lib/time/time.zm` is
`use time`. Each file below therefore claims one global module name.
