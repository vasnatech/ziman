# `prelude`

The functions and types every Ziman program has without a `use`: string
handling, number parsing, the `Option` type, and the builders that assemble strings, arrays,
maps and sets without the quadratic cost of adding to an immutable value in a loop. (`print`,
`len`, `push`, `format`, `ok`/`err` and the other language built-ins are not listed here; see
docs/SYNTAX.md.)

Indexes and lengths of strings are in bytes, not characters: the text is UTF-8, and
`char_at`, `substring` and `index_of` work on byte positions, so cutting inside a multi-byte
character gives text that is not valid UTF-8. `to_upper`, `to_lower` and `equals_ignore_case`
change only the ASCII letters A-Z.

*This module is never imported: everything it declares is available bare in every program.*

## Example

```
// Everything here is available without a `use`.
fn main() {
    // Parsing returns a Result, so bad text is a value you handle.
    print(parse_int("42"))
    print(parse_int("4x2"))
    print(round(2.5))
    print(decimal_round(1.235d, 2))

    // Strings are UTF-8 and positions are bytes.
    print(split("a,b,,c", ","))
    print(split_once("key=a=b", "="))
    print(replace("aaa", "a", "b"))
    print(join(prefix: "[", suffix: "]", separator: ", ", pieces: ["x", "y", "z"]))

    // Option is an ordinary enum.
    val found = split_once("no separator here", "=")
    print(option_unwrap_or<[string]>(found, ["", ""]))

    // Builders assemble a result without copying it on every step.
    val sb = sb_new()
    for i in 0..5 {
        sb_append(sb, str(i), ",")
    }
    print(sb_to_string(sb))
    val ab = ab_new<int>()
    for i in 0..5 {
        ab_append<int>(ab, i * i)
    }
    print(ab_to_array<int>(ab))
}
```

Output:

```
Ok(42)
Err(cannot parse '4x2' as int)
3
1.24
[a, b, , c]
Some([key, a=b])
bbb
[x, y, z]
[, ]
0,1,2,3,4,
[0, 1, 4, 9, 16]
```

## Parsing numbers

### `parse_int`

```
native fn parse_int(s: string) -> Result<int, string>
```

The number in `s` (surrounding blanks allowed), or an Err naming the text when it is not one
or does not fit: `parse_int("9223372036854775808")` is an Err. `parse_int` takes an optional
leading `+` or `-` and decimal digits only (no `0x`, no `_`).

### `parse_float`

```
native fn parse_float(s: string) -> Result<float, string>
```

A float from decimal or exponent text (`3.5`, `1e3`), or an Err.

### `parse_bigint`

```
native fn parse_bigint(s: string) -> Result<bigint, string>
```

A bigint from digits with an optional sign, or an Err. Any length.

### `parse_decimal`

```
native fn parse_decimal(s: string) -> Result<decimal, string>
```

A decimal from digits with an optional fraction, keeping the scale as written (`"1.50"` stays
`1.50`), or an Err.

### `floor`

```
native fn floor(x: float) -> int
```

The largest whole number not above `x`: `floor(-1.5)` is -2.

### `ceil`

```
native fn ceil(x: float) -> int
```

The smallest whole number not below `x`: `ceil(-1.5)` is -1.

### `round`

```
native fn round(x: float) -> int
```

`x` to the nearest whole number, halves away from zero: `round(2.5)` is 3, `round(-2.5)` is -3.

### `decimal_round`

```
native fn decimal_round(d: decimal, scale: int) -> decimal
```

`d` rounded to `scale` decimal places, halves away from zero: `decimal_round(1.235d, 2)` is 1.24.

## Strings

### `concat`

```
native fn concat(a: string, b: string) -> string
```

`a` followed by `b`. For many pieces use `join`, or a `StringBuilder` in a loop.

### `string_to_bytes`

```
native fn string_to_bytes(s: string) -> [byte]
```

The UTF-8 bytes of `s`.

### `bytes_to_string`

```
native fn bytes_to_string(b: [byte]) -> Result<string, string>
```

The text of `b`, or an Err when it holds a 0 byte (a string cannot).

### `bytes_concat`

```
fn bytes_concat(parts: [byte]...) -> [byte]
```

Any number of byte arrays joined into one, in a single pass.

### `substring`

```
native fn substring(s: string, start: int, end: int) -> string
```

The bytes of `s` from `start` up to, not including, `end` (`0 <= start <= end <= len(s)`, a
runtime error otherwise).

### `split`

```
native fn split(s: string, sep: string) -> [string]
```

