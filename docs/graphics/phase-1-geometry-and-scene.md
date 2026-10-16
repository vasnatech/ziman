# Phase 1 — geometry and the display list

No rendering at all. This phase is the vocabulary every later one speaks, and it is entirely pure
value types, so it is testable without a backend, an image or a font.

Read `README.md` first for the architecture and the invariants.

## Scope

`geom`, `shape`, `paint`, `scene`. Four modules, no I/O, no native code.

## `geom`

```ziman
pub struct Point { x: float  y: float }
pub struct Rect  { x: float  y: float  width: float  height: float }

// An affine transform, row-major: [a c e / b d f / 0 0 1], the same six numbers
// PDF's `cm` operator takes and in the same order, which is not a coincidence.
pub struct Matrix { a: float  b: float  c: float  d: float  e: float  f: float }

pub fn identity() -> Matrix
pub fn translation(dx: float, dy: float) -> Matrix
pub fn scaling(sx: float, sy: float) -> Matrix
pub fn rotation(radians: float) -> Matrix
pub fn matrix_concat(outer: Matrix, inner: Matrix) -> Matrix
pub fn transform_point(m: Matrix, p: Point) -> Point
pub fn transform_rect(m: Matrix, r: Rect) -> Rect    // the bounding box; a rotated rect is not one
pub fn invert(m: Matrix) -> Result<Matrix, string>   // Err on a singular matrix
pub fn rotation_of(m: Matrix) -> float               // the decomposition; needs atan2
pub fn scale_x_of(m: Matrix) -> float
pub fn scale_y_of(m: Matrix) -> float
pub fn rect_union(a: Rect, b: Rect) -> Rect
pub fn rect_contains(r: Rect, p: Point) -> bool
```

**Three names differ from the first draft of this document, each for a reason the language gave.**
`matrix_concat` rather than `concat`, because `concat` is a builtin and a module function of that
name reads as the string one at every call site inside the file. `transform_point` rather than
`apply`, which says what it does. And the constants -- `identity()`, `empty_rect()`, `paint.black()`
-- are **functions rather than `pub val`s**, because a qualified global val is not reachable:
`geom.identity` is a type error asking for `use geom.identity` and a bare name, so a caller who
wrote `use geom` would need a second import per constant. `math.pi` has the same wrinkle and is
where it was found.

**The six numbers are in PDF's order on purpose.** PDF's `a b c d e f cm` is the same affine
matrix; keeping the field order identical means the backend writes them out with no reshuffling and
no chance of transposing two of them, which is the classic bug in this code.

`invert` returns a `Result` rather than trapping: a zero-scale matrix is a thing a caller can
construct by accident (`scaling(0.0, 1.0)`), and the backends need to detect it rather than divide.

## `shape`

The five segments, and nothing else:

A variant holds zero or one payload (docs/SYNTAX.md: "never a tuple or inline struct-shaped one"),
so the two curve cases carry a named struct -- which reads better at the use sites anyway:

```ziman
pub struct Quad  { control: Point  end: Point }
pub struct Cubic { control1: Point  control2: Point  end: Point }

pub enum Segment {
    MoveTo(Point)
    LineTo(Point)
    QuadTo(Quad)
    CubicTo(Cubic)
    Close
}

pub enum Winding { NonZero  EvenOdd }

pub struct Path { segments: [Segment]  winding: Winding }
```

Builders, because writing a segment array by hand is miserable:

```ziman
pub fn path() -> PathBuilder                      // a #[thread_bound] builder, like MutableArray
pub fn move_to(b: PathBuilder, p: Point)
pub fn line_to(b: PathBuilder, p: Point)
pub fn quad_to(b: PathBuilder, c: Point, p: Point)
pub fn cubic_to(b: PathBuilder, c1: Point, c2: Point, p: Point)
pub fn close(b: PathBuilder)
pub fn built(b: PathBuilder, winding: Winding) -> Path

// The shapes everyone actually draws, as paths
pub fn rect_path(r: Rect) -> Path
pub fn round_rect_path(r: Rect, radius: float) -> Path
pub fn ellipse_path(r: Rect) -> Path
pub fn line_path(from: Point, to: Point) -> Path
pub fn polygon_path(points: [Point], closed: bool) -> Path

pub fn transform_path(p: Path, m: Matrix) -> Path
pub fn path_bounds(p: Path) -> Rect        // control-point bounds, see below
pub fn path_tight_bounds(p: Path) -> Rect  // the real curve extrema
```

**Two bounds functions, deliberately.** The control-point hull is cheap and always encloses the
curve, which is all a clip test or a page-fit check needs. The tight bounds require solving the
derivative of each cubic, which matters for "shrink the page to the ink" and for nothing else.
Offering only the tight one would make every cheap caller pay; offering only the loose one would
make the one caller that needs precision write it themselves.

**Why `QuadTo` exists at all, since a quadratic *is* a cubic.** Cairo's path vocabulary has only
four elements -- move, line, cubic, close -- and elevates quadratics on the way in; the elevation is
exact (control points at `start + 2/3*(c - start)` and `end + 2/3*(c - end)`), so nothing is lost
and the model is one case smaller. It is kept here for one reason: **TrueType outlines are
quadratic**, and phase 5 rasterises glyph outlines at every size a document uses. Elevating each
glyph on every use, rather than storing what the font already said, is work for nothing. The PDF
backend elevates once at serialisation (phase 2), where it costs one pass over a path that is about
to be written out anyway. If phase 5 never happens, cairo's four-element path is the better model
and this is the decision to revisit.

