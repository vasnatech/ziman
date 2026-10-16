# Phase 2 — the PDF backend, with the standard fonts

The first renderer. Turns a `Scene` into a valid PDF file: paths, fills, strokes, clips, transforms,
and text in the fourteen fonts every reader is required to have. No embedded fonts (phase 3), no
images (phase 4).

Read `README.md` first.

## Scope

`pdfobj` (the object layer) and `pdf` (the backend). Split because they fail differently: the object
layer is about byte offsets and syntax, the backend is about graphics semantics, and debugging a
broken xref table while also debugging a transform is miserable.

## `pdfobj` — the object layer

A PDF file is a set of numbered objects, a cross-reference table of their byte offsets, and a
trailer pointing at the root. That is all this module knows; it has no idea what a path is.

```ziman
pub enum Obj {
    Null
    Bool(bool)
    Int(int)
    Real(float)
    Name(string)              // written /Name
    Str(string)               // written (escaped)
    Bytes([byte])             // written <hex>
    Array([Obj])
    Dict([Entry])
    Stream([Entry], [byte])   // dictionary plus data; Length is added, not supplied
    Ref(int)                  // written "N 0 R"
}

pub struct Entry { key: string  value: Obj }

pub fn writer() -> PdfWriter                       // #[thread_bound]
pub fn add(w: PdfWriter, o: Obj) -> int            // appends, answers the object number
pub fn reserve(w: PdfWriter) -> int                // a number to point at before the object exists
pub fn put(w: PdfWriter, number: int, o: Obj)      // fills a reserved number
pub fn finish(w: PdfWriter, root: int, info: [Entry]) -> [byte]
```

**`reserve`/`put` exists because the object graph has forward references.** A page must name its
parent page-tree node, which must list every page; one of the two has to be written knowing only the
other's number. Without reserve-then-fill, the builder would need a second pass or a mutable graph.

**`Stream` computes its own `/Length`** and, when the data is large enough to pay for it, deflates it
and adds `/Filter /FlateDecode`. `compress.deflate` already writes the zlib framing `FlateDecode`
expects. The threshold matters less than the rule being in one place: a hand-written `Length` that
disagrees with the data is the single most common way to produce a file readers reject.

**Strings are escaped on the way out**, `(`, `)` and `\` at minimum, and non-ASCII goes out as
`Bytes` (hex) rather than as a literal -- phase 3's text needs that anyway, and a literal with a
stray byte is unparseable.

## `pdf` — the backend

```ziman
pub struct PageSize { width: float  height: float }
pub val a4: PageSize            // 595.276 x 841.890 points
pub val letter: PageSize        // 612 x 792

pub struct Options {
    size: PageSize
    title: Option<string>
    author: Option<string>
    compress: bool
}
pub fn options() -> Options

pub fn render(s: scene.Scene, o: Options) -> Result<[byte], string>
pub fn render_pages(pages: [scene.Scene], o: Options) -> Result<[byte], string>
```

`render_pages` is there from the start because a document is the normal case and a one-page document
is the special case -- a library that only does one page gets a page-concatenation layer bolted on
top of it later, and the bolt is always worse than the design.

### The operator mapping

This is the whole backend, and it is nearly mechanical:

| model | PDF |
| --- | --- |
| `MoveTo(p)` | `x y m` |
| `LineTo(p)` | `x y l` |
| `CubicTo(c1, c2, p)` | `x1 y1 x2 y2 x3 y3 c` |
| `QuadTo(c, p)` | elevated to a cubic, then `c` |
| `Close` | `h` |
| `Fill(path, Solid)` with `NonZero` | `... f` |
| `Fill(path, Solid)` with `EvenOdd` | `... f*` |
| `StrokePath(path, Solid, stroke)` | `w J j M d` then `... S` |
| `Fill` with a gradient | `/Pattern cs` + a shading dictionary, `scn` + `f` |
| `Group(transform, None, ops)` | `q`, `a b c d e f cm`, ops, `Q` |
| `Group(transform, Some(clip), ops)` | `q`, `cm`, clip path + `W n`, ops, `Q` |
| `Text(run)` | `BT /F1 size Tf x y Td (text) Tj ET` |

**`QuadTo` is elevated, not approximated.** A quadratic is exactly a cubic: the two control points
are `start + 2/3*(c - start)` and `end + 2/3*(c - end)`. No tolerance, no subdivision, no
flattening. Worth stating because the instinct is to flatten curves, and flattening is lossy for no
reason here.

**`W n` is the clip idiom and the `n` is not optional.** `W` marks the current path as a clip and
`n` is the no-op painting operator that ends it. Writing `W` alone leaves the path pending and the
next operator does something surprising.

### The Y axis, flipped exactly once

The model has Y growing downward (README invariant 4); PDF has it growing upward. The backend emits,
as the outermost transform of every page, `1 0 0 -1 0 H cm` where `H` is the page height. Everything
inside is then written in model coordinates unchanged.

The cost of that choice, stated: **text is also flipped**, so a text run needs its own compensating
flip (`1 0 0 -1 0 0` concatenated at the text matrix) or it renders mirrored. That is one known place
to get right, which is better than every caller converting coordinates and getting it right
inconsistently.

### Text in phase 2

The fourteen standard fonts (`Helvetica`, `Helvetica-Bold`, `Times-Roman`, `Courier`, ...) need no
embedding: a font dictionary with `/BaseFont /Helvetica` and `/Encoding /WinAnsiEncoding` is enough,
and every conforming reader supplies the glyphs.

```ziman
pub enum StandardFont { Helvetica  HelveticaBold  HelveticaOblique  Times  TimesBold  Courier  ... }

pub struct TextRun {    // the `scene.Text` payload, declared in phase 1, filled here
    text: string
    at: geom.Point
    size: float
    font: FontRef       // StandardFont in phase 2; an embedded face in phase 3
    paint: Paint
}
```

**The limitation is Latin-1 and it must be an error, not a mangling.** `WinAnsiEncoding` cannot
represent `ş`, `İ`, Cyrillic or CJK. A character outside it makes `render` return an `Err` naming the
character and pointing at phase 3's embedded fonts -- *not* a `?` substituted into someone's medical
record. This is README invariant 2 (no silent degradation) in its most consequential instance.

Widths for the standard fonts come from the published AFM metrics, as a table in the module. Only
needed for measurement and alignment; the reader does the drawing.

## What phase 2 does *not* do

Embedded fonts, non-Latin text, images, transparency groups, blend modes, annotations, outlines,
tagged/accessible PDF, encryption. `Pattern` paint returns `Err` until phase 4 gives it an image.

## Verification

PDF is a format where "it opened in one viewer" is weak evidence, so three layers:

1. **Structural, in-process.** Parse back what `pdfobj` wrote: every `/Length` matches its stream,
   every `Ref` points at an object that exists, the xref offsets land on object headers, the trailer
   names a `/Root` that is a `/Catalog`. This catches the entire class of "readers reject the file".
2. **External tools, where present.** `qpdf --check` on the output, and `pdftotext` to confirm the
   text is extractable and says what it should -- which also proves text went out as text rather
   than as paths. Both guarded on the tool existing, the way the TLS suite guards on `openssl`.
3. **A fixture that renders a known scene** and asserts on the operator stream: a filled rect, an
   even-odd path, a dashed stroke, a nested clip, a rotated group. Asserting on the content stream
   rather than on pixels means a failure says *which operator* is wrong.

The out-of-encoding case gets its own fixture asserting the `Err` names the character.

## Build changes

Two modules registered. No native code. No new `math` needs beyond phase 1's `atan2`.
