# `test`

Assertions for #[test] functions (see docs/SYNTAX.md "Testing"): `ziman test`
runs every #[test] function it finds, and a failed assertion here is what makes one fail.

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
