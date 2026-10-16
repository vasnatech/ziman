# `lib/graphics` — a device-independent 2D model, and backends that render it

Five phases, one design document each. This file is the architecture they share and the decisions
that hold across all of them; read it first, because each phase document assumes it.

## What this is for

Two outputs from one drawing model: **documents** (PDF first) and **images** (raster). The
motivating case is the health-record backend's export, where PDF is the one format Ziman cannot
produce at all -- but the point of the model is that laying a report out once should yield both.

## The architecture, and why it is Java 2D's

Java 2D's real contribution is not its class count, it is that `Shape` bottoms out in a
`PathIterator` with **five** segment types -- move, line, quadratic, cubic, close -- plus a winding
rule. Everything drawable reduces to those: rectangles, ellipses, arcs, glyph outlines, arbitrary
paths. One vocabulary, many devices.

That vocabulary is also, almost exactly, PDF's content-stream vocabulary (`m`, `l`, `v`/`y`/`c`,
`h`, with `f` and `f*` for the two winding rules). So a PDF backend is mostly a serialiser, and a
raster backend is a scan-converter over the same five cases. This is the whole reason to copy the
shape of that API.

**Three things from Java 2D are deliberately not copied.**

*`Paint` is a pixel producer there.* `Paint.createContext()` returns something that yields a
`Raster`, so any custom paint forces rasterisation -- which is why PDF output from Java 2D quietly
degrades to embedded bitmaps. Here the paint set is **closed**: solid, linear gradient, radial
gradient, image pattern. Those are exactly the cases PDF has native shading dictionaries for, so
every paint survives as vectors in both backends. A closed set is a smaller API *and* a better one.

*`Graphics2D` is a mutable state machine.* Fine for Java; wrong for Ziman, where values are
immutable, there are no methods, and `Mutable*` types are `#[thread_bound]`. The model here is an
**immutable display list**: drawing operations are values, a `Scene` is a tree of them, and a
backend is a function from `Scene` to bytes. That buys three things a stateful cursor cannot -- the
same scene renders to PDF *and* PNG with no redrawing, a scene is inspectable data so tests assert
on structure rather than pixels, and scenes compose as values. PDF's own `q`/`Q` nesting is a tree
anyway, so a tree is closer to the target than a cursor is.

*`GlyphVector.getOutline()` is a trap for documents.* Lowering text to paths is the right escape
hatch for a device with no font machinery and the wrong default for PDF: it loses selectable,
searchable, copyable text and inflates the file. **Text stays a first-class operation** that the PDF
backend emits as text (`BT`/`Tf`/`Tj`) and the raster backend lowers to outlines.

## Modules

Bundled module names are flat whatever directory they live in (see the Makefile's `vpath`), so each
file below claims one global name. They are single lowercase words, matching every existing module.

| phase | module | what it holds |
| --- | --- | --- |
| 1 | `geom` | `Point`, `Rect`, `Matrix` (affine), transform arithmetic |
| 1 | `shape` | `Path` of the five segments, builders, bounds, winding |
| 1 | `paint` | `Color`, `Paint` (closed), `Stroke`, dash patterns |
| 1 | `scene` | the display list: operations, groups, clips, transforms |
| 2 | `pdfobj` | PDF's object layer: objects, dictionaries, streams, xref |
| 2 | `pdf` | the backend: `Scene` to a PDF document |
| 3 | `truetype` | TrueType table parsing (`cmap`, `hmtx`, `head`, `glyf`, ...) |
| 3 | `font` | font model and metrics, over `truetype` |
| 4 | `image` | an image buffer value, and PNG encode/decode |
| 5 | `raster` | scan-conversion, stroking, glyph rasterisation |

## Invariants that hold across every phase

1. **Backends are functions, not objects.** `pdf.render(scene, options) -> Result<[byte], string>`.
   No shared mutable state, so two threads may render two scenes at once.
2. **A `Scene` is renderable by every backend or rejected with a message.** No silent degradation:
   if a backend cannot honour an operation it says so, rather than rasterising behind the caller's
   back. That is the failure Java 2D's open `Paint` causes.
3. **Nothing in `lib/graphics` reads a file or a clock.** Fonts and images are passed in as
   `[byte]`. That keeps the whole library pure and testable, and leaves resource loading to `fs`.
4. **Coordinates are `float`, Y grows downward** -- the convention of every drawing API a reader is
   likely to know (Java 2D, Canvas, SVG). PDF's own Y axis grows *upward*, and the PDF backend is
   the single place that flips it, once, in its page transform. Doing it anywhere else would leak
   the format into the model.
5. **Each phase ships green**: its own fixtures pass and `check-fast` stays clean. No phase depends
   on a later one to be correct, only to be more capable.

## Build changes this needs

- `vpath %.zm` in the top-level `Makefile` gains `lib/graphics`.
- `CORELIB_MODULES` gains each module as it lands, and `src/corelib.c` gains its `#include` and
  table row (the five places registering a bundled module touches -- see `schedule` for the worked
  example).
- `tools/gen_lib_manual.py`'s `GROUPS` gains a "Graphics" group with directory `graphics`.
- **`math` needs `atan2`, and probably `asin`/`acos`.** Arcs, round joins and caps, and decomposing
  a matrix all need `atan2`, and it is absent today (`math` has `sqrt`/`pow`/`exp`/`log`/`sin`/`cos`/
  `tan`/`floor`/`ceil`/`round` and no inverse trigonometry). This is a prerequisite for phase 1, not
  a detail to discover later.

## What is already available and does not need building

- `compress.deflate` writes a **zlib** stream (RFC 1950), which is what both PDF's `FlateDecode`
  and PNG's `IDAT` want. No raw-deflate wrapper is needed for either.
- `bits` has 52 functions, enough for the big-endian integer reads TrueType and PNG need.
- `format` supports `{:.N}` fixed-point output, which is what PDF coordinates want -- a full
  `float` repr would write `0.30000000000000004` into a content stream.
- Packed `[byte]`/`[float]` arrays are real buffers, not boxed arrays, so path coordinates and pixel
  data cost what they should.
