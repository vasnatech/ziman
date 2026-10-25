# `random`

Randomness from the operating system.

`bytes` is the one native, reading real entropy from the OS (`/dev/urandom`). It is the source
of all non-determinism a program has besides the clock; there is deliberately no seedable
pseudo-random generator (a program that wants one can build it on these bytes). `uuid` is
built on it. See docs/DESIGN.md's "random" entry.

Everything else here is ordinary Ziman over `bytes`, and they exist because the obvious
hand-rolled version -- `x % n` over random bytes -- favours the low residues unless `n` divides
the draw's range exactly. The reference backend had three copies of that pattern, and **the
amount of bias depends entirely on how wide the draw is against the span**, which is why one of
them mattered and two did not:

* `mfa.zm` drew **4** bytes and took `% 100000000`. 2^32 mod 10^8 is 94967296, so 95% of the
  eight-digit backup codes were 2.4% more likely than the other 5%. Measurable, and in the
  codes that stand in for a second factor.
* `testdata.zm` drew **6** bytes and took `% 3`. 2^48 mod 3 is 1, a bias of 4e-15 -- correct,
  but by accident of the draw being sixteen orders of magnitude wider than the span rather
  than by anything the code does.

Rejection sampling removes the dependence on that ratio, which is the point: the hand-rolled
form is not wrong, it is *conditionally* right, and the condition is invisible at the call site.

**A `bytes` call is three syscalls** (`fopen`/`fread`/`fclose` on /dev/urandom), so anything
producing *n* values buys its entropy in one draw rather than per value.

`use random` -- or `use random.Name` to bring one item in bare.

## Example

```
use random

fn main() {
    val a = random.bytes(16)
    val b = random.bytes(16)
    print(len(a))
    print(a == b)
    print(len(random.bytes(0)))
}
```

Output:

```
16
false
0
```

### `bytes`

```
pub native fn bytes(n: int) -> [byte]
```

`n` random bytes from the operating system's entropy pool, suitable for keys, tokens and
salts. `n` is 0 or more (a negative `n` is a runtime error); the call fails with a runtime
error if the entropy source cannot be read.

**`random_bytes` until 2026-10-24**, where `random.random_bytes(32)` said random twice. The
rename comes with its call style, because the two pull in opposite directions: the stutter is
bad qualified, but a *bare* `random_bytes(32)` was good -- it said what it answered without a
qualifier, where a bare `bytes(32)` says nothing at all. So this is reached as
`random.bytes(32)` and not imported by name, the same rule `json.format` and `metrics.add`
follow for a different reason.

## Bounded draws

### `below`

```
pub fn below(n: int) -> int
```

**A uniform integer from 0 up to but not including `n`.** The shape that indexes an array, and
the one the reference backend hand-rolled as `x % n`; `between(0, n - 1)` says the same thing
with an off-by-one to get wrong.

### `between`

```
pub fn between(low: int, high: int) -> int
```

**A uniform integer from `low` to `high`, both included.** A runtime error if `high` is below
`low`, or if the range holds more than 2^56 values -- a caller wanting a full-width random `int`
wants `bytes(8)` and its own reading of them, not a bounded draw.

**Not `int`**: `int` is a type keyword, so `random.int(3, 9)` does not parse -- the declaration
compiles and every call site is a syntax error, which is how this was found. `float`, `bool` and
`string` are keywords too, hence `fraction`, `boolean` and `text`.

### `fraction`

```
pub fn fraction() -> float
```

**A uniform float in `[0.0, 1.0)`** -- zero is possible, one is not.

53 bits over 2^53, which is every `float` the interval can represent and no more: `int(0, n) / n`
would reach 1.0 for the top draw and would also leave most representable values unreachable.

### `boolean`

```
pub fn boolean() -> bool
```

One uniform bit, true or false.

## Collections

### `choice`

```
pub fn choice<T>(items: [T]) -> Option<T>
```

**A uniform element of `items`, or None when it is empty.** An `Option` rather than a runtime
error because an empty array is an ordinary input a caller can hold without having made a
mistake -- the same reading `get` and `lookup` take.

### `shuffle`

```
pub fn shuffle<T>(items: [T]) -> [T]
```

**`items` in a uniformly random order**, as a new array: a value in, a value out, like every
other array function here.

Fisher-Yates, walking down and swapping each position with one at or before it. Both naive
alternatives are biased and it is worth naming them: sorting by a random key gives a
distribution that depends on the sort's tie handling, and swapping with an index drawn from the
*whole* array rather than from `0..i` over-represents some orderings by a factor that grows
with the length.

One `bytes` call for the whole shuffle rather than one per position, which matters because each
is three syscalls; a rejected draw falls back to its own, which almost never happens.

## Text

### `text`

```
pub fn text(n: int, alphabet: string) -> string
```

**`n` characters drawn uniformly from `alphabet`**, the general form the three below are built
on. `n` is 0 or more; an empty `alphabet` with a positive `n` is a runtime error.

Uniform over the *characters* of `alphabet`, so a non-ASCII alphabet works and a repeated
character is simply twice as likely. One `bytes` call for the whole string.

### `alpha`

```
pub fn alpha(n: int) -> string
```

**`n` ASCII letters**, upper and lower case mixed -- exactly what the prelude's `is_alpha`
accepts, so `is_alpha(alpha(n))` is true for any positive `n`. The three text generators and the
three predicates are deliberately the same three questions.

### `numeric`

```
pub fn numeric(n: int) -> string
```

`n` ASCII digits, what `is_numeric` accepts.

### `alpha_numeric`

```
pub fn alpha_numeric(n: int) -> string
```

`n` ASCII letters or digits, what `is_alpha_numeric` accepts.
