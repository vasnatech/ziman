# `lib/graphics` — a device-independent 2D model, and backends that render it

Five phases, one design document each. This file is the architecture they share and the decisions
that hold across all of them; read it first, because each phase document assumes it.

## What this is for

Two outputs from one drawing model: **documents** (PDF first) and **images** (raster). The
motivating case is the health-record backend's export, where PDF is the one format Ziman cannot
produce at all -- but the point of the model is that laying a report out once should yield both.

## What "graphics" does not mean here

The folder is called `graphics` because that is what Cairo, Skia and Java 2D call this -- a
device-independent 2D drawing model with backends. **The word is broader elsewhere than it is here**,
so the bound is worth stating rather than leaving a reader to infer it from nine module names.

Not in scope, and not planned -- each would be a different library, not a later phase:

- **3D.** No projection, no depth, no lighting. The display list is two-dimensional by
  construction, and a `Matrix` is 2x3.
- **The GPU.** A backend is a pure function from `Scene` to bytes (invariant 1). No device
  contexts, no shaders, no surfaces owned by a driver. Phase 5 scan-converts in Ziman.
- **A screen.** No windows, no event loop, no input. Nothing here is interactive; the outputs are
  a file's bytes.
- **Animation or video.** A `Scene` is one still frame.
- **Image *processing*.** `image` is a buffer plus a codec, not a filter pipeline: no blur, no
  convolution, no colour correction. (Phase 5 needs *sampling* to draw an image under a transform,
  which is drawing, and belongs there.)

Absent but **additive** -- these fit the architecture and would be new work inside it rather than
against it, which is the distinction that matters when deciding where something belongs:

- CMYK, 16-bit channels and ICC colour management (`image`'s three formats are a deliberate floor,
  not a colour model system).
- More codecs in or out: JPEG *decoding*, GIF, WebP, TIFF.
- Vector *input*: nothing turns a file back into a `Scene`, which is built in code. (`pdfobj.check` reads a PDF's
  structure to validate what this library wrote; it is not a reader.)
- Text shaping, so Arabic and Indic join correctly -- named in phase 3 and refused there rather
  than approximated.
- Text shaping is the real remaining gap. The document layout layer that was listed here as
  "designed and not built" is **built** -- `lib/graphics/layout.zm`, phase 6 (F146); see below.

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
gradient, image pattern. The two gradients are exactly the cases PDF has native shading
dictionaries for, so every paint that is drawn survives as vectors in both backends. A closed set is
a smaller API *and* a better one.

(The image pattern is the one member nothing renders: it needs a `/Pattern` colour space rather than
a shading dictionary, which is a fourth resource category, and `scene.image` already places an image
anywhere a matrix can. Both backends return an `Err` naming it -- phase 4's *As built* section has
the argument.)

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

## Prior art, and what each one contributed

Java 2D is the starting point above, but it is not the only multi-backend 2D model and it is not the
best one on every axis. What was taken from each, having read the APIs rather than remembered them
(cairo, pango and harfbuzz headers are on this machine; Java 2D via `javap`):

**Cairo** is the closest architectural match -- one drawing model with `cairo-pdf.h`,
`cairo-svg.h`, `cairo-ps.h` and `cairo-xlib.h` over it, which is exactly the shape of this library.
Two things come from it. Its pattern set is a **closed six-case enum**, which independently confirms
the closed-`Paint` decision. And its sixth case, `RASTER_SOURCE` -- a callback pixel producer -- is
the cautionary tale: cairo's PDF backend rasterises "fallback regions" for anything it cannot
express natively, which is a long-standing source of surprisingly large PDFs with blurry patches.
That is invariant 2's failure mode, in a mature library, caused by one open escape hatch.

Cairo also has **three text levels** -- `show_text(utf8)`, `show_glyphs(glyphs)` and
`show_text_glyphs(utf8, glyphs, clusters)` -- and the third is why `TextRun` carries `clusters`
(phase 2). And its path vocabulary is **four** elements rather than five, which is why phase 1 now
states *why* `QuadTo` is kept instead of inheriting it from Java.

**Skia** contributes two things. `SkPicture` is a recorded, replayable display list, so the immutable
scene here is what the most heavily deployed 2D engine does rather than an invention. And `SkPath`
has a verb neither Java 2D nor cairo has -- a **conic**, which represents a circular arc exactly;
phase 1 records that as an open question. `SkTextBlob` plus HarfBuzz is also the right shape for a
future shaping phase (phase 3 says so).

**Quartz 2D** designed `CGContext` to *be* PDF's model. That is the wholesale version of the small
trick phase 1 plays by keeping `Matrix`'s six fields in PDF's `cm` order.

**Direct2D** makes geometry immutable once built, which is what `shape.Path` is.

**Canvas 2D** matters for naming rather than architecture: `moveTo`/`lineTo`/`bezierCurveTo`/
`fill`/`stroke` is the vocabulary a language model has seen most, which bears on this project's
AI-first goal. Phase 1's names are close to it already; where they differ it should be on purpose.

**`lyon` and `tiny-skia`** are the references for phase 5 specifically -- `lyon` for stroke
tessellation and the join/miter cases, `tiny-skia` as a benchmark for what subset of Skia is enough
in a pure-language rasteriser.

**ReportLab** points at what all five phases leave out; see the next section.

## The layer above: document layout (phase 6)

**Built** -- `lib/graphics/layout.zm`, 926 lines, designed 2026-10-16 in `phase-6-layout.md` and
shipped as F146: a closed `Block` enum (paragraph, table, spacer, rule, picture, keep-together,
page break) with three exhaustive switches over it -- `measure`, `split_block`, `draw` -- and
`paginate(doc, page) -> Result<[Scene], string>` as the one entry point, because `pdf.render_pages`
already takes a list of scenes for exactly this reason. Greedy line breaking rather than
Knuth-Plass, with the measurement that would justify changing it named in the design. Page numbers
by laying out twice, the only one of three options that changes no type elsewhere. Worked example in
`docs/lib/examples/graphics/layout_example.zm`, pinned by `tests/lang/graphics_layout`.

*Update 2026-10-30: this section and the gap list above still said "designed and not built", found
by asking what was next. The design document opens by calling itself a guess because the export it
exists for had not been written -- that reservation stands, and the export is still allowed to
contradict it.*

The argument below is why it exists.

Every phase here is a *drawing* API: it puts a path or a glyph at a coordinate. The motivating use
case -- a health record exported as a PDF -- does not think in coordinates. It thinks in headings,
paragraphs, tables and page breaks, and something has to turn the second into the first.

ReportLab is the instructive case: it has a coordinate-level canvas *and* a layout layer on top
(Platypus: flowables, paragraph wrapping, tables that split across pages), and the layout layer
exists because reports written directly against the canvas become unmaintainable arithmetic. The
same will be true here: the export will otherwise accumulate its own private layout code, badly.

So a sixth phase, over phases 1-3: a flow model (block, paragraph, table), text wrapping
using `font.advance`, and pagination that emits one `Scene` per page -- `pdf.render_pages` already
takes a list of scenes for exactly this reason. It is deliberately *not* folded into the phases
above, because a drawing model that has not been used yet is the wrong moment to design a layout
model, and the export work will say what it actually needs.

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
