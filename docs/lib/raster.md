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
