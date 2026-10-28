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
}
```

### `options`

```
pub fn options() -> Options
```

### `render`

```
pub fn render(s: Scene, o: Options) -> Result<image.Image, string>
```
