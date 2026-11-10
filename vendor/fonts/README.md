# Vendored font

`Caladea-Regular.ttf`, copyright (c) 2012 Huerta Tipografia, **licensed under the Apache License,
Version 2.0** — the text is in `LICENSE` beside it; keep both with any copy. The licence is not
taken on trust from a distribution's packaging: the font's own `name` table carries
`Licensed under the Apache License, Version 2.0` and
`http://www.apache.org/licenses/LICENSE-2.0`, which is what was checked.

## Why a font is in this repository at all

**So that text rendering has a gate.** `lib/graphics` can rasterise text, and until this was
vendored nothing checked that it did so correctly: `tests/lang/graphics_raster.zm` covers what the
text path *refuses* when no font is loaded, because a fixture cannot portably rely on a system font
being present, let alone on which version of it. So every change to the glyph path through F270 —
four of them — was verified by rendering the same scene before and after and comparing a digest,
which catches the author's mistakes and nothing else. Nothing stopped a regression the next day.

`tests/lang/graphics_text.zm` is that gate, and it needs a font whose bytes never change.

## Why this font

- **Apache 2.0, with no Reserved Font Name.** The SIL Open Font Licence, which most freely
  available faces use, reserves the family name and forbids a modified copy from keeping it — which
  would matter if this were ever subsetted to save space. Apache 2.0 has no such clause.
- **Its `cmap` is format 4.** That is deliberate, not incidental. The system face these tests were
  developed against uses format 12, so `truetype.lookup4` — the other half of the character-map
  code, and the one most fonts in the wild use — had no coverage at all.
- **59 KB**, with TrueType outlines (`glyf`/`loca`), real kerning tables this library ignores, and
  full Latin coverage. Small enough to commit without a subsetting step, which would have made the
  file a derivative work and raised a question this way avoids entirely.

## What it is not for

Not for examples, documentation, or anything a reader might take as a recommendation, and not a
default the library reaches for: `lib/graphics` has no notion of a system font and this does not
give it one. It exists so one fixture can assert pixels.
