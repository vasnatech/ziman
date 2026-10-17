# Phase 3 — fonts: TrueType parsing, metrics, and embedding

What makes the library usable for real text. Parses a TrueType font, measures text with it, and
embeds it in a PDF so that any Unicode the font covers renders -- and stays selectable and
searchable.

Read `README.md` first. This phase supersedes the Latin-1 limitation phase 2 shipped with.

## Scope

`truetype` (the byte-level table reader) and `font` (the model over it). `pdf` gains CID font
embedding. Split because `truetype` is a parser with no opinions and `font` is where decisions about
fallback, metrics and shaping live.

## `truetype` — the table reader

A TrueType/OpenType file is a table directory followed by tables. This module reads the tables the
rest of the library needs and ignores the rest.

```ziman
pub struct Face { data: [byte]  tables: Map<string, Slice>  units_per_em: int  ... }

pub fn parse(data: [byte]) -> Result<Face, string>

pub fn glyph_for(f: Face, codepoint: int) -> Option<int>     // from `cmap`
pub fn advance_of(f: Face, glyph: int) -> int                // from `hmtx`, in font units
pub fn glyph_count(f: Face) -> int                           // from `maxp`
pub fn bbox(f: Face) -> geom.Rect                            // from `head`, font units
pub fn name_of(f: Face, id: int) -> Option<string>           // from `name`: family, PostScript name
pub fn is_fixed_pitch(f: Face) -> bool
pub fn glyph_outline(f: Face, glyph: int) -> Result<shape.Path, string>   // from `glyf`
```

**Tables parsed:** `head` (units per em, bbox, index-to-loc format), `maxp` (glyph count), `hmtx` +
`hhea` (advances), `cmap` (character to glyph), `name` (PostScript and family names), `OS/2` (weight,
flags, cap height, for the PDF descriptor), `loca` + `glyf` (outlines).

**`cmap` subtables: formats 4 and 12, and nothing else.** Format 4 is the BMP segment mapping every
font has; format 12 is the full-Unicode one any font with astral characters has. Formats 0, 2, 6 and
13 exist in fonts old enough that they do not matter here. An unrecognised format is an `Err` naming
it rather than a silent "no glyph for anything".

**`glyph_outline` is needed in this phase even though PDF embedding does not use it** -- it is what
phase 5 rasterises, and parsing `glyf` while the table reader is being written and tested is far
cheaper than coming back for it. It handles simple glyphs and **composite** glyphs (a glyph made of
transformed references to others, which is how `ä` is usually built); skipping composites would make
accented text silently blank in phase 5.

Quadratic curves: TrueType outlines are quadratic, and `shape.Segment` has `QuadTo`, so they are
stored as-is and the PDF backend's existing elevation handles them. No conversion here.

## `font` — the model

```ziman
pub enum FontRef {
    Standard(pdf.StandardFont)     // phase 2's fourteen, still the cheapest choice
    Embedded(Face)
}

pub fn load(data: [byte]) -> Result<FontRef, string>

// Metrics, in ems scaled by size -- what a layout needs
pub fn advance(f: FontRef, text: string, size: float) -> Result<float, string>
pub fn ascent(f: FontRef, size: float) -> float
pub fn descent(f: FontRef, size: float) -> float
pub fn line_height(f: FontRef, size: float) -> float
pub fn covers(f: FontRef, text: string) -> Option<int>   // Some(codepoint) for the first it cannot draw
```

**`covers` answers which character is missing, not whether any is.** A caller choosing between
fonts, or producing an error, needs to name the character; a bool would make every caller re-scan to
find it. This is the same reasoning as `Option`-returning lookups elsewhere in the library.

**`/ToUnicode` is built from `TextRun.clusters`, or from the trivial mapping when it is empty.**
Phase 2 added that field for this: the CMap has to state which characters each glyph came from, and
with one glyph per codepoint the backend can derive it. The derivation is what stops working under
shaping, which is why the field exists rather than the mapping being assumed.

**No shaping, and that is a deliberate limit with a stated cost.** Text is mapped codepoint to
glyph, left to right, advancing by `hmtx`. That is correct for Latin, Greek, Cyrillic, CJK and
Turkish, and **wrong for Arabic, Hebrew and Indic scripts**, which need contextual joining,
reordering and ligature substitution from `GSUB`/`GPOS`. Those are not approximated here: `covers`
reports what the font lacks, but nothing reports that a script needs shaping, so Arabic would render
as disconnected letters in logical order. If that becomes a requirement it is its own phase, not a
patch to this one -- and the shape that phase should take is already visible in two other libraries:
**Skia pushes shaping out entirely** (`SkTextBlob` holds pre-shaped glyph ids and positions, with
HarfBuzz doing the shaping), and cairo's `show_text_glyphs` takes the same three-part input. So the
boundary is: something produces positioned glyphs plus clusters, and this library draws them. That
is why `TextRun` carries `clusters` and why `font` exposes `glyph_for`/`advance_of` separately from
`advance` -- a shaper needs the per-glyph primitives, not the string-level convenience.

## PDF embedding — `CIDFontType2` with Identity-H

