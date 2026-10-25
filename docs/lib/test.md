# `test`

Assertions for #[test] functions (see docs/SYNTAX.md "Testing"): `ziman test`
runs every #[test] function it finds, and a failed assertion here is what makes one fail.

**Every failure names the line that asserted**, and that took a change in the runner rather
than here: the error these raise carries *this file's* `unwrap` line, which is a file the
reader never wrote. interpreter.c's `run_one_test` now asks error.c where the innermost run of
bundled-`test` frames was entered from, which is the asserting line, and reports the assertion
there. An ordinary runtime error inside a test (an index out of range, a `None` unwrapped) is
left exactly as it was, because its own location is already the reader's.

Every assert* function fails through `fail`, which is `unwrap` on a manufactured `Err` -- the
same explicit, opt-in escape hatch to a fatal error every Ziman program already has. `ziman
test` catches it per test (see interpreter.c's interpret_tests), so one failing assertion fails
only the one test it's in, not the whole run. Failure messages are built with `format`'s `{}`
placeholder, which displays any value the same way `print` does (a struct's fields, an enum's
variant, ...) -- not `str`, which is deliberately narrower (int/float/bool/bigint/decimal/byte
only) and, being a fixed allowlist checked before a generic function's own type parameters are
known, rejects a plain `T` outright. So there is no separate "must be printable" bound to
satisfy here -- `assert_eq`'s `T: Eq` is the only requirement, exactly the bound `==`/`!=`
already need.

`use test` -- or `use test.Name` to bring one item in bare.

### `fail`

```
pub fn fail(message: string)
```

Fails the current test with `message`, unconditionally.

### `assert`

```
pub fn assert(cond: bool)
```

Fails unless `cond` is true.

### `assert_false`

```
pub fn assert_false(cond: bool)
```

Fails unless `cond` is false.

### `assert_eq`

```
pub fn assert_eq<T: Eq>(expected: T, actual: T)
```

Fails unless `actual` equals `expected` (structural equality -- see "Equality" in
docs/SYNTAX.md), showing both values on failure.

### `assert_ne`

```
pub fn assert_ne<T: Eq>(unexpected: T, actual: T)
```

Fails if `actual` equals `unexpected`.

### `assert_near`

```
pub fn assert_near(expected: float, actual: float, tolerance: float)
```

Fails unless `actual` is within `tolerance` of `expected` -- **what to reach for instead of
`assert_eq` on two floats**, where `assert_eq(0.3, 0.1 + 0.2)` fails with 0.3 against
0.30000000000000004 and is right to: the two really are different values.

The comparison is `abs(actual - expected) <= tolerance`, so a `tolerance` of 0.0 is exact
equality, and a NaN on either side fails however wide the tolerance -- no comparison with a NaN
is true, which is the answer wanted here, because a NaN is not near anything, itself included
(checked, both ways round). A NaN reaches this from `math.sqrt` of a negative number or
`parse_float("nan")`, not from arithmetic: `0.0 / 0.0` is a runtime error in Ziman, not a NaN.

A negative `tolerance` could never pass, so it fails saying so rather than looking like a
failed comparison.

### `assert_lt`

```
pub fn assert_lt<T: Ord>(actual: T, bound: T)
```

Fails unless `actual < bound`.

### `assert_le`

```
pub fn assert_le<T: Ord>(actual: T, bound: T)
```

Fails unless `actual <= bound`.

### `assert_gt`

```
pub fn assert_gt<T: Ord>(actual: T, bound: T)
```

Fails unless `actual > bound`.

### `assert_ge`

```
pub fn assert_ge<T: Ord>(actual: T, bound: T)
```

Fails unless `actual >= bound`.

## Membership

### `assert_contains`

```
pub fn assert_contains(haystack: string, needle: string)
```

Fails unless `haystack` contains `needle`, **showing both** -- which is the whole reason these
exist. `assert(contains(body, "x"))` says `assertion failed` and nothing else, and it was the
most common assertion in both repositories (109 of them) before this was written: the one shape
where the reader most needs to see the value is the one that printed none of it.

The haystack is shown in full, not clipped. A clipped one hides exactly what is being looked
for when the near-miss is past the cut, and `assert_eq` already shows whole values.

```
pub fn assert_contains<T: Eq>(items: [T], x: T)
```

Fails unless `items` contains `x`.

```
pub fn assert_contains<T: Eq + Hash>(items: Set<T>, x: T)
```

Fails unless `items` contains `x`.

```
pub fn assert_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)
```

Fails unless `m` has an entry under `key`. The *key*, like `contains` on a `Map` itself -- a
test asserting on the value wants `assert_eq(want, get(m, key))`, which shows both.

### `assert_not_contains`

```
pub fn assert_not_contains(haystack: string, needle: string)
```

Fails if `haystack` contains `needle`, showing both.

```
pub fn assert_not_contains<T: Eq>(items: [T], x: T)
```

Fails if `items` contains `x`.

```
pub fn assert_not_contains<T: Eq + Hash>(items: Set<T>, x: T)
```

Fails if `items` contains `x`.

```
pub fn assert_not_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)
```

Fails if `m` has an entry under `key`.

### `assert_some`

```
pub fn assert_some<T>(opt: Option<T>) -> T
```

The value inside `opt`, or a failure if it is None.

### `assert_none`

```
pub fn assert_none<T>(opt: Option<T>)
```

Fails if `opt` is Some.

### `assert_ok`

```
pub fn assert_ok<T, E>(r: Result<T, E>) -> T
```

The `Ok` value inside `r`, or a failure naming the `Err` value found instead.

### `assert_err`

```
pub fn assert_err<T, E>(r: Result<T, E>) -> E
```

The `Err` value inside `r`, or a failure naming the `Ok` value found instead.
