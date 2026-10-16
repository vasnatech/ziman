# `math`

Floating-point math beyond the four basic operators, the rounding functions
(`floor`/`ceil`/`round` to an `int`, and `floor_float`/`ceil_float`/`round_float` that keep a
`float`), and `decimal`'s own rounding and integer power. The transcendental functions are
`float` in, `float` out, one-line wrappers over libm.

A domain error (`sqrt` of a negative number, `log` of zero or a negative number) is NaN, the
same as C, Java and JavaScript, and the same value `parse_float("nan")` already produces --
`compare<T: Ord>` already gives NaN a total order, so it is an ordinary float value to check
for (`is_nan`) rather than something wrapped in a `Result`. (Unlike a domain error, `1.0 / 0.0`
itself is a runtime error in Ziman, not an IEEE-754 infinity -- `/` checks for a zero divisor
on `float` the same as it does on `int`.)

`use math` -- or `use math.Name` to bring one item in bare.

## Example

```
use math
use math.pi

fn main() {
    // Transcendental functions are float in, float out; a domain error is NaN, not a Result.
    print(math.sqrt(2.0))
    print(math.pow(2.0, 10.0))
    print(math.sin(pi / 2.0))
    print(math.is_nan(math.sqrt(-1.0)))

    // floor/ceil/round give an int; the _float versions keep a float.
    print(math.round(2.5))
    print(math.floor(-1.5))
    print(math.ceil_float(1.2))

    // decimal rounds to a scale, exactly, and raises to a whole-number power exactly.
    print(math.decimal_round(1.235d, 2))
    print(math.decimal_floor(-1.21d, 1))
    print(math.decimal_pow(1.5d, 2))
}
```

Output:

```
1.4142135623730951
1024.0
1.0
true
3
-2
2.0
1.24
-1.3
2.25
```

### `pi`

```
pub val pi = 3.14159265358979323846
```

The ratio of a circle's circumference to its diameter. A global val has no `math.pi` dotted
access (see docs/DESIGN.md's "Global val" entry) -- bring it in with `use math.pi`.

### `e`

```
pub val e = 2.71828182845904523536
```

The base of the natural logarithm. Same as `pi`: `use math.e` to use it bare.

### `sqrt`

```
pub native fn sqrt(x: float) -> float
```

The non-negative square root of `x`, or NaN if `x` is negative.

### `pow`

```
pub native fn pow(base: float, exponent: float) -> float
```

`base` raised to `exponent`.

### `exp`

```
pub native fn exp(x: float) -> float
```

`e` raised to `x`.

### `log`

```
pub native fn log(x: float) -> float
```

The natural (base-`e`) logarithm of `x`, or NaN if `x` is zero or negative.

### `sin`

```
pub native fn sin(x: float) -> float
```

Sine, cosine and tangent of `x`, in radians.

### `cos`

```
pub native fn cos(x: float) -> float
```

### `tan`

```
pub native fn tan(x: float) -> float
```

### `asin`

```
pub native fn asin(x: float) -> float
```

The inverses, in radians. `asin` and `acos` answer NaN outside -1..1, which is this module's
convention for a domain error rather than a trap -- see the header.

### `acos`

```
pub native fn acos(x: float) -> float
```

### `atan`

```
pub native fn atan(x: float) -> float
```

### `atan2`

```
pub native fn atan2(y: float, x: float) -> float
```

The angle of the point (`x`, `y`) from the positive x axis, in radians, in -pi..pi -- and the one
that actually gets used, because it is right in all four quadrants where `atan(y / x)` is not,
and it survives `x` being zero. **Note the argument order: `y` first**, as C, Java, Python and
JavaScript all have it. It looks wrong and it is what every caller will expect.

### `floor`

```
pub native fn floor(x: float) -> int
```

The largest whole number not above `x`: `floor(-1.5)` is -2.

### `ceil`

```
pub native fn ceil(x: float) -> int
```

The smallest whole number not below `x`: `ceil(-1.5)` is -1.

### `round`

```
pub native fn round(x: float) -> int
```

`x` to the nearest whole number, halves away from zero: `round(2.5)` is 3, `round(-2.5)` is -3.

### `floor_float`

```
pub native fn floor_float(x: float) -> float
```

The same three roundings, but the result stays a `float`: `floor` and friends convert to `int`,
which loses magnitude past 2^63 and can't represent NaN or infinity.

### `ceil_float`

```
pub native fn ceil_float(x: float) -> float
```

### `round_float`

```
pub native fn round_float(x: float) -> float
```

### `decimal_round`

```
pub native fn decimal_round(d: decimal, scale: int) -> decimal
```

`d` rounded to `scale` decimal places, halves away from zero: `decimal_round(1.235d, 2)` is 1.24.

### `decimal_floor`

```
pub native fn decimal_floor(d: decimal, scale: int) -> decimal
```

`d` rounded down to `scale` decimal places, toward negative infinity: `decimal_floor(1.29d, 1)`
is 1.2, `decimal_floor(-1.21d, 1)` is -1.3.

### `decimal_ceil`

```
pub native fn decimal_ceil(d: decimal, scale: int) -> decimal
```

`d` rounded up to `scale` decimal places, toward positive infinity: `decimal_ceil(1.21d, 1)`
is 1.3, `decimal_ceil(-1.29d, 1)` is -1.2.

### `decimal_pow`

```
pub native fn decimal_pow(base: decimal, exponent: int) -> decimal
```

`base` to a non-negative integer `exponent`, exact: `decimal_pow(1.5d, 2)` is 2.25. A runtime
error if `exponent` is negative -- there's no one right scale to round a negative power's
division to (see docs/DESIGN.md); `numerator / decimal_pow(base, -exponent)` gets it,
`numerator`'s own scale picking `/`'s target the same way it does for any other division
(e.g. `1.0000d / decimal_pow(2d, 3)` is `0.1250`; a bare `1d`, scale 0, would round to `0`).

### `is_nan`

```
pub fn is_nan(x: float) -> bool
```

True for NaN ("not a number": the result of `sqrt(-1.0)`, `log(-1.0)`, `parse_float("nan")`,
...), which is the only float that does not equal itself -- the IEEE-754 definition, used
here directly.
