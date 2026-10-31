# Phase 6 — document layout: flows, wrapping, tables and pagination

The layer that turns *headings, paragraphs and tables* into coordinates. Everything in phases 1-5 is
a drawing API: it puts a path or a glyph at a point. The motivating use case -- a health record
exported as a PDF -- does not think in points, and something has to be the thing that does.

Read `README.md` first for the architecture and the invariants; this phase inherits all five.

## Status

**Built as `lib/graphics/layout.zm` (F146), and the export it was a guess about now exists**:
`service/export/export_pdf.zm` in the health-record backend imports `layout`, `pdf`, `scene`,
`paint`, `geom` and `font`, so the reservation below is discharged -- the design was written against
a guess and the first real export did not contradict it. Worked example in
`docs/lib/examples/graphics/layout_example.zm`, pinned by `tests/lang/graphics_layout`.

*Update 2026-10-30: the warning below still said "the export has still not been written: nothing in
the health-record backend imports a `lib/graphics` module yet". It is kept as written, because it is
the record of what was known when the shape was chosen, and because the judgement it describes --
design ahead of the user, and let the user overrule it -- is the thing worth keeping.*

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

*`PageSize` is `pdf.PageSize` today* (`lib/graphics/pdf.zm:36`), and `layout` should not depend on a
backend -- its whole point is that it feeds both. This is phase 3's circularity in miniature: `pdf`
needed `font` to embed a face, so `StandardFont` moved out of `pdf` and into `font`.

It is **not a blocker**, because layout can carry two floats and the `Scene` it emits already
carries its own width and height -- which is what `pdf` reads for the page box anyway, falling back
to `Options.size` only when a scene has none. But moving `PageSize` down into `geom` is still
better: a page size is not a PDF concept, `raster` has the same need, and a second near-identical
struct is how a library ends up writing conversion functions between its own types.

**And the move is cheap, which was worth checking rather than assuming.** A module does *not*
re-export a type it imports -- `upper.Size` is unreachable for callers of `upper` even when `upper`
does `use lower.Size` -- so moving the struct would break anyone naming `pdf.PageSize`. Nothing
does: every caller in this repository and in the health-record backend uses the *constructors*
(`pdf.a4()`, `pdf.page_size_of(...)`), and the only mention of the type name outside its own
definition is the field inside `pdf.Options`. So the move is source-compatible in practice, and
costs one line in `pdf.zm`.

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

**Measure each word once and accumulate; do not re-measure the line.** The obvious wrapping loop
asks `font.advance` for the whole candidate line at every word, which is quadratic in the line and
measured at **808 ms against 86 ms** for a 1365-word document -- 9.4x, for the same 105 lines.

That shortcut is exact *only because nothing kerns*: `font.advance` sums per-glyph advances from
`hmtx` with no pair adjustment, so the width of `"a b"` really is the width of `"a"` plus a space
plus `"b"`. **If kerning is ever added this breaks silently**, overestimating every line and
wrapping early, with no error anywhere. It is written down here because the fast path and the
correctness argument live in different files, and a future `kern`/GPOS reader has no way to know
this code depends on their absence.

`font.advance` itself costs about 11 microseconds a character -- a `codepoint_of` allocation and a
cmap lookup per character, neither cached. 86 ms a document is nothing, so this is **not** a thing
to fix now; it is recorded as the first place to look if layout ever feels slow, and a per-codepoint
advance cache on `Face` is the obvious shape.

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

## What the export experiment found

The step this document asks for at the end has now been taken, in miniature: one real page of the
health-record export written directly against `scene` and `pdf`, with a patient title block, a mint
rule, a five-column medication table and wrapped note text under each row. **It is 131 lines and it
produces a better page than the reference implementation does** -- `PdfExportService` in the Spring
backend wraps nothing at all (`showText` with no width check) and has no table.

Then it was made to paginate forty medications, and that is where it went wrong.

**1. The page-break estimate is a guess, and the guess fails silently.** The condition is

```ziman
if y > page_h - margin - leading * 3.0 { ... start a new page ... }
```

and the `3.0` means "a row, plus about two lines of notes". It is the only thing that can be written
without measuring. Given a note that wraps to six lines, **three of five pages print 17pt past the
bottom margin** -- not an error, not a warning, just text in the margin, and with a slightly longer
note it would run off the paper.

This is the whole argument for `measure` and `split` being separate from `draw`. A break cannot be
decided without knowing the height of what comes next; the height cannot be known without wrapping;
and wrapping is what `draw` was about to do anyway. A caller doing its own layout has no way to ask
the question, so it guesses, and the guess is wrong exactly when the content is unusual -- which in
a medical record is the case that matters most.

**2. Pagination cost 27% more code, and none of it is separable.** 131 lines to 167. The new code
is not a block at the top or a wrapper at the bottom; it is interleaved with the content, because
the break has to happen *between* two particular rows.

**3. The header-drawing code is duplicated verbatim.** Once before the loop, once inside the break
branch. That is `Table.header` in the model above earning its keep before the module exists: the
field is not a convenience, it is the thing that stops this copy-paste.

**4. Column positions are magic numbers.** `[margin, margin + 150, margin + 230, ...]`, tuned until
it looked right. Nothing measures the content, so a long drug name silently overlaps the next
column. `ColumnWidth.Auto` is what that wants.

**5. Greedy `wrap` has now been written three times in one sitting** -- in two prototypes and in the
export. Nineteen lines each time, and the third one is where the measure-per-word trick had to be
remembered rather than rediscovered. That is the clearest possible signal that it belongs in a
module.

