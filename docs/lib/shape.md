# `shape`

Paths, and the five segment types everything drawable reduces to.

This vocabulary is the narrow waist of the whole library (docs/graphics/README.md): a rectangle,
an ellipse, a glyph outline and an arbitrary curve are all the same five cases, so the PDF
backend is a serialiser over them and the raster backend a scan-converter over them.

`use shape` -- or `use shape.Name` to bring one item in bare.

## Example

```
use geom
use shape

// Paths: the outlines everything else fills, strokes or clips with. A path is built by moving and
// drawing, then frozen with a winding rule -- the rule that decides what "inside" means where the
// outline crosses itself, which is why it belongs to the path rather than to whoever fills it.
fn demo() -> Result<unit, string> {
    val b = shape.path_builder()
    shape.move_to(b, geom.point_of(10.0, 10.0))
    shape.line_to(b, geom.point_of(90.0, 10.0))
    shape.line_to(b, geom.point_of(90.0, 60.0))
    shape.close_path(b)
    val triangle = shape.built(b, shape.Winding.NonZero)
    print(len(triangle.segments))

    // The bounds of a path are the box a renderer can clip against before it does any work.
    val box = shape.path_bounds(triangle)
    printf("{} {} {} {}\n", box.x, box.y, box.width, box.height)

    // Curves are segments like any other, and there are two kinds of bounds for them on purpose:
    // `path_bounds` is the control-point hull -- cheap, and always encloses the curve, which is
    // all a clip test or a page-fit check needs -- while `path_tight_bounds` finds the real
    // extrema. This cubic is pulled toward y=100 but never reaches it.
    val c = shape.path_builder()
    shape.move_to(c, geom.point_of(0.0, 0.0))
    shape.cubic_to(c, geom.point_of(0.0, 100.0), geom.point_of(100.0, 100.0), geom.point_of(100.0, 0.0))
    val curve = shape.built(c, shape.Winding.NonZero)
    printf("{} {}\n", shape.path_bounds(curve).height, shape.path_tight_bounds(curve).height)

    // A rectangle is a path too, so there is one kind of thing to fill rather than two.
    val rect = shape.rect_path(geom.rect_of(0.0, 0.0, 10.0, 10.0))
    print(len(rect.segments))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("shape example failed: {}\n", e)
    }
}
```

Output:

```
4
10.0 10.0 80.0 50.0
100.0 75.0
5
```

### `Quad`

```
pub struct Quad {
    control: Point
    end: Point
}
```

A path is a sequence of these. Nothing else is ever needed, and nothing more is ever accepted.

`QuadTo` is kept even though a quadratic *is* exactly a cubic, and cairo manages with four cases
by elevating on input. The reason is that TrueType outlines are quadratic and the raster backend
will rasterise glyph outlines at every size a document uses, so elevating per glyph per use is
work for nothing. The PDF backend elevates once at serialisation, where it costs one pass over a
path about to be written out anyway. See docs/graphics/phase-1-geometry-and-scene.md.
A variant holds zero or one payload (docs/SYNTAX.md), so the two curve segments carry a named
struct rather than a tuple. That reads better at the use sites anyway: `q.control` beats `q.0`.

### `Cubic`

```
pub struct Cubic {
    control1: Point
    control2: Point
    end: Point
}
```

### `Segment`

```
pub enum Segment {
    MoveTo(Point)
    LineTo(Point)
    QuadTo(Quad)
    CubicTo(Cubic)
    Close
}
```

### `Winding`

```
pub enum Winding {
    NonZero
    EvenOdd
}
```

Which side of a path counts as inside, where it overlaps itself. `NonZero` sums the direction of
each crossing and fills where the sum is not zero; `EvenOdd` counts them and fills on odd. A star
drawn one way is solid and the other way has a hole.

### `Path`

```
pub struct Path {
    segments: [Segment]
    winding: Winding
}
```

### `PathBuilder`

```
pub struct PathBuilder {
    segments: MutableArray<Segment>
}
```

Building one. Thread-bound by holding a `MutableArray`, which is what makes a builder a builder
here -- see the mutability discussion in docs/DESIGN.md.

### `path_builder`

```
pub fn path_builder() -> PathBuilder
```

### `move_to`

```
pub fn move_to(b: PathBuilder, p: Point)
```

### `line_to`

```
pub fn line_to(b: PathBuilder, p: Point)
```

### `quad_to`

```
pub fn quad_to(b: PathBuilder, control: Point, p: Point)
```

### `cubic_to`

```
pub fn cubic_to(b: PathBuilder, c1: Point, c2: Point, p: Point)
```

### `close_path`

```
pub fn close_path(b: PathBuilder)
```

### `built`

```
pub fn built(b: PathBuilder, winding: Winding) -> Path
```

### `path_of`

```
pub fn path_of(segments: [Segment], winding: Winding) -> Path
```

### `empty_path`

```
pub fn empty_path() -> Path
```

## The shapes everyone actually draws

### `line_path`

```
pub fn line_path(from: Point, to: Point) -> Path
```

### `rect_path`

```
pub fn rect_path(r: Rect) -> Path
```

### `polygon_path`

```
pub fn polygon_path(points: [Point], closed: bool) -> Path
```

### `ellipse_path`

```
pub fn ellipse_path(r: Rect) -> Path
```

### `circle_path`

```
pub fn circle_path(centre: Point, radius: float) -> Path
```

### `round_rect_path`

```
pub fn round_rect_path(r: Rect, radius: float) -> Path
```

A rectangle with rounded corners. A radius past half the shorter side is clamped, rather than
refused, because that is what every other drawing API does with it and the alternative is an
error for something with an obvious meaning.

## Transforming and measuring

### `transform_path`

```
pub fn transform_path(p: Path, m: Matrix) -> Path
```

### `path_bounds`

```
pub fn path_bounds(p: Path) -> Rect
```

Every point mentioned by the path, enclosed: the control-point hull.

Always encloses the curve, because a Bézier lies within the convex hull of its control points,
and is cheap -- which is all a clip test or a page-fit check needs. For the real extrema, see
`path_tight_bounds`, and read its comment for why both exist.

### `path_tight_bounds`

```
pub fn path_tight_bounds(p: Path) -> Rect
```

The curve's actual extent: endpoints, plus each curve's turning points.

A cubic whose control points sit far outside its own curve has much looser control-point bounds
than real ones -- which is exactly the case the fixture checks, because if the two agree the
solver is not doing anything. Costs a quadratic solve per curve, so it is the one to call when
fitting ink to a page and not the one to call per clip test.

## Flattening

### `Polyline`

```
pub struct Polyline {
    points: [Point]
    closed: bool
}
```

A subpath, after flattening: a run of points, and whether the last joins back to the first.
`closed` is carried rather than inferred, because an open polyline and a closed one with the same
points are different strokes -- the closed one has a join where the open one has two caps.

### `flattened`

```
pub fn flattened(p: Path, tolerance: float) -> [Polyline]
```

`tolerance` is the greatest distance a chord may sit from the curve it replaces, in the path's own
units. A quarter of a pixel is what both backends ask for; the cost of halving it is roughly
another 40% of the segments, and the cost of doubling it is a visibly faceted large circle.
