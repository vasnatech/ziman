# `bits`

Bitwise and/or/xor/not/shift on `byte`, `int` and `bigint`, and the two
scalar-backed sets they back, `BitSet` (an `int`, at most 64 members) and `BigBitSet` (a
`bigint`, unbounded). Ziman has no bitwise operators in its own syntax (`&`, `|`, `^`, `~`,
`<<`, `>>` don't exist as tokens); this module is the only way to do bit manipulation.

**Names carry no type.** `and(a, b)` is one overload set over all five types -- three scalars
and both sets -- because the arguments say which is meant; so are `or`, `xor`, `not`,
`shift_left`, `shift_right`, `popcount` and `bit_length`. The two sets additionally speak the
prelude's own collection vocabulary, so a `BitSet` needs no API of its own to learn: `len`,
`is_empty`, `remove`, `union`, `intersect`, `difference`, `is_subset`, `to_array` and `format`
mean here exactly what they mean for a `Set<int>`, and `set`/`get`/`flip` are the bit-indexed
accessors java.util.BitSet names the same way. Only the constructors keep a prefix
(`bitset_new`, `bitset_of_int`, `bitset_to_int` and their `big_bitset_` twins), since nothing
but the return type distinguishes them.

**A caller picks one of two spellings**, and the short names are why it matters. `use bits` then
`bits.len(bs)` leaves every prelude name alone. `use bits.len` binds the bare name to *this*
`len` for the whole file, which is what per-name import means -- the prelude's `len` is then
reached as `prelude.len(text)`, and the same goes for `set`, `get`, `format`, `remove`,
`is_empty`, `union`, `intersect`, `difference`, `is_subset` and `to_array`.

`byte` is unsigned, so its operations are the simplest here: no sign to extend, shifts bounded
to 0-7. `int`'s are direct native 64-bit two's-complement operations -- no surprises beyond the
sign. `bigint`'s match Java's `BigInteger`/Python's `int` exactly: `and`/`or`/`xor`/`not` are
defined against *infinite* two's complement, so a negative bigint conceptually has infinitely
many leading 1-bits (`and(-1n, 5n)` is `5n`, the same as Java/Python give). `not(x)` on a bigint
is `-(x + 1)`, exactly Java's definition, needing no bit-level work at all. `shift_right` floors
(rounds toward negative infinity) for `int`/`bigint`, unlike `/` which truncates toward zero:
`shift_right(-3, 1)` is `-2`, not `-1` (`-3 / 2`); a `byte`'s is a logical shift (no sign to
preserve), so floor vs. truncate doesn't apply to it.

`use bits` -- or `use bits.Name` to bring one item in bare.

## Example

```
use bits

// Bitwise operations, and a bit set built on them. Ziman has no `&`, `|`, `^`, `<<` or `>>`
// operators: the operations are named functions instead, so a line that manipulates bits reads as
// one and cannot be confused with the logical operators it resembles in other languages.
fn demo() -> Result<unit, string> {
    // The same four names work on `byte`, `int` and `bigint`.
    print(bits.and(12, 10))
    print(bits.or(12, 10))
    print(bits.xor(12, 10))
    print(bits.and(255b, 15b))

    // A shift amount outside 0..63 is a runtime error rather than the undefined behaviour C gives
    // a shift by the operand's own width or more.
    print(bits.shift_left(1, 10))
    print(bits.shift_right(1024, 3))
    print(bits.popcount(255))
    print(bits.bit_length(1024n))

    // A bit set over the bits of an int, for flags that would otherwise be a handful of booleans.
    var flags = bits.bitset_new()
    flags = bits.set(flags, 0)
    flags = bits.set(flags, 3)
    flags = bits.set(flags, 7)
    print(bits.format(flags))
    print(bits.get(flags, 3))
    print(bits.len(flags))
    print(bits.to_array(flags))
    print(bits.bitset_to_int(flags))

    // Sets combine the way sets do, so a mask is an intersection rather than an `&` to decode.
    val low = bits.bitset_of_int(15)
    print(bits.to_array(bits.intersect(flags, low)))
    print(bits.to_array(bits.difference(flags, low)))
    print(bits.is_subset(bits.bitset_of_int(9), flags))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("bits example failed: {}\n", e)
    }
}
```

