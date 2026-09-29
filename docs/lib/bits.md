# `bits`

Bitwise and/or/xor/not/shift on `byte`, `int` and `bigint`, and the two
scalar-backed sets they back, `BitSet` (an `int`, at most 64 members) and `BigBitSet` (a
`bigint`, unbounded). Ziman has no bitwise operators in its own syntax (`&`, `|`, `^`, `~`,
`<<`, `>>` don't exist as tokens); this module is the only way to do bit manipulation.

`byte` is unsigned, so its six functions are the simplest here: no sign to extend, shifts
bounded to 0-7. `int`'s are direct native 64-bit two's-complement operations -- no surprises
beyond the sign. `bigint`'s match Java's `BigInteger`/Python's `int` exactly: `and`/`or`/`xor`/
`not` are defined against *infinite* two's complement, so a negative bigint conceptually has
infinitely many leading 1-bits (`bigint_and(-1n, 5n)` is `5n`, the same as Java/Python give).
`bigint_not(x)` is `-(x + 1)`, exactly Java's definition, needing no bit-level work at all.
`shift_right` floors (rounds toward negative infinity) for `int`/`bigint`, unlike `/` which
truncates toward zero: `int_shift_right(-3, 1)` is `-2`, not `-1` (`-3 / 2`); `byte_shift_right`
is a logical shift (no sign to preserve), so floor vs. truncate doesn't apply to it.

`use bits` -- or `use bits.Name` to bring one item in bare.

### `byte_and`

```
pub native fn byte_and(a: byte, b: byte) -> byte
```

### `byte_or`

```
pub native fn byte_or(a: byte, b: byte) -> byte
```

### `byte_xor`

```
pub native fn byte_xor(a: byte, b: byte) -> byte
```

### `byte_not`

```
pub native fn byte_not(a: byte) -> byte
```

### `byte_shift_left`

```
pub native fn byte_shift_left(a: byte, n: int) -> byte
```

`n` must be 0 to 7 -- a byte is 8 bits wide.

### `byte_shift_right`

```
pub native fn byte_shift_right(a: byte, n: int) -> byte
```

### `byte_popcount`

```
pub native fn byte_popcount(a: byte) -> int
```

### `int_and`

```
pub native fn int_and(a: int, b: int) -> int
```

### `int_or`

```
pub native fn int_or(a: int, b: int) -> int
```

### `int_xor`

```
pub native fn int_xor(a: int, b: int) -> int
```

### `int_not`

```
pub native fn int_not(a: int) -> int
```

### `int_shift_left`

```
pub native fn int_shift_left(a: int, n: int) -> int
```

`n` must be 0 to 63; a shift amount outside that range is a runtime error rather than the
undefined behavior C itself gives a native shift by the operand's own bit width or more.

### `int_shift_right`

```
pub native fn int_shift_right(a: int, n: int) -> int
```

### `bigint_and`

```
pub native fn bigint_and(a: bigint, b: bigint) -> bigint
```

### `bigint_or`

```
pub native fn bigint_or(a: bigint, b: bigint) -> bigint
```

### `bigint_xor`

```
pub native fn bigint_xor(a: bigint, b: bigint) -> bigint
```

### `bigint_not`

```
pub native fn bigint_not(a: bigint) -> bigint
```

### `bigint_shift_left`

```
pub native fn bigint_shift_left(a: bigint, n: int) -> bigint
```

`n` must not be negative; there is no upper bound of its own (an enormous `n` just produces
an enormous result, the same "no artificial cap on ordinary arithmetic" rule any other bigint
operation already follows).

### `bigint_shift_right`

```
pub native fn bigint_shift_right(a: bigint, n: int) -> bigint
```

### `int_popcount`

```
pub native fn int_popcount(a: int) -> int
```

### `bigint_popcount`

```
pub native fn bigint_popcount(a: bigint) -> int
```

Both are undefined (a runtime error) for a negative bigint -- infinite two's complement gives
a negative number infinitely many 1-bits, so there is no finite popcount or bit length to
return. `BigBitSet` below never produces one, since a set of indices is never negative.

### `bigint_bit_length`

```
pub native fn bigint_bit_length(a: bigint) -> int
```

## BitSet: a set of the integers 0..63, backed by one `int`

### `BitSet`

```
pub struct BitSet {
    bits: int
}
```

### `bitset_new`

```
pub fn bitset_new() -> BitSet
```

### `bitset_of_int`

```
pub fn bitset_of_int(x: int) -> BitSet
```

### `bitset_to_int`

```
pub fn bitset_to_int(bs: BitSet) -> int
```

### `bitset_set`

```
pub fn bitset_set(bs: BitSet, i: int) -> BitSet
```

### `bitset_clear`

```
pub fn bitset_clear(bs: BitSet, i: int) -> BitSet
```

### `bitset_flip`

```
pub fn bitset_flip(bs: BitSet, i: int) -> BitSet
```

### `bitset_get`

```
pub fn bitset_get(bs: BitSet, i: int) -> bool
```

### `bitset_and`

```
pub fn bitset_and(a: BitSet, b: BitSet) -> BitSet
```

### `bitset_or`

```
pub fn bitset_or(a: BitSet, b: BitSet) -> BitSet
```

### `bitset_xor`

```
pub fn bitset_xor(a: BitSet, b: BitSet) -> BitSet
```

### `bitset_and_not`

```
pub fn bitset_and_not(a: BitSet, b: BitSet) -> BitSet
```

`a` with every member of `b` removed.

### `bitset_is_empty`

```
pub fn bitset_is_empty(bs: BitSet) -> bool
```

### `bitset_cardinality`

```
pub fn bitset_cardinality(bs: BitSet) -> int
```

### `bitset_length`

```
pub fn bitset_length(bs: BitSet) -> int
```

The highest member plus one, or 0 for an empty set -- java.util.BitSet's own `length()`.

### `bitset_to_string`

```
pub fn bitset_to_string(bs: BitSet) -> string
```

"{0, 3, 5}" -- the indices in order, java.util.BitSet's own `toString()` shape.

## BigBitSet: an unbounded set of non-negative integers, backed by one `bigint`

### `BigBitSet`

```
pub struct BigBitSet {
    bits: bigint
}
```

### `big_bitset_new`

```
pub fn big_bitset_new() -> BigBitSet
```

### `big_bitset_of_bigint`

```
pub fn big_bitset_of_bigint(x: bigint) -> BigBitSet
```

### `big_bitset_to_bigint`

```
pub fn big_bitset_to_bigint(bs: BigBitSet) -> bigint
```

### `big_bitset_set`

```
pub fn big_bitset_set(bs: BigBitSet, i: int) -> BigBitSet
```

### `big_bitset_clear`

```
pub fn big_bitset_clear(bs: BigBitSet, i: int) -> BigBitSet
```

### `big_bitset_flip`

```
pub fn big_bitset_flip(bs: BigBitSet, i: int) -> BigBitSet
```

### `big_bitset_get`

```
pub fn big_bitset_get(bs: BigBitSet, i: int) -> bool
```

### `big_bitset_and`

```
pub fn big_bitset_and(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

### `big_bitset_or`

```
pub fn big_bitset_or(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

### `big_bitset_xor`

```
pub fn big_bitset_xor(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

### `big_bitset_and_not`

```
pub fn big_bitset_and_not(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

### `big_bitset_is_empty`

```
pub fn big_bitset_is_empty(bs: BigBitSet) -> bool
```

### `big_bitset_cardinality`

```
pub fn big_bitset_cardinality(bs: BigBitSet) -> int
```

### `big_bitset_length`

```
pub fn big_bitset_length(bs: BigBitSet) -> int
```

The highest member plus one, or 0 for an empty set -- same definition as `bitset_length`,
but via `bigint_bit_length` (a loop with no fixed bound to stop at would be far slower for a
set with a very large member) rather than a 64-iteration scan.

### `big_bitset_to_string`

```
pub fn big_bitset_to_string(bs: BigBitSet) -> string
```

"{0, 3, 5}" -- same shape as `bitset_to_string`. Scans by shifting a working copy right one
bit at a time and testing the low bit, rather than calling `big_bitset_get(bs, i)` per index:
that would recompute `1n << i` from scratch for every i, which for a high member is
O(length log length) bignum multiplications instead of this loop's O(length) single-bit shifts.
