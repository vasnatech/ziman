# `paint`

What fills a region, and how a line is drawn.

**The paint set is closed, and that is the central decision of this library.** Java 2D's `Paint`
is an interface whose implementations produce pixels (`createContext` hands back a `Raster`), so
any paint a backend does not recognise forces rasterisation -- which is why PDF output from Java
2D quietly degrades to embedded bitmaps. Cairo makes the same set closed (six cases) and then
reopens it with one callback variant, and its PDF backend rasterises "fallback regions" for
exactly that reason.

These four cases are what PDF has native shading dictionaries for, so every paint here survives
as vectors in both backends. Adding a fifth means adding it to both, on purpose.

`use paint` -- or `use paint.Name` to bring one item in bare.

### `Color`

```
pub struct Color {
    red: float
    green: float
    blue: float
    alpha: float
}
```

Components are 0.0 to 1.0, not bytes, because PDF writes colour as decimals (`rg`, `RG`) and a
raster backend converts to bytes once at the end. Bytes in the middle would lose precision in a
gradient for nothing.

### `rgb`

```
pub fn rgb(r: float, g: float, b: float) -> Color
```

### `rgba`

```
pub fn rgba(r: float, g: float, b: float, a: float) -> Color
```

### `gray`

```
pub fn gray(level: float) -> Color
```

### `black`

```
pub fn black() -> Color
```

Functions rather than `pub val`s: a module-qualified global val is not reachable as
`paint.black` -- Ziman asks for `use paint.black` and a bare name -- and a caller who has written
`use paint` should not then need a second import per colour. The same reason `identity()` and
`empty_rect()` in `geom` are functions.

### `white`

```
pub fn white() -> Color
```

### `transparent`

```
pub fn transparent() -> Color
```

### `hex`

```
pub fn hex(text: string) -> Result<Color, string>
```

`#rgb`, `#rrggbb` or `#rrggbbaa`, with or without the leading `#`. An Err names what it could not
read, since a colour typo is otherwise a silent black.

### `Stop`

```
pub struct Stop {
    offset: float
    color: Color
}
```

One stop of a gradient: `offset` runs 0.0 to 1.0 along the gradient's own axis.

### `stop_of`

```
pub fn stop_of(offset: float, color: Color) -> Stop
```

### `Linear`

```
pub struct Linear {
    from: Point
    to: Point
    stops: [Stop]
}
```

### `Radial`

```
pub struct Radial {
    centre: Point
    radius: float
    stops: [Stop]
}
```

### `Tiled`

```
pub struct Tiled {
    image: int
    transform: Matrix
}
```

An image, placed by a matrix. The handle is an index into the scene's image table (phase 4);
paints do not own images, so the same image can back several paints and be written once.

### `Paint`

```
pub enum Paint {
    Solid(Color)
    LinearGradient(Linear)
    RadialGradient(Radial)
    Pattern(Tiled)
}
```

### `solid`

```
pub fn solid(c: Color) -> Paint
```

### `linear`

```
pub fn linear(from: Point, to: Point, stops: [Stop]) -> Paint
```

### `radial`

```
pub fn radial(centre: Point, radius: float, stops: [Stop]) -> Paint
```

### `pattern`

```
pub fn pattern(image: int, transform: Matrix) -> Paint
```

The constructor exists so the variant is reachable the same way the others are; **no backend
draws it yet**, and both say so rather than painting nothing (lib/graphics/pdf.zm).

### `Cap`

```
pub enum Cap {
    Butt
    Round
    Square
}
```

### `Join`

```
pub enum Join {
    Miter
    Round
    Bevel
}
```

### `Stroke`

```
pub struct Stroke {
    width: float
    cap: Cap
    join: Join
    // Past this ratio a miter join becomes a bevel. Without the fallback a near-180 degree join
    // produces a spike thousands of units long, which is the classic stroking bug.
    miter_limit: float
    // Empty for a solid line. Otherwise alternating on and off lengths.
    dash: [float]
    dash_phase: float
}
```

How a line is drawn. **Parameters, not an outline**: PDF has native `w`, `J`, `j`, `M` and `d`
operators, so the PDF backend passes these straight through and a stroked hairline stays a
hairline at any zoom. Only the raster backend turns a stroke into a fillable outline (phase 5).
Java 2D's `Stroke.createStrokedShape` is the right fallback and the wrong only path.

### `stroke_of`

```
pub fn stroke_of(width: float) -> Stroke
```

The common case: butt caps, miter joins, PDF's and PostScript's default limit of 10, no dash.

### `dashed`

```
pub fn dashed(width: float, dash: [float]) -> Stroke
```
