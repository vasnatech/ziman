# Phase 5 — the raster backend

The second renderer, and much the largest phase: it has to *do* what PDF readers were doing for the
document path. Turns a `Scene` into an `Image`.

Read `README.md` first. Nothing in phases 1-4 depends on this; it only makes the library more
capable.

## Scope

`raster`. One module, but three separable pieces inside it -- scan-conversion, stroking, and glyph
rendering -- which are worth landing and testing in that order rather than together.

```ziman
pub struct Options {
    format: image.Pixels
    samples: int            // anti-aliasing: 1 for none, 4 or 16 for supersampled coverage
    background: Option<Color>
}
pub fn options() -> Options

pub fn render(s: scene.Scene, o: Options) -> Result<image.Image, string>
```

## 1. Scan-conversion — filling a path

The core. Flatten every curve to line segments, build an edge list, and for each scanline find the
crossings, sort them, and fill the spans the winding rule says are inside.

**Flattening tolerance is the one quality knob that matters.** A cubic is subdivided until it is
within a quarter-pixel of a straight line; too loose and circles look like polygons, too tight and a
page of text costs a hundred times what it should. Adaptive subdivision by flatness, not a fixed
number of steps -- a fixed count is wrong at both ends, visibly faceting a large circle while
wasting work on a tiny one.

**Both winding rules, from the same crossing list.** Non-zero sums the direction of each crossing
and fills where the sum is not zero; even-odd counts them and fills on odd. They differ only in the
predicate, so implementing one and "adding the other later" would mean rewriting the span loop.

**Anti-aliasing by coverage, not by post-blur.** Sample each scanline `samples` times vertically and
accumulate horizontal coverage per pixel, giving a value in `0..1` that scales the paint's alpha.
Blurring an aliased fill afterwards is cheaper and looks wrong -- edges get soft *and* still
stair-step.

## 2. Stroking — the outline Java 2D's `Stroke` promises

`StrokePath` becomes a fill of the stroked outline. This is the piece most likely to be subtly wrong,
so what it owes:

- **Caps**: `Butt` ends at the endpoint, `Square` extends by half the width, `Round` adds a
  semicircle (two cubics).
- **Joins**: `Bevel` connects the two offset edges directly, `Round` arcs between them, `Miter`
  extends both until they meet -- **and falls back to `Bevel` past `miter_limit`**, which is what the
  limit is for. A near-180° join without that fallback produces a spike thousands of units long, a
  classic and very visible bug.
- **Dashes**: walk the path accumulating arc length, emitting sub-paths per `dash`/`dash_phase`.
  Arc length of a cubic has no closed form, so it is measured on the flattened segments -- which is
  consistent with the fill path and good to well under a pixel.
- **Degenerate input**: a zero-length subpath with a round cap is a dot; with a butt cap it is
  nothing. Both are correct and the difference surprises people, so it is stated here.

## 3. Glyph rendering

`Text` is lowered to outlines -- the one place where `GlyphVector.getOutline()`'s equivalent is the
*right* answer, since a raster device has no font machinery of its own. `truetype.glyph_outline`
from phase 3 supplies the path; phase 3 implemented it for exactly this.

Per glyph: outline in font units, scale by `size / units_per_em`, translate to the pen position,
fill. **A glyph cache keyed by (face, glyph, size, subpixel offset)** matters more here than
anywhere else -- a page of text is tens of thousands of fills of a few hundred distinct shapes.

Hinting is not implemented. Unhinted small text is blurrier than a hinted renderer's; hinting is an
interpreter for a bytecode language embedded in the font, and it is a project in itself.

## Paints

- `Solid` is a constant.
- `LinearGradient` / `RadialGradient`: project each pixel's centre onto the gradient axis to get a
  parameter in `0..1` and interpolate the stops. Interpolation is in **premultiplied** alpha, or a
  gradient to transparent darkens through the middle -- another defect that reads as a rendering bug.
- `Pattern`: sample the image with the inverse pattern matrix. **Nearest-neighbour first**, bilinear
  later; nearest is honest and obviously improvable, where a half-finished bilinear is neither.

## Compositing and clipping

Source-over only, in premultiplied alpha. Blend modes are a PDF feature with no equivalent need
here.

Clipping is a **coverage mask**, not a rectangle: an arbitrary clip path is rasterised once to a
Gray8 coverage buffer and multiplied into everything the group draws. A rectangle fast path is worth
having because almost every clip is one, but the general case has to exist or `Group` with a path
clip silently ignores it -- which invariant 2 forbids.

## What phase 5 does *not* do

Hinting, subpixel (LCD) anti-aliasing, blend modes, JPEG sources (no decoder -- phase 4 explains
why; this backend rejects them with a message), colour management, SIMD. Performance beyond the
glyph cache: the first version should be obviously correct and measured before it is made fast,
since this project's history is full of structurally obvious optimisations that measured worse.

## Verification

