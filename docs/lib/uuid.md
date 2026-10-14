# `uuid`

UUIDs (RFC 4122): random version 4, time-ordered version 7, and the canonical
text form.

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
    print(len(uuid.uuid_to_string(id)))
    val again = unwrap(uuid.uuid_from_string(uuid.uuid_to_string(id)))
    print(id == again)
    print(is_ok(uuid.uuid_from_string("not-a-uuid")))
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

### `uuid_new_v7`

```
pub fn uuid_new_v7() -> Uuid
```

A fresh time-ordered (version 7) UUID: the first 48 bits are the current time in milliseconds,
the rest random, so UUIDs made later sort after earlier ones -- a good database key, since new
rows land at the end of an index instead of all over it.

### `uuid_nil`

```
pub fn uuid_nil() -> Uuid
```

The all-zero UUID, `00000000-0000-0000-0000-000000000000`: "no id".

### `uuid_to_string`

```
pub fn uuid_to_string(u: Uuid) -> string
```

The lowercase dashed text of `u`: `123e4567-e89b-42d3-a456-426614174000`.

### `uuid_from_string`

```
pub fn uuid_from_string(s: string) -> Result<Uuid, string>
```

The UUID written in the 8-4-4-4-12 dashed form (upper or lower case). An Err for any other
length, misplaced dashes, or a non-hex character; the message names the text.

### `uuid_version`

```
pub fn uuid_version(u: Uuid) -> int
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
