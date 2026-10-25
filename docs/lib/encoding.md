# `encoding`

Base 2, 8, 16, 32 and 64 over `[byte]`, one door per base, each an `_encode`
answering text and a `_decode` answering `Result<[byte], string>`.

**The five bases fall into two families, and the split is the thing to understand here.**

`base2`, `base8` and `base16` are **fixed width per byte**: 8, 3 and 2 digits, each byte rendered
independently. There is no bit packing, no padding and no alignment question -- the text length is
always the byte count times the width, and decoding needs the length to be an exact multiple of
it. `base8` is the odd one of the three in that three octal digits hold nine bits where a byte has
eight, so the values 400 to 777 cannot occur and `base8_decode` rejects them; there is no RFC for
base 8 of a byte stream, and per-byte is the reading that makes it a sibling of the other two
rather than a third packing scheme.

`base32` and `base64` are **RFC 4648 bit packing**: five or six bits per character taken across
byte boundaries, so the text length is not a simple multiple and the last character carries
leftover bits. These are the two the RFC actually defines, and they are the two that interoperate
with anything else.

What that means in practice: a `base16` or `base2` string can be cut anywhere on a digit boundary
and still decode; a `base32` or `base64` one cannot.

`base64url` is base64 over the URL-safe alphabet (`-` and `_` for `+` and `/`), which is what JWT
and anything else that puts bytes in a URL uses.

`use encoding` -- or `use encoding.Name` to bring one item in bare.

## Fixed width per byte

### `base2_encode`

```
pub fn base2_encode(data: [byte]) -> string
```

Each byte as eight binary digits, most significant first: `[1b, 255b]` is `"0000000111111111"`.

### `base2_decode`

```
pub fn base2_decode(text: string) -> Result<[byte], string>
```

Binary digits back to bytes. An Err for a length that is not a multiple of 8, or any character
other than `0` and `1`.

### `base8_encode`

```
pub fn base8_encode(data: [byte]) -> string
```

Each byte as three octal digits: `[8b, 255b]` is `"010377"`.

### `base8_decode`

```
pub fn base8_decode(text: string) -> Result<[byte], string>
```

Octal digits back to bytes. An Err for a length that is not a multiple of 3, a character outside
`0`-`7`, or a group above `377` -- three octal digits hold nine bits where a byte has eight, so
`400` and up name no byte.

### `base16_encode`

```
pub fn base16_encode(data: [byte]) -> string
```

Each byte as two lower-case hex digits: `[1b, 255b]` is `"01ff"`. The same bytes
`number.to_hex` gives -- this is the base-N door to it, and `number`'s also does an `int`,
a `bigint` and a single `byte`, which a byte-stream encoder has no business doing.

### `base16_decode`

```
pub fn base16_decode(text: string) -> Result<[byte], string>
```

Hex digits back to bytes, upper or lower case. An Err for an odd length or a non-hex character.

## RFC 4648 bit packing

### `base32_encode`

```
pub fn base32_encode(data: [byte]) -> string
```

`data` as base32 (RFC 4648) without `=` padding.

### `base32_decode`

```
pub fn base32_decode(text: string) -> Result<[byte], string>
```

base32 text as bytes: upper or lower case, with or without `=` padding, spaces and dashes
ignored (as people write a secret down). An Err for any other character.

### `base64_encode`

```
pub native fn base64_encode(data: [byte]) -> string
```

`data` as base64 (RFC 4648) with `=` padding.

### `base64_decode`

```
pub native fn base64_decode(s: string) -> Result<[byte], string>
```

base64 text as bytes. An Err for a character outside the alphabet or a malformed length.

### `base64url_encode`

```
pub native fn base64url_encode(data: [byte]) -> string
```

`data` as base64url (RFC 4648 section 5): `-` and `_` in place of `+` and `/`, no padding. What
JWT and anything else putting bytes in a URL uses.

### `base64url_decode`

```
pub native fn base64url_decode(s: string) -> Result<[byte], string>
```

base64url text as bytes.
