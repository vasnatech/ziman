# `pdf`

A `scene.Scene` rendered to a PDF document.

The mapping is nearly mechanical, which is the point of the model: PDF's content-stream
vocabulary is almost exactly `shape.Segment`'s five cases, so most of this file is a serialiser.
See docs/graphics/phase-2-pdf-backend.md for the operator table and the reasoning.

`use pdf` -- or `use pdf.Name` to bring one item in bare.

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
