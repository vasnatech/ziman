# `svg`

An SVG file as shapes: one `d` attribute, or a whole document.

**A reader for the geometry, not a graphics stack.** `shape` already has the segment vocabulary
an SVG path is written in -- `move_to`, `line_to`, `quad_to`, `cubic_to`, `close_path` -- and
`geom` has the matrices a `transform` is written in, so reading an SVG is a parser over types
this library already had. What it answers is where the shapes are and what colour each one asked
for. Text, gradients, patterns, clips, masks, filters and `<use>` are out, and **it says so
rather than dropping them**: a reader that silently ignored a clip would draw a shape nobody
designed, and nothing in the result could explain it.

**Why this exists at all**: `widget.IconSource.Drawn` takes a path in a unit box, and before
this the only way to get one was to write it by hand with the builder. An icon set on disk was
unreachable, which is what made the drawn source a developer's tool rather than a designer's.
`svg.icon(text)` is now the whole of that: parse the file, merge what it paints, scale it in.

**Two halves, usable separately.** `path_data` reads one `d` attribute -- every command
(`M L H V C S Q T A Z`, absolute and relative), implicit repetition, the number grammar as SVG
actually writes it (`.5`, `-.5`, `1e-3`, and `10-5` as two numbers), and **arcs lowered to
cubics**, because `shape` has no arc primitive and neither does PDF (`ellipse_path` makes the
same choice with the same constant). `parse` reads the document around it: the `viewBox`, the
basic shapes (`rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`), the groups and their
`transform`s, and the presentation each figure inherits -- fill, stroke, fill rule, dashes,
opacity, `currentColor`.

**A `Figure` is in the document's own coordinates with every transform already applied**, which
is what makes the figures comparable without the tree they came out of. `filled_path` then
merges them into the single path a monochrome icon wants, strokes turned into areas by
`stroker.outline` -- and refuses, rather than flattening, the two things one path cannot say:
two fill rules, and a part that is not fully opaque. `solid_path` is the approximation for a
caller who would rather have the shape than the exactness, and `Drawing.figures` is there for
one who wants to paint them properly.

**What that comes to on a real icon theme, which is the measurement this was built against**:
the 1,206 SVG files under `/usr/share/icons` on a Fedora desktop, each through `parse` and then
one path.


```
exact (filled_path)                   718 read
dropping the faint (solid_path, 1.0)  917 read, 20 of them empty
```

and the 289 the second one still refuses, by cause:


```
139  a gradient or pattern fill -- `fill="url(#a)"`, which a path cannot carry
 69  an even-odd fill on a figure that is also stroked
 30  a clip, a mask or a filter
 30  two fill rules across the figures
 15  `<use>`
  3  `<text>`
  2  `<image>`
  1  a declaration saying iso-8859-1, read through `parse(text)` instead of `parse(bytes)`
```

Three things worth reading off that list. The **99 winding conflicts are not a scope decision
but an arithmetic one**: an even-odd outline and a nonzero one fill differently wherever
subpaths nest or a subpath crosses itself, and a bounding-box test cannot tell the difference
(a pentagram is one subpath that crosses itself and is hollow under one rule). The **20 empty
drawings are files whose every figure is faint**, which a lower cut reads. And the **one
encoding refusal is the charset work doing its job**: a file whose declaration is not UTF-8 has
to arrive as bytes, which is what `parse([byte])` is for.

`use svg` -- or `use svg.Name` to bring one item in bare.

## Example