Output:

```
8
14
6
15
1024
128
8
11
{0, 3, 7}
true
3
[0, 3, 7]
137
[0, 3]
[7]
true
```

### `and`

```
pub native fn and(a: byte, b: byte) -> byte
```

Bitwise AND, OR, XOR and complement of bytes.

### `or`

```
pub native fn or(a: byte, b: byte) -> byte
```

### `xor`

```
pub native fn xor(a: byte, b: byte) -> byte
```

### `not`

```
pub native fn not(a: byte) -> byte
```

### `and`

```
pub native fn and(a: int, b: int) -> int
```

Bitwise AND, OR, XOR and complement of ints (two's complement, 64-bit).

### `or`

```
pub native fn or(a: int, b: int) -> int
```

### `xor`

```
pub native fn xor(a: int, b: int) -> int
```

### `not`

```
pub native fn not(a: int) -> int
```

### `and`

```
pub native fn and(a: bigint, b: bigint) -> bigint
```

Bitwise AND, OR, XOR and complement of bigints, against infinite two's complement (see the
file comment above).

### `or`

```
pub native fn or(a: bigint, b: bigint) -> bigint
```

### `xor`

```
pub native fn xor(a: bigint, b: bigint) -> bigint
```

### `not`

```
pub native fn not(a: bigint) -> bigint
```

### `shift_left`

```
pub native fn shift_left(a: byte, n: int) -> byte
```

`n` must be 0 to 7 -- a byte is 8 bits wide.

### `shift_right`

```
pub native fn shift_right(a: byte, n: int) -> byte
```

### `shift_left`

```
pub native fn shift_left(a: int, n: int) -> int
```

`n` must be 0 to 63; a shift amount outside that range is a runtime error rather than the
undefined behavior C itself gives a native shift by the operand's own bit width or more.

### `shift_right`

```
pub native fn shift_right(a: int, n: int) -> int
```

### `shift_left`

```
pub native fn shift_left(a: bigint, n: int) -> bigint
```

`n` must not be negative; there is no upper bound of its own (an enormous `n` just produces
an enormous result, the same "no artificial cap on ordinary arithmetic" rule any other bigint
operation already follows).

### `shift_right`

```
pub native fn shift_right(a: bigint, n: int) -> bigint
```

### `popcount`

```
pub native fn popcount(a: byte) -> int
```

The number of 1 bits in `a`.

```
pub native fn popcount(a: int) -> int
```

The number of 1 bits in `a`'s 64-bit two's-complement representation (a negative `a` is fine
here, unlike the `bigint` member below -- a fixed width has no infinitely-many leading 1-bits
to worry about).

```
pub native fn popcount(a: bigint) -> int
```

Both are undefined (a runtime error) for a negative bigint -- infinite two's complement gives
a negative number infinitely many 1-bits, so there is no finite popcount or bit length to
return. `BigBitSet` below never produces one, since a set of indices is never negative.

### `bit_length`

