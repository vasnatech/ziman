# `number`

`use number` -- or `use number.Name` to bring one item in bare.

## Example

```
use number

// A pattern and a hex read can both be rejected, so they answer Result and `try` carries them.
fn demo() -> Result<unit, string> {
    // Formatting with a pattern: thousands separators and fixed decimals.
    val fmt = try number.parse_pattern("#,##0.00")
    print(number.format(1234567, fmt))
    print(number.format(3.14159, fmt))

    // Hex, both ways, and shortest-text floats.
    print(number.to_hex(255))
    print(try number.int_from_hex("ff"))
    print(number.to_hex([1b, 171b, 255b]))
    print(number.float_to_text(0.1))

    // Fixed-width big-endian integers, as binary protocols use them.
    print(number.u32_to_bytes(258))
    print(number.u16_from_bytes([1b, 2b], 0))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("number example failed: {}\n", e)
    }
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

## Limits

### `byte_min`

```
pub val byte_min = 0b
```

### `byte_max`

```
pub val byte_max = 255b
```

### `int_min`

```
pub val int_min = -9223372036854775808
```

### `int_max`

```
pub val int_max = 9223372036854775807
```

### `float_min`

```
pub val float_min = -1.7976931348623157e+308
```

The most negative and largest finite `float`s -- not "closest to zero", the gotcha Java's own
`Double.MIN_VALUE` has (it means smallest positive nonzero, not most negative, unlike
`Integer.MIN_VALUE`). `float_min` is exactly `-float_max`, so the pair reads as a symmetric
range the same way `int_min`/`int_max` already do. Going past either end doesn't error -- it
overflows to the IEEE-754 infinity, the same as any other float arithmetic that leaves this
range (`float_max * 2.0` is `inf`).

### `float_max`

```
pub val float_max = 1.7976931348623157e+308
```

### `float_min_positive`

```
pub val float_min_positive = 5e-324
```

The smallest positive `float` greater than zero -- a denormalized value, the same one Java
calls (confusingly) `Double.MIN_VALUE`. Named `float_min_positive`, not `float_min`, to avoid
exactly that confusion: `float_min` above is the most negative value, this is the closest to
zero on the positive side. Half of it underflows to `0.0`, not a smaller denormal.

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

### `to_hex`

```
pub native fn to_hex(b: byte) -> string
```

Two lowercase hex digits: `to_hex(255b)` is "ff".

### `byte_from_hex`

```
pub native fn byte_from_hex(s: string) -> Result<byte, string>
```

The byte written as exactly two hex digits. An Err for any other length or a non-hex character.

### `to_hex`

```
pub native fn to_hex(x: int) -> string
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

### `to_hex`

```
pub native fn to_hex(data: [byte]) -> string
```

Lowercase hex text of a byte array, two digits per byte: `to_hex([1b, 255b])` is
"01ff". The array counterpart of `to_hex`.

### `bytes_from_hex`

```
pub native fn bytes_from_hex(s: string) -> Result<[byte], string>
```

The bytes of hex text (upper or lower case, an even number of digits). An Err on an odd length
or a character that is not a hex digit.

### `to_hex`

```
pub fn to_hex(n: bigint) -> string
```