```
use geom
use shape
use svg

// An SVG file as shapes: one `d` attribute with `path_data`, or the whole document with `parse`.
// What it is not is a graphics stack -- no text, no gradients, no clipping, and it says so rather
// than drawing a shape nobody designed.
// A box in one line, rounded: an arc lowered to cubics lands a rounding error away from a whole
// number, and `0.00` is what a reader wants to see rather than `-3.4e-16`.
fn box_text(r: geom.Rect) -> string {
    return format("{:.2},{:.2} {:.2}x{:.2}", r.x, r.y, r.width, r.height)
}

fn fill_text(f: svg.Fill) -> string {
    return switch f {
        Color(c) { format("#{:.2},{:.2},{:.2}", c.red, c.green, c.blue) }
        Current { "currentColor" }
        Nothing { "unpainted" }
    }
}

fn demo() -> Result<unit, string> {
    // Absolute and relative spell the same square.
    val absolute = try svg.path_data("M 0 0 H 10 V 10 H 0 Z")
    val relative = try svg.path_data("m 0 0 h 10 v 10 h -10 z")
    print(shape.path_bounds(absolute) == shape.path_bounds(relative))

    // The number grammar is the part that surprises: `10-5` is two numbers, because a minus sign
    // begins one, and `20.5.5` is two more.
    print(box_text(shape.path_bounds(try svg.path_data("M0 0L10-5L20.5.5Z"))))

    // An arc becomes cubics, since neither `shape` nor PDF has an arc primitive. A half circle of
    // radius 5 is ten across and five tall, however it is written.
    print(box_text(shape.path_bounds(try svg.path_data("M 0 0 A 5 5 0 0 1 10 0"))))
    // A radius too small to reach its endpoint is scaled up rather than refused -- which is what
    // an exported file looks like after its numbers were rounded.
    print(box_text(shape.path_bounds(try svg.path_data("M 0 0 A 1 1 0 0 1 10 0"))))

    // **And the step that makes a path an icon**: an icon set draws in a 24-unit grid and
    // `widget.IconSource.Drawn` wants a unit box. Aspect is kept and the drawing is centred, so a
    // wide icon floats rather than stretching.
    val tick = "M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"
    print(box_text(shape.path_bounds(try svg.icon_path(tick, geom.rect_of(0.0, 0.0, 24.0, 24.0)))))
    print(box_text(shape.path_bounds(try svg.icon_path("M 0 0 H 24 V 12 H 0 Z", geom.rect_of(0.0, 0.0, 24.0, 12.0)))))

    // **And the document around it**, which is how an icon actually arrives: a file with a
    // viewBox, a group or two, and a title nobody draws.
    val file = concat(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">",
        "<title>a tick in a ring</title>",
        "<circle cx=\"12\" cy=\"12\" r=\"11\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>",
        "<g transform=\"translate(1 0)\"><path fill=\"currentColor\" d=\"",
        tick,
        "\"/></g>",
        "</svg>"
    )
    val drawing = try svg.parse(file)
    printf("{} figures in {}\n", str(len(drawing.figures)), box_text(drawing.view))
    // Each figure is in the viewBox's own coordinates: the `translate(1 0)` above the tick is
    // already in its path, so a caller never sees the tree it came out of.
    for f in drawing.figures {
        printf("  {} {}\n", box_text(shape.path_bounds(f.path)), fill_text(f.fill))
    }

    // **`currentColor` reaches the caller as a case rather than a colour**, because the colour of
    // an icon is the ink of the control it sits on, and only the control knows that.

    // One path out of the whole drawing: the ring is stroked, not filled, so its stroke becomes an
    // area on the way in -- which is what makes an outline icon set come out as something.
    printf("merged: {}\n", box_text(shape.path_bounds(try svg.filled_path(drawing))))
    // And the file as an icon, in one call: parsed, merged, scaled into the unit box.
    printf("icon: {}\n", box_text(shape.path_bounds(try svg.icon(file))))

    // What it will not read, it refuses -- a path that quietly skipped a command it did not know
    // would draw something nobody drew, and so would a document.
    if Err(why) = svg.path_data("M 0 0 X 5 5") {
        print(why)
    }
    if Err(why) = svg.parse("<svg viewBox=\"0 0 8 8\"><text x=\"0\" y=\"0\">hi</text></svg>") {
        print(why)
    }
    if Err(why) = svg.parse("<svg viewBox=\"0 0 8 8\"><path fill=\"url(#g)\" d=\"M0 0h8v8H0z\"/></svg>") {
        print(why)
    }
    // Two fill rules, or a part at 30%, are things one path cannot say: `figures` is still there.
    val faint = try svg.parse(concat(
        "<svg viewBox=\"0 0 8 8\">",
        "<path opacity=\"0.3\" d=\"M0 0h8v8H0z\"/><path d=\"M2 2h4v4H2z\"/></svg>"
    ))
    if Err(why) = svg.filled_path(faint) {
        print(why)
    }
    // And the door for a caller who would rather have the shape: the faint layer is dropped and
    // the solid one kept, which is how a fifth of a desktop icon theme has to be read.
    printf("solid: {}\n", box_text(shape.path_bounds(try svg.solid_path(faint, 1.0))))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("svg example failed: {}\n", e)
    }
}
```

Output:

```
true
0.00,-5.00 20.50x5.50
-0.00,-5.00 10.00x5.00
-0.00,-5.00 10.00x5.00
0.14,0.23 0.73x0.56
0.00,0.25 1.00x0.50
2 figures in 0.00,0.00 24.00x24.00
  1.00,1.00 22.00x22.00 unpainted
  4.41,5.59 17.59x13.41 currentColor
merged: -0.01,-0.01 24.01x24.01
icon: -0.00,-0.00 1.00x1.00
svg: expected a number at 6, found 'X'
svg: <text> is not something this reader can draw -- it reads paths, rects, circles, ellipses, lines, polylines, polygons and the groups around them
svg: fill="url(#g)" is a gradient or a pattern, and a path can only carry a colour -- read the document with xml and build the paint yourself
svg: figure 1 is at 0.3 opacity, which one path cannot express -- paint Drawing.figures yourself
solid: 2.00,2.00 4.00x4.00
```

### `path_data`

```
pub fn path_data(d: string) -> Result<shape.Path, string>
```

`d` as a path, or an `Err` naming what it could not read and where.

**Refuses rather than guessing**, which for a file format read off disk is the only defensible
answer: a path that silently drops a command it did not understand draws something that is not
the icon anybody drew, and nobody can tell from the result.

## A path as an icon

### `in_unit_box`

```
pub fn in_unit_box(p: shape.Path, view: geom.Rect) -> shape.Path
```

A path scaled into the **unit box** `widget.IconSource.Drawn` wants, from the box the drawing
was authored in.

