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
