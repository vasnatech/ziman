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

    // Strings are UTF-8 and positions are bytes.
    print(split("a,b,,c", ","))
    print(split_once("key=a=b", "="))
    print(replace("aaa", "a", "b"))
    print(join(prefix: "[", suffix: "]", separator: ", ", pieces: ["x", "y", "z"]))

    // Option is an ordinary enum.
    val found = split_once("no separator here", "=")
    print(option_unwrap_or<[string]>(found, ["", ""]))

    // Builders assemble a result without copying it on every step.
    val sb = mutable_string_new()
    for i in 0..5 {
        mutable_string_append(sb, str(i), ",")
    }
    print(snapshot(sb))
    val ab = mutable_array_new<int>()
    for i in 0..5 {
        mutable_array_push<int>(ab, i * i)
    }
    print(snapshot(ab))
}
```

Output:

```
Ok(42)
Err(cannot parse '4x2' as int)
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

The number in `s`, or an Err naming the text when it is not one or does not fit:
`parse_int("9223372036854775808")` is an Err. `parse_int` takes an optional leading `-` and decimal
digits only: no blanks around the number, no `+`, no `0x`, no `_` (`trim` first to allow blanks).

### `parse_float`

```
native fn parse_float(s: string) -> Result<float, string>
```

A float from decimal or exponent text (`3.5`, `1e3`), or an Err: like `parse_int`, no blanks and no `+`, and
no hex (`0x10`). `nan`, `inf` and `-inf` (what `str` prints) are the non-finite floats it reads.
Anything `str` writes reads back, including a subnormal such as `5e-324`. Text naming a value
smaller than any subnormal (`1e-400`) rounds to `0.0`, the same rounding that makes `0.1` inexact;
text naming one too large (`1e400`) is an Err rather than silently becoming infinity, which is why
`inf` is accepted by name but `1e400` is not.

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

## Strings

### `format_dynamic`

```
native fn format_dynamic(template: string, args: [string]) -> Result<string, string>
```

`format` for a template that is a value -- read from a message catalog, say, or a translation -- with string arguments:
`format_dynamic("{} of {}", ["3", "10"])` is `Ok("3 of 10")`. `{}` is the next argument and `{{`/`}}` are literal braces, as
in `format`. `format` checks its literal template and the number of its arguments before the program runs; this cannot, so it
is an Err (never a crash) if the template is malformed, has a different number of placeholders than arguments, or has a
precision (`{:.2}`: the arguments are strings, so there is nothing to round).

### `concat`

```
native fn concat(pieces: variadic string) -> string
```

Every piece, in order, joined with no separator (`concat("a", "b", "c")` is `"abc"`; zero
pieces is `""`). For a separator or a prefix/suffix use `join`; for many pieces built up over
a loop, a `MutableString` (a fresh `concat` on every iteration copies everything so far, same
quadratic cost `--lint` already flags for `x = concat(x, ..)`).

### `string_to_bytes`

```
native fn string_to_bytes(s: string) -> [byte]
```

The UTF-8 bytes of `s`.

### `bytes_to_string`

```
native fn bytes_to_string(b: [byte]) -> Result<string, string>
```

The text of `b`, or an Err when `b` is not valid UTF-8 (naming the byte offset of the first
invalid sequence) or holds a 0 byte (a string cannot) -- the one real boundary where untrusted
bytes (a socket, a file, an HTTP body) become a `string`, so it's the one place this is
checked; every other string function keeps working on whatever bytes a `string` already holds,
same as always. See docs/DESIGN.md's UTF-8 correctness entry.

### `bytes_concat`

