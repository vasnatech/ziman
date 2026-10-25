# `uuid`

UUIDs: random version 4, time-ordered version 7, and the canonical text form.

**RFC 9562**, which obsoleted RFC 4122 and is where version 7 comes from -- 4122 defines
versions 1 through 5 and knows nothing of v7. The variant bits are still 4122's (`10` in the
top two bits of byte 8), which is why the constant below keeps that name.

**Two spellings on purpose, and the call sites chose them.** `format`, `parse`, `version`,
`is_nil` and `timestamp_millis` are bare and reached through the module -- `uuid.parse(s)`,
26 of those against 1 by-name import, which is aliased (`use uuid.parse as parse_id`) because
a bare `parse` collides with json's and toml's. The constructors and the derive bridges carry
the prefix and are imported by name -- `use uuid.uuid_new` then `uuid_new()`, 303 bare call
sites against 6 qualified ones. A reader writes `uuid_new()`, never `uuid.new()`.

That is the same question `random.bytes` answered the other way round, and worth seeing as one
rule rather than two: does the *call site* say what the function answers? `random_bytes(32)`
read well bare and `bytes(32)` says nothing, so that one became qualified-only. Here
`uuid_new()` reads well bare and `new()` says nothing, so the prefix stays.

A `Uuid` is 16 bytes. It satisfies `Eq`, `Hash` and `Ord` (structurally, and byte-wise for
order), so it works as a struct field, a `Map` key or a `Set` element and sorts with
`array_sort<Uuid>`. Its text is the lowercase 8-4-4-4-12 dashed form; text is read case
insensitively but must have exactly that shape (no braces, no missing dashes). It has
`_to_json` / `_from_json` and `_to_sql` / `_from_sql` functions, so `#[derive(Json)]` and
`#[derive(Row)]` handle a `Uuid` field: a JSON string, and a TEXT column.

The version and variant bits are set with plain arithmetic, since Ziman has no bitwise
operators: clearing the top nibble of a byte and setting it to 4 is `(b % 16) + 64`, the same
as `(b & 0x0F) | 0x40`.

`use uuid` -- or `use uuid.Name` to bring one item in bare.

## Example

```
use uuid

fn main() {
    val id = uuid.uuid_new()
    print(byte_len(uuid.format(id)))
    val again = unwrap(uuid.parse(uuid.format(id)))
    print(id == again)
    print(is_ok(uuid.parse("not-a-uuid")))
}
```

Output:

```
36
true
false
```

### `Uuid`

```
pub struct Uuid {
    bytes: [byte]
}
```

16 bytes. `bytes` is public because Ziman has no per-field privacy; treat it as read-only.

### `uuid_new`

```
pub fn uuid_new() -> Uuid
```

A fresh random (version 4) UUID, from the operating system's entropy.

**About 9.4 microseconds each, and 7.2 of those are the entropy**: `random.bytes` is three
syscalls (`fopen`/`fread`/`fclose` on /dev/urandom) and this asks for 16 bytes at a time, where
one draw of 80,000 bytes measures at under a millisecond. Buffering a block and handing out
slices would be roughly four times faster and is deliberately not done: it needs state shared
between every caller, and a program generating UUIDs from a `spawn` would be racing on it. One
UUID per request costs nothing; a loop making a million of them should draw its own bytes with
`random.bytes` and call `uuid_of`.

### `uuid_new_v7`

```
pub fn uuid_new_v7() -> Uuid
```

A fresh time-ordered (version 7) UUID: the first 48 bits are the current time in milliseconds,
then 12 bits of finer time, then random -- so UUIDs made later sort after earlier ones. That is
what makes it a good database key: new rows land at the end of an index instead of all over it.

**The 12 bits of finer time are the reason that claim is true.** With milliseconds alone and the
rest random, two UUIDs made in the same millisecond sort in random order -- measured at 974 of
1999 consecutive pairs out of order, which is what a tight loop does. RFC 9562 offers exactly
this fix ("replace leftmost random bits with increased clock precision"): `rand_a`, the 12 bits
after the version nibble, holds the fraction of the millisecond instead. 4096 steps across a
millisecond is one per 244 nanoseconds, and `clock.now_raw` has real nanosecond resolution
(consecutive reads differ by a few hundred), so two calls in a row land in different steps.

