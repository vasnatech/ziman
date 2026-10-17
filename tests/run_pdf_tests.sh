#!/usr/bin/env bash
# Checks `lib/graphics`'s PDF output against an **independent** implementation -- Poppler's
# `pdfinfo` and `pdftotext`. Not part of `make test`, because it needs those binaries; run by
# `make pdf-if-available`, which says so and passes when they are missing.
#
# Why this exists separately from tests/lang/graphics_pdf.zm: that fixture checks the structure with
# `pdfobj.check`, which is this project's own reader agreeing with this project's own writer. A
# malformed file can be perfectly self-consistent. Poppler has no such sympathy, and it is what
# catches the class of defect where one reader opens a file and another rejects it.
#
# Usage: tests/run_pdf_tests.sh [ziman-binary]
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=$(cd "$(dirname "${1:-./ziman}")" && pwd)/$(basename "${1:-./ziman}")
if ! command -v pdfinfo >/dev/null || ! command -v pdftotext >/dev/null; then
    echo "SKIP: no pdfinfo/pdftotext (poppler-utils)"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1 -- $2"; fails=$((fails + 1)); }

# A document whose text is axis-aligned, so extraction is comparable.
#
# **Rotated text is deliberately not asserted on**: `pdftotext` splits a rotated baseline into
# fragments on separate lines ("rotated text" comes back as "rota / ted t / e / xt"), which is its
# layout analysis working and not a defect in the file. Checking it would be testing `pdftotext`.
cat > "$WORK/make.zm" <<'ZM'
use font
use fs
use geom
use paint
use pdf
use scene
use shape
fn main() {
    val black = paint.solid(paint.black())
    val target = os.args()[0]
    val pages = [
        scene.with_ops(scene.scene_of(200.0, 100.0), [
            scene.fill(shape.rect_path(geom.rect_of(10.0, 60.0, 50.0, 30.0)), black),
            scene.text_of("first page text", geom.point_of(10.0, 20.0), 12.0, 0, black)
        ]),
        scene.with_ops(scene.scene_of(300.0, 150.0), [
            scene.text_of("second page — em dash and “quotes”", geom.point_of(10.0, 20.0), 11.0, 1, black)
        ])
    ]
    val o = with pdf.options() {
        fonts: [font.standard(font.StandardFont.Helvetica), font.standard(font.StandardFont.TimesBold)],
        title: "Poppler check",
        author: "lib/graphics"
    }
    val _n = unwrap(fs.write_bytes(target, unwrap(pdf.render_pages(pages, o))))
}
use os
ZM

if ! "$BIN" "$WORK/make.zm" -- "$WORK/doc.pdf" 2>"$WORK/err"; then
    fail "the document is produced" "$(head -3 "$WORK/err")"
    echo "$fails failure(s)"
    exit 1
fi
pass "the document is produced"

info=$(pdfinfo "$WORK/doc.pdf" 2>&1)
# Per-page sizes need -l; without it pdfinfo reports only the first page's.
sizes=$(pdfinfo -l 2 "$WORK/doc.pdf" 2>&1)
if echo "$info" | grep -q "^Pages: *2"; then pass "pdfinfo reads two pages"; else fail "pdfinfo reads two pages" "$info"; fi
if echo "$info" | grep -q "Poppler check"; then pass "the title reaches the info dictionary"; else fail "the title reaches the info dictionary" "$info"; fi
# Page 1 is 200x100 and page 2 is 300x150, so a per-page MediaBox is being written rather than one
# size for the document.
if echo "$sizes" | grep -qE "Page +1 size: *200 x 100"; then pass "page 1 is 200x100"; else fail "page 1 is 200x100" "$sizes"; fi
if echo "$sizes" | grep -qE "Page +2 size: *300 x 150"; then pass "page 2 is 300x150"; else fail "page 2 is 300x150" "$sizes"; fi

