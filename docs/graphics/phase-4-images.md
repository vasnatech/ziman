# Phase 4 — images: a pixel buffer, PNG, and PDF image XObjects

Gives the library pixels: an image value, encoding and decoding PNG, and placing images in a PDF.
This is what phase 5 renders *into*. It was also meant to make `Paint.Pattern` work; it does not,
and the "As built" section at the end says why.

Read `README.md` first.

## Scope

`image`. One module -- the buffer type and PNG live together because PNG's filtering is defined in
terms of the buffer's layout, and splitting them would mean exposing the layout publicly just so the
codec could see it.

## The buffer

```ziman
pub enum Pixels { Gray8  Rgb8  Rgba8 }

pub struct Image {
    width: int
    height: int
    format: Pixels
    data: [byte]        // row-major, no padding, width * channels bytes per row
}

pub fn image_of(width: int, height: int, format: Pixels) -> Result<Image, string>
pub fn pixel_at(i: Image, x: int, y: int) -> Result<Color, string>
pub fn with_pixel(i: Image, x: int, y: int, c: Color) -> Result<Image, string>
pub fn canvas(width: int, height: int, format: Pixels) -> ImageBuilder   // #[thread_bound]
pub fn set(b: ImageBuilder, x: int, y: int, c: Color)
pub fn snapshot_image(b: ImageBuilder) -> Image
```

**Both an immutable `Image` and a `#[thread_bound]` builder.** `with_pixel` on an immutable image
copies the whole buffer, which is right for the odd pixel and catastrophic for a rasteriser writing
a million of them. Phase 5 uses the builder; callers passing images around use the value. This is
the same split the language already makes between `[T]` and `MutableArray<T>`, so it needs no new
concept.

**Three formats, not a general colour-model system.** Gray8 for masks and single-channel data, Rgb8
for photographs, Rgba8 for anything composited. CMYK, 16-bit channels and ICC profiles are real
things PDF supports and nothing here needs; adding them later is additive.

**`data` is a packed `[byte]`**, which in Ziman is a real byte buffer rather than boxed values, so a
1024x1024 RGBA image is 4 MB and not 80 bytes per pixel.

## PNG

```ziman
pub fn encode_png(i: Image) -> [byte]
pub fn decode_png(data: [byte], max_pixels: int) -> Result<Image, string>
```

Encoding is small: an 8-byte signature, `IHDR`, one `IDAT` of zlib-compressed filtered rows, `IEND`,
each chunk with a CRC-32. `compress.deflate` already writes the zlib framing `IDAT` requires.

**Filtering: implement `None` and `Up` for encoding, support all five for decoding.** A real encoder
tries each filter per row and picks the cheapest; that is a compression-ratio optimisation, not a
correctness one. Decoding has no choice -- a file may use any of the five per row, so `None`, `Sub`,
`Up`, `Average` and `Paeth` all have to be there. Stating the asymmetry up front stops someone
"finishing" the encoder for no measurable gain.

**`max_pixels` on decode, like `compress`'s `max_size`.** A 32-bit width and height in a header
costs four bytes and asks for 16 exabytes of buffer; every decoder that does not check this has a
denial-of-service in it. The parameter is mandatory rather than defaulted for the same reason
`inflate`'s is.

**CRC-32 is checked on decode and an error names the chunk.** It is eight lines and it turns silent
corruption into a message.

Not supported, deliberately: interlaced PNG (Adam7 -- rare, and a second code path through the whole
decoder), 16-bit channels, palettes with transparency chunks beyond `tRNS`, and the ancillary chunks
nothing here reads. Each is an `Err` naming what it found.

## JPEG

```ziman
pub fn jpeg_dimensions(data: [byte]) -> Result<[int], string>   // [width, height, components]
```

**Pass-through only, and no decoder.** PDF's `DCTDecode` filter means a JPEG's compressed bytes go
into the file *unchanged* -- the reader decodes it. So embedding a photograph needs only its
dimensions and component count, read from the `SOF0`/`SOF2` marker. That is ~40 lines against the
several thousand a baseline JPEG decoder would be, and it produces smaller PDFs than re-encoding
would.

A JPEG cannot be drawn by the raster backend without a decoder, so phase 5 will reject it and say
so. That asymmetry is worth having: the document path, which is the motivating case, gets JPEG
support for almost nothing.

## PDF image XObjects

`pdf` gains:

- `DrawImage(handle, rect)` emits `q`, a `cm` that maps the unit square to `rect`, `/Im1 Do`, `Q`.
  **PDF images are always drawn into the unit square**, so the placement *is* the matrix -- there is
  no width or height on the operator.
