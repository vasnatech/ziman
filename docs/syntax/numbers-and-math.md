# Number formatting, math and bits

Part of the [v0 syntax reference](README.md).

## Number formatting and parsing

The `number` core module is shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/
`byte`, pure Ziman on top of each type's own `str()`.

```ziman
use number
use number.parse_pattern

fn main() {
    val fmt = unwrap(parse_pattern("#,##0.00"))
    print(number.format(1234567, fmt))    // 1,234,567.00
    print(number.format(1234567, unwrap(parse_pattern("#,###.##"))))  // 1,234,567
    print(unwrap_err(parse_pattern("#.##0.00")))  // more than one decimal point
}
```

- **One engine, six entry points.** `number.format(s, fmt) -> string`/`number.parse(s, fmt) ->
  string` operate on the *string* each type already produces via `str()`/expects via `parse_int`,
  and that string member is the whole implementation; the `int`/`float`/`bigint`/`decimal`/`byte`
  members are thin wrappers over it. They are **members of `format`** rather than
  `format_int`/`_float`/... as of 2026-10-24: the argument says which type, so the name need not,
  and this section argued the opposite ("not an overload set per numeric type") only because
  overloading did not exist when it was written.
- **`NumberFormat { thousands_separator, decimal_separator: string, min_decimal_places: int,
  max_decimal_places: Option<int>, min_integer_digits: int }`** -- empty `thousands_separator`
  disables grouping; `max_decimal_places: None` leaves the value's own digit count;
  `min_integer_digits` zero-pads the integer side (sign outside the padding, so `-42` under
  `00000` is `-00042`). `number_format_plain()` is the no-grouping, no-forced-places default and
  `number_format_for(locale)` takes a locale's separators.
- **`parse_pattern(pattern) -> Result<NumberFormat, string>`** builds one from a
  `DecimalFormat`-shaped pattern, and **`parse_pattern(pattern, locale)`** does the same with that
  locale's separators -- the pattern gives the shape, the locale gives the symbols, which is
  `DecimalFormat`'s own split between a pattern and its `DecimalFormatSymbols`. A comma anywhere
  in the integer part turns on group-by-3 grouping.
- **`0` and `#` mean different things**, as they do in Java: after the point a `0` is a mandatory
  place and a `#` an optional one, so `#,##0.00` always writes two (`1,234,567.00`) while
  `#,###.##` writes at most two and drops trailing zeros (`1,234,567.5`, and `1,234,567` for a
  whole number). A `0` may not follow a `#` there. In the integer part the `0`s are a minimum
  width: `00000` writes 42 as `00042`.
- **A pattern that cannot be read is an `Err` naming the problem**, not a format that means
  something else. `#.##0.00` -- a plausible thing to write, being what a European *rendering*
  looks like -- is "more than one decimal point", and `"hello.xx"` names the `h`. Still
  unsupported, deliberately: `%`, `E`, `;`-separated subpatterns, literal prefix/suffix text and
  custom grouping sizes (always 3).
