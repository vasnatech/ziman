# `layout`

Flows, wrapping, tables and pagination: the layer that turns headings, paragraphs
and tables into coordinates.

Everything else in `lib/graphics` is a drawing API that puts a path or a glyph at a point. This
one is why that is not enough. The design is `docs/graphics/phase-6-layout.md`, and it was
written after a hand-made export page **silently printed 17pt past its own bottom margin** on
three of five pages: the break condition could only guess how tall the next row would be, because
a caller doing its own layout has no way to ask.

So the whole module is three functions over one closed enum -- `measure`, `split` and `draw` --
and that separation is the fix for that bug, not a matter of taste. A break is decided by asking
a block its height, which it answers by doing the same wrapping it would have drawn.

`use layout` -- or `use layout.Name` to bring one item in bare.

## The model

### `Align`

```
pub enum Align {
    Left
    Right
    Centre
    Justify
}
```

### `Span`

```
pub struct Span {
    text: string
    font: int
    size: float
    fill: Paint
}
```

A run of text in one face at one size. A paragraph is a list of these, because **bold inside a
sentence** is the first thing a real document needs and retrofitting it would change every
signature that touches a paragraph.

### `span_of`

```
pub fn span_of(text: string, face: int, size: float, fill: Paint) -> Span
```

### `ParaStyle`

```
pub struct ParaStyle {
    align: Align
    // Baseline to baseline. Zero means "from the fonts on the line", which is the usual case.
    leading: float
    space_before: float
    space_after: float
    indent_first: float
    indent_left: float
    indent_right: float
    // Lines that may not be stranded at the bottom or the top of a page. 2 and 2 is the
    // typographic convention; 0 turns the rule off.
    orphans: int
    widows: int
}
```

### `para_style`

```
pub fn para_style() -> ParaStyle
```

### `Para`

```
pub struct Para {
    spans: [Span]
    style: ParaStyle
}
```

### `RuleLine`

```
pub struct RuleLine {
    thickness: float
    fill: Paint
    space_before: float
    space_after: float
}
```

### `ColumnWidth`

```
pub enum ColumnWidth {
    Fixed(float)
    Proportional(float)
    Auto
}
```

How wide a column is. `Auto` measures the widest thing in it, which is what stops a long drug
name silently overlapping its neighbour -- the failure the hand-written export had.

### `Column`

```
pub struct Column {
    width: ColumnWidth
    align: Align
}
```

### `column`

```
pub fn column(width: ColumnWidth, align: Align) -> Column
```

### `Cell`

```
pub struct Cell {
    blocks: [Block]
}
```

A cell holds blocks, not a string, which is what lets it contain a wrapped paragraph or another
table. It costs nothing: `measure`, `split_block` and `draw` already recurse.

### `cell`

```
pub fn cell(blocks: [Block]) -> Cell
```

### `Row`

```
pub struct Row {
    cells: [Cell]
}
```

### `row`

```
pub fn row(cells: [Cell]) -> Row
```

### `TableStyle`

```
pub struct TableStyle {
    // A rule under the header. Zero for none.
    header_rule: float
    rule_fill: Paint
    cell_padding: float
    row_gap: float
}
```

### `Grid`

```
pub struct Grid {
    columns: [Column]
    // Repeated at the top of every page the table continues onto. Separate from `rows` and not a
    // flag on the first one, because repeating it is the single thing that makes a long table
    // readable -- and because a hand-written export has to copy the header-drawing code into its
    // page-break branch to get it.
    header: [Row]
    rows: [Row]
    style: TableStyle
}
```

### `Block`

```
pub enum Block {
    Paragraph(Para)
    Table(Grid)
    Spacer(float)
    Rule(RuleLine)
    PageBreak
}
```

### `table_style`

```
pub fn table_style(padding: float, gap: float, rule: float, rule_fill: Paint) -> TableStyle
```

### `table`

```
pub fn table(columns: [Column], header: [Row], rows: [Row], style: TableStyle) -> Block
```

### `paragraph`

```
pub fn paragraph(spans: [Span], style: ParaStyle) -> Block
```

### `text_block`

```
pub fn text_block(body: string, face: int, size: float, fill: Paint, style: ParaStyle) -> Block
```

The common case: one run of text in one face.

### `spacer`

```
pub fn spacer(height: float) -> Block
```

### `rule`

```
pub fn rule(thickness: float, fill: Paint, before: float, after: float) -> Block
```

### `Insets`

```
pub struct Insets {
    top: float
    right: float
    bottom: float
    left: float
}
```

### `insets`

```
pub fn insets(all: float) -> Insets
```

### `Band`

```
pub struct Band {
    height: float
    draw: fn(int, int) -> [Op]
}
```

A header or footer. `draw` takes the page number and the total and returns ops positioned
relative to the band's own top-left corner.

**Calling it needs a local binding** -- `(band.draw)(page, total)` is a syntax error, so every
call site here does `val render = band.draw` first. Checked, not assumed.

### `PageTemplate`

```
pub struct PageTemplate {
    width: float
    height: float
    margins: Insets
    header: Option<Band>
    footer: Option<Band>
}
```

### `page_template`

```
pub fn page_template(width: float, height: float, margin: float) -> PageTemplate
```

### `Document`

```
pub struct Document {
    blocks: [Block]
    fonts: [FontRef]
}
```

### `document`

```
pub fn document(blocks: [Block], fonts: [FontRef]) -> Document
```

## Measure, split, draw

### `measure`

```
pub fn measure(b: Block, width: float, d: Document) -> Result<float, string>
```

### `Split`

```
pub struct Split {
    fits: Option<Block>
    rest: Option<Block>
}
```

### `split_block`

```
pub fn split_block(b: Block, width: float, room: float, d: Document) -> Result<Split, string>
```

`fits` and `rest` are both optional because every combination means something real:


```
* `fits` only -- the block is done, carry on down the page
* both -- part of it is placed and the remainder goes to the next page
* `rest` only -- nothing fits here; start a new page and try again there
* **neither -- the block is consumed and the page ends**, which is what `PageBreak` and an
  overlarge `Spacer` both answer
```

A sentinel empty block in place of an `Option` would have to be checked for at every call site,
and the fourth case would have had no way to say itself at all.

**`split_block` and not `split`**, because `split` is the string builtin this very file needs for
wrapping, and a module function of that name shadows it everywhere inside the file. The same
collision gave phase 1 `matrix_concat` instead of `concat`; it is worth the extra word.

### `draw`

```
pub fn draw(b: Block, at: Point, width: float, d: Document) -> Result<[Op], string>
```

## Pagination

### `paginate`

```
pub fn paginate(d: Document, t: PageTemplate) -> Result<[Scene], string>
```
