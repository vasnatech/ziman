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

### `outline`

```
pub fn outline(p: Path, how: Stroke) -> Result<Path, string>
```

### `outline_with`

```
pub fn outline_with(p: Path, how: Stroke, tolerance: float) -> Result<Path, string>
```