Pixels are hard to assert on, so the checks are chosen to be *diagnostic* rather than golden where
possible:

- **Coverage arithmetic**: a fill of the whole canvas gives every pixel the paint's colour exactly;
  a fill of exactly half the canvas gives a sum of coverage equal to half the pixels. Off-by-one in
  the span loop shows up as a count, not as a visual difference.
- **Winding rules against each other**: a star path drawn non-zero is solid, drawn even-odd has a
  hole. Assert the centre pixel differs between the two. One assertion, catches a predicate mix-up.
- **Anti-aliasing**: a diagonal edge at `samples: 1` has only the two extreme colours; at
  `samples: 16` it has intermediate ones. Assert the count of distinct values *increases*.
- **The miter limit**: a near-180° join must produce a bounding box close to the stroke width, not a
  spike. Assert on the bounds of the resulting ink.
- **Stroke symmetry**: stroking a centred circle gives an image symmetric about both axes. Cheap,
  and catches a half-width error on one side.
- **Round trip against the document path**: render the same `Scene` to PDF and to an image, run
  `pdftotext` on the PDF and assert the text matches; the image cannot be compared to the PDF
  directly, but both coming from one scene is the property worth having.
- **Golden images only where nothing else will do**, written by the tests on first run and reviewed
  once -- and kept small, because a golden PNG in a repo is a file nobody ever re-examines.

## Build changes

One module registered. No native code. Possibly `math.atan2` is not enough and `asin`/`acos` are
needed for arc joins -- phase 1's build note already adds them.

## As built: part 1 (scan-conversion)

Landed 2026-10-16. `raster.render` fills paths with both winding rules, both gradients, image
patterns and `DrawImage`, under an arbitrary clip path and nested group transforms, to `Gray8`,
`Rgb8` or `Rgba8`. **Stroking and text are not implemented** and both return an `Err` naming the
part that will do them -- which is invariant 2, not a placeholder.

**`Options` gained two fields the sketch above does not have.** `images: [image.Image]`, because
`DrawImage` and `paint.Pattern` index a table and the sketch gave them nowhere to index into --
`pdf.Options` carries the same field for the same reason. And `max_pixels`, the guard
`image.decode_png` takes: a scene's size can come from a request, so a 50000x50000 page should be a
message rather than 40 GB of allocation.

**`paint.Pattern` renders here, and does not in the PDF backend.** That asymmetry is the point of
invariant 2: a raster device can sample an image through an inverse matrix, a PDF content stream
would need a `/Pattern` colour space, and the one that cannot do it says so rather than
approximating. Sampling is nearest-neighbour, as designed.

**Where the design's verification was wrong, and what replaced it.** Three of the listed checks did
not work as written:

