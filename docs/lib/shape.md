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
A variant holds zero or one payload (docs/syntax/), so the two curve segments carry a named
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
    // The control-point hull, **computed once when the path is made** rather than on every
    // question about it.
    //
    // **Profiled 2026-11-14, and it was the largest single cost in a regional repaint**: a
    // one-pixel damage region on a 901-op scene took 10.8 ms, of which 37% was recomputing these
    // boxes -- `path_bounds` -> `bounds_builder` -> `include` -> `rect_union` -- for ops that
    // then failed the window test and drew nothing. The scene is rebuilt every frame, so the
    // walk happened every frame, per op, to answer a question each path could have answered at
    // birth.
    //
    // **Free for the shapes a user interface is made of.** A rectangle's hull is the rectangle; a
    // round rect's, an ellipse's and a circle's are too, because their control points all lie
    // inside the box and the curves touch all four edges. Those constructors set this from their
    // own argument and walk nothing. The general ones (`built`, `path_of`, `polygon_path`) walk
    // their segments once, which is what every draw used to do.
    //
    // It is not an optimisation to be undone later: `path_bounds` is the same function it was,
    // and `tests/lang/graphics_shape.zm` checks each constructor's stored box against a walk of
    // its own segments, so the analytic shortcuts cannot drift from the definition.
    bounds: Rect
}
```

### `PathBuilder`

```
pub struct PathBuilder {
    segments: MutableArray<Segment>
}
```

Building one. Thread-bound by holding a `MutableArray`, which is what makes a builder a builder
here -- see the mutability discussion in docs/design/journal/.

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

```
pub fn round_rect_path(r: Rect, tl: float, tr: float, br: float, bl: float) -> Path
```

**Corners that are elliptical rather than circular**, which is the general form and the one a
file on disk writes: SVG's `<rect>` has an `rx` and an `ry`, and a drawing tool that scaled a
rounded rectangle non-uniformly writes two radii that differ -- sometimes by 30%, often by a
rounding error (`rx="3.9631047" ry="3.9649484"` is one from an icon theme). A reader with only
the circular form has to refuse those or quietly pick one.

Each radius is clamped to half of **its own** axis, as the specification says: `rx` to half the
width and `ry` to half the height. A zero in either axis means square corners, because a corner
with no height is not a corner.
A rounded rectangle whose **four corners are given one at a time**, clockwise from the top
left, each circular.

**Written for a tab, which is the shape one radius cannot make.** An index card is rounded along
its top and square along the bottom, where it meets the page; drawn with one radius it is a
button with a gap under it, which is exactly what `lib/ui`'s tab strip looked like. Every other
asymmetric box has the same shape -- the first and last segment of a segmented control, a pane
that rounds only away from the edge it is docked to.

**Four floats rather than a `Corners` kind**, because this layer has no opinions about meaning:
`widget.Corners` says *which corners a look rounds* and resolves to these numbers, the same way
`widget.Radius` resolves to the single radius above. Geometry takes numbers.

Each radius is clamped to half the box on both axes independently, so a corner can never eat
more than its own half and two adjacent ones cannot overlap.

```
pub fn round_rect_path(r: Rect, rx: float, ry: float) -> Path
```

### `ring_path`

```
pub fn ring_path(outer: Path, inner: Path) -> Path
```

**The area between two closed contours** -- `outer` with `inner` punched out of it, as one path
that any fill can draw. A ring, an annulus, a frame, a washer; the border of a box is the case
this was written for.

**`EvenOdd` rather than a reversed inner contour**, which is what makes this a two-line function
instead of a path-reversal one. Even-odd asks how many contours enclose a point and fills the odd
answers, so a nested `inner` leaves a hole whichever direction either contour was built in --
where `NonZero` would need `inner` wound against `outer` and would quietly fill solid if a
constructor ever changed direction. Neither `round_rect_path` nor `ellipse_path` promises a
direction, so depending on one would be depending on an accident.

**An empty `inner` is no hole**, which is the degenerate case a caller actually hits: a border
half the thickness of its box leaves no inside, and a ring with nothing taken out of it is the
outer contour. Answered here rather than guarded at each call.

The bounds are the union and not `outer`'s. Even-odd of two contours that do *not* nest covers
the symmetric difference, which can reach outside `outer` -- so a caller who passes two
overlapping circles gets a correct box rather than a clipped one. For the nested case the union
*is* `outer`'s box, so the general answer costs the common one nothing.

**Why a caller wants this rather than a stroke.** A stroked outline is the general offsetting
problem -- flatten every curve, offset the polyline, join every corner, mind the miter limit --
and `raster.draw_ops`'s own comment calls `stroker.outline` the most expensive single thing in
that module. Measured on one 60x24 face: **649 us to widen a rounded border against 123 us for a
square one**, which was the whole cost of a second look and feel through a damage region (F286).
A ring says the same ink analytically, because the offset of a rounded rectangle is another
rounded rectangle and nothing has to be discovered.

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
