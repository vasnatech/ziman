# `scene`

The display list: drawing operations as values, and a scene as a tree of them.

**This is where the library stops looking like Java 2D.** `Graphics2D` is a mutable cursor --
set a paint, set a transform, draw, repeat -- which is a poor fit for a language whose values are
immutable, which has no methods, and whose `Mutable*` types are thread-bound. A scene is instead
a value, and a backend is a function from it to bytes.

Three things that buys, none of which a stateful cursor can: the same scene renders to PDF *and*
to an image with no redrawing; a scene is inspectable data, so a test asserts on structure rather
than on pixels; and scenes compose as values. PDF's own `q`/`Q` nesting is a tree anyway, so a
tree is closer to the target than a cursor is. Skia's `SkPicture` is the same idea.

`use scene` -- or `use scene.Name` to bring one item in bare.

## Example

```
use geom
use paint
use scene
use shape

// A drawing as a value. A `Scene` is a size and a list of operations -- fill this path, stroke
// that one, place this group under that transform -- and it renders the same whether the backend
// is PDF or a raster image. Nothing is drawn while the scene is built; it is a description.
fn demo() -> Result<unit, string> {
    val square = shape.rect_path(geom.rect_of(10.0, 10.0, 80.0, 80.0))

    val b = shape.path_builder()
    shape.move_to(b, geom.point_of(20.0, 20.0))
    shape.line_to(b, geom.point_of(80.0, 80.0))
    val diagonal = shape.built(b, shape.Winding.NonZero)

    // Operations are values in a list, so a drawing can be assembled, passed around and inspected
    // before anything renders it.
    val ops = [
        scene.fill(square, paint.solid(paint.rgb(0.9, 0.9, 0.2))),
        scene.stroke(diagonal, paint.solid(paint.black()), paint.stroke_of(3.0)),
        // A group applies one transform to everything inside it, which is how a drawing is reused
        // at another size or place without rebuilding its paths.
        scene.group(geom.scaling(0.25, 0.25), [
            scene.fill(square, paint.solid(paint.rgb(0.2, 0.4, 0.8)))
        ])
    ]
    val s = scene.with_background(scene.with_ops(scene.scene_of(100.0, 100.0), ops), paint.solid(paint.white()))

    printf("{} {} {}\n", s.width, s.height, len(s.ops))
    print(Option.is_Some(s.background))

    // `op_count` walks into groups, so it counts the drawing's real work rather than the three
    // entries at the top -- which is what a backend budgets against.
    print(scene.op_count(s))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("scene example failed: {}\n", e)
    }
}
```

Output:

```
100.0 100.0 3
true
4
```

### `TextRun`

```
pub struct TextRun {
    text: string
    at: Point
    size: float
    font: int
    fill: Paint
    // Which bytes of `text` each run of glyphs came from. Empty means one glyph per codepoint, left
    // to right, which a backend can derive for itself -- and which stops being true the moment
    // shaping exists. Carried from the start because PDF's `/ToUnicode` needs it (phase 3) and
    // because adding it later would change a struct every backend reads. Cairo's
    // `cairo_show_text_glyphs` takes the same three-part input for the same reason.
    clusters: [Cluster]
}
```

Text is a first-class operation, **not lowered to paths**. A PDF backend emits it as text
(`BT`/`Tf`/`Tj`) so it stays selectable, searchable and copyable, and the file stays small; only
a raster backend, which has no font machinery of its own, turns it into outlines. Java 2D offers
`GlyphVector.getOutline()` and it is the right escape hatch for the second case and the wrong
default for the first.

`font` is an index into the scene's font table, for the same reason `Tiled.image` is an index:
a face is embedded once per document however many runs use it.

### `Cluster`

```
pub struct Cluster {
    bytes: int
    glyphs: int
}
```

`glyphs` consecutive glyphs came from `bytes` bytes of the source text.

### `Filled`

```
pub struct Filled {
    path: Path
    fill: Paint
}
```

### `Stroked`

```
pub struct Stroked {
    path: Path
    fill: Paint
    stroke: Stroke
}
```

### `Placed`

```
pub struct Placed {
    image: int
    into: Rect
}
```

An image from the scene's table, drawn into `into`. PDF draws images into the unit square and
places them by matrix, so a rect is the portable way to say where it goes.

### `Group`

```
pub struct Group {
    transform: Matrix
    clip: Option<Path>
    ops: [Op]
}
```

A transform and a clip applied to a list of operations -- exactly PDF's `q ... Q`. Transform and
clip live here rather than on individual operations because nesting is how both compose, and
because the common case (a table of rows at an offset) would otherwise repeat the matrix on every
cell.

### `Op`

```
pub enum Op {
    Fill(Filled)
    StrokePath(Stroked)
    Text(TextRun)
    DrawImage(Placed)
    Nested(Group)
}
```

### `fill`

```
pub fn fill(path: Path, using: Paint) -> Op
```

### `stroke`

```
pub fn stroke(path: Path, using: Paint, how: Stroke) -> Op
```

### `text`

```
pub fn text(run: TextRun) -> Op
```

### `text_of`

```
pub fn text_of(content: string, at: Point, size: float, font: int, using: Paint) -> Op
```

### `image`

```
pub fn image(handle: int, into: Rect) -> Op
```

### `group`

```
pub fn group(transform: Matrix, ops: [Op]) -> Op
```

### `clipped`

```
pub fn clipped(clip: Path, ops: [Op]) -> Op
```

### `group_of`

```
pub fn group_of(transform: Matrix, clip: Option<Path>, ops: [Op]) -> Op
```

### `Scene`

```
pub struct Scene {
    width: float
    height: float
    background: Option<Paint>
    ops: [Op]
}
```

A page, or an image to be. `width` and `height` are points for a document and pixels for a
raster target -- the model does not know which, and the backend decides what a unit means.

### `scene_of`

```
pub fn scene_of(width: float, height: float) -> Scene
```

### `with_ops`

```
pub fn with_ops(s: Scene, ops: [Op]) -> Scene
```

### `with_background`

```
pub fn with_background(s: Scene, p: Paint) -> Scene
```

### `op_count`

```
pub fn op_count(s: Scene) -> int
```

How many operations the scene holds, counting into groups. For a test asserting on structure, and
for a backend deciding whether a page is worth compressing.