**6. "Page 3 of 17" was not attempted**, because the total is not known while the pages are being
built. The two-pass decision above is what it needs.

None of this changes the design. All six are things the model already has an answer for, which is
mildly reassuring and should not be over-read: the experiment was run by the person who wrote the
model. What it does establish is that the answers are **needed** -- in particular that `measure`,
`split` and `draw` being three separate functions is not an aesthetic preference but the fix for a
page that silently prints into its own margin.

## The question this phase should answer first

Before any of the above is built, write one real page of the health-record export directly against
`scene` and `pdf`. It will be unpleasant in a specific way, and **that specific way is this phase's
actual requirements document**. If what hurts is wrapping, start with `Paragraph` and ship
`paginate` with nothing else in the enum. If what hurts is the medication table spilling off the
page, start with `Table` and `header`.

Building all of `Block` first is the failure mode this document is most likely to cause.

**Two prototypes and a real export page have already run**, which is why the three decisions above are stated as
confidently as they are. It wraps a paragraph greedily with `font.advance`, breaks it into pages by
`font.line_height`, emits one `Scene` per page, and renders an 11-page PDF that `pdfobj.check`
validates and `pdftotext` reads back in reading order. **Nothing had to change for it to work.**
Everything layout needs -- `split`, `font.advance`, `font.ascent`, `font.line_height`,
`scene.text_of`, `pdf.render_pages` taking per-page sizes from each scene -- already exists and fits
together. What the prototype found was not a missing feature but the quadratic measuring loop above,
which is a layout-side mistake and is now written down before anyone makes it again.

## As built

Landed 2026-10-16. `layout.paginate(doc, template) -> Result<[Scene], string>`, a closed `Block`
enum (`Paragraph`, `Table`, `Spacer`, `Rule`, `PageBreak`) and the three exhaustive switches over
it. Wrapping across spans, all four alignments, widows and orphans, tables with `Fixed`,
`Proportional` and `Auto` columns, repeating headers, and header/footer bands that get the page
number and the total.

**The module is measured against the bug that motivated it.** The same forty-medication table that
printed 17pt past its own bottom margin on three of five pages by hand produces, through `layout`,
**zero words below the frame** -- asserted in `tests/run_pdf_tests.sh` using Poppler's own word
geometry, so it is not this project agreeing with itself. It also needs three pages where the hand
version took five, because `Auto` columns size to content instead of to numbers tuned by eye.

### Five things the build found that the design above did not

**Pagination needs one pass, not two.** The design chose "lay out twice" for page numbers, and that
was unnecessary: a band's *height* is fixed even though its *content* needs the total, so the frame
is identical on every page and the content can be paginated before the total is known. The bands
are drawn afterwards, when it is. The design's reasoning about the three options was right; the
premise that pagination depends on the footer was not.

**`split_block`, not `split`.** `split` is the string builtin this very module needs for wrapping,
and a module function of that name shadows it throughout the file. Phase 1 hit the same wall with
`concat` and came away with `matrix_concat`.

**`Split { fits: None, rest: None }` means "consumed, end the page"**, and that fourth case is load
bearing. The first implementation used it only for `PageBreak` and gave an oversized `Spacer` the
same answer by accident -- so thirty 20pt spacers in a 100pt frame rendered as **one** page, with
twenty-five of them silently dropped and no break. Reading all four combinations as meaningful
fixes it and deletes the special case `PageBreak` used to need. A spacer that does not fit is still
*consumed* rather than carried, because space at a page break is the break; thirty spacers give
five pages where thirty rules give six, and both numbers are right.

**A page can hold content that draws nothing.** The final page was being dropped when its op list
was empty, which is wrong for a page of spacers and wrong after an explicit `PageBreak` -- the test
asked for two pages and got one. The condition is on the cursor, not on the op count.

**An `Auto` column wrapped the line it was measured from.** Found later, by the health-record
export's six-column Procedures table, where `Performed By` came out on two lines. The column is as
wide as its widest line *plus the cell padding*, and the cell hands the text back
`width - padding * 2` -- and `(w + 2p) - 2p` is not always `w`. For a 61.2pt header it returns
1.4e-14pt short, two ulps, because the two sums accumulate in different orders. Compared exactly,
that is enough to wrap. The fix is a `fit_slack` of 0.001pt in the break condition, which is
1/72000 inch -- far below anything a device resolves and far above what a measurement can lose on
the way. It is not a tolerance on *whether text fits*; it is the rounding error of a round trip
this module performs itself. The lang fixture's new case prints 2 lines with the slack and 3
without.

### On the verification

The lang fixture uses **Courier**, the one standard font this library can measure, at exactly
0.6 em a character. At 10pt that is 6pt a character, so every break position is arithmetic and no
font file is needed. The display list being inspectable data is what makes it checkable at all: the
test reads back the `Text` ops to recover the lines that were drawn, which is the property phase 1
claimed for an immutable display list and the first time it has been used for that.

Two checks were confirmed by mutation, and **the first attempt at one of them was worthless**:
conservation under splitting was being checked on a paragraph with no `space_before` or
`space_after`, so removing the code that redistributes them changed nothing and the mutation passed.
Checked on a spaced paragraph it fails loudly (24 + 19 against 36). Dropping the header from a
split table's continuation is caught too -- page 2 starts "drug4" instead of "Name".

### Still not built

`Together` (keep-with-next) and `Picture`, which the enum does not yet have; justified text is
implemented but hyphenation is not, so a narrow measure sets loose lines; and nothing recurses into
a table inside a table beyond what `measure`/`draw` already do by construction. Rows do not split,
by design -- a row taller than the frame is an error rather than a clipped row.