```
fn bytes_concat(parts: variadic [byte]) -> [byte]
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

### `is_empty`

```
native fn is_empty(s: string) -> bool
```

Whether `s` has no characters: `s == ""`.

### `is_blank`

```
native fn is_blank(s: string) -> bool
```

Whether `s` is empty or only blanks (whatever `trim` removes): `trim(s) == ""`, without making the trimmed string.

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

## Code points

### `chars`

```
native fn chars(s: string) -> [string]
```

`s` split into its individual code points, each its own one-character string:
`chars("héllo")` is `[h, é, l, l, o]`, not six one-byte pieces. A runtime error naming the
byte offset if `s` is not valid UTF-8 (only reachable by chaining a byte-level function like
`substring` into this one -- `bytes_to_string` already rejects invalid UTF-8 at the boundary
where it would otherwise enter a program).

### `char_count`

```
native fn char_count(s: string) -> int
```

The number of code points in `s`: `char_count("héllo")` is 5, where `len("héllo")` (bytes) is
6. Cheaper than `len(chars(s))` when only the count is wanted. Same UTF-8 requirement as `chars`.

### `truncate_chars`

```
native fn truncate_chars(s: string, max_chars: int) -> string
```

`s` shortened to at most `max_chars` code points, without ever cutting one in half -- the safe
way to truncate text for display (`substring` counts bytes and can produce invalid UTF-8).
`max_chars >= char_count(s)` returns `s` unchanged; `max_chars` must not be negative. Same
UTF-8 requirement as `chars`.

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
native fn join(
    prefix: Option<string>,
    suffix: Option<string>,
    separator: Option<string>,
    pieces: variadic string
) -> string
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

### `Selected`

```
enum Selected<A, B> {
    First(A)
    Second(B)
}
```

Which of `select2`'s two channels had a value first, and the value: `First(a)` or `Second(b)`.
Named for what it is rather than a general `Either`, which programs commonly declare themselves
and a bare prelude `Either` would then collide with. Available bare, like `Option`.

### `option_unwrap_or`

```
fn option_unwrap_or<T>(o: Option<T>, default_value: T) -> T
```

The value inside `o`, or `default_value` when `o` is None.

## MutableString

### `MutableString`

```
struct MutableString {
    handle: int
}
```

A mutable buffer for assembling a string piece by piece. Adding to a string with `concat` in
a loop copies the whole string each time and leaves the old copies as garbage; appending to a
MutableString is amortized O(1). `handle` is an opaque reference to the native buffer: do not
build or edit one directly, only through `mutable_string_new` and the `sb_*` functions. It belongs to the
thread that made it (`#[thread_bound]`): it cannot be a `Channel`'s element, a `spawn` argument or a
module-level `val`, and using one from another thread (through a closure, say) is a runtime error.
Hand the finished string over, not the builder.

### `mutable_string_new`

```
native fn mutable_string_new(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> MutableString
```

An empty builder. `capacity` is the bytes to reserve up front and `grow_factor` the multiple
its capacity grows by when an append would overflow it; both are optional, so `mutable_string_new()` is
usually all that is written.

### `mutable_string_append`

```
native fn mutable_string_append(sb: MutableString, pieces: variadic string)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place and
nothing is returned.

### `mutable_string_append_byte`

```
native fn mutable_string_append_byte(sb: MutableString, b: variadic byte) -> bool
```

Appends raw bytes, for text assembled from decoded bytes (a percent-decoder, say):
`mutable_string_append_byte(sb, 195b, 169b)`. A string cannot hold a 0 byte, so if any byte is `0b` NOTHING
is appended and the result is false (true otherwise). The bytes need not be valid UTF-8 on
their own.

### `mutable_string_get`

```
native fn mutable_string_get(sb: MutableString, index: int) -> string
```

Reading and writing one byte of the buffer, and searching it: what lets a string being built be
inspected without `snapshot`, which copies all of it per call. Byte-indexed, like every string
operation in the language -- `len` is bytes and `char_at(s, i)` is the one-byte string at `i`, so
there is no UTF-8 question left to settle here.

### `mutable_string_set`

```
native fn mutable_string_set(sb: MutableString, index: int, value: string)
```

Replaces one byte, mirroring the immutable `set(arr, i, x)`: one element in, one element out. A
`value` of any other length is a runtime error rather than a splice, because splicing would
change the length and `set` nowhere else in the language does that.

### `mutable_string_index_of`

```
native fn mutable_string_index_of(sb: MutableString, needle: string, from: int) -> int
```

The byte index of the first `needle` at or after `from` (`0 <= from <= len(sb)`, a runtime error
otherwise), or -1. An empty `needle` is found at `from`. Takes a `from` because the immutable
`index_of(s, needle, from)` does -- mirroring the twin matters more than saving an argument.

### `mutable_string_last_index_of`

```
native fn mutable_string_last_index_of(sb: MutableString, needle: string) -> int
```

The byte index of the last `needle`, or -1; an empty `needle` is found at `len(sb)`. No `from`,
because `last_index_of(s, needle)` has none either.

## Mutable

### `Mutable`

```
struct Mutable<T> {
    handle: int
}
```

One cell holding one value: the root the other four mutable types specialise, and the primitive
that lets a new mutable shape be written in Ziman instead of added to the language. A ring buffer
is a struct over a `MutableArray` plus two `Mutable<int>`s; an LRU adds a `MutableMap`; a UI signal
is a cell plus a `MutableArray` of subscribers.

Like the other four it belongs to the thread that made it (`#[thread_bound]`), it is neither
comparable nor hashable, and it is always initialised -- there is no empty `mutable_new<T>()`,
because `T` may have no zero, so a cell never holds nothing.