The tier-2 plan from the earlier PDF discussion, now as part of the backend:

- A `/Type0` font with `/Encoding /Identity-H`, whose `/DescendantFonts` is a `/CIDFontType2`.
- `/CIDToGIDMap /Identity`, so a CID *is* a glyph id and text is written as two bytes per glyph.
- `/W` carrying the advances of the glyphs actually used, in 1000ths of an em.
- `/FontDescriptor` with flags, bbox, ascent, descent, cap height, stem width, from `head`/`OS/2`.
- `/FontFile2` is the **whole font file**, Flate-compressed -- no subsetting.
- A `/ToUnicode` CMap so that text extraction and copy-paste give back the original characters.

**No subsetting, and the cost is explicit:** the full face goes in each document, around 400 KB for
LiberationSans, roughly 160 KB after deflate. For an export that is fine. Subsetting means rewriting
`glyf` and `loca` and chasing composite-glyph dependencies, and its failure mode is a file that one
reader opens and another rejects. It sits behind this same API as a later optimisation -- the public
surface does not change when it arrives.

**`/ToUnicode` is not optional.** Without it the text draws correctly and extracts as mojibake, which
is exactly the failure a reader would not notice until someone copies a medication name out of a
record. It is also what phase 2's `pdftotext` check will be verifying, so the test already exists.

**Text written as Identity-H is two bytes per glyph in a hex string**, so `pdfobj`'s `Bytes` variant
is what carries it -- phase 2 built that for this reason.

## What phase 3 does *not* do

Shaping (above). Subsetting (above). Type1/CFF/OpenType-CFF fonts -- `/FontFile3` and CFF charstring
parsing are a different format inside the same container, and TrueType covers the fonts this project
has. Vertical writing. Font fallback chains (a caller picks a font; the library does not go looking
for another one).

## Verification

- **Against the fonts already in the repo**: `LiberationSans-Regular.ttf` and `-Bold.ttf` are in
  `health-record-backend-spring/src/main/resources/fonts/`, under SIL OFL. Parse both and assert
  `units_per_em`, glyph count, and the PostScript name from `name`.
- **`cmap` correctness by known mapping**: a handful of codepoints whose glyph ids can be read
  independently with `fc-query`/`ttx` if present, and otherwise pinned once and checked for drift.
- **Advance sanity**: `advance("iii", 12.0) < advance("WWW", 12.0)` in a proportional face, and
  *equal* in Courier. That one assertion catches a swapped `hmtx` index, which is the likeliest bug.
- **Composite glyphs**: an accented glyph's outline must have more contours than the base letter's.
  If it has the same, composites are being skipped.
- **Round trip through PDF**: render text containing `ş`, `İ` and a Cyrillic word, then `pdftotext`
  the result and assert the extracted text equals the input. This is the single check that proves
  the CMap, the `/ToUnicode` and the encoding all agree; everything else can be right while this
  fails. *Built and passing:* `şeker İstanbul — Привет` comes back byte-identical, and `pdffonts`
  reports the face as `CID TrueType / Identity-H / emb yes / sub no / uni yes` -- which also makes
  the no-subsetting decision visible rather than merely documented.
- **The phase-2 error goes away**: the fixture that asserted `Err` on an out-of-encoding character
  now asserts it renders, with the standard-font case still erroring.

## Build changes

Two modules registered. No native code -- TrueType parsing is `[byte]` reads and ordinary
arithmetic; `bits` turned out not to be needed, since big-endian assembly is `hi * 256 + lo`.

*Built 2026-10-16.* Four decisions the implementation settled, none of them in this document's
first draft:

**Test fonts come from the system, via `fc-match`, and neither option this document offered was
taken.** Not the sibling checkout (fragile) and not a copy in the repository (400 KB of binary that
every clone carries forever to test a parser). `tests/run_pdf_tests.sh` asks fontconfig for
Liberation Sans and skips when there is none, the same shape as the openssl and Poppler guards. The
cost, stated: a machine without fontconfig tests the error paths only -- which is what
`tests/lang/graphics_font.zm` covers, and it is the half that needs no font.

**`StandardFont` moved from `pdf` into `font`**, because `pdf` needs `font` in order to embed a face
and `font` must not need `pdf` in order to name a built-in one. `Options.fonts` is now
`[font.FontRef]`, so standard and embedded faces mix freely in one document.

**`font.advance` refuses for a proportional standard font**, rather than shipping a width table or
guessing. Courier is monospaced at exactly 600/1000 em and is answered exactly; Helvetica and Times
would need their published AFM metrics, about three thousand numbers for something an embedded face
answers properly. The error names Courier and embedding as the two ways forward.

**`/ToUnicode` needs the reverse mapping, which a font does not have.** `cmap` maps characters to
glyphs, and the CMap needs the opposite, so it is found by scanning -- over Latin, Greek, Cyrillic,
the Latin Extended Additional block and punctuation, rather than all of Unicode, because the scan
runs per glyph. A glyph reachable only from a code point outside those ranges gets no `/ToUnicode`
entry and so extracts as nothing; the ranges are listed in one place in `pdf.zm` for that reason.