- **Forcing fewer decimal places truncates, never rounds** (rounding is `math.round`'s job). **Scientific notation passes through unchanged** -- `float`'s `str()`
  switches to it past 6 significant digits, where grouping/place-forcing has no sensible
  meaning, so `number.format` checks for `"e"`/`"E"` first rather than mangling it.
- **Hex covers the three integral types plus `[byte]`**: one `number.to_hex` over `byte`, `int`,
  `bigint` and `[byte]`, told apart by the argument. Its readers cannot share a name -- all four
  take a `string` and differ only in what they answer -- so each keeps its type:
  `byte_from_hex`, `int_from_hex`, `bigint_from_hex`, `bytes_from_hex`. (Not `float`/`decimal`:
  no natural hex for a non-whole value.)
  `byte` is trivial (2 unsigned digits). `int` uses fixed-width two's complement like C's
  `printf("%x")` (`-1` is `"ffffffffffffffff"`, via `bigint` arithmetic since the unsigned
  64-bit equivalent doesn't fit a signed `int64`). `bigint` has no fixed width, so it uses
  sign-prefixed magnitude (`"-ff"`), matching Java's `BigInteger.toString(16)`.
- **`parse_byte(s) -> Result<byte, string>`** -- delegates to `parse_int`, range-checks 0-255.
- **Limits, as global vals**: `byte_min`/`_max` (0, 255), `int_min`/`_max` (the `int64` range),
  `float_min`/`_max` (most-negative/largest finite double, `float_min` exactly `-float_max`),
  `float_min_positive` (smallest positive nonzero, denormalized -- named distinctly from
  `float_min` unlike Java's `Double.MIN_VALUE`). No `bigint`/`decimal` limits (arbitrary
  precision, no finite bound). Crossing `float_min`/`_max` overflows to infinity (doesn't
  error); under `float_min_positive` underflows to `0.0`. Crossing `int_min`/`_max` *is* a
  runtime error (doesn't wrap), same as any `int` overflow. Bring a limit in with `use
  number.int_max` -- no `number.int_max` dotted access on a global val.

## The `math` module

`sqrt`, `pow`, `exp`, `log` (natural), `sin`/`cos`/`tan` (radians), constants `pi`/`e` (global
vals, `use math.pi` to bring in bare). All `float` in/out, one-line libm wrappers.

```ziman
use math
fn main() {
    print(math.sqrt(2.0))          // 1.4142135623730951
    print(math.is_nan(math.sqrt(-1.0)))  // true
}
```

- **A domain error is NaN, not a `Result`** (`sqrt` of a negative, `log` of zero/negative --
  like C/Java/JS). `compare<T: Ord>` already gives NaN a total order, so check it with
  `math.is_nan(x)` (`x != x`) rather than unwrapping. Unlike `1.0 / 0.0`, which is a runtime
  error in Ziman (not infinity) -- `math` never produces a value your own arithmetic couldn't.
- **`math.is_finite(x)`/`is_infinite(x)`** join `is_nan`: finite means neither NaN nor an
  infinity. `x - x` is what separates them (`0.0` for any finite `x`, NaN for an infinity, NaN
  for NaN -- so NaN is excluded first), which is worth having a name for rather than inline.
- **`math.log2(x)`/`log10(x)`**, which are *not* `log(x) / log(2.0)`: they are exact for an
  exact power of their base where the division is not, so `log10(1000.0)` is `3.0` while
  `log(1000.0) / log(10.0)` is `2.9999999999999996` -- and `floor` of the two differs by one,
  which is how a digit-counting loop goes wrong.
- **`math.sign(x) -> int`** is -1/0/1 for `int`, `float`, `bigint` and `decimal`. An infinity has
  a sign; **NaN is a runtime error**, because the answer is an `int` and every candidate lies (0
  would agree with `sign(0.0)`, +/-1 would claim an ordering NaN lacks) -- ask `is_nan` first.
- **`math.floor`/`ceil`/`round`** round a `float` to an `int` (ties away from zero: `round(2.5)`
  is 3, `round(-2.5)` is -3); **`floor_float`/`ceil_float`/`round_float`** apply the same rules
  but keep the result a `float` (for a magnitude past `int`'s range, or NaN/infinity). These
  moved here (with the `decimal_*` functions below) from bare prelude builtins -- `floor(x)`
  alone is now undefined; write `math.floor(x)`.
- **No decimal equivalent for the irrational ones** (`sqrt`/`exp`/`log`/`sin`/`cos`/`tan`) --
  cast through `float` and round with `math.round`. `math.round`/`floor`/`ceil` of a `decimal`
  *are* exact: with a second argument they round to that scale (nearest-away-from-zero, toward
  -inf, toward +inf) and answer a `decimal`; with none they answer the whole number as a
  **`bigint`**, since a decimal's whole part can exceed 2^63 where a float's cannot.
  `math.pow(base: decimal, exponent: int)` is an exact non-negative integer power. A negative
  `exponent` is a runtime error naming `numerator / math.pow(base, -exponent)` instead --
  there's no scale to pick for that division on `math.pow`'s own.

## The `bits` module

Bitwise `and`/`or`/`xor`/`not`/`shift_left`/`shift_right` on `byte`/`int`/`bigint`, plus two
scalar-backed sets: `BitSet` (an `int`, indices 0-63) and `BigBitSet` (a `bigint`, unbounded).
Ziman has no bitwise operators in its own syntax -- this module is the only way to touch bits.

```ziman
use bits
fn main() {
    print(bits.and(12, 10))          // 8
    print(bits.shift_right(-8, 1))   // -4, sign-extending

    var bs = bits.set(bits.bitset_new(), 3)
    bs = bits.set(bs, 5)
    print(bits.format(bs))           // {3, 5}
    print(bits.len(bs))              // 2
}
```

- **One name per operation, over all five types**: `and`, `or`, `xor`, `not`, `shift_left`,
  `shift_right`, `popcount` and `bit_length` are each a single overload set spanning `byte`,
  `int`, `bigint` and both sets -- the argument says which is meant, so nothing carries a type
  prefix. Only the constructors do (`bitset_new`, `bitset_of_int`/`_to_int` and the
  `big_bitset_` twins), nothing but their return type telling them apart.
- **`byte`** is unsigned (`shift_right` is logical); shift amount 0-7.
- **`int`** is a direct native 64-bit two's-complement op; shift amount 0-63 (else runtime
  error, not C's undefined behavior); `shift_right` is arithmetic/sign-extending.
- **`bigint`** matches Java's `BigInteger`/Python's `int` exactly: `and`/`or`/`xor`/`not` are
  defined against *infinite* two's complement, so a negative bigint has infinitely many leading
  1-bits (`bits.and(-1n, 5n)` is `5n`). Shift amount must be non-negative, no upper bound.
  `shift_right` floors (toward -infinity) for both `int` and `bigint`, unlike `/`'s truncation:
  `bits.shift_right(-3, 1)` is `-2`, not `-1`.
- **`popcount`/`bit_length` on a bigint are a runtime error for a negative one** (infinite
  1-bits, no finite answer); `BigBitSet` never produces one (a set of indices is never negative).
- **`BitSet` speaks the prelude's own set vocabulary**, because it *is* a set of ints: `len`,
  `is_empty`, `remove`, `union`/`intersect` (variadic, as the prelude's are), `difference`,
  `is_subset`, `to_array` (ascending -- a bit index has a natural order a hash set's iteration
  does not) and `format` (`"{0, 3, 5}"`) mean here what they mean for a `Set<int>`. The
  bit-indexed accessors keep java.util.BitSet's own names: `set`/`get`/`flip`, index 0-63 or a
  runtime error. `xor` has no `Set<T>` equivalent to borrow, so it keeps the bitwise name, and
  `not` is the complement within 0..63. No mutable builder -- the whole state is one scalar, so
  there's no O(n)-copy cost to save; `var` plus these functions already do the job.
- **`BigBitSet` mirrors it over a `bigint`** -- unbounded (only a negative index rejected),
  `bit_length` in O(digits) rather than an unbounded scan. It has **no `not`**: a `BitSet`'s
  complement is taken within the 0..63 the type itself supplies, and an unbounded set's
  complement is infinite, so `difference(a, b)` is the bounded question.
- **A per-name import replaces the bare name, it does not extend the prelude's set.** `use
  bits.len` makes `len(x)` this module's for the whole file, so `len("abc")` no longer
  type-checks there and the prelude's is reached as `prelude.len("abc")`. `use bits` with
  `bits.len(bs)` leaves every prelude name alone, and is what most callers should write
  (`tests/lang/bits_bare_import_shadows_prelude.zm` pins both).
