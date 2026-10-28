# `markdown`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

Markdown as a document tree: a reader over a declared subset, a writer, and
the escaping that makes generated documents safe. Written in pure Ziman, like `toml`, `yaml`
and `xml`.

Marked `#![experimental]` below: new, and no Ziman program depends on it yet.

## The thing to know before using the reader

**Every string is valid Markdown.** That is what makes this module different from every other
format here. `xml` can refuse a DOCTYPE, `toml` can refuse `inf`, `yaml` can refuse an anchor
-- each says so and stops. Markdown has no syntax errors: a construct a reader does not
understand is not an error, it is a paragraph. So a partial Markdown reader does not fail on
what it is missing, it quietly returns something wrong, and the caller cannot tell.

`parse` reads the subset listed below and treats anything else as text, which is
what Markdown itself prescribes. `parse_strict` reads the same subset but returns an
Err when it sees a construct it knows it does not handle -- an indented code block, a setext
heading, a link reference definition, a raw HTML block. That is the one that belongs in a
pipeline, because it is the one that can tell you it was given something it would misread.

Reads: ATX headings (`# h`), paragraphs, fenced code blocks (``` and ~~~, with an info
string), block quotes, bullet and ordered lists (nested by indentation), GFM tables with
alignment, and thematic breaks. Inline: text, code spans, emphasis and strong, links, images
and hard line breaks.

Does not read, and says so in strict mode: setext headings (`===` under a line), indented
code blocks, link reference definitions (`[x]: url`), raw HTML blocks, autolinks, footnotes,
task lists and nested emphasis beyond the simple cases.

## The writer is the point

Generating Markdown by pasting strings together is how a table breaks: one `|` in a patient's
note and every column after it shifts. The writer escapes what each position needs -- `|` in
a table cell, the `*_[]#` family in text, a longer fence when the code itself contains one --
so a document built from this tree cannot be malformed by its own content.

`use markdown` -- or `use markdown.Name` to bring one item in bare.

## Example

```
use markdown

// Markdown is a tree here, not a string being pasted together, because pasting is how a table
// breaks: one `|` in a note and every column after it shifts.
fn demo() -> Result<unit, string> {
    val notes = "2 puffs | as needed\nmorning only"
    val doc = markdown.document([
        markdown.heading(1, "Health record"),
        markdown.paragraph("Exported for Ann Lee."),
        markdown.heading(2, "Medications"),
        try markdown.text_table(
            ["Name", "Dose", "Notes"],
            [markdown.Align.AlignLeft, markdown.Align.AlignRight, markdown.Align.AlignNone],
            [["Salbutamol", "100 mcg", markdown.flatten_cell(notes)]]
        ),
        markdown.text_list(false, ["bring inhaler", "book review"]),
        markdown.paragraph("# not a heading, because it is text")
    ])
    print(try markdown.format(doc))

    // Reading. The subset is in the module header; anything outside it is text, as Markdown says.
    val read = try markdown.parse("# Title\n\nA *word* and a [link](http://x.test).\n")
    print(format("blocks: {}", len(read.blocks)))
    print(markdown.text_of(read))

    // Markdown has no syntax errors, so only the strict reader can say it was given something
    // it would have misread -- and that refusal is the point here, so switch names it.
    print(is_ok(markdown.parse("Title\n=====\n")))
    if Err(e) = markdown.parse_strict("Title\n=====\n") {
        print(e)
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("markdown example failed: {}\n", e)
    }
}
```

Output:

```
# Health record

Exported for Ann Lee.

## Medications

| Name | Dose | Notes |
| :--- | ---: | --- |
| Salbutamol | 100 mcg | 2 puffs \| as needed morning only |

- bring inhaler
- book review

\# not a heading, because it is text

blocks: 2
Title
A word and a link.

true
line 1: a setext heading (=== under the text); use '# text' instead
```

## The model

### `Align`

```
pub enum Align {
    AlignNone
    AlignLeft
    AlignRight
    AlignCenter
}
```

How a table column is aligned, which GFM writes into the delimiter row as `:---`, `---:` or
`:---:`.

### `Link`

```
pub struct Link {
    text: [Inline]
    destination: string
    title: Option<string>
}
```

### `Inline`

```
pub enum Inline {
    Text(string)
    Emphasis([Inline])
    Strong([Inline])
    Code(string)
    Link(Link)
    Image(Link)
    SoftBreak
    HardBreak
}
```

A run of inline content. `Code` holds its text literally -- a code span is never escaped,
only fenced with enough backticks to contain it.

### `Heading`

```
pub struct Heading {
    level: int
    content: [Inline]
}
```

### `CodeBlock`

```
pub struct CodeBlock {
    info: string
    text: string
}
```

### `ListItem`

```
pub struct ListItem {
    blocks: [Block]
}
```

### `List`

```
pub struct List {
    ordered: bool
    start: int
    tight: bool
    items: [ListItem]
}
```

`start` is the first number of an ordered list; it is ignored for a bullet list. `tight` is
GFM's distinction between a list whose items are paragraphs and one whose items are not --
it decides whether blank lines separate the items when written.

### `Table`

```
pub struct Table {
    headers: [[Inline]]
    alignments: [Align]
    rows: [[[Inline]]]
}
```

### `Block`

```
pub enum Block {
    Heading(Heading)
    Paragraph([Inline])
    CodeBlock(CodeBlock)
    BlockQuote([Block])
    List(List)
    Table(Table)
    ThematicBreak
}
```

### `Document`

```
pub struct Document {
    blocks: [Block]
}
```

## Building

### `document`

```
pub fn document(blocks: [Block]) -> Document
```

### `heading`

```
pub fn heading(level: int, text: string) -> Block
```

A heading, with `level` clamped into 1..6 rather than written out as a line of hashes that no
reader would take as a heading.

### `paragraph`

```
pub fn paragraph(text: string) -> Block
```

### `code_block`

```
pub fn code_block(info: string, text: string) -> Block
```

### `link`

```
pub fn link(text: string, destination: string) -> Inline
```

### `image`

```
pub fn image(alt: string, destination: string) -> Inline
```

### `text_table`

```
pub fn text_table(headers: [string], alignments: [Align], rows: [[string]]) -> Result<Block, string>
```

A table of plain strings, which is what a report generator almost always has: `headers` names
the columns and each row must have the same number of cells. An Err otherwise, naming the row
-- a ragged table is a bug in the caller, and writing it out would corrupt every row after it.

### `text_list`

```
pub fn text_list(ordered: bool, items: [string]) -> Block
```

A bullet or ordered list whose items are each one paragraph of plain text.

## Writing

### `format`

```
pub fn format(doc: Document) -> Result<string, string>
```

`doc` as Markdown text, with a trailing newline. Blocks are separated by a blank line.

Content is escaped for the position it sits in, which is the whole reason to build a document
rather than paste strings: `*` and `_` and `[` in running text, `|` in a table cell, a leading
`#` or `1.` that would otherwise start a heading or a list, a code span fenced with enough
backticks to hold whatever is inside it. Nothing a caller puts in a string can change the
structure of the output.

An Err for the one thing Markdown cannot express: a line break inside a table cell, which GFM
has no syntax for. The message names the row. `flatten_cell` is the usual answer.

### `to_bytes`

```
pub fn to_bytes(doc: Document) -> Result<[byte], string>
```

`format` as UTF-8 bytes: an HTTP body, or what `fs.write_bytes` takes.

### `format`

```
pub fn format(block: Block) -> Result<string, string>
```

One block as Markdown text, for embedding in a document built some other way.

### `flatten_cell`

```
pub fn flatten_cell(text: string) -> string
```

Text made safe to put in a table cell: every line break becomes a space, since GFM has no
way to write one. Call it on free text -- a note, a comment -- before building a row, rather
than letting `format` refuse the document. The pipes do not need handling here;
the writer escapes those wherever they appear.

## Reading

### `parse`

```
pub fn parse(text: string) -> Result<Document, string>
```

Reads a Markdown document. Never fails on content: anything outside the subset in the module
header is read as text, which is what Markdown prescribes -- and which is exactly why
`parse_strict` exists.

### `parse_strict`

```
pub fn parse_strict(text: string) -> Result<Document, string>
```

`parse`, but an Err naming the line when the document uses a construct this reader
knows it does not handle: an indented code block, a setext heading, a link reference
definition, a raw HTML block. Use this one when something downstream depends on the document
having been understood, because the lenient reader cannot tell you that it was not.

### `parse`

```
pub fn parse(data: [byte]) -> Result<Document, string>
```

`parse` over the bytes of a document. The bytes must be UTF-8.

### `text_of`

```
pub fn text_of(doc: Document) -> string
```

All the text of a document with its formatting removed: headings, paragraphs and table cells
run together, one block per line. What a search index or a plain-text preview wants.
