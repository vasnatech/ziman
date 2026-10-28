# `truetype`

Reading the TrueType tables the rest of `lib/graphics` needs, and nothing else.

A TrueType/OpenType file is a table directory followed by tables. This module has no opinions:
it reports what the font says. Decisions about fallback, metrics and what to do when a glyph is
missing live in `font`.

Tables read: `head` (units per em, bounding box, the loca format), `maxp` (glyph count), `hhea`
and `hmtx` (advances), `cmap` (character to glyph), `name` (the PostScript and family names),
`OS/2` (weight and the descriptor's metrics), `loca` and `glyf` (outlines, including composite
glyphs). Everything else in the file is ignored.

`use truetype` -- or `use truetype.Name` to bring one item in bare.

## Example

```
use truetype

// Reading a TrueType font file. The parser's job is to turn 400 KB of binary into tables a
// renderer can ask questions of -- and, just as much, to refuse the files that are not what they
// claim. Those refusals are what a caller actually meets, so they are what this shows: every one
// below is built from a few bytes, with no font file involved.
//
// The other half -- parsing a real face, embedding it, and checking the text survives a round trip
// through a PDF reader -- is in `tests/run_pdf_tests.sh`, which takes a font from the system. A
// real face is 400 KB of binary that every clone would otherwise carry for ever.

// A four-byte big-endian tag, as a font file's header carries it.
fn tagged(a: int, b: int, c: int, d: int) -> [byte] {
    return [a as byte, b as byte, c as byte, d as byte]
}

fn refusal(r: Result<truetype.Face, string>) -> string {
    return switch r {
        Ok(f) { concat("unexpectedly parsed: ", str(f.glyph_count)) }
        Err(why) { why }
    }
}

fn main() {
    val empty: [byte] = []
    print(refusal(truetype.parse(empty)))

    // A valid sfnt version, and then nothing to read.
    print(refusal(truetype.parse(tagged(0, 1, 0, 0))))

    // `OTTO` is an OpenType font whose outlines are CFF rather than TrueType -- a different format
    // inside the same container, which is worth naming rather than failing obscurely later.
    print(refusal(truetype.parse(concat(tagged(79, 84, 84, 79), tagged(0, 0, 0, 0), tagged(0, 0, 0, 0)))))

    // `ttcf` is a collection holding several faces, which needs a face chosen before it is parsed.
    print(refusal(truetype.parse(concat(tagged(116, 116, 99, 102), tagged(0, 0, 0, 0), tagged(0, 0, 0, 0)))))

    // And something that is simply not a font.
    print(refusal(truetype.parse(concat(tagged(1, 2, 3, 4), tagged(0, 0, 0, 0), tagged(0, 0, 0, 0)))))
}
```

Output:

```
truetype: not a font (fewer than 12 bytes)
truetype: not a font (fewer than 12 bytes)
truetype: this is an OpenType/CFF font ('OTTO'); only TrueType outlines are read
truetype: this is a font collection ('ttcf'); extract a single face first
truetype: unrecognised sfnt version 0x01020304
```

### `Face`

```
pub struct Face {
    data: [byte]
    units_per_em: int
    glyph_count: int
    // `loca` holds u32 offsets rather than u16 halves.
    long_loca: bool
    // Table offsets, or -1 when the table is absent.
    head: int
    hhea: int
    hmtx: int
    maxp: int
    cmap: int
    name: int
    os2: int
    loca: int
    glyf: int
    num_h_metrics: int
    ascender: int
    descender: int
    line_gap: int
    x_min: int
    y_min: int
    x_max: int
    y_max: int
}
```

### `parse`

```
pub fn parse(data: [byte]) -> Result<Face, string>
```

Reads the table directory and the handful of fixed-position fields everything else needs.

An `Err` names what was wrong rather than returning a half-built face: a font this module cannot
read is a caller's problem to report, and a `Face` that lies about its own glyph count would
produce a PDF that readers reject for reasons nothing here could explain.

### `bbox`

```
pub fn bbox(f: Face) -> Rect
```

The font's bounding box, in font units.

### `glyph_for`

```
pub fn glyph_for(f: Face, codepoint: int) -> Result<Option<int>, string>
```

The glyph for a code point, or `None` when the font has none.

Only `cmap` subtable formats **4** and **12** are read: format 4 is the basic-plane segment
mapping every font has, and 12 is the full-Unicode one any font with astral characters has.
Formats 0, 2, 6 and 13 belong to fonts old enough not to matter here, and an unrecognised format
is reported rather than silently answering "no glyph for anything".

### `advance_of`

```
pub fn advance_of(f: Face, glyph: int) -> Result<int, string>
```

The glyph's advance width, in font units.

`hmtx` holds `num_h_metrics` entries and then leaves the rest implicit: every glyph past that
point has the *last* listed advance, which is how a monospaced font stores one width for
thousands of glyphs. Reading past the array instead of clamping is the classic bug here, and it
returns whatever bytes follow.

### `name_of`

```
pub fn name_of(f: Face, name_id: int) -> Result<Option<string>, string>
```

A name-table string, by name id: 1 is the family, 4 the full name, 6 the PostScript name.

Prefers a Windows record, whose strings are UTF-16BE, and falls back to a Macintosh record, whose
strings are single-byte. Both are decoded to a Ziman string; a record in any other encoding is
skipped rather than mangled.

### `weight_of`

```
pub fn weight_of(f: Face) -> Result<int, string>
```

`OS/2`'s weight class (400 regular, 700 bold), or 400 when the table is absent.

### `cap_height_of`

```
pub fn cap_height_of(f: Face) -> Result<int, string>
```

`OS/2`'s cap height, or an estimate when the table is too old to carry one (version 0 and 1 stop
before this field).

### `is_italic`

```
pub fn is_italic(f: Face) -> Result<bool, string>
```

## Glyph outlines

### `glyph_outline`

```
pub fn glyph_outline(f: Face, glyph: int) -> Result<shape.Path, string>
```

The glyph's outline in **font units**, y up, as a path of quadratic segments.

Implemented in this phase even though PDF embedding does not need it: it is what the raster
backend rasterises (phase 5), and parsing `glyf` while this reader is being written and tested is
far cheaper than coming back for it.

**Composite glyphs are followed**, because that is how most accented letters are built -- `ä` is
a reference to `a` plus a reference to the diaeresis, with an offset. Skipping them would make
accented text silently blank, which is the kind of defect that looks like a font problem rather
than a parser one. Nesting is bounded (see `max_component_depth`) because a font can, in
principle, reference itself.
