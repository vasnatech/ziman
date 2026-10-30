# Types and numbers

Part of the [v0 syntax reference](README.md).

## Types

- `int` — 64-bit signed integer. **Never wraps**: `+ - * /` (`MIN / -1`), unary `-`, `math.abs(MIN)`
  overflow are a runtime error, consistent with `bigint as int`/`int as byte` clamping instead
  of wrapping. A literal must fit `[-9223372036854775808, 9223372036854775807]` (overflow is a
  compile error naming the `n` bigint suffix, not silent saturation). `float as int` truncates
  toward zero and saturates to `INT64_MAX`/`INT64_MIN` (NaN is 0).
- `float` — double-precision IEEE 754. `bool` — `true`/`false`. `string` — immutable text.
- `bigint`/`decimal`/`byte` — see "Numbers" below for the full rules; in brief, arbitrary-
  precision integer (`n` suffix), arbitrary-precision exact decimal (`d` suffix), and an
  unsigned 0-255 non-arithmetic scalar (`b` suffix) — bare digits always mean `int`/`float`.
- `unit` — the type with exactly one value, also written `unit`: what "nothing to give back"
  looks like where a value is required (`Result<unit, DbError>`, a generic argument, `-> unit`).
  A bare `return` in a `-> unit`/`Result<unit, E>` function is `return unit` (auto-wrapping to
  `Ok(unit)` in the latter); a `-> unit` function running off its end returns `unit`. A function
  with *no* declared return type still has no value (`val x = f()` on one errors) -- declare
  `-> unit` when the caller wants one. `==`-comparable, hashable, prints as `unit`; a word, not
  a keyword (the type where a type is expected, an ordinary name otherwise, so a JSON field
  called `unit` is fine).
- `[T]` — a growable, homogeneous, indexed array, nesting freely (`[[int]]`). Value semantics
  like everything else in Ziman: assigning/passing always yields an independent value, never a
  shared reference. See "Arrays".
- A `struct` name — a named, nominal aggregate. Same name required for two structs to be the
  same type, even with identical fields (`Point{x,y}` and `Vector{x,y}` don't unify). Value
  semantics. See "Structs".
- `Result<T, E>` — a success carrying `T` or a failure carrying `E`; a hardcoded parametric
  type like `[T]` (not general user-definable generics), so `T`/`E` can be any Ziman type but
  you can't declare your own `Result`-shaped type. `<>` not `[]`, since `[]` stays scoped to
  sequences. See "Result".
- An `enum` name — a named, nominal tagged union, one declared variant optionally carrying one
  payload. Unlike `[T]`/`Result`, a type *users* declare, but still concrete (monomorphic), not
  generic on its own. See "Enums".
- `Channel<T>` — a thread-safe FIFO queue and Ziman's concurrency primitive (`send`/`receive`,
  `spawn`). A third hardcoded parametric type, but not a generics shortcut like the other two --
  its real value (safe blocking send/receive across real OS threads) is inherently native. See
  "Concurrency".
- **No implicit conversion between any scalar type.** `as` is the only cast among
  `int`/`float`/`bigint`/`decimal` (never arrays/structs/enums/`Result`/`Channel`) -- each
  lossy in at least one direction (see "Numbers"). `byte` only casts to/from `int`, saturating
  on the way in.

## Numbers

`bigint` (arbitrary-precision integer) and `decimal` (arbitrary-precision exact decimal —
an unscaled `bigint` magnitude plus a scale, exactly Java's `BigDecimal` shape) extend
`int`/`float` for exact, unbounded arithmetic. Neither ever implicitly mixes with any
other numeric type — cast explicitly with `as`, same rule as everywhere else.

```ziman
fn factorial(n: bigint, limit: int, i: int) -> bigint {
    if i > limit { return n }
    return factorial(n * (i as bigint), limit, i + 1)
}
fn main() { print(factorial(1n, 100, 1)) }   // a 158-digit number
```

**`bigint`**: literal needs `n` (`12345678901234567890n`; bare digits always mean `int`,
no suffix on a fractional literal). All of `int`'s operators/builtins work (`+ - * / %`,
unary `-`, `< > <= >= == !=`, `str`, `abs`, `min`/`max`, `hash`, `parse_bigint`); `/`/`%`
truncate toward zero like `int`, not floor division; division/modulo by zero is a runtime
error. Casts: `bigint as int`/`as float` saturate to the type's min/max rather than wrap;
`int as bigint` is exact; `float as bigint` truncates the fractional part then converts
the exact remaining value (NaN/infinity is a runtime error, same for `float as decimal`).
Heap-allocated internally, unlike `int`/`float`.

```ziman
use math

fn main() {
    val total = 19.99d + (19.99d * 0.0825d)
    print(total)             // 21.639175 -- exact, no binary rounding drift
    print(10d / 3d)          // 3 -- rounds half-up to the operands' own scale
    print(math.round(1.2345d, 2))  // 1.23 (`use math`)
}
```

**`decimal`**: literal needs `d` (`19.99d`, `20d`; bare digits always mean `float`/`int`).
All of `float`'s operators/builtins work, plus `parse_decimal` and
`math.round(d, scale)` — a member of `round`'s overload set, not a separate name: it takes a
`decimal` where the `float` member takes a `float`, which is exactly what resolution reads.
(This section claimed the opposite until 2026-10-24, saying the two "differ only in what they
return"; that is true of `round`/`round_float`, both of which take a `float`, and was transplanted
onto a pair it does not describe.) `+`/`-` align scale first and are always exact; `*`
needs no alignment and is exact; `/` doesn't always terminate, so it rounds
half-up-away-from-zero to `max(a.scale, b.scale)` (`10d / 3d` is `3`, `-2d / 3d` is `-1`);
`%` is an exact remainder, no rounding. Division/modulo by zero is a runtime error.
`==`/`hash` ignore trailing-zero scale differences (`1.0d == 1.00d`, unlike Java's
`BigDecimal.equals()`). Casts are lossy through `float` either way, and truncate (not
round) going to `int`/`bigint`; `int`/`bigint as decimal` are exact.

```ziman fragment
print(300 as byte)   // 255 -- saturates, doesn't wrap
print(-5 as byte)     // 0
```

**`byte`**: a fifth scalar, unsigned 0-255, but not a number to compute with — no
arithmetic, no `abs`, only `== !=`, `str`, `hash`, and `compare`/`sort`/`min`/`max` (it
satisfies `Ord`; ordering isn't arithmetic). Literal needs `b` (`200b`); out-of-range is a
compile-time error (`300b` fails to parse), not a silent wrap, since a literal's value is
already fully known at parse time. Casts only to/from `int`: `int as byte` saturates to
`[0, 255]`, `byte as int` is always exact; no cast to/from `float`/`bigint`/`decimal`.

All three extra scalar types work everywhere `int`/`float` already do: a struct field, an
array element (`[byte]` supports `==`/`contains`, since `byte` is scalar), a generic type
argument, and a `Map`/`Set` key where `Eq`/`Hash` apply.