It answers two of the four bare builtins and not the other two: `len(c)` is 1 and
`contains(c, v)` asks whether the value equals `v`, while `clear` has no zero to reset to and
`snapshot` names an O(n) copy where `mutable_get` is an O(1) read. A cell needs no
`from`/`snapshot` pair at all: `mutable_new` takes a value and `mutable_get` returns one, so the
crossing between the two worlds *is* the API.

### `mutable_new`

```
native fn mutable_new<T>(value: T) -> Mutable<T>
```

A cell holding `value`.

### `mutable_get`

```
native fn mutable_get<T>(c: Mutable<T>) -> T
```

The value in the cell.

### `mutable_set`

```
native fn mutable_set<T>(c: Mutable<T>, value: T)
```

Replaces the value in the cell.

### `mutable_update`

```
fn mutable_update<T>(c: Mutable<T>, f: fn(T) -> T) -> T
```

`f` applied to the value in the cell, stored back, and returned -- the counter idiom,
`mutable_update(n, fn(x) { x + 1 })`, and the same shape `mutable_map_update` has. Ordinary
Ziman over `mutable_get`/`mutable_set`, since there is no lookup for a native to save.

## MutableArray

### `MutableArray`

```
struct MutableArray<T> {
    handle: int
}
```

The array counterpart of MutableString: `push` copies the whole array on every call, so
pushing in a loop is quadratic, while appending to a MutableArray is amortized O(1). Same
opaque `handle`, same thread-bound rule.

### `concat_arrays`

