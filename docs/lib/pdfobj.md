# `pdfobj`

PDF's object layer: objects, dictionaries, streams, the cross-reference table.

This module knows PDF's *syntax* and nothing about graphics: it has no idea what a path is. It is
split from `pdf` because the two fail differently -- this one gets byte offsets and escaping
wrong, that one gets transforms and winding rules wrong -- and debugging a broken xref table
while also debugging a matrix is miserable.

A PDF file is: a header, a run of numbered objects, a table of their byte offsets, and a trailer
naming the root. `finish` is where that shape lives; everything above it just builds values.

`use pdfobj` -- or `use pdfobj.Name` to bring one item in bare.

### `Entry`

```
pub struct Entry {
    key: string
    value: Obj
}
```

### `entry`

```
pub fn entry(key: string, value: Obj) -> Entry
```

### `StreamObj`

```
pub struct StreamObj {
    dict: [Entry]
    data: [byte]
}
```

A stream is a dictionary plus data. One payload per variant, so it is a struct.

### `Obj`

```
pub enum Obj {
    Null
    Bool(bool)
    Int(int)
    Real(float)
    // Written `/Name`.
    Name(string)
    // Written `(escaped)`.
    Str(string)
    // Written `<hex>` -- for a byte string, and for Identity-H text in phase 3, where two bytes per
    // glyph is not representable as a literal.
    Bytes([byte])
    Array([Obj])
    Dict([Entry])
    Stream(StreamObj)
    // `N 0 R`, an indirect reference.
    Ref(int)
}
```

### `stream`

```
pub fn stream(dict: [Entry], data: [byte]) -> Obj
```

`data` as a stream, with `/Length` added. Never compressed; see `flate_stream`.

### `flate_stream`

```
pub fn flate_stream(dict: [Entry], data: [byte]) -> Obj
```

`data` deflated, with `/Filter /FlateDecode` and the compressed `/Length`.

`compress.deflate` writes a zlib stream (RFC 1950), which is exactly what `FlateDecode` expects
-- no raw-deflate wrapper needed. Compression is explicit rather than decided by a size
threshold inside the writer, so a caller reading the output can tell which it got.

### `PdfWriter`

```
pub struct PdfWriter {
    bodies: MutableArray<[byte]>
    filled: MutableArray<bool>
}
```

The document under construction. Object numbers are 1-based, as PDF's are.

### `writer`

```
pub fn writer() -> PdfWriter
```

### `add`

```
pub fn add(w: PdfWriter, o: Obj) -> int
```

Appends `o` and answers its object number.

### `reserve`

```
pub fn reserve(w: PdfWriter) -> int
```

An object number to point at before the object exists, filled later by `put`.

The object graph has forward references -- a page names its parent page-tree node, which lists
every page, so one of the two must be written knowing only the other's number. Without
reserve-then-fill this module would need a second pass or a mutable graph.

### `put`

```
pub fn put(w: PdfWriter, number: int, o: Obj) -> Result<unit, string>
```

### `finish`

```
pub fn finish(w: PdfWriter, root: int, info: [Entry]) -> Result<[byte], string>
```

The whole file. `root` is the object number of the catalog; `info` goes in a document
information dictionary, or is empty for none.

## Serialising one object

### `real_text`

```
pub fn real_text(x: float) -> string
```

Fixed-point, never scientific. A full float repr would write `0.30000000000000004` into a
content stream, and PDF has no exponent notation at all -- `1e-05` is a syntax error there.
Trailing zeros are trimmed because a page of coordinates is mostly zeros.

### `hex_of`

```
pub fn hex_of(data: [byte]) -> string
```

## Validating what this module wrote

### `check`

```
pub fn check(data: [byte]) -> Result<unit, string>
```

Checks the structural invariants a reader relies on, and names the first one that fails.

**This is the verification layer that matters**, because the whole class of "some readers open
the file and others reject it" is byte offsets and lengths rather than graphics. It checks that
the header is there, that `startxref` points at a real xref table, that **every xref offset lands
on the object header it claims**, that every declared `/Length` matches the bytes actually
between `stream` and `endstream`, and that the trailer's `/Root` names an object that exists and
is a `/Catalog`.

It is a validator for *this writer's* output, not a general PDF parser: it assumes the fixed-width
xref table and the exact object framing `finish` emits. A general parser would have to cope with
cross-reference streams, incremental updates and object streams, none of which this module
produces.