text=$(pdftotext "$WORK/doc.pdf" - 2>&1)
# Extractable text at all is the point: it proves the run went out as text and not as paths.
if echo "$text" | grep -q "first page text"; then pass "page 1 text extracts"; else fail "page 1 text extracts" "$text"; fi
if echo "$text" | grep -q "second page"; then pass "page 2 text extracts"; else fail "page 2 text extracts" "$text"; fi
# The CP1252 block above Latin-1, which is where WinAnsi differs from ISO-8859-1.
if echo "$text" | grep -q "em dash"; then pass "the em dash survives WinAnsi"; else fail "the em dash survives WinAnsi" "$text"; fi
if echo "$text" | grep -q "quotes"; then pass "curly quotes survive WinAnsi"; else fail "curly quotes survive WinAnsi" "$text"; fi

# ---- an embedded TrueType face, which is phase 3's whole point
#
# The font comes from the system via fc-match rather than from the repository: Liberation Sans is
# 400 KB of binary, and checking it in to test a parser is a cost every clone pays forever. If
# fontconfig finds nothing, this part skips.
FONT=$(fc-match -f '%{file}' 'Liberation Sans' 2>/dev/null || true)
if [ -z "$FONT" ] || [ ! -r "$FONT" ] || ! command -v pdffonts >/dev/null; then
    echo "SKIP: no system TrueType font via fc-match, or no pdffonts"
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
    exit $?
fi

cat > "$WORK/embed.zm" <<'ZM'
use font
use fs
use geom
use os
use paint
use pdf
use scene
fn main() {
    val face = unwrap(font.load(unwrap(fs.read_bytes(os.args()[0]))))
    val s = scene.with_ops(scene.scene_of(300.0, 100.0), [
        scene.text_of("şeker İstanbul — Привет", geom.point_of(15.0, 50.0), 14.0, 0, paint.solid(paint.black()))
    ])
    val _n = unwrap(fs.write_bytes(os.args()[1], unwrap(pdf.render(s, with pdf.options() { fonts: [face] }))))
}
ZM

if ! "$BIN" "$WORK/embed.zm" -- "$FONT" "$WORK/embed.pdf" 2>"$WORK/err2"; then
    fail "the embedded-font document is produced" "$(head -3 "$WORK/err2")"
else
    pass "the embedded-font document is produced"
    fonts=$(pdffonts "$WORK/embed.pdf" 2>&1)
    # The three columns that matter: it is a CID font, Identity-H, and actually embedded.
    if echo "$fonts" | grep -q "CID TrueType"; then pass "the face is a CID TrueType"; else fail "the face is a CID TrueType" "$fonts"; fi
    if echo "$fonts" | grep -q "Identity-H"; then pass "the encoding is Identity-H"; else fail "the encoding is Identity-H" "$fonts"; fi
    # The `emb` column sits immediately after the encoding, so match on that rather than counting
    # fields from the end -- a font name containing a space shifts every column.
    if echo "$fonts" | grep -qE "Identity-H +yes"; then pass "the font file is embedded"; else fail "the font file is embedded" "$fonts"; fi
    # `sub no` is the design's decision made visible: the whole face goes in, unsubsetted.
    if echo "$fonts" | grep -qE "Identity-H +yes +no"; then pass "the face is not subsetted, as designed"; else fail "the face is not subsetted, as designed" "$fonts"; fi

    # **The check that proves the whole chain.** Encoding, CIDToGIDMap and /ToUnicode all have to
    # agree for this to come back: Turkish ş and İ, an em dash, and Cyrillic.
    got=$(pdftotext "$WORK/embed.pdf" - 2>&1 | tr -d '\n\f')
    want="şeker İstanbul — Привет"
    if [ "$got" = "$want" ]; then
        pass "Unicode text round-trips through /ToUnicode"
    else
        fail "Unicode text round-trips through /ToUnicode" "wanted '$want', got '$got'"
    fi
fi

echo "$fails failure(s)"
[ "$fails" -eq 0 ]