```
native fn concat_arrays<T>(parts: variadic [T]) -> [T]
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

### `mutable_array_new`

```
native fn mutable_array_new<T>(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> MutableArray<T>
```

An empty builder; `capacity` and `grow_factor` are as in `mutable_string_new`.

### `mutable_array_push`

```
native fn mutable_array_push<T>(ab: MutableArray<T>, pieces: variadic T)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place.

### `mutable_array_get`

```
native fn mutable_array_get<T>(ab: MutableArray<T>, index: int) -> T
```

The element at `index`, counting from 0; a runtime error outside `0..len(ab)`.

This and `mutable_array_set` are why a loop that needs what it has already appended does not have to call
`snapshot`, which copies the whole buffer every time -- in a loop, the quadratic cost a builder exists
to avoid (`--lint` flags that shape). `for x in ab` reads one the same way, without the indexing.

### `mutable_array_set`

```
native fn mutable_array_set<T>(ab: MutableArray<T>, index: int, value: T)
```

Replaces the element at `index`; a runtime error outside `0..len(ab)`. Appending is `mutable_array_push`: this
only overwrites what is already there.

### `mutable_array_insert`

```
native fn mutable_array_insert<T>(xs: MutableArray<T>, index: int, value: T)
```

Inserts `value` at `index`, moving everything after it along. `index` may be `len(xs)`, which
appends -- the same rule the immutable `insert(arr, i, x)` follows, so adding at the end needs no
second function. O(n), where `mutable_array_push` is amortized O(1): a loop of
`mutable_array_insert(xs, 0, v)` is quadratic, which is why there is no `push_front` -- that name
would read as `push`'s twin and promise a cost it cannot give. Build tail-first and reverse once.

### `mutable_array_remove`

```
native fn mutable_array_remove<T>(xs: MutableArray<T>, index: int) -> T
```

Removes the element at `index` and answers it, moving everything after it back. O(n). There is no
`pop` because this is one: `mutable_array_remove(xs, len(xs) - 1)`.

### `mutable_array_truncate`

```
native fn mutable_array_truncate<T>(xs: MutableArray<T>, length: int)
```

Drops everything past `length`, keeping the buffer -- the only cheap shrink. A `length` at or
above the current one leaves it alone, since "keep at most n" is the useful meaning. `clear(xs)`
is this with 0.

### `mutable_array_swap`

```
fn mutable_array_swap<T>(xs: MutableArray<T>, i: int, j: int)
```

Exchanges the elements at `i` and `j`. Three calls otherwise, and every in-place algorithm wants
it; ordinary Ziman, since `mutable_array_get`/`_set` already reach the buffer.

### `mutable_array_reverse`

```
fn mutable_array_reverse<T>(xs: MutableArray<T>)
```

Reverses the buffer in place -- no copy at all, where `array_reverse(snapshot(xs))` makes two.

### `mutable_array_filter`

```
native fn mutable_array_filter<T>(xs: MutableArray<T>, keep: fn(T) -> bool)
```

Keeps only the elements `keep` answers true for, in place. The one filter in the language that
allocates nothing: it walks once with a write cursor trailing the read cursor, where
`array_filter(snapshot(xs), keep)` makes two copies. `keep` must not change the builder -- a
reallocation inside it cannot corrupt anything, but the rest of the walk would then be visiting
the buffer mid-compaction, the same class of thing as appending during a `for`.

### `mutable_array_sort`

```
fn mutable_array_sort<T: Ord>(xs: MutableArray<T>)
```

Sorts the buffer by the element type's own `compare`, ascending or descending. These two make
two copies -- the snapshot, and the sorted array -- and that is the cheaper option rather than a
compromise: for a packed `[int]`, `[float]` or `[byte]` buffer, `array_sort` compares *natively*,
with no interpreter call per comparison, which is worth far more than the two memcpys it costs.
`mutable_array_sort_with` below has no such option and so is genuinely in place.
The write-back is `mutable_array_set` per element rather than `clear` and push, so the buffer
keeps its capacity and the length never changes. Spelled out in both rather than shared, because
a helper here would be *public* -- everything in the prelude is -- and "overwrite the buffer with
this array" is not a function the design asked for.

### `mutable_array_sort_desc`

```
fn mutable_array_sort_desc<T: Ord>(xs: MutableArray<T>)
```

### `mutable_array_sort_with`

```
native fn mutable_array_sort_with<T>(xs: MutableArray<T>, cmp: fn(T, T) -> int)
```

Sorts the buffer by `cmp`, **copying no elements at all**: a merge sort over positions, then the
permutation applied by following its cycles. It allocates two int64 order buffers and an n-byte
map -- 17 bytes per element, against the 80 a boxed element costs, twice, through a snapshot. A
comparator of your own always went through the interpreter per comparison, so unlike `sort` and
`sort_desc` there is no native comparison here to trade those copies for. Stable: equal elements
keep their input order.

## MutableMap and MutableSet

### `MutableMap`

```
struct MutableMap<K: Eq + Hash, V> {
    handle: int
}
```

The mutable counterparts of Map and Set, for building one entry by entry. `set` and `insert` on
a Map copy a few nodes each time; a builder changes its own private structure in place, so
adding n entries allocates O(n) in all, not O(n) per entry. `snapshot` and `snapshot`
snapshot what has been added so far into an ordinary Map or Set, and the builder stays usable.
Keys keep the order they were first added, and follow the same rule as a Map's (`Eq + Hash`).
Thread-bound like the other builders.

### `MutableSet`

```
struct MutableSet<T: Eq + Hash> {
    handle: int
}
```

A mutable set under construction; see MutableMap.

### `mutable_map_new`

```
native fn mutable_map_new<K: Eq + Hash, V>() -> MutableMap<K, V>
```

An empty map builder.

### `mutable_map_set`

```
native fn mutable_map_set<K: Eq + Hash, V>(mb: MutableMap<K, V>, key: K, value: V)
```

Adds `key`, or replaces its value while keeping its position.

### `mutable_map_get`

```
native fn mutable_map_get<K: Eq + Hash, V>(mb: MutableMap<K, V>, key: K) -> Option<V>
```

The value of `key`, or None.

### `mutable_map_remove`

```
native fn mutable_map_remove<K: Eq + Hash, V>(mb: MutableMap<K, V>, key: K) -> Option<V>
```

Removes `key`; nothing happens when it is absent.

### `mutable_map_get_or`

```
fn mutable_map_get_or<K: Eq + Hash, V>(mb: MutableMap<K, V>, key: K, default_value: V) -> V
```

The value stored under `key`, or `default_value` when there is none -- `map_get_or` for a builder.
Ordinary Ziman rather than a builtin, since it is exactly this switch: the point is that a counting
loop (`mutable_map_set(counts, k, mutable_map_get_or(counts, k, 0) + 1)`) reads as one line instead of four.

### `mutable_set_new`

```
native fn mutable_set_new<T: Eq + Hash>() -> MutableSet<T>
```

An empty set builder.

### `mutable_set_add`

```
native fn mutable_set_add<T: Eq + Hash>(sb: MutableSet<T>, item: T) -> bool
```

Adds `item`; nothing happens when it is already there.

### `mutable_set_remove`

```
native fn mutable_set_remove<T: Eq + Hash>(sb: MutableSet<T>, item: T) -> bool
```

Removes `item`; nothing happens when it is absent.

### `mutable_map_update`

```
fn mutable_map_update<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K, f: fn(Option<V>) -> V) -> V
```

`f` applied to whatever is at `key` -- `None` when nothing is -- stored back, and returned. One
lookup where `get` then `set` is two, which is the whole reason this exists where
`mutable_set`-on-a-cell did not need an equivalent.

### `mutable_map_update_all`

```
fn mutable_map_update_all<K: Eq + Hash, V>(m: MutableMap<K, V>, f: fn(K, V) -> V)
```

`f` applied to every value, keys and their order unchanged. Safe to write as a `for` plus a
`set` because iterating a MutableMap walks a snapshot of the entries, so the writes cannot
disturb the walk.

### `mutable_map_filter`

```
fn mutable_map_filter<K: Eq + Hash, V>(m: MutableMap<K, V>, keep: fn(K, V) -> bool)
```

Drops every entry `keep` answers false for, in place -- the counterpart of `map_filter`, which
returns a new Map instead. Safe for the same snapshot reason as `update_all`.

### `mutable_map_remove_all`

```
fn mutable_map_remove_all<K: Eq + Hash, V>(m: MutableMap<K, V>, keys: variadic K) -> bool
```

Drops each of `keys`, answering whether any of them was actually there. `_all` takes the plural
of what the singular takes, which for a map's `remove` is keys rather than entries.

### `mutable_map_keys`

```
fn mutable_map_keys<K: Eq + Hash, V>(m: MutableMap<K, V>) -> Set<K>
```

The keys as an immutable `Set<K>`, in insertion order -- the same answer `map_keys` gives for a
Map, and the reason this returns a value rather than a handle: a result like this is nearly
always consumed by a `for` or returned, so handing back a handle would allocate a thread-bound
object per loop for nothing.

### `mutable_map_values`

```
fn mutable_map_values<K: Eq + Hash, V>(m: MutableMap<K, V>) -> [V]
```

The values as an array, in the keys' insertion order, as `map_values` gives for a Map.

### `mutable_set_add_all`

```
fn mutable_set_add_all<T: Eq + Hash>(s: MutableSet<T>, items: variadic T) -> bool
```

Adds each of `items`, answering whether any of them was new. The singular `mutable_set_add` is
not variadic, which is what keeps a `MutableSet<[int]>` from being a special case -- one element
or several is never ambiguous when only the `_all` form takes many.

### `mutable_set_remove_all`

```
fn mutable_set_remove_all<T: Eq + Hash>(s: MutableSet<T>, items: variadic T) -> bool
```

Drops each of `items`, answering whether any of them was there.

### `mutable_set_filter`

```
fn mutable_set_filter<T: Eq + Hash>(s: MutableSet<T>, keep: fn(T) -> bool)
```

Drops every element `keep` answers false for, in place -- the counterpart of `set_filter`.

### `mutable_set_items`

```
fn mutable_set_items<T: Eq + Hash>(s: MutableSet<T>) -> [T]
```

The elements as an array, in insertion order, as `set_items` gives for a Set.

## Iter

### `Iter`

```
struct Iter<T> {
    handle: int
}
```

A lazy source of values, produced one at a time as a `for` loop asks for them: `for line in
lines { ... }` over an `Iter<Result<string, string>>` from `fs.lines`. Nothing is read ahead, so
the source can be far larger than memory, and stopping early (`break`, `return`, or `try`) leaves the
rest unread. `handle` is an opaque reference to the native source; a source that is not finished
or closed is closed when it is garbage collected. It can be walked once: a second `for` over the
same Iter carries on where the first stopped. It belongs to the thread that made it, like a builder
(`#[thread_bound]`): not a `Channel` element, a `spawn` argument or a module-level `val`, and walking or
closing it from another thread is a runtime error.

### `iter_from`

```
native fn iter_from<T>(next: fn() -> Option<T>, close: fn()) -> Iter<T>
```

An Iter written in Ziman. `next` returns the next item as `Some(item)`, or `None` at the end; `close` is
called once when the source is finished, left early by a `for` loop (`break`, `return`, `try`) or closed
with `iter_close`. A closure cannot change a variable it captured, so the state lives in whatever the
closures call: a native handle such as a socket or a database cursor, or a Channel. Unlike the built-in
sources it is NOT closed by the garbage collector (that would run your code from a finalizer), so
finish it or close it.

### `iter_of_array`

```
native fn iter_of_array<T>(items: [T]) -> Iter<T>
```

The elements of `items` as an Iter, for code that takes a lazy source but has an array.

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

## Map and Set helpers

### `MapEntry`

```
struct MapEntry<K, V> {
    key: K
    value: V
}
```

One key and its value, as `map_entries` returns them.

### `map_entries`

```
fn map_entries<K: Eq + Hash, V>(m: Map<K, V>) -> [MapEntry<K, V>]
```

The entries of `m` as an array, in the map's insertion order: for collecting them, sorting them (by value,
say, with `array_sort_by_keys`) or taking the first few. Written with the type arguments:
`map_entries<string, int>(stock)`. To only walk a map, `for k, v in m` needs no array.