Lowercase hex of `n`, sign-prefixed for a negative one: `to_hex(-255n)` is "-ff".

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
    // **The fraction's shape, as `0` and `#` after the point state it.** `min_decimal_places` is
    // how many places are always written (the `0`s); `max_decimal_places` is the most allowed (the
    // `0`s plus the `#`s), and `None` means the value keeps its own. `.00` is min 2 / max 2 -- a
    // fixed two places; `.##` is min 0 / max 2 -- up to two, trailing zeros dropped; `.0#` is
    // min 1 / max 2. A pattern with no point at all is min 0 / max None.
    //
    // Until 2026-10-24 this was one `decimal_places: Option<int>`, which could only say "exactly
    // N", so `#,##0.00` and `#,###.##` formatted identically -- the one piece of real
    // `DecimalFormat` semantics that was missing rather than merely absent.
    min_decimal_places: int
    max_decimal_places: Option<int>
    // The integer side's minimum width, zero-padded: the count of `0`s in the integer part, so
    // `00000` writes 42 as `00042`. It only ever pads -- a value whose integer part is `0` keeps
    // that digit whatever this says, since a number has to have one.
    min_integer_digits: int
}
```

How a number is written: the character between groups of three integer digits ("" for none),
the character before the decimals, and how many decimals to force (`None` keeps the value's own).

### `number_format_for`

```
pub fn number_format_for(l: Locale) -> NumberFormat
```

The separators `l` writes numbers with, and the value's own decimals: `1.234,56` under `tr_TR`
and `de_DE`, `1,234.56` under `en_US`, a space for grouping under `sv_SE`, and no grouping at all
under `ar_SA` -- all read from the host's locale data, so they are as good as the operating
system's.

**`str(x)` is deliberately not locale-aware** and never will be: it is what a wire format, a SQL
value and a PDF object number are written with, and `1234,56` in any of those is a bug. A locale
reaches numbers here, where a caller has asked for a rendering a person will read.

### `number_format_plain`

```
pub fn number_format_plain() -> NumberFormat
```

No grouping, "." as the decimal separator, the value's own decimals: formats a number as `str` would.

### `parse_pattern`

```
pub fn parse_pattern(pattern: string) -> Result<NumberFormat, string>
```

**Builds a `NumberFormat` from a `DecimalFormat`-shaped pattern, or an Err naming what is wrong.**

`#,##0.00` is two fixed places with grouping; `#,###.##` is up to two with trailing zeros
dropped; `00000` is a zero-padded five-digit integer. The separators are `,` and `.`; for a
locale's own, pass one (`parse_pattern(p, l)`) or set the two fields with `with`.

**It answers a `Result` as of 2026-10-24**, and that is the point of the change. The older
reader split on `.` and read the pieces positionally, so `#.##0.00` -- a plausible thing to
write, being what a European *rendering* looks like -- silently produced no grouping and three
decimal places, and `"hello.xx"` produced a format too. A pattern that cannot be read is now
said so.

What is still not supported, deliberately: `%`, `E`, a `;`-separated negative subpattern,
literal prefix or suffix text, and custom grouping sizes (always 3).

```
pub fn parse_pattern(pattern: string, l: Locale) -> Result<NumberFormat, string>
```

**The pattern gives the shape, the locale gives the symbols** -- `DecimalFormat`'s own split
between a pattern and its `DecimalFormatSymbols`. `parse_pattern("#,##0.00", locale.of("de"))`
formats 1234567.5 as `1.234.567,50`.

Without this the two could not be combined: a pattern baked in `,` and `.`, and
`number_format_for` gave a locale's separators but no way to ask for a fixed number of places.

### `format`

```
pub fn format(s: string, fmt: NumberFormat) -> string
```

`s`, the plain text of a number, written according to `fmt`: `format("1234567.5",
parse_pattern("#,###.00"))` is "1,234,567.50". Text containing an exponent (`1e-7`) is returned
unchanged, since grouping and fixed decimals have no sensible meaning for it.

### `parse`

```
pub fn parse(s: string, fmt: NumberFormat) -> string
```

The inverse of `format`: strips the thousands separator and turns the decimal separator
back into ".", giving text `parse_int`, `parse_float`, `parse_bigint`, `parse_decimal` or
`parse_byte` accepts. It only normalizes: malformed input is the parser's job to reject.

### `format`

```
pub fn format(x: int, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

```
pub fn format(x: float, fmt: NumberFormat) -> string
```

`x` written according to `fmt`, with the float's full precision (`float_to_text`, not `str`).

```
pub fn format(x: bigint, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

```
pub fn format(x: decimal, fmt: NumberFormat) -> string
```

`x` written according to `fmt`.

```
pub fn format(x: byte, fmt: NumberFormat) -> string
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
