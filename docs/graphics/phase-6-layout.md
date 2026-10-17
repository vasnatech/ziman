# Phase 6 — document layout: flows, wrapping, tables and pagination

The layer that turns *headings, paragraphs and tables* into coordinates. Everything in phases 1-5 is
a drawing API: it puts a path or a glyph at a point. The motivating use case -- a health record
exported as a PDF -- does not think in points, and something has to be the thing that does.

Read `README.md` first for the architecture and the invariants; this phase inherits all five.

## A warning about this document

**Phases 1-5 were each designed against something concrete** -- a PDF content stream, a TrueType
table, a PNG chunk, a scanline. This one is not. `README.md` said a layout model should wait until
the drawing model had a user, because *"the export work will say what it actually needs"*, and the
export has still not been written: nothing in the health-record backend imports a `lib/graphics`
module yet.

So this is a design against a guess, written because having the shape on paper before the export is
worth more than the risk, and it should be read with that in mind. The parts most likely to be wrong
are the ones nothing has pulled on: the style model, the table column sizing, and whether `Block`
is the right granularity at all. The parts least likely to be wrong are the ones phases 1-5 already
constrain -- measurement, splitting, and what a page is.

**The first real export should be allowed to contradict this.** If it does, the document is wrong,
not the export.

## Scope

`layout`. One module. No I/O, no native code. Depends on `geom`, `shape`, `paint`, `scene` and
`font`, which puts it at layer 5 beside `pdf` -- and **on neither backend**, because its output is a
`Scene` and both backends already take one. (One thing stands in the way of that today; see
"Pages".)

```ziman
pub fn paginate(doc: Document, page: PageTemplate) -> Result<[Scene], string>
```

That signature is the whole phase. `pdf.render_pages` already takes a list of scenes, and it takes
one **for exactly this reason**; `raster.render` takes them one at a time. A layout layer that
emitted anything else would be inventing a third thing for the backends to learn.

## The model

### Blocks

```ziman
pub struct Document {
    blocks: [Block]
    // The faces blocks index, the same table `pdf.Options` and `raster.Options` carry.
    fonts: [FontRef]
    images: [image.Image]
}

pub enum Block {
    Paragraph(Para)
    Table(Table)
    Spacer(float)          // vertical space
    Rule(RuleLine)         // a horizontal line, for separators
    Picture(Framed)        // an image handle and a box to fit it in
    Together(Keep)         // a run of blocks that must not be split
    PageBreak
}
```

A **closed enum**, like `Paint` and for the same reason: an open "any block that can measure and
draw itself" interface is what a language with methods would reach for, and this language does not
have methods. Closing the set means three exhaustive `switch`es -- measure, split, draw -- and a
compiler error when a variant is added, rather than a default branch that silently drops it.

`Together` is the escape hatch that keeps the set honest: a heading that must not be orphaned from
its first paragraph is a `Together` of two blocks, not a new variant and not a flag on `Paragraph`.

### Paragraphs

```ziman
pub struct Para {
    spans: [Span]          // a paragraph is runs of differently-styled text
    style: ParaStyle
}

pub struct Span {
    text: string
    font: int              // index into Document.fonts
    size: float
    fill: Paint
}

pub struct ParaStyle {
    align: Align           // Left | Right | Centre | Justify
    leading: float         // baseline to baseline; 0 means 1.2 * the largest span size
    space_before: float
    space_after: float
    indent_first: float
    indent_left: float
    indent_right: float
    // Lines that may not be left alone at the bottom or top of a page. 2 and 2 is the
    // typographic convention; 0 and 0 turns the rule off.
    widows: int
    orphans: int
}
```

Spans rather than one string plus a style, because "**bold** inside a sentence" is the first thing
any real document needs and retrofitting it would change every signature that touches a paragraph.
A `Span` carries its own font index and size for the same reason `TextRun` does.

### Tables

```ziman
pub struct Table {
    columns: [Column]
    header: [Row]          // repeated at the top of every page the table continues onto
    rows: [Row]
    style: TableStyle
}

pub struct Column {
    width: ColumnWidth     // Fixed(float) | Proportional(float) | Auto
    align: Align
}

pub struct Row { cells: [Cell] }
pub struct Cell { blocks: [Block] }   // a cell is a document in miniature, so it nests
```