### `map_map_values`

```
fn map_map_values<K: Eq + Hash, V, R>(m: Map<K, V>, f: fn(V) -> R) -> Map<K, R>
```

`m` with `f` applied to every value, keys and their order unchanged:
`map_map_values<string, int, string>(stock, fn(n) { str(n) })`.

### `set_map`

```
fn set_map<T: Eq + Hash, U: Eq + Hash>(s: Set<T>, f: fn(T) -> U) -> Set<U>
```

The set of `f(x)` for every `x` in `s`, in the order first produced. Two elements that map to the same
value become one: `set_map<int, int>(Set<int> { 1, 2, 3 }, fn(n) { n % 2 })` is `{1, 0}`.

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

The bare array helpers, callable with no import, beside the bare `map_*` and `set_*` helpers
that `Map` and `Set` already have. `array_map`/`filter`/`flat_map`/`reverse`/`sort`/
`sort_desc`/`sort_with` are thin wrappers over these and stay for the call sites that use them;
these are the canonical spelling. The first four are native: one allocation for the result
instead of a MutableArray's growing buffers.

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

### `array_present`

```
fn array_present<T>(items: variadic Option<T>) -> [T]
```

The values that are actually there, in order, with the `None`s dropped. What builds a list out
of fields that may each be absent, without an `if` per field:


