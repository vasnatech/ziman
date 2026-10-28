# `pdf`

A `scene.Scene` rendered to a PDF document.

The mapping is nearly mechanical, which is the point of the model: PDF's content-stream
vocabulary is almost exactly `shape.Segment`'s five cases, so most of this file is a serialiser.
See docs/graphics/phase-2-pdf-backend.md for the operator table and the reasoning.

`use pdf` -- or `use pdf.Name` to bring one item in bare.

## Example

```
use geom
use paint
use pdf
use scene
use shape

// The PDF backend: the same `Scene` the raster backend draws, written as a document instead. A
// page is a scene, a document is a list of them, and the answer is the bytes -- so a report can be
// built, tested and handed back without a file ever being involved.
fn page(label: float) -> scene.Scene {
    return scene.with_ops(scene.scene_of(200.0, 100.0), [
        scene.fill(
            shape.rect_path(geom.rect_of(10.0, 10.0, label, 40.0)),
            paint.solid(paint.rgb(0.2, 0.4, 0.8))
        ),
        scene.text_of("Ziman", geom.point_of(10.0, 70.0), 12.0, 0, paint.solid(paint.black()))
    ])
}

fn demo() -> Result<unit, string> {
    val bytes = try pdf.render(page(50.0), pdf.options())

    // A PDF is bytes, and these are the bytes every PDF starts and ends with.
    print(try bytes_to_string(slice(bytes, 0, 8)))
    print(contains(try bytes_to_string(slice(bytes, len(bytes) - 6, len(bytes))), "%%EOF"))

    // Several scenes make several pages, which is the only difference between a page and a
    // document.
    val one = try pdf.render_pages([page(50.0)], pdf.options())
    val three = try pdf.render_pages([page(50.0), page(60.0), page(70.0)], pdf.options())
    print(len(three) > len(one))

    // The page size is a value, so the same drawing can be placed on A4 or Letter without the
    // drawing knowing which.
    val a4 = pdf.a4()
    val letter = pdf.letter()
    printf("{} {}\n", a4.width, letter.width)

    // Compression is off by default so the output can be read while developing, and on when it
    // matters -- the same drawing, fewer bytes.
    val plain = try pdf.render(page(50.0), with pdf.options() { compress: false })
    val packed = try pdf.render(page(50.0), with pdf.options() { compress: true })
    print(len(packed) < len(plain))

    // Text in a standard font is encoded in WinAnsi, and a character that encoding has no room
    // for is refused rather than silently replaced.
    print(is_ok(pdf.win_ansi("Ziman")))
    print(is_ok(pdf.win_ansi("Zimān")))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("pdf example failed: {}\n", e)
    }
}
```

Output:

```
%PDF-1.7
true
true
595.276 612.0
true
true
false
```

### `PageSize`

```
pub struct PageSize {
    width: float
    height: float
}
```

### `a4`

```
pub fn a4() -> PageSize
```

Functions rather than vals, so `pdf.a4()` works without a second import -- see geom's note.

### `letter`

```
pub fn letter() -> PageSize
```

### `page_size_of`

```
pub fn page_size_of(width: float, height: float) -> PageSize
```

### `Options`

```
pub struct Options {
    size: PageSize
    // The faces a scene's `TextRun.font` indexes into -- standard or embedded, mixed freely.
    fonts: [FontRef]
    // The images a scene's `DrawImage` indexes into. Images are document-level for the same reason
    // fonts are: one XObject however many pages place it.
    images: [image.Image]
    title: string
    author: string
    compress: bool
}
```

### `options`

```
pub fn options() -> Options
```

### `render`

```
pub fn render(s: Scene, o: Options) -> Result<[byte], string>
```

### `render_pages`

```
pub fn render_pages(pages: [Scene], o: Options) -> Result<[byte], string>
```

A document is the normal case and one page the special case, so this is the primitive -- a
library that only writes one page gets a concatenation layer bolted on later, and the bolt is
always worse than the design.

## WinAnsiEncoding

### `win_ansi`

```
pub fn win_ansi(text: string) -> Result<[byte], string>
```

`text` as WinAnsi bytes, or an `Err` naming the first character the encoding cannot represent.

**This refuses rather than substitutes, and that is the point.** `WinAnsiEncoding` has no `ş`,
no `İ`, no Cyrillic and no CJK, and quietly writing `?` into someone's medical record is the
failure invariant 2 exists to prevent. The error says which character and points at phase 3.
