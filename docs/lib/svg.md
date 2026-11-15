# `svg`

An SVG path's `d` attribute as a `shape.Path`.

**A reader for one attribute, not a graphics stack**, which is the scope the icon work settled
on: `shape` already has the segment vocabulary an SVG path is written in -- `move_to`,
`line_to`, `quad_to`, `cubic_to`, `close_path` -- so importing an icon is a parser for the
string in `d=` and nothing more. Styling, gradients, text, clipping and the document around it
are all somebody else's problem, and most of them are problems an *icon* does not have: an icon
set ships one outline per file.

**Why this exists at all**: `widget.IconSource.Drawn` takes a path in a unit box, and before
this the only way to get one was to write it by hand with the builder. An icon set on disk was
unreachable, which is what made the drawn source a developer's tool rather than a designer's.

What is here: every path command (`M L H V C S Q T A Z`, absolute and relative), implicit
repetition, the number grammar as SVG actually writes it (`.5`, `-.5`, `1e-3`, and `10-5` as
two numbers), and **arcs lowered to cubics**, because `shape` has no arc primitive and neither
does PDF -- `ellipse_path` makes the same choice with the same constant.

`use svg` -- or `use svg.Name` to bring one item in bare.

## Example

```
use geom
use shape
use svg

// An SVG path's `d` attribute as a `shape.Path`: what importing an icon is, once `shape` already
// has the segments an SVG path is written in. Not a graphics stack -- no styling, no document, no
// text: an icon set ships one outline per file and this reads that outline.
// A box in one line, rounded: an arc lowered to cubics lands a rounding error away from a whole
// number, and `0.00` is what a reader wants to see rather than `-3.4e-16`.
fn box_text(r: geom.Rect) -> string {
    return format("{:.2},{:.2} {:.2}x{:.2}", r.x, r.y, r.width, r.height)
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

    // What it will not read, it refuses: a path that quietly skipped a command it did not know
    // would draw something nobody drew.
    if Err(why) = svg.path_data("M 0 0 X 5 5") {
        print(why)
    }
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
svg: expected a number at 6, found 'X'
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