The pieces of `s` between each `sep`, empty pieces included: `split("a,b,,c", ",")` is
`[a, b, , c]`. `sep` must not be empty.

### `trim`

```
native fn trim(s: string) -> string
```

`s` without leading and trailing blanks (space, tab, CR, LF).

### `char_at`

```
native fn char_at(s: string, i: int) -> string
```

The one-byte string at position `i` (`0 <= i < len(s)`, a runtime error otherwise), so
`char_at(s, i) == "x"` replaces `substring(s, i, i + 1) == "x"`. It is a byte, not a character.

### `last_index_of`

```
native fn last_index_of(s: string, needle: string) -> int
```

The byte position of the last `needle` in `s`, or -1. An empty `needle` is found at `len(s)`.

### `is_char_at`

```
native fn is_char_at(s: string, i: int, c: string) -> bool
```

True when `c` appears in `s` starting at position `i`. False, never an error, when `i` is out
of range or `c` does not fit, so `is_char_at(s, p, "x")` replaces the guarded
`p < len(s) && char_at(s, p) == "x"`. `c` is normally one character but any text works.

### `equals_ignore_case`

```
native fn equals_ignore_case(a: string, b: string) -> bool
```

True when `a` and `b` are equal when the ASCII letters A-Z are read as a-z (other bytes,
non-ASCII included, must match exactly). Builds no intermediate strings, unlike comparing
`to_lower(a)` and `to_lower(b)`.

### `index_of`

```
native fn index_of(s: string, needle: string, from: int) -> int
```

The byte position of the first `needle` in `s` at or after `from` (`0 <= from <= len(s)`, a
runtime error otherwise), or -1. An empty `needle` is found at `from`.

### `starts_with`

```
native fn starts_with(s: string, prefix: string) -> bool
```

True when `s` begins with `prefix`.

### `ends_with`

```
native fn ends_with(s: string, suffix: string) -> bool
```

True when `s` ends with `suffix`.

### `replace`

```
native fn replace(s: string, old: string, new: string) -> string
```

`s` with every non-overlapping `old` replaced by `new`. `old` must not be empty.

### `to_upper`

```
native fn to_upper(s: string) -> string
```

`s` with the ASCII letters a-z made upper case; other bytes are unchanged.

### `to_lower`

```
native fn to_lower(s: string) -> string
```

`s` with the ASCII letters A-Z made lower case; other bytes are unchanged.

### `join`

```
native fn join(prefix: Option<string>, suffix: Option<string>, separator: Option<string>, pieces: string...) -> string
```

The strings `pieces` joined into one in a single pass, given one by one
(`join(pieces: "a", "b", "c")`) or as an array (`join(pieces: arr)`). `separator` goes between
the pieces, and `prefix` and `suffix` around the whole; each is optional in a named call:
`join(prefix: "[", suffix: "]", separator: ", ", pieces: arr)` is `[a, b, c]`. Written as a
native so that joining N strings is O(N), where adding them with `concat` one at a time is not.

### `split_once`

```
fn split_once(s: string, sep: string) -> Option<[string]>
```

Splits `s` at the FIRST `sep`: Some([before, after]), or None when `sep` is not in `s`. The
right tool for `key=value`, `host:port` and similar, where the value may itself contain the
separator (`split` would cut it apart). `sep` must not be empty.

## Option

### `Option`

```
enum Option<T> {
    Some(T)
    None
}
```

A value that may be absent: `Some(value)` or `None`. Test with `Option.is_Some(o)` and read
with `Option.unwrap_Some(o)`, or with a `switch`. Available bare, like `Result`; unlike
`Result` it is ordinary Ziman source, an enum with one helper function.

### `option_unwrap_or`

```
fn option_unwrap_or<T>(o: Option<T>, default_value: T) -> T
```

The value inside `o`, or `default_value` when `o` is None.

## StringBuilder

### `StringBuilder`

```
struct StringBuilder {
    handle: int
}
```

A mutable buffer for assembling a string piece by piece. Adding to a string with `concat` in
a loop copies the whole string each time and leaves the old copies as garbage; appending to a
StringBuilder is amortized O(1). `handle` is an opaque reference to the native buffer: do not
build or edit one directly, only through `sb_new` and the `sb_*` functions. Not safe to share
between threads.

### `sb_new`

```
native fn sb_new(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> StringBuilder
```

An empty builder. `capacity` is the bytes to reserve up front and `grow_factor` the multiple
its capacity grows by when an append would overflow it; both are optional, so `sb_new()` is
usually all that is written.

### `sb_append`

