# `number`

Numbers as text and as bytes: formatting and parsing with separators, hex,
shortest-text floats, and the fixed-width big-endian integers binary protocols use.

Four parts below: float text, hex, formatting and parsing with separators, and bytes. The
hex, byte and float-text functions are native; the formatting engine and `bigint` hex are Ziman.

`use number` -- or `use number.Name` to bring one item in bare.

## Example

```
use number

fn main() {
    // Formatting with a pattern: thousands separators and fixed decimals.
    val fmt = number.parse_pattern("#,##0.00")
    print(number.format_int(1234567, fmt))
    print(number.format_float(3.14159, fmt))

    // Hex, both ways, and shortest-text floats.
    print(number.int_to_hex(255))
    print(unwrap(number.int_from_hex("ff")))
    print(number.bytes_to_hex([1b, 171b, 255b]))
    print(number.float_to_text(0.1))

    // Fixed-width big-endian integers, as binary protocols use them.
    print(number.u32_to_bytes(258))
    print(number.u16_from_bytes([1b, 2b], 0))
}
```

Output:

```
1,234,567.00
3.14
ff
255
01abff
0.1
[0, 0, 1, 2]
258
```

## Float text

### `float_to_text`

```
pub native fn float_to_text(x: float) -> string
```

The shortest text that reads back as exactly `x`: `0.1`, `1234567.891`, `150`, `1e-7`,
`1.5e+300`. Plain notation for magnitudes from 1e-7 up to (not including) 1e21, scientific
beyond, `nan`, `inf` and `-inf` for the special values. Unlike `str(x)`, which keeps only six
significant digits, this never loses precision; use it when a float is written for a machine
or a person to read back (the `json`, `toml`, `yaml` and `csv` writers do).

## Hex

### `byte_to_hex`

```
pub native fn byte_to_hex(b: byte) -> string
```

Two lowercase hex digits: `byte_to_hex(255b)` is "ff".

### `byte_from_hex`

```
pub native fn byte_from_hex(s: string) -> Result<byte, string>
```

The byte written as exactly two hex digits. An Err for any other length or a non-hex character.

### `int_to_hex`

```
pub native fn int_to_hex(x: int) -> string
```

Lowercase hex of `x` with no leading zeros ("0" for zero); a negative `x` is its 16-digit
two's complement.

### `int_from_hex`

```
pub native fn int_from_hex(s: string) -> Result<int, string>
```

The int written in 1 to 16 hex digits, read as a 64-bit two's complement value:
`int_from_hex("ffffffffffffffff")` is -1. An Err for an empty string, more than 16 digits, or a
non-hex character.

### `bytes_to_hex`

```
pub native fn bytes_to_hex(data: [byte]) -> string
```

Lowercase hex text of a byte array, two digits per byte: `bytes_to_hex([1b, 255b])` is
"01ff". The array counterpart of `byte_to_hex`.

### `bytes_from_hex`

```
pub native fn bytes_from_hex(s: string) -> Result<[byte], string>
```

The bytes of hex text (upper or lower case, an even number of digits). An Err on an odd length
or a character that is not a hex digit.

### `bigint_to_hex`

```
pub fn bigint_to_hex(n: bigint) -> string
```

Lowercase hex of `n`, sign-prefixed for a negative one: `bigint_to_hex(-255n)` is "-ff".

### `bigint_from_hex`

```
pub fn bigint_from_hex(s: string) -> Result<bigint, string>
```

The bigint written in hex, with an optional leading "-". An Err for an empty string, a lone "-",
or a non-hex character.

### `parse_byte`

```
pub fn parse_byte(s: string) -> Result<byte, string>
```

The byte written as a decimal number, 0 to 255: `parse_byte("200")` is `Ok(200b)`. An Err for
text that is not an integer or is out of range.

## Formatting and parsing

### `NumberFormat`

```
pub struct NumberFormat {
    thousands_separator: string
    decimal_separator: string
    decimal_places: Option<int>
}
```

How a number is written: the character between groups of three integer digits ("" for none),
the character before the decimals, and how many decimals to force (`None` keeps the value's own).

### `number_format_plain`

```
pub fn number_format_plain() -> NumberFormat
```

No grouping, "." as the decimal separator, the value's own decimals: formats a number as `str` would.

### `parse_pattern`

```
pub fn parse_pattern(pattern: string) -> NumberFormat
```

A format from a pattern such as "#,###.00": a comma anywhere in the integer part turns
grouping on (groups of three), and the digit count after the dot forces that many decimal
places. The decimal separator is always ".".

### `format_number`

```
pub fn format_number(s: string, fmt: NumberFormat) -> string
```

`s`, the plain text of a number, written according to `fmt`: `format_number("1234567.5",
parse_pattern("#,###.00"))` is "1,234,567.50". Text containing an exponent (`1e-7`) is returned
unchanged, since grouping and fixed decimals have no sensible meaning for it.

### `parse_number`

```
pub fn parse_number(s: string, fmt: NumberFormat) -> string
```

The inverse of `format_number`: strips the thousands separator and turns the decimal separator
back into ".", giving text `parse_int`, `parse_float`, `parse_bigint`, `parse_decimal` or
`parse_byte` accepts. It only normalizes: malformed input is the parser's job to reject.

### `format_int`

```
pub fn format_int(x: int, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

### `format_float`

```
pub fn format_float(x: float, fmt: NumberFormat) -> string
```

`x` written according to `fmt`, with the float's full precision (`float_to_text`, not `str`).

### `format_bigint`

```
pub fn format_bigint(x: bigint, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

### `format_decimal`

```
pub fn format_decimal(x: decimal, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

### `format_byte`

```
pub fn format_byte(x: byte, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

## Bytes

### `u16_to_bytes`

```
pub native fn u16_to_bytes(n: int) -> [byte]
```

`n` (0 to 65535) as 2 big-endian bytes.

### `u32_to_bytes`

```
pub native fn u32_to_bytes(n: int) -> [byte]
```

`n` (0 to 4294967295) as 4 big-endian bytes, network byte order.

### `u16_from_bytes`

```
pub native fn u16_from_bytes(b: [byte], offset: int) -> int
```

The unsigned big-endian integer of 2 bytes starting at `offset` in `b`.

### `u32_from_bytes`

```
pub native fn u32_from_bytes(b: [byte], offset: int) -> int
```

The unsigned big-endian integer of 4 bytes starting at `offset` in `b`.

### `i32_from_bytes`

```
pub native fn i32_from_bytes(b: [byte], offset: int) -> int
```

The signed (two's complement) big-endian integer of 4 bytes starting at `offset` in `b`.

### `i64_from_bytes`

```
pub native fn i64_from_bytes(b: [byte], offset: int) -> int
```

The signed (two's complement) big-endian integer of 8 bytes starting at `offset` in `b`.

### `f64_from_bytes`

```
pub native fn f64_from_bytes(b: [byte], offset: int) -> float
```

The little-endian IEEE-754 double (8 bytes) at `offset`.

### `f32_from_bytes`

```
pub native fn f32_from_bytes(b: [byte], offset: int) -> float
```

The little-endian IEEE-754 float (4 bytes) at `offset`, widened to a `float`.

### `f64_to_bytes`

```
pub native fn f64_to_bytes(f: float) -> [byte]
```

A double as 8 little-endian IEEE-754 bytes.