- *"A diagonal edge at `samples: 1` has only the two extreme colours; at `samples: 16` it has
  intermediate ones."* It does not, because this implementation computes horizontal coverage
  **exactly** (`add_span` takes each span's real overlap with each pixel) and only quantises
  vertically. A 45-degree edge therefore gives every partial pixel *the same* coverage, and the
  distinct-level count is 3 at every sample setting. The check now uses a 1-in-8 slope, where
  vertical sampling is what decides whether the eight pixels under the edge are binary or a ramp:
  the count goes 2, 5, 10 for samples 1, 4, 16, while the area stays exactly 64 at both ends.
- *"A fill of exactly half the canvas gives a sum of coverage equal to half the pixels."* Kept, and
  it is the best check here -- 256.0000 for a full 16x16 canvas and 128.0000 for half, exactly, plus
  128.0314 for a triangle of the same area where the diagonal runs through partial pixels.
- *"Assert the XObject..."* -- the clip check was written against `pi*r^2` and should not be. A disc
  of radius 10 measures 312.30 against 314.16, which reads as a 0.6% bug and is not one: the
  flattened polygon is **inscribed** in the arc, and the deficit falls to 0.128% at radius 30,
  0.0122% at 100 and 0.0039% at 300 -- an absolute tolerance behaving exactly as designed. The
  check now compares clipping a full-canvas fill to a disc against *filling that disc directly*,
  which agree to every digit printed, and which is what actually proves the mask.

**The premultiplication check needed two attempts.** "A gradient to transparent darkens through the
middle" cannot be caught by a ramp from opaque red to transparent *red*: both stops have the same
RGB, so premultiplied and straight interpolation agree exactly, and so does fading to transparent
*black*. Interpolating on straight colour was mutated in and the first fixture passed anyway. The
two only diverge when the transparent stop carries a colour of its own, so the check is now opaque
**black** fading to transparent **white**: correct gives 0.0 at the midpoint, straight-colour
interpolation gives 1.0.

**Performance: measured, and two things kept because they measured.** Both left the output
byte-identical, and both were timed interleaved rather than across runs:

| | 600x800, 400 round rects, `samples: 4` |
| --- | --- |
| first working version | 3592 ms |
| + fill bounded to its own horizontal extent | 2253 ms (1.6x) |
| + `Solid` evaluated once per fill, not per pixel | 1667 ms (1.33x, 2.2x total) |

The first is why a 26-pixel-wide rectangle no longer costs 600 pixels a row; 1.6x rather than the
23x the widths suggest, because the rest is elsewhere. **What is still slow is named rather than
fixed**: `crossings_at` walks the whole edge list per sub-scanline, and holding the scene fixed while
raising `samples` from 1 to 8 moves the page from 1730 ms to 2863 ms -- about 160 ms per sample step
in the edge scan, leaving ~1570 ms that sampling does not touch (the per-pixel loop and interpreter
overhead). An active edge table is the textbook fix and is deferred to part 3, because text is the
workload with the most edges and is the right thing to measure it against.

**Still owed by this phase:** stroking (part 2), glyph rendering and its cache (part 3), and the
round trip against the document path -- "render the same `Scene` to PDF and to an image, run
`pdftotext` on the PDF and assert the text matches" -- which needs text to exist first.

## As built: part 2 (stroking)

Landed 2026-10-16. `StrokePath` renders: caps, joins, the miter limit, and dashes.

**It is its own module, `stroker`, not part of `raster`.** Widening a path is a path-to-path
operation with no device in it, and `pdf` refuses a gradient-stroked path with *"fill the stroked
outline instead"* -- advice that now names a real function. Had the stroker lived inside `raster`,
reaching it from `pdf` would have made one backend depend on the other, which is the wrong shape for
two siblings. `stroker` sits at layer 3 (`geom`, `paint`, `shape`) and `raster` calls it.

**Adaptive flattening moved into `shape.flattened`**, which both the stroker and the rasteriser now
use. Keeping raster's private copy would have left two implementations of the same subdivision in
one library -- the thing the previous commit had just removed from `pdf`. The swap is numerically
identical (every fixture number unchanged) and costs nothing measurable: 1730 ms against 1740 ms on
the same page, interleaved, which is noise. It removed 107 lines.

**The outline is a union of simple contours, not one offset curve.** Each flattened segment
contributes a rectangle, each interior vertex a join wedge, each end a cap, all wound the same way
and filled `NonZero`. The textbook alternative walks a single offset up one side and back down the
other, which has to solve self-intersection wherever the path turns tighter than its own width --
and that is the part that produces visible artefacts when it is nearly right. The union needs none
of it, and for filling it is exactly equivalent: one fill operation, so a self-overlapping stroke
blends once rather than twice. A round cap or join is therefore a **whole circle**, since the half
inside the adjoining rectangle is already covered. What it costs is more edges for the rasteriser,
and an outline nobody could usefully stroke *again*; neither backend needs either.

**That union has one real hazard, and it is now pinned by a test.** `NonZero` fills where crossing
directions sum to non-zero, so a contour wound *against* its neighbours punches a hole through the
overlap instead of joining it. All of `shape`'s builders are positively wound, so `stroker` emits
positive contours too -- and `tests/lang/graphics_stroke.zm` asserts `shape`'s orientation directly,
because the coupling is invisible otherwise. Reversing it in a mutation drops the round cap from
52.251 to **40.000**: the circle cancels the rectangle exactly, so the cap contributes nothing.

**Verification is areas computed by hand, not pixels blessed after the fact.** A 10-unit line of
width 4 is 40 square units with butt caps and 56 with square ones -- both exact to three decimals.
A closed 10x10 box stroked at width 2 is the band between a 12x12 and an 8x8 square, so 144-64 =
**80.000**, which is also perimeter times width and is exact *only* if all four miter joins fill
their corners; bevel cuts each by a 1x1 triangle for 78, and round replaces each with a quarter disc
for 78.824 (79.142 in exact arithmetic, less the 0.31 an inscribed radius-1 circle loses at this
tolerance). Dashes: a 20-long line at 4-on-4-off is 12 units of length, so 24.000, and 16.000 at
phase 4. The design's stroke-symmetry check passes with **0 and 0** mismatched pixels about both
axes.

**The miter limit** got the assertion the design asked for and it is decisively load-bearing. Two
segments meeting at about 2 degrees reach 180 units past their vertex unlimited, against 0.022 with
the limit in force -- and removing the fallback makes both cases read 180, so the test fails loudly
rather than subtly.

**Degenerate subpaths** are implemented deliberately, not left to fall out: a zero-length subpath is
a dot with a round cap (27.545, against pi*9 = 28.274 for an inscribed radius-3 circle), a square
with a square cap (36.000 exactly), and **nothing at all** with a butt cap (0.000). All three are
correct and the difference gets reported as a bug either way, so all three are pinned.

Dashing a *closed* subpath produces open runs that are capped at the start point rather than joined
across it. Most renderers do the same; it is recorded here rather than discovered later.

**Still owed:** glyph rendering and its cache (part 3), the active edge table deferred from part 1 so
it can be measured against text, and the round trip against the document path, which needs text.
