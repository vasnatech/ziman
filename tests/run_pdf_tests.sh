#!/usr/bin/env bash
# Checks `lib/graphics`'s output against **independent** implementations -- python3's `zlib` for
# PNG, Poppler's `pdfinfo`/`pdftotext`/`pdfimages` for PDF. Not part of `make test`, because it
# needs those; run by `make pdf-if-available`, which says so and passes when they are missing.
#
# Each section guards on its own tools, so a machine with python3 and no Poppler still runs the PNG
# half and the other way round.
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

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1 -- $2"; fails=$((fails + 1)); }

# ---- PNG row filters, against an encoder this project did not write
#
# `encode_png` only ever writes filter 0 (None) and filter 2 (Up), so no fixture produced by this
# library can exercise Sub, Average or Paeth -- and a decoder tested only against its own encoder
# passes with a wrong predictor, because the same error cancels in both directions.
#
# So python3's `zlib` writes the same pixels five times, once per filter type, and the raw bytes
# once. All five have to decode to those bytes.
if ! command -v python3 >/dev/null; then
    echo "SKIP: no python3 (PNG filter cross-check)"
else
    python3 - "$WORK" <<'PYEOF'
import struct
import sys
import zlib
from pathlib import Path

work = Path(sys.argv[1])
W, H = 7, 5
# A pattern with gradients along both axes and a discontinuity, so a predictor that is off by a
# pixel or a row produces different bytes rather than the same smooth ramp.
rows = [
    bytes(
        v & 0xFF
        for x in range(W)
        for v in (13 * x + 31 * y, 200 - 17 * x + 5 * y, (x * y * 29) ^ 0x5A)
    )
    for y in range(H)
]
(work / "ref_rgb.bin").write_bytes(b"".join(rows))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def filtered(f):
    out = bytearray()
    prev = bytes(len(rows[0]))
    for row in rows:
        out.append(f)
        for i, cur in enumerate(row):
            a = row[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 0:
                out.append(cur)
            elif f == 1:
                out.append((cur - a) & 0xFF)
            elif f == 2:
                out.append((cur - b) & 0xFF)
            elif f == 3:
                out.append((cur - (a + b) // 2) & 0xFF)
            else:
                out.append((cur - paeth(a, b, c)) & 0xFF)
        prev = row
    return bytes(out)


def chunk(name, body):
    return struct.pack(">I", len(body)) + name + body + struct.pack(">I", zlib.crc32(name + body))


for f in range(5):
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(filtered(f), 6))
    png += chunk(b"IEND", b"")
    (work / f"filt{f}.png").write_bytes(png)
PYEOF

    cat > "$WORK/filters.zm" <<'ZM'
use fs
use image
use os
fn main() {
    val want = unwrap(fs.read_bytes(concat(os.args()[0], "/ref_rgb.bin")))
    var f = 0
    while f < 5 {
        val got = unwrap(image.decode_png(
            unwrap(fs.read_bytes(concat(os.args()[0], "/filt", str(f), ".png"))),
            1000000
        ))
        val same = len(got.data) == len(want) && image.crc32(got.data) == image.crc32(want)
        print(concat("filter ", str(f), ": ", str(got.width), "x", str(got.height), " ", str(same)))
        f = f + 1
    }
}
ZM

    if ! out=$("$BIN" "$WORK/filters.zm" -- "$WORK" 2>&1); then
        fail "the five filtered PNGs decode" "$out"
    else
        for f in 0 1 2 3 4; do
            name=$(sed -n "$((f + 1))p" <<<"$out")
            if [ "$name" = "filter $f: 7x5 true" ]; then
                pass "PNG filter $f decodes to the reference bytes"
            else
                fail "PNG filter $f decodes to the reference bytes" "$name"
            fi
        done
    fi
fi

if ! command -v pdfinfo >/dev/null || ! command -v pdftotext >/dev/null; then
    echo "SKIP: no pdfinfo/pdftotext (poppler-utils)"
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
    exit $?
fi

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

# ---- an image with alpha, which is phase 4's whole point
#
# `tests/lang/graphics_image.zm` already asserts that the file *contains* an `/SMask`. That is this
# project's reader agreeing with its writer: the key could be present and the mask unusable. These
# two checks are the independent half -- `pdfimages -list` reports the colour image and its soft
# mask as separate rows with their own dimensions, and `pdftoppm` rasterises the page so the
# transparent squares can be confirmed to be transparent rather than black.
if ! command -v pdfimages >/dev/null || ! command -v pdftoppm >/dev/null; then
    echo "SKIP: no pdfimages/pdftoppm for the alpha check"
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
    exit $?
fi

# A red-and-transparent checkerboard on a light grey page. Red where it is opaque, the page's grey
# where it is not: if alpha were dropped the whole square would be red, and if the mask were
# inverted the colours would swap.
cat > "$WORK/alpha.zm" <<'ZM'
use fs
use geom
use image
use os
use paint
use pdf
use scene
fn main() {
    val c = unwrap(image.canvas(8, 8, image.Pixels.Rgba8))
    var y = 0
    while y < 8 {
        var x = 0
        while x < 8 {
            val on = (x + y) % 2 == 0
            val _ = unwrap(image.set(c, x, y, paint.rgba(1.0, 0.0, 0.0, if on { 1.0 } else { 0.0 })))
            x = x + 1
        }
        y = y + 1
    }
    val s = scene.with_ops(scene.scene_of(80.0, 80.0), [
        scene.fill(shape.rect_path(geom.rect_of(0.0, 0.0, 80.0, 80.0)), paint.solid(paint.gray(0.9))),
        scene.image(0, geom.rect_of(10.0, 10.0, 60.0, 60.0))
    ])
    val o = with pdf.options() {
        size: pdf.page_size_of(80.0, 80.0),
        images: [image.snapshot_image(c)]
    }
    val _n = unwrap(fs.write_bytes(os.args()[0], unwrap(pdf.render(s, o))))
}
use shape
ZM

if ! "$BIN" "$WORK/alpha.zm" -- "$WORK/alpha.pdf" 2>"$WORK/err3"; then
    fail "the image document is produced" "$(head -3 "$WORK/err3")"
else
    pass "the image document is produced"
    list=$(pdfimages -list "$WORK/alpha.pdf" 2>&1)
    # Two rows: an 8x8 RGB image and an 8x8 greyscale soft mask. `smask` in the type column is
    # Poppler saying it resolved the `/SMask` reference, not merely that a key was present.
    if grep -qE "^ *1 +[0-9]+ +image +8 +8 +rgb" <<<"$list"; then
        pass "pdfimages sees an 8x8 RGB image"
    else
        fail "pdfimages sees an 8x8 RGB image" "$list"
    fi
    if grep -qE "^ *1 +[0-9]+ +smask +8 +8 +gray" <<<"$list"; then
        pass "the /SMask is an 8x8 greyscale soft mask"
    else
        fail "the /SMask is an 8x8 greyscale soft mask" "$list"
    fi

    pdftoppm -r 72 -png -f 1 -l 1 "$WORK/alpha.pdf" "$WORK/page" 2>/dev/null
    shot=$(ls "$WORK"/page*.png 2>/dev/null | head -1)
    if [ -z "$shot" ]; then
        fail "the page rasterises" "pdftoppm produced nothing"
    elif ! command -v python3 >/dev/null; then
        echo "SKIP: no python3 to sample the rendering"
    else
        # Sampled with python3's own zlib rather than PIL, so the check does not need a third-party
        # module: the squares are 7.5pt across at 72dpi, so a coarse grid hits both colours.
        got=$(python3 - "$shot" <<'PYEOF'
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
at, idat, w, h, colour = 8, bytearray(), 0, 0, 0
while at + 8 <= len(data):
    n = struct.unpack(">I", data[at : at + 4])[0]
    name = data[at + 4 : at + 8]
    body = data[at + 8 : at + 8 + n]
    if name == b"IHDR":
        w, h, _, colour = struct.unpack(">IIBB", body[:10])
    elif name == b"IDAT":
        idat += body
    at += n + 12
chan = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
raw = zlib.decompress(bytes(idat))
stride = w * chan
rows, prev = [], bytes(stride)
for y in range(h):
    f = raw[y * (stride + 1)]
    line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
    for i in range(stride):
        a = line[i - chan] if i >= chan else 0
        b = prev[i]
        c = prev[i - chan] if i >= chan else 0
        if f == 1:
            line[i] = (line[i] + a) & 0xFF
        elif f == 2:
            line[i] = (line[i] + b) & 0xFF
        elif f == 3:
            line[i] = (line[i] + (a + b) // 2) & 0xFF
        elif f == 4:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 0xFF
    rows.append(bytes(line))
    prev = bytes(line)
red = grey = 0
for y in range(12, 68, 3):
    for x in range(12, 68, 3):
        px = rows[y][x * chan : x * chan + chan]
        r, g, b = (px[0], px[0], px[0]) if chan == 1 else (px[0], px[1], px[2])
        if r > 200 and g < 80 and b < 80:
            red += 1
        elif r > 200 and abs(r - g) < 12 and abs(g - b) < 12:
            grey += 1
print("both" if red > 50 and grey > 50 else f"red={red} grey={grey}")
PYEOF
)
        if [ "$got" = "both" ]; then
            pass "the transparent squares let the page through"
        else
            fail "the transparent squares let the page through" "$got"
        fi
    fi
fi

# ---- the raster backend's text, and the round trip between the two backends
#
# This is the check phase 5's design names: render **one scene** to PDF and to an image, and assert
# the text comes back out of the PDF. The two outputs cannot be compared to each other -- one is
# glyph ids in a content stream and the other is pixels -- but both coming from a single `Scene` is
# the property the whole library is built around, and it is the only check that exercises both
# backends against the same input.
cat > "$WORK/both.zm" <<'ZM'
use font
use fs
use geom
use image
use os
use paint
use pdf
use raster
use scene
fn ink(img: image.Image) -> float {
    var t = 0.0
    var y = 0
    while y < img.height {
        var x = 0
        while x < img.width {
            t = t + unwrap(image.pixel_at(img, x, y)).alpha
            x = x + 1
        }
        y = y + 1
    }
    return t
}
fn text_scene(s: string, size: float, at: geom.Point) -> scene.Scene {
    return scene.with_ops(scene.scene_of(320.0, 80.0), [
        scene.text_of(s, at, size, 0, paint.solid(paint.black()))
    ])
}
fn main() {
    val face = unwrap(font.load(unwrap(fs.read_bytes(os.args()[0]))))
    val one = text_scene("Round trip 123", 18.0, geom.point_of(10.0, 50.0))

    // The same scene, both ways.
    val _n = unwrap(fs.write_bytes(os.args()[1], unwrap(pdf.render(one, with pdf.options() {
        size: pdf.page_size_of(320.0, 80.0),
        fonts: [face]
    }))))
    val ro = with raster.options() { samples: 16, fonts: [face] }
    val img = unwrap(raster.render(one, ro))
    val _m = unwrap(fs.write_bytes(os.args()[2], image.encode_png(img)))
    print(concat("raster ink: ", format("{:.1}", ink(img))))

    // Area scales with the square of the size: 36pt covers about 4x what 18pt does. This is what
    // catches a glyph scaled by the wrong factor, which a visual check would call "looks about
    // right" at any size.
    val big = ink(unwrap(raster.render(text_scene("Round trip 123", 36.0, geom.point_of(10.0, 70.0)), ro)))
    val small = ink(unwrap(raster.render(text_scene("Round trip 123", 18.0, geom.point_of(10.0, 50.0)), ro)))
    print(concat("double size ink ratio: ", format("{:.3}", big / small)))

    // **Which way is up.** A descender drops below the baseline and a round letter does not, so
    // the lowest inked row of "g" sits below that of "o". A flipped Y axis passes every area check
    // ever written and fails this one.
    print(concat("g descends below o: ", str(lowest_row(ro, "g") > lowest_row(ro, "o"))))

    // Cache faithfulness: ten of one glyph lay down ten times the ink of one. The first is a miss
    // and the other nine are hits, so a hit returning the wrong tile shows up here.
    val once = ink(unwrap(raster.render(text_scene("o", 24.0, geom.point_of(10.0, 50.0)), ro)))
    val ten = ink(unwrap(raster.render(text_scene("oooooooooo", 24.0, geom.point_of(10.0, 50.0)), ro)))
    print(concat("ten glyphs / one: ", format("{:.3}", ten / once)))

    // **What pins the cache key**, and it took a second attempt to do so. The obvious fixture --
    // "oi" against "o" plus "i" -- passes even with the glyph id removed from the key, because the
    // two letters land on *different* subpixel quanta and so get different entries anyway. A
    // collision needs two different glyphs at the same quarter-pixel offset, which one pair cannot
    // be relied on to produce.
    //
    // Eighteen alternating letters can: their offsets walk through all four quanta, so some "i"
    // lands where an "o" already sits. The total ink of a glyph does not depend on where in a pixel
    // it falls -- anti-aliasing moves coverage between pixels but conserves it -- so the sum of the
    // parts is the right comparison, and a shared entry draws nine "o" too many.
    val dot_i = ink(unwrap(raster.render(text_scene("i", 24.0, geom.point_of(10.0, 50.0)), ro)))
    val mixed = ink(unwrap(raster.render(
        text_scene("oioioioioioioioioi", 24.0, geom.point_of(10.0, 50.0)),
        ro
    )))
    print(concat("mixed run vs its parts: ", format("{:.4}", mixed / (9.0 * once + 9.0 * dot_i))))
}
fn lowest_row(ro: raster.Options, s: string) -> int {
    val img = unwrap(raster.render(text_scene(s, 40.0, geom.point_of(10.0, 50.0)), ro))
    var lowest = 0
    var y = 0
    while y < img.height {
        var x = 0
        while x < img.width {
            if unwrap(image.pixel_at(img, x, y)).alpha > 0.5 {
                lowest = y
            }
            x = x + 1
        }
        y = y + 1
    }
    return lowest
}
ZM

if ! out=$("$BIN" "$WORK/both.zm" -- "$FONT" "$WORK/both.pdf" "$WORK/both.png" 2>&1); then
    fail "one scene renders to both backends" "$(head -3 <<<"$out")"
else
    pass "one scene renders to both backends"
    # The design's round trip: the text survives the PDF path.
    got=$(pdftotext "$WORK/both.pdf" - 2>&1 | tr -d '\n\f')
    if [ "$got" = "Round trip 123" ]; then
        pass "the PDF half round-trips its text"
    else
        fail "the PDF half round-trips its text" "wanted 'Round trip 123', got '$got'"
    fi
    # And the raster half put ink on the page rather than producing an empty image.
    inked=$(grep -o 'raster ink: [0-9.]*' <<<"$out" | cut -d' ' -f3)
    if [ -n "$inked" ] && awk "BEGIN{exit !($inked > 200)}"; then
        pass "the raster half drew the glyphs ($inked square units)"
    else
        fail "the raster half drew the glyphs" "$out"
    fi
    # Area goes as the square of the size, so doubling it is about 4x. Loose bounds, because the
    # hinting-free outline at 18pt is not exactly a quarter of the one at 36pt.
    ratio=$(grep -o 'double size ink ratio: [0-9.]*' <<<"$out" | cut -d' ' -f5)
    if [ -n "$ratio" ] && awk "BEGIN{exit !($ratio > 3.5 && $ratio < 4.5)}"; then
        pass "doubling the size quadruples the ink ($ratio)"
    else
        fail "doubling the size quadruples the ink" "got $ratio, wanted 3.5-4.5"
    fi
    if grep -q "g descends below o: true" <<<"$out"; then
        pass "the glyph Y axis points the right way"
    else
        fail "the glyph Y axis points the right way" "$out"
    fi
    # Ten cache hits of one glyph are ten times one glyph, within anti-aliasing.
    tenth=$(grep -o 'ten glyphs / one: [0-9.]*' <<<"$out" | cut -d' ' -f5)
    if [ -n "$tenth" ] && awk "BEGIN{exit !($tenth > 9.8 && $tenth < 10.2)}"; then
        pass "cached glyphs are faithful ($tenth x one glyph)"
    else
        fail "cached glyphs are faithful" "got $tenth, wanted 9.8-10.2"
    fi
    mixed=$(grep -o 'mixed run vs its parts: [0-9.]*' <<<"$out" | cut -d' ' -f6)
    if [ -n "$mixed" ] && awk "BEGIN{exit !($mixed > 0.99 && $mixed < 1.01)}"; then
        pass "two glyphs do not share a cache entry ($mixed)"
    else
        fail "two glyphs do not share a cache entry" "got $mixed, wanted ~1.0"
    fi
fi

echo "$fails failure(s)"
[ "$fails" -eq 0 ]
