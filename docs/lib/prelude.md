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
a loop, a `StringBuilder` (a fresh `concat` on every iteration copies everything so far, same
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
build or edit one directly, only through `sb_new` and the `sb_*` functions. It belongs to the
thread that made it (`#[thread_bound]`): it cannot be a `Channel`'s element, a `spawn` argument or a
module-level `val`, and using one from another thread (through a closure, say) is a runtime error.
Hand the finished string over, not the builder.

### `sb_new`

```
native fn sb_new(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> StringBuilder
```

An empty builder. `capacity` is the bytes to reserve up front and `grow_factor` the multiple
its capacity grows by when an append would overflow it; both are optional, so `sb_new()` is
usually all that is written.

### `sb_append`

```
native fn sb_append(sb: StringBuilder, pieces: variadic string)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place and
nothing is returned.

### `sb_append_byte`

```
native fn sb_append_byte(sb: StringBuilder, b: variadic byte) -> bool
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

### `ab_new`

```
native fn ab_new<T>(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> ArrayBuilder<T>
```

An empty builder; `capacity` and `grow_factor` are as in `sb_new`.

### `ab_append`

```
native fn ab_append<T>(ab: ArrayBuilder<T>, pieces: variadic T)
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
Thread-bound like the other builders.

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

### `mapb_get_or`

```
fn mapb_get_or<K: Eq + Hash, V>(mb: MapBuilder<K, V>, key: K, default_value: V) -> V
```

The value stored under `key`, or `default_value` when there is none -- `map_get_or` for a builder.
Ordinary Ziman rather than a builtin, since it is exactly this switch: the point is that a counting
loop (`mapb_set(counts, k, mapb_get_or(counts, k, 0) + 1)`) reads as one line instead of four.

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
say, with `collections.sort_by_keys`) or taking the first few. Written with the type arguments:
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