**Ordering follows the clock, and the clock is `CLOCK_REALTIME`.** A clock stepped backwards by
NTP or by hand produces UUIDs that sort before ones already made. That is inherent to v7 -- the
timestamp is wall-clock time by definition -- and the alternative, a monotonic counter, needs
state carried between calls and shared between threads.

### `timestamp_millis`

```
pub fn timestamp_millis(u: Uuid) -> Option<int>
```

The unix milliseconds a version 7 UUID carries, or None for any other version -- the 48 bits
`uuid_new_v7` wrote, read back. For a `time.Instant`, `instant_of(millis / 1000, millis % 1000 *
1000000)`; this answers a plain number so the module needs no dependency on `time` for it.

The 12 bits of finer time are deliberately *not* included: they are a sub-millisecond ordering
device, not a measurement, and reporting them as part of a timestamp would claim a precision
the clock fraction does not carry across a process restart.

### `uuid_of`

```
pub fn uuid_of(bytes: [byte]) -> Result<Uuid, string>
```

A `Uuid` from exactly 16 bytes, or an Err naming the length found.

**This is how to build one from bytes.** Ziman has no per-field privacy, so `Uuid { bytes: .. }`
is writable and nothing can stop a 2-byte one being constructed -- but every function here reads
16, so `format` on a short one dies inside this module (`substring index out of bounds`) rather
than at the line that built it. Going through here turns that into an `Err` at the point of the
mistake.

### `uuid_nil`

```
pub fn uuid_nil() -> Uuid
```

The all-zero UUID, `00000000-0000-0000-0000-000000000000`: "no id".

### `uuid_max`

```
pub fn uuid_max() -> Uuid
```

The all-ones UUID, `ffffffff-ffff-ffff-ffff-ffffffffffff`, which RFC 9562 defines beside Nil as
the other end of the range: a sort key that compares after every other UUID. Its version nibble
reads 15, which is not a real version -- like Nil's 0.

### `is_nil`

```
pub fn is_nil(u: Uuid) -> bool
```

True when `u` is the all-zero UUID. `u == uuid_nil()` says the same thing and builds a Uuid to
throw away; this reads the bytes.

### `format`

```
pub fn format(u: Uuid) -> string
```

The lowercase dashed text of `u`: `123e4567-e89b-42d3-a456-426614174000`.

### `parse`

```
pub fn parse(s: string) -> Result<Uuid, string>
```

The UUID written in the 8-4-4-4-12 dashed form (upper or lower case). An Err for any other
length, misplaced dashes, or a non-hex character; the message names the text.

### `version`

```
pub fn version(u: Uuid) -> int
```

The UUID's version (4 for `uuid_new`, 7 for `uuid_new_v7`): the high nibble of byte 6.

### `uuid_to_json`

```
pub fn uuid_to_json(u: Uuid) -> Json
```

A `Uuid` as a JSON string of its canonical text. Named for `#[derive(Json)]`'s
`<type>_to_json` convention, so a `Uuid` field works in a derived struct.

### `uuid_from_json`

```
pub fn uuid_from_json(j: Json) -> Result<Uuid, string>
```

A `Uuid` from a JSON string. An Err if `j` is not a string or not a valid UUID.

### `uuid_to_sql`

```
pub fn uuid_to_sql(u: Uuid) -> SqlValue
```

A `Uuid` as a TEXT value in the canonical dashed form, for `#[derive(Row)]` (SQLite has no
UUID type, and Postgres's `uuid` column reads and writes the same text).

### `uuid_from_sql`

```
pub fn uuid_from_sql(v: SqlValue) -> Result<Uuid, DbError>
```

A `Uuid` from a TEXT value. An Err if the value is not text or not a valid UUID.

### `uuid_compare`

```
pub fn uuid_compare(a: Uuid, b: Uuid) -> int
```

Byte-wise order (the order of the canonical text), negative, zero or positive; what makes
`Uuid` satisfy `Ord`, so it works with `array_sort<Uuid>` and `compare`.