```
native fn sb_append(sb: StringBuilder, pieces: string...)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place and
nothing is returned.

### `sb_append_byte`

```
native fn sb_append_byte(sb: StringBuilder, b: byte...) -> bool
```

Appends raw bytes, for text assembled from decoded bytes (a percent-decoder, say):
`sb_append_byte(sb, 195b, 169b)`. A string cannot hold a 0 byte, so if any byte is `0b` NOTHING
is appended and the result is false (true otherwise). The bytes need not be valid UTF-8 on
their own.

### `sb_to_string`

```
native fn sb_to_string(sb: StringBuilder) -> string
```

The text appended so far, as an ordinary string. It is a copy: appending afterwards is fine,
and it can be called more than once. (`str(sb)` would print the struct, not the text.)

### `sb_len`

```
native fn sb_len(sb: StringBuilder) -> int
```

The number of bytes appended so far, without building the string.

## ArrayBuilder

### `ArrayBuilder`

```
struct ArrayBuilder<T> {
    handle: int
}
```

The array counterpart of StringBuilder: `push` copies the whole array on every call, so
pushing in a loop is quadratic, while appending to an ArrayBuilder is amortized O(1). Same
opaque `handle`, same one-thread-at-a-time rule.

### `concat_arrays`

```
native fn concat_arrays<T>(parts: [T]...) -> [T]
```

One array from many, in a single allocation: `concat_arrays<int>(a, b, c)` or
`concat_arrays<int>(parts: arrays)`. The array counterpart of `join`.

### `slice`

```
native fn slice<T>(a: [T], start: int, end: int) -> [T]
```

The elements of `a` from `start` up to, not including, `end` (`0 <= start <= end <= len(a)`, a
runtime error otherwise). It does not copy: arrays are immutable, so the result shares `a`'s
storage, like `substring`. That makes it free to build and keeps the whole of `a` alive.

### `ab_new`

```
native fn ab_new<T>(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> ArrayBuilder<T>
```

An empty builder; `capacity` and `grow_factor` are as in `sb_new`.

### `ab_append`

```
native fn ab_append<T>(ab: ArrayBuilder<T>, pieces: T...)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place.

### `ab_to_array`

```
native fn ab_to_array<T>(ab: ArrayBuilder<T>) -> [T]
```

The elements appended so far, as an ordinary array. It is a copy: appending afterwards is
fine, and it can be called more than once.

### `ab_len`

```
native fn ab_len<T>(ab: ArrayBuilder<T>) -> int
```

The number of elements appended so far.

## MapBuilder and SetBuilder

### `MapBuilder`

```
struct MapBuilder<K: Eq + Hash, V> {
    handle: int
}
```

The mutable counterparts of Map and Set, for building one entry by entry. `set` and `insert` on
a Map copy a few nodes each time; a builder changes its own private structure in place, so
adding n entries allocates O(n) in all, not O(n) per entry. `mapb_to_map` and `setb_to_set`
snapshot what has been added so far into an ordinary Map or Set, and the builder stays usable.
Keys keep the order they were first added, and follow the same rule as a Map's (`Eq + Hash`).
One thread at a time.

### `SetBuilder`

```
struct SetBuilder<T: Eq + Hash> {
    handle: int
}
```

A mutable set under construction; see MapBuilder.

### `mapb_new`

```
native fn mapb_new<K: Eq + Hash, V>() -> MapBuilder<K, V>
```

An empty map builder.

### `mapb_set`

```
native fn mapb_set<K: Eq + Hash, V>(mb: MapBuilder<K, V>, key: K, value: V)
```

Adds `key`, or replaces its value while keeping its position.

### `mapb_get`

```
native fn mapb_get<K: Eq + Hash, V>(mb: MapBuilder<K, V>, key: K) -> Option<V>
```

The value of `key`, or None.

### `mapb_remove`

```
native fn mapb_remove<K: Eq + Hash, V>(mb: MapBuilder<K, V>, key: K)
```

Removes `key`; nothing happens when it is absent.

### `mapb_contains`

```
native fn mapb_contains<K: Eq + Hash, V>(mb: MapBuilder<K, V>, key: K) -> bool
```

True when `key` has been added.

### `mapb_len`

```
native fn mapb_len<K: Eq + Hash, V>(mb: MapBuilder<K, V>) -> int
```

The number of keys.

### `mapb_to_map`

```
native fn mapb_to_map<K: Eq + Hash, V>(mb: MapBuilder<K, V>) -> Map<K, V>
```

The entries added so far, as an ordinary Map.

### `setb_new`

```
native fn setb_new<T: Eq + Hash>() -> SetBuilder<T>
```

An empty set builder.

### `setb_add`

```
native fn setb_add<T: Eq + Hash>(sb: SetBuilder<T>, item: T)
```

Adds `item`; nothing happens when it is already there.

### `setb_remove`

```
native fn setb_remove<T: Eq + Hash>(sb: SetBuilder<T>, item: T)
```

Removes `item`; nothing happens when it is absent.

### `setb_contains`

```
native fn setb_contains<T: Eq + Hash>(sb: SetBuilder<T>, item: T) -> bool
```

True when `item` has been added.

### `setb_len`

```
native fn setb_len<T: Eq + Hash>(sb: SetBuilder<T>) -> int
```

The number of items.

### `setb_to_set`

```
native fn setb_to_set<T: Eq + Hash>(sb: SetBuilder<T>) -> Set<T>
```

The items added so far, as an ordinary Set.

## Iter

### `Iter`

```
struct Iter<T> {
    handle: int
}
```

A lazy source of values, produced one at a time as a `for` loop asks for them: `for line in
lines { ... }` over an `Iter<Result<string, string>>` from `fs.lines`. Nothing is read ahead, so
the source can be far larger than memory, and stopping early (`break`, `return`, or `?`) leaves the
rest unread. `handle` is an opaque reference to the native source; a source that is not finished
or closed is closed when it is garbage collected. It can be walked once: a second `for` over the
same Iter carries on where the first stopped.

### `iter_next`

```
native fn iter_next<T>(it: Iter<T>) -> Option<T>
```

The next item of `it`, or None when it is exhausted. For a `while` loop that wants the items one
by one; `for` does this for you.

### `iter_close`

```
native fn iter_close<T>(it: Iter<T>)
```

Releases what `it` holds (a file, say) now instead of when it is collected. A `for` loop does this
itself when it is left early; call it when you take items by hand and stop before the end. Harmless
on one that is finished or already closed.

## Bytes

### `bytes_index_of`

```
native fn bytes_index_of(haystack: [byte], needle: [byte], from: int) -> int
```

The position of the first occurrence of `needle` in `haystack` at or after `from`, or -1; an
empty `needle` is found at `from`. For finding a delimiter (a CRLF CRLF, a boundary) in bytes.

## Used by the bundled modules

### `is_sortable_scalars`

```
native fn is_sortable_scalars<T>(items: [T]) -> bool
```

True for a packed `[int]`, `[float]` or `[byte]`, or an array of strings; `sort_scalar_order`
then returns the positions of `items` in stable sorted order (descending with `descending`)
without calling back into the interpreter.

### `sort_scalar_order`

```
native fn sort_scalar_order<T>(items: [T], descending: bool) -> [int]
```

The positions of `items` in stable sorted order; see `is_sortable_scalars`.

### `sort_scalars`

```
native fn sort_scalars<T>(items: [T], descending: bool) -> [T]
```

`items` itself sorted, with no index array in between.

### `permute`

```
native fn permute<T>(items: [T], order: [int]) -> [T]
```

`items[order[0]], items[order[1]], ...` for any element type, in one allocation.

### `sort_positions`

```
native fn sort_positions(n: int, cmp: fn(int, int) -> int) -> [int]
```

The positions 0..n ordered by `cmp(a, b)` (negative, zero or positive, like `compare`, over
two positions): a stable merge sort in two reused buffers.

### `array_map`

```
native fn array_map<T, U>(items: [T], f: fn(T) -> U) -> [U]
```

The natives behind `collections.map`, `filter`, `flat_map` and `reverse`: one allocation for
the result instead of an ArrayBuilder's growing buffers.

### `array_filter`

```
native fn array_filter<T>(items: [T], keep: fn(T) -> bool) -> [T]
```

See `array_map`.

### `array_flat_map`

```
native fn array_flat_map<T, U>(items: [T], f: fn(T) -> [U]) -> [U]
```

See `array_map`.

### `array_reverse`

```
native fn array_reverse<T>(items: [T]) -> [T]
```

See `array_map`.

### `Guard`

```
struct Guard {
    handle: int
}
```

A guard sends `message` on `channel` when nothing references it any more: a database
connection holds one whose message is its "close" request, so a connection dropped without an
explicit close still ends the thread that owns its socket (once the garbage collector notices).

### `guard_new`

```
native fn guard_new<T>(ch: Channel<T>, message: T) -> Guard
```

A guard that sends `message` on `ch` when it is collected.
