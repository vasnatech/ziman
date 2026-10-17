# `lib/graphics`

A device-independent 2D drawing model, and two backends that render it: PDF documents and raster
images.

**All five phases are built.** The design was written first, one
document per phase, under `docs/graphics/` -- start with its `README.md`, which carries the
architecture and the invariants every phase shares. A built phase's document ends with an
*As built* section recording where the code went a different way and why.

| phase | modules | state | document |
| --- | --- | --- | --- |
| 1 | `geom`, `shape`, `paint`, `scene` | built | `docs/graphics/phase-1-geometry-and-scene.md` |
| 2 | `pdfobj`, `pdf` | built | `docs/graphics/phase-2-pdf-backend.md` |
| 3 | `truetype`, `font` | built | `docs/graphics/phase-3-fonts.md` |
| 4 | `image` | built | `docs/graphics/phase-4-images.md` |
| 5 | `raster`, `stroker` | built | `docs/graphics/phase-5-raster-backend.md` |

So one `Scene` renders two ways. A PDF carries vector paths, gradients, the standard fourteen fonts,
an embedded TrueType face with Unicode text, and images with alpha; the raster backend turns the
same scene into a `Gray8`, `Rgb8` or `Rgba8` image, with anti-aliased fills under both winding
rules, stroking with caps, joins and dashes, arbitrary clip paths, and cached glyph rendering.

Where the two differ, the one that cannot do something **says so** rather than approximating it
(invariant 2): `paint.Pattern` draws in the raster backend and is refused by PDF, and a standard
font names a face a PDF reader resolves but a raster device cannot.

A sixth phase is **designed but not built**, in `docs/graphics/phase-6-layout.md`: a **document
layout layer** (flows, paragraph wrapping,
tables, pagination) over phases 1-3. `docs/graphics/README.md` says why it is deliberately not
folded into the five -- a drawing model nothing has used yet is the wrong moment to design a layout
model on top of it.

Module names are flat however they are nested here, because that is how the Makefile's `vpath`
resolves a bundled module -- `lib/graphics/geom.zm` is `use geom`, exactly as `lib/time/time.zm` is
`use time`. Each file below therefore claims one global module name.
