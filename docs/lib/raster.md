# `raster`

The second backend: a `Scene` to an `Image`.

The PDF backend is a serialiser; this one has to *do* what a PDF reader was doing. Three
separable pieces live here -- scan-conversion, stroking, and glyph rendering -- and they land in
that order, because each is testable without the next and the first is the one the other two are
written in terms of.

**Correct first, and measured rather than guessed at.** The design note for this phase says the
first version should be obviously correct and measured before it is made fast, because this
project's history is full of structurally obvious optimisations that measured worse. Two were
kept because they were measured, both with the output byte-identical: bounding a fill to its own
horizontal extent (1.6x) and evaluating a `Solid` paint once per fill instead of once per pixel
(a further 1.33x). Together 3592 ms to 1667 ms for a 600x800 page of 400 round rectangles at
`samples: 4`.

**What is still slow, measured and not fixed here.** `crossings_at` walks the whole edge list for
every sub-scanline, so cost grows with (rows x samples x edges). Holding the samples at 1 and
raising them to 8 moves that page from 1730 ms to 2863 ms, which puts roughly 160 ms per sample
step in the edge scan and leaves about 1570 ms that sampling does not touch -- the per-pixel loop
and plain interpreter overhead. The textbook fix is an active edge table (edges sorted by y, a
list maintained across scanlines), which would cut the scan by about the edge count; it is not
here because part 1's job is to be right, and because that is a change worth measuring against
text, which is the workload with the most edges and does not exist yet.

`use raster` -- or `use raster.Name` to bring one item in bare.

## Example

```
use geom
use image
use paint
use raster
use scene
use shape

// Rendering a scene to pixels. This is where a drawing stops being a description and becomes an
// image -- and where a graphics example becomes checkable, because the answer is a value whose
// pixels can be read back rather than a file somebody has to look at.
fn demo() -> Result<unit, string> {
    val red_square = scene.fill(
        shape.rect_path(geom.rect_of(20.5, 20.5, 59.0, 59.0)),
        paint.solid(paint.rgb(1.0, 0.0, 0.0))
    )
    val drawing = scene.with_background(
        scene.with_ops(scene.scene_of(100.0, 100.0), [red_square]),
        paint.solid(paint.white())
    )

    val out = try raster.render(drawing, raster.options())
    printf("{} {}\n", out.width, out.height)

    // Inside the square is the fill; outside it is the background. Asserting the pixels is what
    // makes this a test rather than a picture.
    val inside = try image.pixel_at(out, 50, 50)
    printf("{} {} {}\n", inside.red, inside.green, inside.blue)
    val outside = try image.pixel_at(out, 5, 5)
    printf("{} {} {}\n", outside.red, outside.green, outside.blue)

    // The square's left edge is at x=20.5, so pixel 20 is half covered -- and the renderer says so
    // rather than rounding it in or out. That is anti-aliasing, and `samples` is how much of it
    // you pay for.
    val edge = try image.pixel_at(out, 20, 50)
    print(edge.green > 0.0 && edge.green < 1.0)
    val smoother = try raster.render(drawing, with raster.options() { samples: 4 })
    val edge4 = try image.pixel_at(smoother, 20, 50)
    print(edge4.green > 0.0 && edge4.green < 1.0)

    // The format is asked for, not guessed: a grayscale render of the same scene is one channel.
    val grey = try raster.render(drawing, with raster.options() { format: image.Pixels.Gray8 })
    print(image.channels_of(grey.format))
    print(len(grey.data))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("raster example failed: {}\n", e)
    }
}
```

Output:

```
100 100
1.0 0.0 0.0
1.0 1.0 1.0
true
true
1
10000
```

### `Options`

```
pub struct Options {
    format: image.Pixels
    // Anti-aliasing: vertical sub-scanlines per pixel row. 1 is none, 4 is good, 16 is as much as
    // this method can usefully give. Horizontal coverage is *exact* either way -- see `add_span` --
    // so this number only quantises the vertical direction.
    samples: int
    // What the canvas starts as. `None` is transparent, which an `Rgb8` or `Gray8` output then
    // flattens to black, because those formats have nowhere to keep the transparency.
    background: Option<Color>
    // The images `DrawImage` and `paint.Pattern` index into, exactly as `pdf.Options` carries them.
    images: [image.Image]
    // The faces a `TextRun` indexes, the same table `pdf.Options` has. **Only embedded faces can be
    // drawn here**: the standard fourteen are names a PDF reader resolves against fonts it already
    // has, and a raster device has no such library to resolve against.
    fonts: [font.FontRef]
    // The same guard `image.decode_png` takes, for the same reason: a scene's size can come from a
    // request, and a 50000x50000 page should be a message rather than 40 GB of allocation.
    max_pixels: int
    // How many device pixels one unit of the scene becomes.
    //
    // **This is what makes the scene's units points rather than pixels**, and it is here rather
    // than on `scene.Scene` deliberately: a `Scene` still does not know what a unit is -- its own
    // comment promises that the backend decides -- and this is the backend deciding. The PDF
    // backend reads the same scene as points at 1/72 inch and wants no such field.
    //
    // 1.0 means a point is a pixel, which is the default and what every existing caller gets.
    // 1.5 means a 100-unit box is drawn 150 pixels wide, with glyph *outlines* scaled before being
    // rasterised rather than a 12-unit bitmap stretched -- so text at 1.5 is 18 real pixels of
    // type, not a blurred 12.
    //
    // A fractional ratio leaves a one-unit line straddling two pixels, which reads grey rather
    // than black. Accepted here, as CSS and Flutter accept it: snapping thin edges to whole pixels
    // is a rendering feature, not a property of the unit.
    px_per_pt: float
}
```

