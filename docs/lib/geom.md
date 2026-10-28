# `geom`

Points, rectangles and affine transforms. The coordinate vocabulary everything else in
`lib/graphics` is written in. Pure value types: no rendering, no I/O, no state.

**Y grows downward**, as it does in Java 2D, Canvas and SVG, so a reader coming from any of those
is not surprised. PDF's own Y axis grows *upward*, and the PDF backend flips it exactly once in
its page transform -- see docs/graphics/README.md's invariant 4 for why that belongs there and
nowhere else.

`use geom` -- or `use geom.Name` to bring one item in bare.

## Example

```
use geom
use math
use math.pi

// Points, rectangles and the affine matrix every other graphics module is positioned by. Nothing
// here draws: these are the coordinates a drawing is expressed in, kept in one place so `shape`,
// `scene`, `pdf` and `raster` all mean the same thing by a point.
fn demo() -> Result<unit, string> {
    val r = geom.rect_of(10.0, 20.0, 100.0, 50.0)
    printf("{} {} {} {}\n", r.x, r.y, geom.right(r), geom.bottom(r))
    print(geom.rect_contains(r, geom.point_of(50.0, 40.0)))
    print(geom.rect_contains(r, geom.point_of(5.0, 40.0)))

    // A rectangle given by two corners does not care which corner came first.
    val between = geom.rect_between(geom.point_of(30.0, 40.0), geom.point_of(10.0, 20.0))
    printf("{} {} {} {}\n", between.x, between.y, between.width, between.height)

    // Transforms compose, and `matrix_concat` applies the inner one first -- the order that reads
    // as the sentence "scale it, then move it".
    val grow = geom.scaling(2.0, 2.0)
    val both = geom.matrix_concat(geom.translation(5.0, 5.0), grow)
    val p = geom.transform_point(both, geom.point_of(10.0, 10.0))
    printf("{} {}\n", p.x, p.y)

    // A matrix can be asked what it is, which is how a backend decides what it can do natively.
    print(geom.scale_x_of(grow))
    print(geom.determinant(grow))

    // Inversion is a Result: a matrix that collapses space has no inverse, and saying so beats
    // returning an identity nobody asked for.
    print(is_ok(geom.invert(grow)))
    print(is_ok(geom.invert(geom.scaling(0.0, 1.0))))

    // A quarter turn, rounded to the precision worth printing.
    val turned = geom.transform_point(geom.rotation(pi / 2.0), geom.point_of(1.0, 0.0))
    printf("{} {}\n", math.round(turned.x), math.round(turned.y))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("geom example failed: {}\n", e)
    }
}
```

Output:

```
10.0 20.0 110.0 70.0
true
false
10.0 20.0 20.0 20.0
25.0 25.0
2.0
4.0
true
false
0 1
```

### `Point`

```
pub struct Point {
    x: float
    y: float
}
```

### `point_of`

```
pub fn point_of(x: float, y: float) -> Point
```

### `Rect`

```
pub struct Rect {
    x: float
    y: float
    width: float
    height: float
}
```

A rectangle by its origin and size. Width or height may be negative, and `normalized` is what
fixes that -- rather than refusing it, because a rect computed by subtracting two corners is a
natural thing to write and flipping it is the caller's business, not an error.

### `rect_of`

```
pub fn rect_of(x: float, y: float, width: float, height: float) -> Rect
```

### `rect_between`

```
pub fn rect_between(a: Point, b: Point) -> Rect
```

The rect through two opposite corners, in either order.

### `normalized`

```
pub fn normalized(r: Rect) -> Rect
```

The same rectangle with a non-negative width and height.

### `right`

```
pub fn right(r: Rect) -> float
```

### `bottom`

```
pub fn bottom(r: Rect) -> float
```

### `is_empty`

```
pub fn is_empty(r: Rect) -> bool
```

### `rect_contains`

```
pub fn rect_contains(r: Rect, p: Point) -> bool
```

### `rect_union`

```
pub fn rect_union(a: Rect, b: Rect) -> Rect
```

The smallest rect containing both. An empty rect contributes nothing, so a union over a list can
start from `empty_rect` without the first iteration being a special case.

### `empty_rect`

```
pub fn empty_rect() -> Rect
```

### `Matrix`

```
pub struct Matrix {
    a: float
    b: float
    c: float
    d: float
    e: float
    f: float
}
```

An affine transform. Six numbers, laid out so that


```
x' = a*x + c*y + e
y' = b*x + d*y + f
```

**The field order is PDF's `a b c d e f cm` order, deliberately.** The PDF backend writes these
six out in this order with no reshuffling, which removes the one bug this code is most prone to:
transposing `b` and `c`. Nothing else about the model knows that PDF exists.

### `identity`

```
pub fn identity() -> Matrix
```

A function, not a `pub val`: a qualified global val is not reachable as `geom.identity`, and a
caller who wrote `use geom` should not need a second import for it.

### `translation`

```
pub fn translation(dx: float, dy: float) -> Matrix
```

### `scaling`

```
pub fn scaling(sx: float, sy: float) -> Matrix
```

### `rotation`

```
pub fn rotation(radians: float) -> Matrix
```

Rotation by `radians`. With Y growing downward this turns clockwise on screen, which is what the
same call does in Canvas and Java 2D -- the handedness follows from the axis, not from a choice
made here.

### `shearing`

```
pub fn shearing(sx: float, sy: float) -> Matrix
```

### `matrix_concat`

```
pub fn matrix_concat(outer: Matrix, inner: Matrix) -> Matrix
```

`inner` first, then `outer`: `transform_point(matrix_concat(o, i), p)` is
`transform_point(o, transform_point(i, p))`.

Named `matrix_concat` rather than `concat` because `concat` is a prelude name over strings and
arrays: a module function of that name would shadow it inside this file, and would read as the
string one at every call site here.

### `transform_point`

```
pub fn transform_point(m: Matrix, p: Point) -> Point
```

### `transform_rect`

```
pub fn transform_rect(m: Matrix, r: Rect) -> Rect
```

The transformed rect's **bounding box**, not a rect -- a rotated rectangle is not a rectangle, so
all four corners are transformed and enclosed. Callers that wanted the parallelogram want a path
(`shape.rect_path` then `shape.transform_path`).

### `determinant`

```
pub fn determinant(m: Matrix) -> float
```

### `invert`

```
pub fn invert(m: Matrix) -> Result<Matrix, string>
```

The inverse, or an `Err` naming the problem.

A `Result` rather than a trap because a singular matrix is something a caller can build by
accident -- `scaling(0.0, 1.0)` is one -- and both backends need to detect it rather than divide
by zero and emit a page of NaNs.

### `rotation_of`

```
pub fn rotation_of(m: Matrix) -> float
```

What a transform does, taken apart. For a backend asking "does this rotate text" or "how thick is
a hairline after this", which is otherwise six numbers and some trigonometry at the call site.

### `scale_x_of`

```
pub fn scale_x_of(m: Matrix) -> float
```

### `scale_y_of`

```
pub fn scale_y_of(m: Matrix) -> float
```

### `shear_of`

```
pub fn shear_of(m: Matrix) -> float
```

The angle between the transformed axes, in radians: `pi/2` for any transform that does not shear.
