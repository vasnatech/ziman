# `image`

A pixel buffer, PNG in and out, and enough of JPEG to put one in a PDF.

The buffer and the codec live together because PNG's row filtering is defined in terms of the
buffer's layout, and splitting them would mean making that layout public just so the codec could
see it.

`use image` -- or `use image.Name` to bring one item in bare.

### `Pixels`

```
pub enum Pixels {
    Gray8
    Rgb8
    Rgba8
}
```

Three formats, not a colour-model system. Gray8 for masks and single-channel data, Rgb8 for
photographs, Rgba8 for anything composited. CMYK, 16-bit channels and ICC profiles are real
things PDF supports and nothing here needs; each would be additive.

### `channels_of`

```
pub fn channels_of(f: Pixels) -> int
```

### `Image`

```
pub struct Image {
    width: int
    height: int
    format: Pixels
    data: [byte]
}
```

Row-major, no padding: `width * channels` bytes per row. `data` is a packed `[byte]`, which in
Ziman is a real byte buffer rather than boxed values, so a 1024x1024 RGBA image is 4 MB and not
eighty bytes a pixel.

### `image_of`

```
pub fn image_of(width: int, height: int, format: Pixels, data: [byte]) -> Result<Image, string>
```

### `pixel_at`

```
pub fn pixel_at(i: Image, x: int, y: int) -> Result<Color, string>
```

### `Canvas`

```
pub struct Canvas {
    width: int
    height: int
    format: Pixels
    data: MutableArray<byte>
}
```

A mutable buffer to draw into.

**Both this and the immutable `Image` exist on purpose.** Writing a pixel into a value copies the
whole buffer, which is right for the odd pixel and catastrophic for a rasteriser writing a
million of them; phase 5 uses this, and callers passing images around use the value. It is the
same split the language already makes between `[T]` and `MutableArray<T>`, so it needs no new
idea.

### `canvas`

```
pub fn canvas(width: int, height: int, format: Pixels) -> Result<Canvas, string>
```

### `set`

```
pub fn set(c: Canvas, x: int, y: int, colour: Color) -> Result<unit, string>
```

### `snapshot_image`

```
pub fn snapshot_image(c: Canvas) -> Image
```

### `alpha_channel`

```
pub fn alpha_channel(i: Image) -> Option<Image>
```

The alpha channel on its own, as a Gray8 image -- what PDF needs for an `/SMask`, since it has no
RGBA image type. An image without alpha has none.

### `without_alpha`

```
pub fn without_alpha(i: Image) -> Image
```

The colour channels without alpha, which is the other half of an `/SMask` pair.

## PNG

### `encode_png`

```
pub fn encode_png(i: Image) -> [byte]
```

### `decode_png`

```
pub fn decode_png(data: [byte], max_pixels: int) -> Result<Image, string>
```

`max_pixels` is mandatory for the same reason `compress.inflate`'s `max_size` is: a width and a
height are four bytes each and can ask for more memory than exists, so a decoder that does not
check has a denial of service in it.

### `crc32`

```
pub fn crc32(data: [byte]) -> int
```

The standard CRC-32, computed without a table: 256 entries would be faster and this runs four
times per PNG.

## JPEG, dimensions only

### `jpeg_dimensions`

```
pub fn jpeg_dimensions(data: [byte]) -> Result<[int], string>
```

`[width, height, components]` read from the frame header.

**No decoder, deliberately.** PDF's `DCTDecode` filter takes a JPEG's compressed bytes
*unchanged* and the reader decodes them, so embedding a photograph needs only its shape. That is
about forty lines against the several thousand a baseline decoder would be, and it produces
smaller PDFs than re-encoding would. The raster backend cannot draw one and says so (phase 5).