- An `/XObject` of `/Subtype /Image` per image, with `/Width`, `/Height`, `/ColorSpace`
  (`/DeviceGray` or `/DeviceRGB`), `/BitsPerComponent 8`, and `/Filter`: `/FlateDecode` for raw
  pixels, `/DCTDecode` for a passed-through JPEG.
- **Alpha becomes an `/SMask`**: PDF has no RGBA image, it has an image plus a soft-mask image
  holding the alpha channel. An `Rgba8` image is therefore split into an RGB XObject and a Gray8
  `/SMask`. Missing this makes transparent PNGs render with black fringes, which is the kind of
  defect that looks like a rendering bug rather than a format mistake.
- ~~`Paint.Pattern(handle, matrix)` becomes a tiling pattern whose content stream draws the image
  once; the matrix is the pattern matrix.~~ **Not built** -- see "As built".

## What phase 4 does *not* do

JPEG decoding, GIF, WebP, TIFF, SVG. Resampling or scaling of pixel data (PDF scales by matrix;
phase 5 will need sampling and that belongs there). Colour management.

## Verification

- **PNG round trip**: encode a known image, decode it, assert pixel-for-pixel equality for all three
  formats. Then decode files this library did *not* write -- at minimum one per filter type -- since
  a codec that only reads its own output tests one of the five filters.
- **Against an external tool where present**: `pngcheck` on the output, or Python's `zlib` plus a
  hand-rolled chunk walk, to confirm the CRCs and the chunk order are right rather than
  self-consistent.
- **The DoS guard**: a header claiming 65535x65535 must be an `Err`, not an allocation.
- **Alpha in PDF**: render an `Rgba8` image, parse the output back, and assert the XObject has an
  `/SMask` whose dimensions match. This is the one that catches the black-fringe bug.
- **JPEG dimensions** against a file whose size is known independently.

## Build changes

One module registered. No native code. Test images should be *generated* by the tests rather than
checked in, except the handful of foreign PNGs needed to exercise the five decode filters.

## As built

Three things came out differently from the plan above, recorded here rather than quietly left as a
difference between the document and the code.

**`Paint.Pattern` is deferred, and says so.** A tiling pattern is not an image XObject with a
matrix: it is a `/Pattern` colour space, a pattern object carrying its own `/Resources` and content
stream, and `scn` instead of `rg` -- a fourth resource category, written for a caller that does not
exist yet. `scene.image` already places an image anywhere a matrix can put it, which is what phase 4
was actually for. So `paint.pattern()` constructs the variant, and every backend that meets it
returns `Err("pdf: an image pattern (paint.Pattern) is not implemented; draw the image with
scene.image instead")`. Deferring it is a decision; drawing nothing would have been a silent
degradation, which the library's first invariant forbids.

**The foreign PNGs are generated, not checked in.** "Build changes" above allowed a handful of
foreign files for the five decode filters. They are not in the repository: `tests/run_pdf_tests.sh`
has python3's `zlib` write one PNG per filter type over identical pixels and asserts all five decode
to the same bytes. That is strictly better than a fixture, because the encoder is independent of
this decoder -- *the* property that matters here, since a predictor that is wrong in both directions
cancels itself out and a self-encoded fixture cannot see it. The check is load-bearing: replacing
the Paeth predictor with the naive `a + b - c` fails filter 4 and nothing else.

**Row filters are validated by the filter byte, not by the result.** The first implementation left
`value = -1` as a "no branch matched" sentinel and reported `value < 0` as an unknown filter. With a
correct predictor that can never happen, so it read as defensive; with a *wrong* one it reports a
broken predictor as a corrupt file, which sends anyone debugging it to the wrong place. The filter
byte is now range-checked once per row, where the only thing that can actually be out of range is.
This was found by mutating the predictor to see whether the new test failed, and the first failure
it produced was that misleading message.

The verification the design called the decisive one -- "assert the XObject has an `/SMask` whose
dimensions match. This is the one that catches the black-fringe bug" -- is in both layers, and both
were checked by mutation. `tests/lang/graphics_image.zm` asserts the key is in the file;
`tests/run_pdf_tests.sh` has `pdfimages -list` confirm Poppler resolves it to an 8x8 greyscale soft
mask, then rasterises the page with `pdftoppm` and samples it to confirm the transparent squares
show the page through. Suppressing the `/SMask` entry fails all three, the last of them as
`red=361 grey=0` -- the black-fringe bug itself, measured.