```
xml.element(xml.name("patient"), [], array_present<xml.Node>(
    xml.opt_element("firstName", p.first_name),
    xml.opt_element("dateOfBirth", p.date_of_birth)
))
```

Variadic rather than taking an array, because the call site is nearly always a written-out
list of maybes rather than one already in hand; an array you do have goes in as
`array_present<T>(items: existing)`, the same spelling `concat_arrays` takes.

`array_filter` cannot do this: it keeps elements, so it would hand back `[Option<T>]` still
wrapped, and `array_flat_map` only does it by turning each value into a one- or zero-element
array, which allocates per element and reads like a puzzle.
A builder sized to the input up front, rather than `out = push(out, v)` into a value. Both
were measured, because which wins is not obvious -- `snapshot` is an O(n) copy, so a builder
is not free. Repeated value `push` was the fastest of the three at eight elements and the
slowest from a hundred up; a builder left to grow on its own was the reverse, about a
quarter faster at scale and about a quarter slower on the short lists this is usually called
with. Sizing it to `len(items)` is the one that never loses either way: level with value
`push` at eight, roughly a tenth faster from a hundred to two hundred thousand. The capacity
is an upper bound, since the `None`s drop out.

### `array_sort`

```
fn array_sort<T: Ord>(items: [T]) -> [T]
```

