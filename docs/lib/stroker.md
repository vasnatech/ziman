# `stroker`

The outline of a stroked path, as a path.

`BasicStroke.createStrokedShape`'s job, and phase 5's part 2. It is a **path to path** operation
with no device in it, which is why it is its own module rather than part of `raster`: the PDF
backend refuses a gradient-stroked path today with "fill the stroked outline instead", and that
refusal becomes an implementation the moment something calls this. A stroker inside `raster`
would have made `pdf` depend on the raster backend to reach it, which is the wrong way round for
two sibling backends.

**The outline is a union of simple contours, not a single offset curve.** Each flattened segment
contributes a rectangle, each interior vertex a join wedge, each end a cap -- all wound the same
way and filled `NonZero`, where overlapping contours accumulate rather than cancel. The textbook
alternative is to walk one offset curve up one side and back down the other, which has to solve
self-intersection where the path turns tighter than its own width: a hard problem that produces
visible artefacts when it is got slightly wrong. The union needs none of that, and for *filling*
it is exactly equivalent -- one fill operation, so a self-overlapping stroke still blends once
rather than twice.

What it costs: more contours, so more edges for the rasteriser, and the result is not a clean
outline anyone could stroke *again*. Neither is needed by either backend.

`use stroker` -- or `use stroker.Name` to bring one item in bare.

## Example

```
use geom
use paint
use shape
use stroker

// Turning a stroke into a fill. A stroked line is conceptually a thick line; to a renderer that
// has only fills -- and to PDF, which has its own stroking but not every dash and join we allow --
// it has to become an outline: the closed path that encloses the thick line. That is this module.
fn demo() -> Result<unit, string> {
    val b = shape.path_builder()
    shape.move_to(b, geom.point_of(10.0, 50.0))
    shape.line_to(b, geom.point_of(90.0, 50.0))
    val line = shape.built(b, shape.Winding.NonZero)
    print(len(line.segments))

    // A 10-wide stroke of a horizontal line is a rectangle 10 tall, so the outline's bounds say
    // the width was applied to both sides of the line, not one.
    val outlined = try stroker.outline(line, paint.stroke_of(10.0))
    val box = shape.path_bounds(outlined)
    printf("{} {} {} {}\n", box.x, box.y, box.width, box.height)

    // Caps extend the ends; a square cap makes the outline longer than the line by half a width
    // at each end, where a butt cap does not.
    val butt = shape.path_bounds(try stroker.outline(line, with paint.stroke_of(10.0) { cap: paint.Cap.Butt }))
    val square = shape.path_bounds(try stroker.outline(line, with paint.stroke_of(10.0) { cap: paint.Cap.Square }))
    printf("{} {}\n", butt.width, square.width)

    // A dashed stroke outlines each dash separately, so the result is several subpaths rather than
    // one -- which is exactly why a backend without dashes can still draw it.
    val dashed = try stroker.outline(line, paint.dashed(10.0, [10.0, 10.0]))
    print(len(dashed.segments) > len(outlined.segments))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("stroker example failed: {}\n", e)
    }
}
```

Output:

```
2
10.0 45.0 80.0 10.0
80.0 90.0
true
```

### `outline`

```
pub fn outline(p: Path, how: Stroke) -> Result<Path, string>
```

### `outline_with`

```
pub fn outline_with(p: Path, how: Stroke, tolerance: float) -> Result<Path, string>
```