**This is the step that makes an imported path usable as an icon**, and it is here rather than
in `widget` because it is geometry: an icon set draws in a 24-by-24 grid, or 16, or 512, and the
control it lands on knows none of those. `view` is the SVG `viewBox`.

**Aspect is preserved and the drawing is centred**, which is what every icon set assumes: a
24x18 drawing in a square box is stretched by nothing and floats in the middle, because an icon
squashed to fill its square is a wrong icon rather than a differently-sized one.

### `icon_path`

```
pub fn icon_path(d: string, view: geom.Rect) -> Result<shape.Path, string>
```

The same from a `d` attribute and a view box, which is the whole of importing an icon:
`svg.icon_path(d, geom.rect_of(0.0, 0.0, 24.0, 24.0))`.

## The document side: a file, not one attribute

### `Fill`

```
pub enum Fill {
    Color(paint.Color)
    Current
    Nothing
}
```

**How a figure is painted, including the two answers that are not colours.** `currentColor` means
the colour of whatever this drawing is placed in -- a control's ink, for an icon -- and `none`
means the part is not painted at all. Resolving either here would be a guess, so both travel as
cases and the caller decides: an icon tints `Current` with `style.ink`, and `filled_path` keeps
`Current` and drops `Nothing`.

### `Figure`

```
pub struct Figure {
    path: shape.Path
    fill: Fill
    stroke: Fill
    // Width, caps, joins and dashes, as the document asked for them. Meaningful when `stroke` is
    // not `Nothing`; `stroker.outline` turns it into an area.
    edge: paint.Stroke
    // `opacity` times `fill-opacity`, 0.0 to 1.0. A figure at 0 is dropped before it gets here, so
    // this is above 0 and at most 1.
    opacity: float
}
```

One thing the document draws, in the coordinates its `viewBox` is written in: every `transform`
on it and on every group above it is already in the path.

**Flattening the transforms here rather than carrying a matrix per figure** is what makes the
figures comparable -- a caller can union their bounds, hit-test them or merge them without
knowing the tree they came out of, which was the whole point of reading the document.

### `Drawing`

```
pub struct Drawing {
    // The `viewBox`, or `0 0 width height` when the root gives those instead, or the union of what
    // the figures cover when it gives neither.
    view: geom.Rect
    figures: [Figure]
}
```

### `parse`

```
pub fn parse(text: string) -> Result<Drawing, string>
```

An SVG document as the figures it draws. `Err` names the element or attribute it could not read.

```
pub fn parse(data: [byte]) -> Result<Drawing, string>
```

The same from bytes, which is how a file arrives. The charset comes from the XML declaration or
the byte-order mark, because that is `xml.parse`'s job and not this reader's.

### `drawing_of`

```
pub fn drawing_of(doc: xml.Document) -> Result<Drawing, string>
```

For a caller who already has the document -- a theme index that reads several things out of one
file, or a test that builds the tree by hand.

## The whole drawing as one path

### `filled_path`

```
pub fn filled_path(d: Drawing) -> Result<shape.Path, string>
```

**Everything the drawing paints, as one path**: each filled figure's own outline, and each
stroked figure's stroke turned into an area by `stroker.outline`. This is what a monochrome icon
needs, because a themed icon is tinted by the control it sits on and its parts are not separately
coloured.

**It refuses rather than flattening away a difference it cannot express.** Two fill rules in one
drawing are not one path -- an even-odd outline inside a nonzero one fills differently in each --
and a figure at 30% opacity is not solid. Both cases return an `Err` naming the figure and leave
`Drawing.figures` to a caller that wants to paint them one at a time, which is the only way to
honour either.

### `solid_path`

```
pub fn solid_path(d: Drawing, least: float) -> Result<shape.Path, string>
```

**Every figure at or above `least` opacity, taken as solid, and the fainter ones left out.** The
approximation `filled_path` refuses to make, offered as its own function so that a caller asks
for it on purpose.

**Why it exists, measured rather than assumed**: of the 1,206 SVGs under `/usr/share/icons` on a
Fedora desktop, **235 have a figure that is not fully opaque** -- Adwaita draws two-tone icons as
a solid outline over a fill at 0.35 or 0.75 -- so an exact reader refuses a fifth of a desktop's
icons. Dropping the faint layer keeps the part that carries the shape, which is the right trade
for an icon that is going to be tinted with one ink anyway; keeping it solid would turn a
two-tone icon into a blob, and that is the trade this does not make.

`least` is a cut rather than a blend: at 1.0 only the fully opaque figures survive, at 0.5 the
0.75 layers come too and arrive solid. Anything a *caller* wants to shade properly is in
`Drawing.figures` with its own opacity, which is the only way to get it right.

### `icon`

```
pub fn icon(text: string) -> Result<shape.Path, string>
```

**A file on disk as an icon**: parse it, merge what it paints, and scale that into the unit box
`widget.IconSource.Drawn` wants. The three steps are separately available for a caller who wants
the figures or the document's own coordinates.

```
pub fn icon(data: [byte]) -> Result<shape.Path, string>
```
