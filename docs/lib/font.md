# `font`

The font model: which faces a document may use, and what they measure.

`truetype` reports what a font file says; this module decides what a *caller* needs -- metrics
scaled to a size, whether a face can draw a string at all, and the choice between an embedded
face and one of the fourteen standard fonts a reader already has.

`StandardFont` lives here rather than in `pdf` so that nothing is circular: `pdf` needs this
module to embed a face, and this module must not need `pdf` to name a built-in one.

`use font` -- or `use font.Name` to bring one item in bare.

### `StandardFont`

```
pub enum StandardFont {
    Helvetica
    HelveticaBold
    HelveticaOblique
    HelveticaBoldOblique
    Times
    TimesBold
    TimesItalic
    TimesBoldItalic
    Courier
    CourierBold
    CourierOblique
    CourierBoldOblique
    Symbol
    ZapfDingbats
}
```

The fourteen fonts every conforming PDF reader supplies, so none of them is embedded or needs a
font file. Latin-1 only, which is what `pdf.win_ansi` refuses past.

### `base_font_name`

```
pub fn base_font_name(f: StandardFont) -> string
```

### `FontRef`

```
pub enum FontRef {
    Standard(StandardFont)
    Embedded(Face)
}
```

### `standard`

```
pub fn standard(f: StandardFont) -> FontRef
```

### `load`

```
pub fn load(data: [byte]) -> Result<FontRef, string>
```

A TrueType face from its bytes. Nothing here reads a file: fonts arrive as `[byte]`, which keeps
the whole library pure and leaves loading to `fs`.

### `covers`

```
pub fn covers(f: FontRef, text: string) -> Result<Option<int>, string>
```

The first code point `f` cannot draw, or `None` when it can draw all of `text`.

**Answers *which* character is missing, not whether any is**, because a caller choosing between
faces or producing an error needs to name it, and a bool would make every caller scan again to
find it.

### `advance`

```
pub fn advance(f: FontRef, text: string, size: float) -> Result<float, string>
```

How wide `text` is at `size`, in the same units as `size`.

**Measurement needs real metrics, so a proportional standard font is an `Err`.** Courier is
monospaced at exactly 600/1000 em per glyph and is answered exactly; Helvetica and Times would
need their published AFM width tables, about three thousand numbers for something an embedded
face answers properly. Rather than ship a half-table or guess, this says so and names the fix.

### `ascent`

```
pub fn ascent(f: FontRef, size: float) -> float
```

How far above the baseline the face reaches, at `size`. The standard fonts use the conventional
0.75/0.25 split, which is what a reader's own metrics give within a percent or two.

### `descent`

```
pub fn descent(f: FontRef, size: float) -> float
```

Below the baseline, as a positive number -- the opposite sign to `hhea`'s own descender, which is
negative. Returning the font's sign would make every caller remember which convention it is.

### `line_height`

```
pub fn line_height(f: FontRef, size: float) -> float
```

### `glyphs_of`

```
pub fn glyphs_of(f: Face, text: string) -> Result<[int], string>
```

The glyphs `text` becomes, in order, with the cluster mapping a `scene.TextRun` carries.

One glyph per code point, left to right, advancing by `hmtx` -- correct for Latin, Greek,
Cyrillic, CJK and Turkish, and **wrong for Arabic, Hebrew and Indic**, which need contextual
joining, reordering and ligature substitution from `GSUB`/`GPOS`. That is not approximated here;
see the phase document. When shaping arrives it produces this same shape -- positioned glyphs
plus clusters -- which is why this returns both.

### `codepoints_of`

```
pub fn codepoints_of(text: string) -> [int]
```

The code points of `text`, which `/ToUnicode` needs alongside the glyphs.

### `codepoint_of`

```
pub fn codepoint_of(ch: string) -> int
```

### `hex4`

```
pub fn hex4(cp: int) -> string
```