A cell holding `[Block]` rather than a string is what makes a table able to contain a wrapped
paragraph or another table, and it costs nothing: the measure/split/draw functions already recurse.

`header` is separate from `rows` and not a flag on the first row, because **repeating it on
continuation pages is the single feature that makes long tables readable** and it is the thing a
health record's medication list will need on page two.

### Pages

```ziman
pub struct PageTemplate {
    size: PageSize
    margins: Insets
    header: Option<Band>   // drawn on every page, outside the text frame
    footer: Option<Band>
}

pub struct Band {
    height: float
    // Given the page number and the total, produce ops. A function rather than text, so a footer
    // can draw a rule and a page number without the template needing a vocabulary for either.
    draw: fn(int, int) -> [Op]
}
```

**Two things here were checked against the language rather than assumed**, because both would have
been discovered at implementation time otherwise.

*`PageSize` is `pdf.PageSize` today* (`lib/graphics/pdf.zm:36`), and `layout` must not depend on a
backend -- its whole point is that it feeds both. This is phase 3's circularity exactly: `pdf`
needed `font` to embed a face, so `StandardFont` moved out of `pdf` and into `font`. The same call
applies, and the options are to move `PageSize` down into `geom` (where `Rect` already lives and
every module can see it), or for `layout` to take a plain `Rect` and let the caller pass
`pdf.a4()`'s numbers. **Moving it is better**: a page size is not a PDF concept, `raster` has the
same need, and a second near-identical struct is how a library ends up converting between its own
types.

*A function in a struct field works, and is called through a local.* `Band { draw: footer }` is
accepted and so is a closure capturing an outer value -- which a footer wants, to carry a document
title. But `(band.draw)(page, total)` is a **syntax error**; the call site has to be:

```ziman
val render = band.draw
val ops = render(page, total)
```

Worth knowing before the design leans on it, and worth revisiting if the parser ever learns to call
a field directly.

## Three decisions worth making on purpose

### 1. Greedy line breaking, not Knuth-Plass

Each line takes as many words as fit, then breaks. Knuth-Plass minimises a badness function over the
whole paragraph and produces visibly better justified text, but it needs a penalty model, a
hyphenation dictionary to be worth much, and three times the code.

The export sets ragged-right body text, where the difference is small. **Greedy first, and the
measurement that would justify changing it is named**: the variance of line fill ratios across a
page. If ragged-right ever becomes justified, revisit it then.

### 2. Page numbers need two passes, and the design says which

"Page 3 of 17" cannot be drawn on page 3 until page 17 is known, and the total is not known until
pagination finishes. Three options, and the choice matters because it leaks:

- **Lay out twice.** Paginate to count, then paginate again with the total known. Simple and
  obviously correct; costs double. It is only wrong if layout is not deterministic, which it is.
- **Placeholder, patched afterwards.** Fast, and it quietly constrains the footer to text whose
  width does not change when the number does.
- **Deferred ops.** A footer returns ops that reference a value filled in later, which means a new
  kind of op and a mutable scene.

**Take the first.** It is the only one that does not change a type elsewhere, and the cost is a
second pass over a document that already fits in memory. If profiling ever makes that matter, the
second pass can skip blocks whose geometry is already known.

### 3. Measurement is where this phase meets a real constraint

Wrapping needs `font.advance`, and **`font.advance` refuses for a proportional standard font**
(`lib/graphics/font.zm`): the standard fourteen have no width table in this library, and only
Courier can be measured, at exactly 0.6 em.

So layout over the standard fonts works monospaced and not otherwise. That is not a defect to work
around here -- it is phase 3's decision not to ship ~3000 AFM numbers, and the right response is to
**let the refusal through** with its own message rather than substituting an estimate. A paragraph
measured with a guessed width lays out wrongly in a way nobody notices until it is printed.

In practice the export embeds a face anyway, which already works end to end. The constraint is
recorded because a layout layer that assumed otherwise would hit it on its first paragraph.

## The algorithm

