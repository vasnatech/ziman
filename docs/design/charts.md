# Charts

Status: **built, 2026-11-20.** §4's shape is what shipped, unchanged: `lib/graphics/chart.zm`
(F332, the drawing) and `controls.ChartView` (F333, the widget, its keyboard and what §3 says a
reader is told), each with a gallery demo. This is how **P1** of
[the health-record port](health-record-port.md) §4.2 was answered -- build the layer, rather than
port the twelve views without it.

## 1. What is actually needed, measured

Eleven files, one library (`recharts`). Counted by component across all of them:

| | |
|---|---|
| Line charts | `LineChart` 35, `Line` 32 |
| Bar charts | `BarChart` 31, `Bar` 30 |
| Both on one | `ComposedChart` 6 |
| Axes / grid | `XAxis` 43, `YAxis` 39, `CartesianGrid` 39 |
| Hover readout | `Tooltip` 40 |
| **Shaded bands** | `ReferenceArea` 11 |
| Per-point colour | `Cell` 7 |
| Legend | 9 |
| Pie, area, scatter, radar, brush | **none** |

**Two marks, not a grammar of graphics.** Line and bar, and a chart that has both. Nothing in the
target needs a pie, an area, a scatter or a radar, so building a general plotting grammar would be
building the hardest thing for nobody -- the same conclusion the catalogue measurement reached
about plural rules.

Two details that are easy to get wrong by assuming:

- **The x axis is categorical, not a time axis.** Every one is `dataKey="date"`: evenly spaced
  points carrying date labels. A continuous time axis -- irregular spacing, tick selection, zoom --
  is not needed and is most of the work in a charting library.
- **The bands are horizontal.** `<ReferenceArea y1={low} y2={high} />`: a shaded range drawn behind
  the data. This is the one feature that is not decoration -- a reading is meaningful against a
  range, and a chart that drew the line and left the range out has shown the number and hidden the
  meaning.

## 2. Two layers, because a chart has to leave the screen

**`lib/graphics/chart.zm` turns data and a box into a scene.** No window, no pointer, no state: a
chart is a drawing, and this layer draws it.

**`lib/ui/` wraps that in a widget**, which adds the hover readout and the accessibility.

The split is not tidiness. `lib/graphics` already has a PDF backend and a raster backend, and this
library exports thirteen formats; **a chart that existed only as a widget could not go in an
exported report**, and the export is where a chart is most often actually read. Drawing to a scene
gets PDF and PNG for nothing.

## 3. What a reader is told, which is the part nobody builds

A chart is the one thing in a window that a screen reader cannot read at all. The usual answers are
a text summary (which is a guess at what matters) or nothing (which is most of the web).

**The chart's accessible form is its table.** `controls.Table` already holds `Value`, already
spells a number or a date in a locale, already sorts, and a reader already knows how to walk one.
A chart and the table of the same data are the same `[[Value]]`, so the chart publishes rows rather
than inventing a vocabulary -- and the data a reader hears cannot drift from the data drawn,
because there is one of it.

This also answers "is a chart focusable": **yes**, unlike a card or a screen. It carries
information that is reachable no other way, so it is a tab stop and its rows are what it announces.

## 4. The shape

```ziman
// lib/graphics/chart.zm
pub enum Mark { Line  Bars }
pub struct Series { name: string  mark: Mark  points: [float]  ink: Color }
pub struct Band   { from: float  to: float  ink: Color }   // horizontal, behind everything
pub struct Chart  { labels: [string]  series: [Series]  bands: [Band] }

pub fn chart_scene(c: Chart, box: Rect, style: ChartStyle) -> scene.Scene
```

- **Points are `[float]` against `labels`, not pairs.** The axis is categorical, so a point's x is
  its index; a series with a different length than the labels is a caller error worth catching
  rather than a case worth supporting.
- **A band is behind everything**, which is what makes it a band and not a series.
- **The y range is computed from the data and the bands together.** A band outside the data's range
  that got clipped would silently redraw the chart's meaning -- a reading inside a normal range,
  drawn as though the range were not there.

## 5. Not in scope

- **Pie, area, scatter, radar.** §1: nothing uses them.
- **A continuous or time axis.** §1: every axis in the target is categorical.
- **Zoom, brush, pan, animation.** Nothing uses them, and animation wants a clock the library does
  not have.
- **Stacking.** No `stackId` appears in the target.
- **Anything about any particular application.** A series has a name, a mark and numbers. What
  those numbers are is the caller's business.
