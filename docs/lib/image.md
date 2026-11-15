# `image`

A pixel buffer, PNG in and out, JPEG in.

The buffer and the codec live together because PNG's row filtering is defined in terms of the
buffer's layout, and splitting them would mean making that layout public just so the codec could
see it.

`use image` -- or `use image.Name` to bring one item in bare.

## Example

```
use image
use paint

// Pixels: a width, a height, a format and the bytes. An `Image` is the raster backend's output
// and the pattern paint's input, and it is a plain value -- so it can be built by hand, read back
// a pixel at a time, and asserted on.
fn demo() -> Result<unit, string> {
    // A canvas is the mutable way in; an Image is the immutable snapshot of one.
    val c = try image.canvas(4, 3, image.Pixels.Rgba8)
    try image.set(c, 0, 0, paint.rgb(1.0, 0.0, 0.0))
    try image.set(c, 3, 2, paint.rgba(0.0, 0.0, 1.0, 0.5))
    val img = image.snapshot_image(c)
    printf("{} {} {}\n", img.width, img.height, image.channels_of(img.format))

    // Four bytes a pixel, twelve pixels.
    print(len(img.data))

    val corner = try image.pixel_at(img, 0, 0)
    printf("{} {} {} {}\n", corner.red, corner.green, corner.blue, corner.alpha)
    print((try image.pixel_at(img, 3, 2)).alpha)

    // A pixel outside the image is an Err rather than a wrap-around or a zero, because reading
    // past an edge is a bug in the caller and should say so.
    print(is_ok(image.pixel_at(img, 4, 0)))

    // Alpha can be split off or dropped -- the two things a backend that has no alpha needs.
    print(Option.is_Some(image.alpha_channel(img)))
    val flat = image.without_alpha(img)
    print(image.channels_of(flat.format))
    print(Option.is_None(image.alpha_channel(flat)))

    // Building an Image from bytes checks that the bytes match the size, so a truncated buffer is
    // refused here rather than read past later.
    print(is_ok(image.image_of(2, 2, image.Pixels.Gray8, [0b, 1b, 2b, 3b])))
    print(is_ok(image.image_of(2, 2, image.Pixels.Gray8, [0b, 1b, 2b])))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("image example failed: {}\n", e)
    }
}
```

Output:

```
4 3 4
48
1.0 0.0 0.0 1.0
0.5019607843137255
false
true
3
true
true
false
```

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

**This is what a PDF needs, and it is not the decoder below.** PDF's `DCTDecode` filter takes a
JPEG's compressed bytes *unchanged* and the reader decodes them, so embedding a photograph needs
only its shape -- forty lines, and smaller PDFs than re-encoding would give. `decode_jpeg` is for
the other case: a picture this library has to draw itself.

## JPEG, decoded

### `decode_jpeg`

```
pub fn decode_jpeg(data: [byte], max_pixels: int) -> Result<Image, string>
```

A JPEG, decoded. `Gray8` for a one-component file and `Rgb8` for a three-component one.

`max_pixels` is mandatory for `decode_png`'s reason: a width and a height are two bytes each and
their product is what gets allocated, so a decoder that does not check has a denial of service
in it.

**What this costs, measured on the machine it was written on** -- a noisy photograph at quality
85, which is the unkind case because almost no block is flat:


```
64x64      19 ms
256x256   185 ms
800x600   1.4 s      (3 us a pixel)
```

**Those are after two rounds of making it faster, and the first set is worth keeping**: the
decoder as first written was 41 ms / 493 ms / 3.5 s, and 2.7x of that came from two changes that
did not touch what it computes -- Arai-Agui-Nakajima's transform in place of the definition, and
hoisting two integer divisions out of the per-sample upsample.

**The shape of what is left is the interpreter, and the profile is now flat**: 23% in
`idct_block`, 19% in the bit reader, 17% in the upsample, 12% each in the block decode and the
Huffman decode. Nothing dominates, which is what it looks like when the remaining cost is ~90 ns
an arithmetic operation rather than a mistake. The fix for *that* is the one `native_json.c`
already is -- a native -- and it is a decision to take when something needs it. What this is for
today is icons and small pictures, which it does in a frame.