```
pub native fn bit_length(a: bigint) -> int
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

The empty set.

### `bitset_of_int`

```
pub fn bitset_of_int(x: int) -> BitSet
```

The set whose membership is `x`'s bit pattern (bit `i` set means `i` is a member).

### `bitset_to_int`

```
pub fn bitset_to_int(bs: BitSet) -> int
```

`bs`'s membership as a raw bit pattern.

### `set`

```
pub fn set(bs: BitSet, i: int) -> BitSet
```

`bs` with `i` added.

### `remove`

```
pub fn remove(bs: BitSet, i: int) -> BitSet
```

`bs` with `i` removed.

### `flip`

```
pub fn flip(bs: BitSet, i: int) -> BitSet
```

`bs` with `i`'s membership toggled.

### `get`

```
pub fn get(bs: BitSet, i: int) -> bool
```

Whether `i` is a member of `bs`.

### `intersect`

```
pub fn intersect(sets: variadic BitSet) -> BitSet
```

The intersection of every argument -- `Set<T>`'s own `intersect`, over `BitSet`.

### `union`

```
pub fn union(sets: variadic BitSet) -> BitSet
```

The union of every argument.

### `difference`

```
pub fn difference(a: BitSet, b: BitSet) -> BitSet
```

`a` with every member of `b` removed.

### `is_subset`

```
pub fn is_subset(a: BitSet, b: BitSet) -> bool
```

Whether every member of `a` is also a member of `b`.

### `xor`

```
pub fn xor(a: BitSet, b: BitSet) -> BitSet
```

The symmetric difference of `a` and `b` (members of exactly one). Keeps the bitwise name: a
`Set<T>` has no symmetric-difference function to borrow one from.

### `not`

```
pub fn not(bs: BitSet) -> BitSet
```

The complement of `bs` within 0..63.

### `is_empty`

```
pub fn is_empty(bs: BitSet) -> bool
```

Whether `bs` has no members.

### `len`

```
pub fn len(bs: BitSet) -> int
```

The number of members of `bs`.

### `bit_length`

```
pub fn bit_length(bs: BitSet) -> int
```

The highest member plus one, or 0 for an empty set -- java.util.BitSet's own `length()`, and
the same question `bit_length` answers for a `bigint`.

### `to_array`

```
pub fn to_array(bs: BitSet) -> [int]
```

`bs`'s members in ascending order -- `Set<T>`'s own `to_array`, except that this one is sorted,
a bit index having a natural order where a hash set's iteration does not.

### `format`

```
pub fn format(bs: BitSet) -> string
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

The empty set.

### `big_bitset_of_bigint`

```
pub fn big_bitset_of_bigint(x: bigint) -> BigBitSet
```

The set whose membership is `x`'s bit pattern (bit `i` set means `i` is a member).

### `big_bitset_to_bigint`

```
pub fn big_bitset_to_bigint(bs: BigBitSet) -> bigint
```

`bs`'s membership as a raw bit pattern.

### `set`

```
pub fn set(bs: BigBitSet, i: int) -> BigBitSet
```

`bs` with `i` added.

### `remove`

```
pub fn remove(bs: BigBitSet, i: int) -> BigBitSet
```

`bs` with `i` removed.

### `flip`

```
pub fn flip(bs: BigBitSet, i: int) -> BigBitSet
```

`bs` with `i`'s membership toggled.

### `get`

```
pub fn get(bs: BigBitSet, i: int) -> bool
```

Whether `i` is a member of `bs`.

### `intersect`

```
pub fn intersect(sets: variadic BigBitSet) -> BigBitSet
```

The intersection of every argument.

### `union`

```
pub fn union(sets: variadic BigBitSet) -> BigBitSet
```

The union of every argument.

### `difference`

```
pub fn difference(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

`a` with every member of `b` removed.

### `is_subset`

```
pub fn is_subset(a: BigBitSet, b: BigBitSet) -> bool
```

Whether every member of `a` is also a member of `b`.

### `xor`

```
pub fn xor(a: BigBitSet, b: BigBitSet) -> BigBitSet
```

The symmetric difference of `a` and `b` (members of exactly one).

### `is_empty`

```
pub fn is_empty(bs: BigBitSet) -> bool
```

Whether `bs` has no members.

### `len`

```
pub fn len(bs: BigBitSet) -> int
```

The number of members of `bs`.

### `bit_length`

```
pub fn bit_length(bs: BigBitSet) -> int
```

The highest member plus one, or 0 for an empty set -- same definition as `BitSet`'s, but via the
`bigint` member of `bit_length` (a loop with no fixed bound to stop at would be far slower for a
set with a very large member) rather than a 64-iteration scan.

### `to_array`

```
pub fn to_array(bs: BigBitSet) -> [int]
```

`bs`'s members in ascending order. Scans by shifting a working copy right one bit at a time and
testing the low bit, rather than calling `get(bs, i)` per index: that would recompute `1n << i`
from scratch for every i, which for a high member is O(length log length) bignum multiplications
instead of this loop's O(length) single-bit shifts.

### `format`

```
pub fn format(bs: BigBitSet) -> string
```

"{0, 3, 5}" -- same shape as `BitSet`'s.