`items` in ascending order of their own `compare`, which orders any number, string or byte,
and any struct or enum with a `<type>_compare` function (`#[derive(Ord)]` writes one). Stable:
equal elements keep their input order. Packed numbers and strings sort natively; anything else
goes through the merge sort in `sort_positions`.

### `array_sort_desc`

```
fn array_sort_desc<T: Ord>(items: [T]) -> [T]
```

`items` in descending order of their own `compare`. Also stable, so this is not `array_sort`'s
result reversed -- that would flip ties.

### `array_sort_with`

```
fn array_sort_with<T>(items: [T], cmp: fn(T, T) -> int) -> [T]
```

`items` ordered by a comparator function (negative, zero or positive, like `compare`), for an
order that is neither the type's own nor a single key -- a two-field ordering, say. Stable.
`array_sort_by_keys` covers the single-key case.

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

### `result_map_err`

```
fn result_map_err<T, E, F>(r: Result<T, E>, convert: fn(E) -> F) -> Result<T, F>
```

`r`'s Ok value, or None on Err -- the error itself is dropped, so use this only where the
caller truly doesn't care why something failed, not as a shortcut past handling it.
`r` with its error replaced by `convert(error)`; an Ok passes through untouched. The piece that was
missing wherever one module's error type has to become another's -- `fs` answers an `FsError` and a
caller whose own errors are strings wants `try result_map_err(fs.read_text(p), fs.fs_error_message)`
rather than four lines of switch at every call.

### `lines`

```
pub fn lines(text: string) -> [string]
```

The lines of `text`, without their terminators. Written because a cold model reaching for
`text.lines()` was sent to `split(text, "\n")`, which is close but not the same thing: `split`
leaves a `\r` on every line of CRLF text and leaves a trailing empty string after a final
newline, so "a\nb\n" splits into three pieces where it has two lines. Both of those are the
kind of difference that shows up as an off-by-one in someone's line count rather than as an
error (docs/reviews/tasks.md F35).

A final newline is a terminator, not a separator -- "a\nb\n" and "a\nb" both have two lines --
which is how every line-oriented tool reads a text file. An empty string has no lines.

### `reverse_string`

```
pub fn reverse_string(s: string) -> string
```

`s` with its characters in the opposite order. Reverses *code points*, not bytes, so reversing
text outside ASCII gives text rather than mojibake -- the byte-level version of this is the
classic way to corrupt UTF-8, and `chars` already answers in code points.

### `result_to_option`

```
pub fn result_to_option<T, E>(r: Result<T, E>) -> Option<T>
```

### `option_to_result`

```
pub fn option_to_result<T, E>(o: Option<T>, err_supplier: fn() -> E) -> Result<T, E>
```

The reverse of result_to_option: Some(value) becomes Ok(value), and None becomes
Err(err_supplier()) -- a function, not a plain value, so building the error (an allocation, a
formatted message) is skipped entirely on the common Some path.

## Array algorithms

### `array_sort_indices`

```
fn array_sort_indices<K: Ord>(keys: [K]) -> [int]
```

The positions of `keys` in ascending order: the array of indices that, read in order,
visits `keys` smallest first. Stable: equal keys keep their input order. `sort` and
`sort_by_keys` are built on this.

### `array_sort_indices_desc`

```
fn array_sort_indices_desc<K: Ord>(keys: [K]) -> [int]
```

The positions of `keys` in descending order. Also stable: equal keys keep their input
order (this is not the ascending result reversed, which would flip ties).

### `array_order_of`

```
fn array_order_of<K: Ord>(keys: [K], descending: bool) -> [int]
```

Packed numbers and strings are sorted natively; anything else (a struct, an enum, a bigint)
by the merge sort `sort_positions`, calling `compare`.

### `array_sort_by_keys`

```
fn array_sort_by_keys<T, K: Ord>(items: [T], keys: [K]) -> [T]
```

