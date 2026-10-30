# `test`

Assertions for #[test] functions (see docs/syntax/ "Testing"): `ziman test`
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

## Example

```
use test

// The assertions a `#[test]` function is written with. `ziman test` finds every `#[test]` function
// in the files it is given and runs them, and a failed assertion here is what makes one fail --
// reported at **the line that asserted**, not inside this module, which took a change in the
// runner rather than in the assertions.
//
// Run this file two ways and both work: `ziman <file>` runs `main` below, and `ziman test <file>`
// runs the `#[test]` functions.

#[test]
fn arithmetic_holds() {
    test.assert_eq(4, 2 + 2)
    test.assert_ne(5, 2 + 2)
    test.assert_lt(2 + 2, 5)
    // Floats are compared with a tolerance, because exact equality is the wrong question to ask
    // of them.
    test.assert_near(0.3, 0.1 + 0.2, 0.000001)
}

#[test]
fn collections_are_asserted_about_directly() {
    val names = ["ada", "grace", "alan"]
    test.assert_len(3, names)
    test.assert_contains(names, "grace")
    test.assert_not_contains(names, "hedy")
    test.assert_not_empty(names)
    test.assert_all_match(names, fn(n: string) -> bool { return len(n) > 2 })
}

#[test]
fn option_and_result_have_their_own() {
    // These say what is meant, and -- more usefully -- what failed: `assert_ok` on an Err reports
    // the error it found rather than only that the value was not Ok.
    test.assert_some(first(["ada"]))
    test.assert_none(first<string>([]))
    test.assert_ok(parse_int("12"))
    test.assert_err(parse_int("twelve"))
}

fn main() {
    // An assertion that holds produces nothing at all: no output is the passing case, which is
    // why a test run's output is a list of names rather than a transcript.
    test.assert_eq(4, 2 + 2)
    test.assert(true)
    test.assert_false(false)
    test.assert_starts_with("ziman.dev", "ziman")
    test.assert_entry(Map<string, int> { "ada": 36 }, "ada", 36)
    print("every assertion above held, which is why nothing was printed by them")

    // A failure goes through `fail`, which is `unwrap` on a manufactured Err -- the same opt-in
    // route to a fatal error every Ziman program already has. `ziman test` catches it per test,
    // so one failing assertion fails its own test and not the whole run.
    print(len(["this line is reached"]))
}
```

Output:

```
every assertion above held, which is why nothing was printed by them
1
```

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
docs/syntax/), showing both values on failure.

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

## Size

### `assert_len`

```
pub fn assert_len(expected: int, items: string)
```

Fails unless `items` holds exactly `expected` characters, **showing the value** -- which is why
this exists beside `assert_eq(expected, len(items))`, written 57 times across the two
repositories and reporting `expected: 3 / actual: 5` without ever showing which five. Same
defect as `assert(contains(..))` above, in the same place: the number tells the reader that
something is wrong and nothing about what.

```
pub fn assert_len<T>(expected: int, items: [T])
```

Fails unless `items` holds exactly `expected` elements, showing them.

```
pub fn assert_len<T: Eq + Hash>(expected: int, items: Set<T>)
```

Fails unless `items` holds exactly `expected` elements, showing them.

```
pub fn assert_len<K: Eq + Hash, V>(expected: int, m: Map<K, V>)
```

Fails unless `m` holds exactly `expected` entries, showing them.

### `assert_empty`

```
pub fn assert_empty(items: string)
```

Fails unless `items` is empty. `assert_len(0, items)` says the same thing; this says it the way
the reader thinks it, and 15 of those 57 call sites were asking exactly this.

The prelude's `is_empty` is `string`-only, so for a collection there was no spelling of this
question shorter than comparing `len` to zero.

```
pub fn assert_empty<T>(items: [T])
```

Fails unless `items` is empty.

```
pub fn assert_empty<T: Eq + Hash>(items: Set<T>)
```

Fails unless `items` is empty.

```
pub fn assert_empty<K: Eq + Hash, V>(m: Map<K, V>)
```

Fails unless `m` is empty.

### `assert_not_empty`

```
pub fn assert_not_empty(items: string)
```

Fails if `items` is empty.

```
pub fn assert_not_empty<T>(items: [T])
```

Fails if `items` is empty.

```
pub fn assert_not_empty<T: Eq + Hash>(items: Set<T>)
```

Fails if `items` is empty.

```
pub fn assert_not_empty<K: Eq + Hash, V>(m: Map<K, V>)
```

Fails if `m` is empty.

## Every element

### `assert_all_match`

```
pub fn assert_all_match<T>(items: [T], test: fn(T) -> bool)
```

Fails unless `test` holds for every element of `items`, **naming the first one it does not hold
for** -- which is the whole value over `assert(all(items, test))`, since the builtin can only
answer true or false and the reader is left to find the offender.

`[T]` only, because that is as far as the `all`/`any`/`none` builtins reach (those and a
`MutableArray`); a `Set` or `Map` is `to_array`/`entries` away.

### `assert_any_match`

```
pub fn assert_any_match<T>(items: [T], test: fn(T) -> bool)
```

Fails unless `test` holds for at least one element of `items`. There is no offending element to
name here -- the failure *is* that no element qualified -- so it shows the array.

### `assert_none_match`

```
pub fn assert_none_match<T>(items: [T], test: fn(T) -> bool)
```

Fails if `test` holds for any element of `items`, naming the first one it holds for.

## Map entries

### `assert_entry`

```
pub fn assert_entry<K: Eq + Hash, V: Eq>(m: Map<K, V>, key: K, value: V)
```

Fails unless `m` holds `value` under `key`, **saying which of the two ways it failed**: no entry
at all, or an entry holding something else. `assert_eq(value, unwrap(get(m, key)))` cannot --
it fails inside `unwrap` when the key is missing, before the assertion is reached, and when the
value differs it shows neither the key nor the map.

## Text shape

### `assert_starts_with`

```
pub fn assert_starts_with(s: string, prefix: string)
```

Fails unless `s` starts with `prefix`, showing both.

### `assert_ends_with`

```
pub fn assert_ends_with(s: string, suffix: string)
```

Fails unless `s` ends with `suffix`, showing both.

### `assert_blank`

```
pub fn assert_blank(s: string)
```

Fails unless `s` is empty or nothing but whitespace. "Blank" is `trim(s)` being empty, which is
the definition `trim` already carries rather than a second one invented here.

### `assert_not_blank`

```
pub fn assert_not_blank(s: string)
```

Fails if `s` is empty or nothing but whitespace.

### `assert_matches`

```
pub fn assert_matches(s: string, pattern: string)
```

**Fails unless `pattern` matches somewhere in `s`** -- the question `regex.is_match` asks, which
is *not* the one AssertJ's `matches` or Java's `String.matches` asks: those require the pattern
to cover the whole string. A whole-string match here is `^...$`, and the anchors work.

A pattern that does not compile fails the test with the compiler's own message (`missing closing
parenthesis at position 3`), not with "did not match": a malformed pattern is a mistake in the
test, and reporting it as a mismatch would send the reader looking at the wrong thing.

### `assert_not_matches`

```
pub fn assert_not_matches(s: string, pattern: string)
```

Fails if `pattern` matches anywhere in `s`.

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
