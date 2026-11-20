# `chart`

A line or a bar chart, drawn into a scene.

**A drawing, not a widget.** No window, no pointer, no state: this turns numbers and a box into
the ops that draw them, which is what lets a chart go into a PDF or a PNG as readily as into a
window. A chart that existed only as a control could not be exported, and an exported report is
where a chart is most often actually read.

**Two marks and not a grammar of graphics**, which is a measurement rather than a preference:
see `docs/design/charts.md`.

`use chart` -- or `use chart.Name` to bring one item in bare.

### `Mark`

```
pub enum Mark {
    Line
    Bars
}
```

How a series is drawn.

### `Series`

```
pub struct Series {
    name: string
    mark: Mark
    points: [float]
    ink: Color
}
```

One run of numbers, drawn one way, in one colour.

**Numbers against labels, not pairs.** The axis is categorical -- a point's x is its index --
so a pair would carry an x that is always its own position, and a caller could write two series
whose points disagreed about where the third label is.

### `Band`

```
pub struct Band {
    from: float
    to: float
    ink: Color
}
```

A shaded range across the whole width, behind everything.

**This is the feature that is not decoration.** A reading is meaningful against a range, and a
chart that drew the line and left the range out has shown the number and hidden the meaning.

### `Chart`

```
pub struct Chart {
    // One per position on the x axis. A series longer or shorter than this is drawn only as far as
    // the two agree -- see `points_shown`.
    labels: [string]
    series: [Series]
    bands: [Band]
}
```

### `ChartStyle`

```
pub struct ChartStyle {
    ink: Color
    grid: Color
    // The axis labels.
    text_size: float
    font: int
    line_width: float
    // How much of each slot a bar fills, 0 to 1.
    bar_fill: float
    // Room for the y labels on the left and the x labels below.
    left_gutter: float
    bottom_gutter: float
}
```

The colours and sizes a chart draws with, which belong to the caller's look rather than here.

### `chart_of`

```
pub fn chart_of(labels: [string], series: [Series]) -> Chart
```

### `line_series`

```
pub fn line_series(name: string, points: [float], ink: Color) -> Series
```

### `bar_series`

```
pub fn bar_series(name: string, points: [float], ink: Color) -> Series
```

### `points_shown`

```
pub fn points_shown(c: Chart, s: Series) -> int
```

How many points of a series are actually drawn.

**The shorter of the two, rather than an error or a guess.** A series with more points than
there are labels has points with nowhere to stand; one with fewer simply stops. Both happen
while data is still arriving, and neither is worth refusing to draw the rest of the chart over.

### `y_range`

```
pub fn y_range(c: Chart) -> geom.Point
```

The range the y axis covers.

**The bands are part of the range, not drawn on top of it.** A band outside the data's own range
would otherwise be clipped away, which redraws the chart's meaning in silence: a reading sitting
inside a normal range, drawn as though the range were somewhere else entirely.

A flat series -- every point the same -- would be a range of zero height and a division by it,
so it is widened to sit in the middle.

### `plot_box`

```
pub fn plot_box(c: Chart, box: Rect, style: ChartStyle) -> Rect
```

The box the data is drawn in: the chart's box less the room the labels need.

### `y_at`

```
pub fn y_at(c: Chart, value: float, plot: Rect, style: ChartStyle) -> float
```

Where a value sits, vertically. **Shared by the drawing and by anything asking where a point
is**, so the two cannot disagree -- which is the defect the calendar cost twenty-two points to.

### `x_at`

```
pub fn x_at(c: Chart, index: int, plot: Rect, style: ChartStyle) -> float
```

Where a slot's centre sits, horizontally.

### `chart_scene`

```
pub fn chart_scene(c: Chart, box: Rect, style: ChartStyle) -> scene.Scene
```

The whole chart, as ops.