Three functions over `Block`, each an exhaustive switch:

```ziman
fn measure(b: Block, width: float, d: Document) -> Result<float, string>
fn split(b: Block, width: float, room: float, d: Document) -> Result<Split, string>
fn draw(b: Block, at: Point, width: float, d: Document) -> Result<[Op], string>

pub struct Split {
    fits: Option<Block>    // what goes on this page
    rest: Option<Block>    // what continues onto the next
}
```

`split` returning two `Option`s rather than a pair of blocks is deliberate: *nothing fits* and
*everything fits* are both ordinary answers, and a sentinel empty block would have to be checked for
at every call site.

Pagination is then a loop: take the frame's remaining height, ask the next block to split into it,
emit what fits, start a new page when nothing does. The only subtlety is **making progress** -- a
block that cannot fit even an empty page must be emitted anyway and overflow, or the loop does not
terminate. That is a `Result` case, not a silent clip:

> `layout: a table row 840pt tall cannot fit a 720pt frame, even on an empty page`

## What phase 6 does *not* do

Floats and text wrapping around them. Multi-column balancing (a fixed two-column frame is in scope;
balancing the columns' heights is not). Hyphenation. Bidirectional text and RTL, which need the
shaping phase 3 excluded. Footnotes and margin notes. Vertical writing modes. A styling *language* --
styles are structs the caller builds, not a cascade, not a selector engine, and not CSS. Automatic
tables of contents, cross-references and indexes, all of which need a second pass over resolved page
numbers and are a layer above this one.

## Verification

Layout output is a tree of coordinates, which is as hard to eyeball as pixels were, so the checks
are chosen the same way phase 5's were: **arithmetic that must hold**, not pictures to approve.

- **Wrapping against known advances.** A monospaced face at a known size makes every character the
  same width, so the break positions are computable by hand: 40 characters at 0.6 em of 10pt in a
  240pt frame breaks after exactly the 40th. Assert the break *positions*, not the rendering.
- **Conservation under splitting.** The heights of a split block's two halves sum to the unsplit
  height, within the leading. This is phase 5's coverage-sum check in another costume, and it
  catches the same class of error: a fragment silently dropped or counted twice.
- **Pagination is exact.** Thirty blocks of height 20 in a frame of height 100 produce exactly six
  pages. An off-by-one in "does it fit" is a page count, not a visual difference.
- **A split table repeats its header.** Assert the header row's text appears in the ops of *both*
  pages. This is the feature most likely to be quietly dropped, because the table still looks fine
  on page one.
- **Widows and orphans.** A paragraph whose last line would sit alone at the top of a page moves two
  lines instead of one. Assert the line counts per page, which is a number; the rule is invisible
  otherwise.
- **The standard-font refusal is let through.** Wrapping proportional Helvetica must return
  `font.advance`'s own error, not an estimate. One assertion, and it is the no-silent-degradation
  invariant at this layer.
- **The round trip, reusing phase 5's.** Lay out a known document, render to PDF, `pdftotext` it,
  and assert the words come back in reading order. That is the only check that exercises layout,
  the document backend and the font encoding together -- and the infrastructure already exists in
  `tests/run_pdf_tests.sh`.
- **No golden layouts.** Phase 4 chose generated fixtures over committed ones and phase 5 avoided
  golden images; a committed layout dump would be the same mistake a third time -- a file nobody
  ever re-examines, that fails on every intentional change.

## Build changes

One module registered, the usual five places (`Makefile`'s `CORELIB_MODULES`, `src/corelib.c`'s
include and table row, `tools/gen_lib_manual.py`'s `GROUPS`). No native code. No new dependency
outside `lib/graphics` except `font`, which it already shares with both backends.

## The question this phase should answer first

Before any of the above is built, write one real page of the health-record export directly against
`scene` and `pdf`. It will be unpleasant in a specific way, and **that specific way is this phase's
actual requirements document**. If what hurts is wrapping, start with `Paragraph` and ship
`paginate` with nothing else in the enum. If what hurts is the medication table spilling off the
page, start with `Table` and `header`.

Building all of `Block` first is the failure mode this document is most likely to cause.
