# `random`

Randomness from the operating system.

One native function, reading real entropy from the OS (`/dev/urandom`). It is the source of
all non-determinism a program has besides the clock; there is deliberately no seedable
pseudo-random generator (a program that wants one can build it on these bytes). `uuid` is
built on it. See docs/DESIGN.md's "random" entry.

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
