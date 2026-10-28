# `compress`

Gzip and zlib (deflate) compression, on zlib. Data is `[byte]` in and out.

Three container formats share the same deflate algorithm: gzip (RFC 1952, what `Content-Encoding:
gzip` and `.gz` files use), zlib (RFC 1950, what HTTP calls `Content-Encoding: deflate`), and raw
deflate (RFC 1951, no header at all). `gzip` and `deflate` write the first two; the `un` functions
read them, and `decompress` recognizes gzip or zlib by itself.

Decompressing untrusted data needs a size limit -- a few kilobytes can expand to gigabytes -- so every
reading function takes `max_size` (the most bytes it will produce; negative for no limit) and returns
an `Err` if the data is corrupt, truncated, or would expand past it.

`use compress` -- or `use compress.Name` to bring one item in bare.

## Example

```
use compress

// Unpacking can fail -- the data may not be gzip, or may not fit the limit -- so it answers a
// Result and this function propagates it.
fn demo() -> Result<unit, string> {
    val data = to_bytes("hello hello hello hello hello hello")
    val packed = compress.gzip(data)
    print(len(packed) < len(data))

    // Reading back takes a size limit, so a small file cannot expand into memory without bound.
    val unpacked = try compress.gunzip(packed, 1000)
    print(try bytes_to_string(unpacked))
    print(is_ok(compress.gunzip(packed, 10)))
    print(is_ok(compress.gunzip(to_bytes("not gzip"), 1000)))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("compress example failed: {}\n", e)
    }
}
```

Output:

```
true
hello hello hello hello hello hello
false
false
```

### `gzip`

```
pub fn gzip(data: [byte]) -> [byte]
```

`data` as a gzip stream.

### `deflate`

```
pub fn deflate(data: [byte]) -> [byte]
```

`data` as a zlib stream (HTTP's "deflate").

### `gunzip`

```
pub fn gunzip(data: [byte], max_size: int) -> Result<[byte], string>
```

The bytes of a gzip stream. An Err if it is not gzip, is corrupt or truncated, or decompresses to
more than `max_size` bytes.

### `inflate`

```
pub fn inflate(data: [byte], max_size: int) -> Result<[byte], string>
```

The bytes of a zlib stream. Same errors as `gunzip`.

### `decompress`

```
pub fn decompress(data: [byte], max_size: int) -> Result<[byte], string>
```

The bytes of a gzip or zlib stream, whichever it is. Same errors as `gunzip`.

### `inflate_raw`

```
pub fn inflate_raw(data: [byte], max_size: int) -> Result<[byte], string>
```

The bytes of a raw deflate stream (no header or checksum). Same errors as `gunzip`.