`items` ordered by `keys[i]` for each `items[i]` -- the key array is how a caller sorts by
a field or a computed value without a comparator function. The two arrays differing in length
is a bug in the caller's own logic (both are always the program's own arrays, never data from
outside it), not a recoverable condition -- a fatal error naming both lengths, the same policy
every programmer-error case in the library follows now (see docs/DESIGN.md's "Programmer
errors are fatal" entry), not a `Result` a caller could forget to check.

### `array_sort_by_keys_desc`

```
fn array_sort_by_keys_desc<T, K: Ord>(items: [T], keys: [K]) -> [T]
```

The descending form of `sort_by_keys`.

### `array_is_sorted`

```
fn array_is_sorted<T: Ord>(items: [T]) -> bool
```

True when no element is greater than the one after it (an empty or one-element array is).

### `array_lower_bound`

```
fn array_lower_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element that is not less than
`target` -- where `target` would be inserted to keep the order, before any equal
elements. `len(sorted)` when every element is smaller. O(log n).

### `array_upper_bound`

```
fn array_upper_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element greater than `target` --
just after any equal elements. `upper_bound - lower_bound` is how many equal `target`.

### `array_binary_search`

```
fn array_binary_search<T: Ord>(sorted: [T], target: T) -> Option<int>
```

The position of the first element equal to `target` in an already sorted array, or None.
O(log n).

### `array_find`

```
fn array_find<T>(items: [T], test: fn(T) -> bool) -> Option<T>
```

The first element for which `test` is true, or None.

### `array_first`

```
fn array_first<T>(items: [T]) -> Option<T>
```

The first element, or None for an empty array.

### `array_last`

```
fn array_last<T>(items: [T]) -> Option<T>
```

The last element, or None for an empty array.

### `array_min_by`

```
fn array_min_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is smallest (the first such, on a tie), or None for an empty array.

### `array_max_by`

```
fn array_max_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is largest (the first such, on a tie), or None for an empty array.

### `array_fold`

```
fn array_fold<T, A>(items: [T], initial: A, f: fn(A, T) -> A) -> A
```

Left to right: `f(f(initial, items[0]), items[1])` and so on.

### `array_count`

```
fn array_count<T>(items: [T], test: fn(T) -> bool) -> int
```

How many elements `test` is true for.

### `array_any`

```
fn array_any<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for at least one element (false for an empty array).

### `array_all`

```
fn array_all<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for every element (true for an empty array).

### `array_for_each`

```
fn array_for_each<T>(items: [T], f: fn(T))
```

Calls `f` on every element, in order, for its effect.

### `array_distinct`

```
fn array_distinct<T: Eq + Hash>(items: [T]) -> [T]
```

The distinct elements, each at its first position: `distinct([3, 1, 3, 2, 1])` is `[3, 1, 2]`.

### `array_group_by`

```
fn array_group_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, [T]>
```

The elements grouped by `key`: a map from each key to the elements that have it, in input
order, with the keys in the order they first appear.

### `array_join`

```
fn array_join(items: [string], sep: string) -> string
```

The pieces of `items` with `sep` between them, under the name a reader reaches for. It exists
because `parts.join("-")` has no method syntax here and the obvious search lands on `fs.path_join`
(docs/reviews/tasks.md F35). `sep` goes between, not after, so a single element joins to itself
and an empty array to the empty string.

One line, because `join` already does the work in a single pass. It used to be a hand-written
loop that re-copied the whole accumulated string per element -- 18.05s on 40,000 pieces against
0.01s now, for byte-identical output -- kept there by a comment that wrongly denied `join` could
take an array (D17).

### `array_sum`

```
fn array_sum(items: [int]) -> int
```

The total of `items`. A left fold with `+`, which is what anyone writes by hand, but `+` on
`int` traps on overflow in this language -- so this either answers the exact total or stops with
an error, and never the silently wrong number a wrapping sum would give.

Integers only, deliberately: a float total depends on summation order for precision, so the
right answer there is a decision (naive, pairwise, Kahan) rather than a default, and `fold`
already expresses whichever one a caller wants.

### `array_frequencies`

```
fn array_frequencies<T: Eq + Hash>(items: [T]) -> Map<T, int>
```

How many times each distinct element occurs: `array_frequencies<string>(["a", "b", "a"])` is
`{a: 2, b: 1}`, keys in the order they first appear.

This comment was sitting above `array_join` instead, with no blank line between the two blocks,
so the generator published it as `array_join`'s documentation and shipped this function with
none at all (D21).

### `array_index_by`

```
fn array_index_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, T>
```

The elements keyed by `key`: a map from each key to the element that has it. When two elements share a
key the later one wins, like `set` on a map; use `group_by` to keep them all. Keys are in the order they
first appear.