### `options`

```
pub fn options() -> Options
```

### `Surface`

```
pub struct Surface {
    target: Target
    // How many rasterised glyphs to keep -- `glyph_cache_limit` unless a caller says otherwise.
    //
    // **A field rather than only a constant, so that the bound can be tested.** An untested bound
    // in a long-lived process is the thing `Target`'s comment was worried about, and a fixture
    // cannot reach 8192 distinct glyphs without a font and a minute; with a surface of limit 2 it
    // takes three renders. `with s { glyph_limit: 2 }` is how a test asks.
    glyph_limit: int
}
```

A render target held across frames, so that repainting does not allocate one.

**Called a surface and not a canvas because `image.Canvas` already exists one module away** and
is a different thing: a buffer of bytes a caller sets pixels on by hand. This is an accumulation
buffer of floats that `render_into` writes whole scenes to, and the name every library that has
both uses for it.

**This exists because the probe said so.** A render asks `new_target` for `width * height * 4`
floats -- 4,096,000 at 1280x800, measured at 16.5 ms a frame to allocate and zero, with the
collector warning about the repeated 32 MB block of its own accord. That, and the collection it
causes, was about a quarter of a frame: more than the edge walk and twice the character map, and
nothing that could be fixed by moving a loop to C, because the cost is asking for the memory at
all. `render` keeps doing exactly that, because a one-off render of a page should not have to
manage one; a window repainting sixty times a second should.

**It also keeps the glyph cache**, which `render` throws away per call. The comment on `Target`
explains that choice -- a cache outliving the call "would need invalidating when a face changed
and bounding so a long-lived process could not grow one forever. A page is where the reuse is
anyway." That was right when the target was a page. F269 and F277 make it a window, so the reuse
is across frames, and the two conditions it names are met here: the key now carries
`truetype.face_id`, so a changed face cannot be served a stale entry, and the cache is bounded
below.

Holds a `MutableArray` and a `MutableMap`, so **a surface belongs to the thread that made it** and
using it from another fails at run time. One thread paints a window; that is the shape of every
toolkit and of F269's event loop.

### `glyph_cache_limit`

```
pub val glyph_cache_limit = 8192
```

How many rasterised glyphs a surface keeps before it starts again.

Checked once at the **start** of a frame rather than on each insertion, which matters: a frame
needing more than this still gets a working cache for its whole length and pays once, where
evicting on insertion would thrash inside the frame and be slower than no cache at all.

Dropped wholesale rather than least-recently-used, because an LRU needs a linked list and an
order to maintain per hit, and the next frame refills what it actually draws. A window whose text
is stable never reaches this; one that does is paying a fraction of what it would pay to
re-rasterise everything every frame.

8192 entries: a glyph is keyed by face, transform, id and one of sixteen subpixel positions, so
this is a few hundred distinct glyphs across a handful of sizes.

### `surface_of`

```
pub fn surface_of(width_pt: float, height_pt: float, px_per_pt: float) -> Result<Surface, string>
```

A surface of a fixed pixel size. Reused by `render_into` for as many frames as the window keeps
that size; a resize makes a new one, which is the right trade because a resize is rare and a
repaint is not.
Takes **scene units and the ratio, not pixels, so that a surface and a scene cannot disagree.**
An application sizes its window in points; the surface has to be however many pixels those points
come to. Given pixels directly, one number would be derived in two places, and the first caller
to set a ratio other than 1.0 would meet `render_into`'s size refusal -- a correct error about a
mistake that did not need to be possible.

### `surface_width`

```
pub fn surface_width(c: Surface) -> int
```

### `surface_height`

```
pub fn surface_height(c: Surface) -> int
```

### `cached_glyphs`

```
pub fn cached_glyphs(c: Surface) -> int
```

How many glyphs the surface is holding -- for a caller that wants to see the cache working, and
for the test that proves it does.

### `render_into`

```
pub fn render_into(c: Surface, s: Scene, o: Options) -> Result<image.Image, string>
```

A scene onto a surface the caller owns, reusing its pixels and its glyph cache.

**The scene must be exactly the surface's size**, and a mismatch is refused rather than scaled or
cropped: a window whose scene and surface disagree has a bug in its resize handling, and quietly
drawing the wrong thing would hide it. The message names both sizes.

### `render`

```
pub fn render(s: Scene, o: Options) -> Result<image.Image, string>
```
