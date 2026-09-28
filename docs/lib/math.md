# `math`

Floating-point math beyond the four basic operators and the three rounding
builtins (`floor`/`ceil`/`round`, which take a float but return an int -- see docs/SYNTAX.md
"Numbers"). Everything here is a `float` in, `float` out, one-line wrapper over libm.

A domain error (`sqrt` of a negative number, `log` of zero or a negative number) is NaN, the
same as C, Java and JavaScript, and the same value `parse_float("nan")` already produces --
`compare<T: Ord>` already gives NaN a total order, so it is an ordinary float value to check
for (`is_nan`) rather than something wrapped in a `Result`. (Unlike a domain error, `1.0 / 0.0`
itself is a runtime error in Ziman, not an IEEE-754 infinity -- `/` checks for a zero divisor
on `float` the same as it does on `int`.)

`use math` -- or `use math.Name` to bring one item in bare.

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

### `floor`

```
pub native fn floor(x: float) -> float
```

`floor`/`ceil`/`round`'s own float-preserving counterparts: the bare builtins convert to
`int`, which loses magnitude past 2^63 and can't represent NaN or infinity -- these round the
same three ways (down, up, nearest with ties away from zero) but keep the value a `float`.

### `ceil`

```
pub native fn ceil(x: float) -> float
```

### `round`

```
pub native fn round(x: float) -> float
```

### `is_nan`

```
pub fn is_nan(x: float) -> bool
```

True for NaN ("not a number": the result of `sqrt(-1.0)`, `log(-1.0)`, `parse_float("nan")`,
...), which is the only float that does not equal itself -- the IEEE-754 definition, used
here directly.