**Open question: conics, for arcs that are exact rather than close.** Skia's `SkPath` has a verb
neither Java 2D nor cairo does -- a *conic*, a rational quadratic with a weight, which represents a
circular arc **exactly**. The four-cubic ellipse below is within about 0.02% of the radius, which is
invisible in a document and not invisible in a large-radius arc on a chart. Adding `ConicTo(Point,
Point, float)` later is a breaking change to a `switch`ed enum in every backend, which argues for
deciding now; against it, both backends would have to lower conics anyway (PDF has no conic
operator either), so the exactness survives only as far as the first serialisation. Recorded rather
than decided: it is cheap while there are no backends and expensive once there are two.

**Ellipses are cubics, not a primitive.** Four cubic arcs with the magic constant
`0.5523` (`4/3 * (sqrt(2) - 1)`) approximate a quarter circle to within about 0.02% of the radius.
PDF has no ellipse operator either, so a primitive would have to be lowered anyway; lowering it
once here means both backends get it right.

## `paint`

Closed, for the reason the architecture note gives:

```ziman
pub struct Color { red: float  green: float  blue: float  alpha: float }   // 0.0 - 1.0

pub struct Stop { offset: float  color: Color }                           // offset 0.0 - 1.0

pub enum Paint {
    Solid(Color)
    LinearGradient(Point, Point, [Stop])
    RadialGradient(Point, float, [Stop])   // centre, radius
    Pattern(int, Matrix)                   // an image handle, phase 4; placed by the matrix
}

pub enum Cap  { Butt  Round  Square }
pub enum Join { Miter  Round  Bevel }

pub struct Stroke {
    width: float
    cap: Cap
    join: Join
    miter_limit: float
    dash: [float]       // empty for solid
    dash_phase: float
}

pub fn stroke_of(width: float) -> Stroke     // Butt, Miter, limit 10.0, no dash: the common case
pub fn rgb(r: float, g: float, b: float) -> Color
pub fn rgba(r: float, g: float, b: float, a: float) -> Color
pub fn gray(level: float) -> Color
pub fn hex(text: string) -> Result<Color, string>    // "#336699", "#336699ff"
```

**Colour is float RGBA, not packed bytes.** PDF writes colour as decimals in `0..1` (`rg`, `RG`), so
bytes would be converted on the way out; a raster backend converts to bytes once at the end
regardless. Keeping the model in floats means no precision is lost in the middle and no backend has
to guess a colour space.

**`Stroke` carries parameters, it does not produce an outline.** PDF has native `w`, `J`, `j`, `M`
and `d` operators, so the PDF backend passes these straight through -- a stroked hairline stays a
hairline at any zoom. Only the raster backend turns a stroke into an outline (phase 5). Java 2D's
`Stroke.createStrokedShape` is the right *fallback* and the wrong *only* path.

## `scene`

```ziman
pub enum Op {
    Fill(Path, Paint)
    StrokePath(Path, Paint, Stroke)
    Text(TextRun)                      // phase 3 fills this in; phase 1 declares it
    DrawImage(int, Rect)               // phase 4; an image handle and where it goes
    Group(Group)
}

pub struct Group {
    transform: Matrix
    clip: Option<Path>
    ops: [Op]
}

pub struct Scene {
    width: float          // in points for a document, pixels for an image
    height: float
    background: Option<Paint>
    ops: [Op]
}

pub fn scene_of(width: float, height: float) -> Scene
pub fn with_ops(s: Scene, ops: [Op]) -> Scene
pub fn group(transform: Matrix, clip: Option<Path>, ops: [Op]) -> Op
```

**`Group` is where transform and clip live, not `Scene`.** A nested group is exactly PDF's
`q ... Q` pair, and nesting is how both clipping and coordinate systems compose. Putting a transform
on individual operations instead would make the common case (a table of rows at an offset) repeat
the matrix on every cell.

**`Text` and `DrawImage` are declared now and implemented later** so that the enum does not change
shape in phase 3 or 4 -- adding a variant to a `switch`ed enum breaks every exhaustive match in
every backend. Declaring them up front costs a stub arm per backend and buys a stable type.

## What phase 1 does *not* do

No rendering, no fonts, no images, no text measurement (which needs font metrics, phase 3), no
boolean path operations (union/intersect of paths -- Java 2D's `Area`; genuinely wanted eventually,
and a whole project of its own).

## Verification

Entirely in-process, no golden files:

- Matrix algebra against known results: `concat(translation, scaling)` applied to a point,
  `invert` round-tripping a point, `invert` of a singular matrix answering `Err`.
- `rotation(PI/2)` mapping `(1,0)` to `(0,1)` within a tolerance -- which is also the first use of
  the `atan2`-adjacent trigonometry and will catch a degrees/radians mix-up immediately.
- Path bounds: a cubic whose control points lie far outside its curve, where the loose and tight
  bounds must *differ*. If they agree, the tight implementation is not doing anything.
- `ellipse_path` bounds matching the input rect to within the 0.02% the cubic approximation allows.
- `hex` round-tripping, and rejecting `"#xyz"` with a message naming the input.

## Build changes

`vpath` gains `lib/graphics`; four modules registered in `CORELIB_MODULES` and `src/corelib.c`; the
"Graphics" doc group added to `tools/gen_lib_manual.py`; **`math` gains `atan2`, `asin`, `acos` and
`atan`** as natives in `src/native_math.c`.

*Built 2026-10-16.* Phase 1's own API needs only `atan2`, for `rotation_of`; `asin` is used by
`shear_of` and `acos`/`atan` are there because the native boundary was open and phase 5 needs them
for round joins. All four follow this module's existing convention of answering NaN on a domain
error rather than trapping, and `atan2` takes `y` before `x` as every other standard library does.
