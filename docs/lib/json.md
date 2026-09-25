# `json`

JSON as a `Json` tree: a native parser and serializer, and Ziman helpers for
reading and building trees.

`Json` is a recursive enum: `JArray([Json])` and
`JObject(Map<string, Json>)`. An object is a built-in `Map`, so a field lookup is
near-O(1) (not a scan), a duplicate key has one defined meaning (the last one
wins; `parse_json_strict` rejects it instead), and iteration -- and so
`json_to_string` -- keeps the order keys first appeared in the document.

Numbers: `JInt(int)` for an integer literal that fits in 64 bits, `JNumber(float)`
for everything else (a fraction, an exponent, or an integer too large for `int`).
So a large id or count round-trips exactly instead of losing precision to a float.
`int_from_json` accepts either (a `JNumber` only if it is a whole number in range);
`float_from_json` accepts either.

Strings: the full JSON escape set is read (`\" \\ \/ \b \f \n \r \t` and `\uXXXX`,
including surrogate pairs, decoded to UTF-8) and written (control characters as
`\u00XX`). `\u0000` is rejected: a Ziman string cannot hold a 0 byte. A CR counts
as structural whitespace between tokens, so a CRLF body parses.

`Json` does NOT support `==`/`!=` -- array equality requires a scalar element type
(see docs/SYNTAX.md "Arrays"), and `[Json]`'s element is an enum. A known limitation.

This is the general engine; a schema-specific parser for one request shape is
expected to be built on top of it (see `json_object_get`, `json_get_path` and the
`json_require_*` helpers below, or `#[derive(Json)]`), not inside this module.

`use json` -- or `use json.Name` to bring one item in bare.

## Example

```
use json
use json.Json

// Parse text into a tree, read from it, build a new tree and print it.
fn main() {
    val doc = unwrap(json.parse_json("{\"name\": \"Ada\", \"langs\": [\"en\", \"fr\"], \"age\": 36}"))

    print(unwrap(json.json_require_string_field(doc, "name")))
    print(unwrap(json.json_require_int_field(doc, "age")))

    // A missing field is a value you can test, not a crash.
    print(Option.is_None(json.json_object_get(doc, "email")))
    print(is_ok(json.json_require_string_field(doc, "email")))

    // A path into nested values.
    val first = json.json_get_path(doc, ["langs"])
    print(Option.is_Some(first))

    // Build a tree and write it out, compact or indented.
    val out = Json.JObject(Map<string, Json> { "ok": Json.JBool(true), "count": Json.JInt(2) })
    print(json.json_to_string(out))
    print(json.json_to_pretty_string(out, 2))

    // Bad input is an Err carrying where it went wrong.
    print(is_ok(json.parse_json("{\"a\": ")))
}
```

Output:

```
Ada
36
true
false
true
{"ok":true,"count":2}
{
  "ok": true,
  "count": 2
}
false
```

### `Json`

```
pub enum Json {
    JNull
    JBool(bool)
    JInt(int)
    JNumber(float)
    JString(string)
    JArray([Json])
    JObject(Map<string, Json>)
}
```

A JSON value: null, a boolean, an integer (`JInt`, a number written without a fraction or
exponent), any other number (`JNumber`), a string, an array, or an object (a `Map<string,
Json>`).

## Parsing

### `parse_json`

```
pub fn parse_json(s: string) -> Result<Json, string>
```

Parses one JSON document. A repeated key in an object is allowed: the last one wins, keeping
the position of the first. Errors say what was expected and the byte position
(`expected ',' or ']' at position 12`); nesting deeper than 512 levels is an error. The whole
input must be one value: `trailing content at position N` otherwise.

### `parse_json_strict`

```
pub fn parse_json_strict(s: string) -> Result<Json, string>
```

parse_json, but a repeated key in any object is an error -- for input where a duplicate
means something is wrong (a signed or audited document, a config file).

## Writing

### `json_to_string`

```
pub fn json_to_string(j: Json) -> string
```

`j` as compact JSON text (no spaces), objects in key insertion order. A float is written
with the fewest digits that read back as the same number (`0.1`, `1e+20`); NaN and infinity,
which JSON cannot represent, are written as `null`. Control characters in strings are
escaped (`\b \f \n \r \t`, else `\u00XX`); everything else, including non-ASCII, is written as is.

## Reading a tree

### `json_object_get`

```
pub fn json_object_get(j: Json, key: string) -> Option<Json>
```

The core convenience lookup provided beyond parse/serialize -- looking
up a field by key is common enough (every schema-specific parser
built on this module needs it at least once per field) to be worth
including, without turning this into a general "Json query language."

### `json_get_path`

```
pub fn json_get_path(j: Json, path: [string]) -> Option<Json>
```

Follows `path` down from `j`: each segment is an object key, or -- when the current
value is an array -- a decimal index. None if any step is missing or of the wrong
kind: `json_get_path(j, ["items", "0", "name"])`.

### `json_require_field`

```
pub fn json_require_field(j: Json, key: string) -> Result<Json, string>
```

json_object_get, treating a missing field as an error instead of
None -- pulled out of two near-identical copies that grew
independently in health-record-backend-ziman's own request/response
mapping layers before moving here. The common case for a *required*
field in a request/response shape, where "not present" is always a
failure worth a clear message, not an Option every caller has to
unwrap into one anyway.

### `json_require_string_field`

```
pub fn json_require_string_field(j: Json, key: string) -> Result<string, string>
```

json_require_field, further requiring the value be a JSON string --
the single most common required-field shape (an id, a name, a
status), so a request/response mapping layer doesn't have to spell
out the is_JString check every time.

### `json_require_int_field`

```
pub fn json_require_int_field(j: Json, key: string) -> Result<int, string>
```

The integer in field `key` of object `j`; an Err naming the field when `j` is not an object,
lacks it, or it is not an integer.

### `json_require_float_field`

```
pub fn json_require_float_field(j: Json, key: string) -> Result<float, string>
```

The number in field `key` of object `j` (an integer is widened); an Err naming the field when it
is missing or not a number.

### `json_require_bool_field`

```
pub fn json_require_bool_field(j: Json, key: string) -> Result<bool, string>
```

The boolean in field `key` of object `j`; an Err naming the field when it is missing or not a
boolean.

### `json_to_map`

```
pub fn json_to_map<V>(j: Json, decode: fn(Json) -> Result<V, string>) -> Result<Map<string, V>, string>
```

An object as a Map, each value decoded by `decode`: `json_to_map<int>(j, int_from_json)`.

### `json_from_map`

```
pub fn json_from_map<V>(m: Map<string, V>, encode: fn(V) -> Json) -> Json
```

The other direction: a Map as a JSON object, each value encoded by `encode`.

### `json_to_string_map`

```
pub fn json_to_string_map(j: Json) -> Result<Map<string, string>, string>
```

A JSON object of strings as a Map<string, string>.

### `json_to_pretty_string`

```
pub fn json_to_pretty_string(j: Json, indent: int) -> string
```

Indented output: `indent` spaces per level, one array element or object field per
line; an empty array or object stays `[]` / `{}`.

### `json_to_json`

```
pub fn json_to_json(j: Json) -> Json
```

Identity wrappers, named to match #[derive(Json)]'s own <type>_to_json/
<type>_from_json convention -- so a struct field declared plain `Json`
composes with derive like any other field type (tier 1 of its
priority order: does the field's type have its own _to_json/
_from_json), instead of derive having no way to handle a bare `Json`
field at all. The escape hatch this is for: a struct shared across
several callers where one field's *concrete* type genuinely differs
per caller (e.g. one response envelope shared by several resources,
each embedding its own distinct error enum) -- that field is declared
`Json` instead of the type that can't be unified, each caller
converts its own value to `Json` before constructing the struct, and
decodes it back with its own from_json after reading the shared
struct back out.

### `json_from_json`

```
pub fn json_from_json(j: Json) -> Result<Json, string>
```

`j` itself, so a `Json` field of a derived struct passes through unchanged.

### `int_to_json`

```
pub fn int_to_json(n: int) -> Json
```

Converter functions for the scalar types. A generic #[derive(Json)] type takes
one converter per type parameter (`page_to_json<int>(page, int_to_json)`), and
a builtin type has no function of its own to pass -- these are those.

### `int_from_json`

```
pub fn int_from_json(j: Json) -> Result<int, string>
```

A JInt, or a JNumber that is a whole number within int's range (`3.0`, `1e3`).

### `float_to_json`

```
pub fn float_to_json(f: float) -> Json
```

`f` as a JSON number.

### `float_from_json`

```
pub fn float_from_json(j: Json) -> Result<float, string>
```

A float from a JSON number, integers included; an Err otherwise.

### `bool_to_json`

```
pub fn bool_to_json(b: bool) -> Json
```

`b` as a JSON boolean.

### `bool_from_json`

```
pub fn bool_from_json(j: Json) -> Result<bool, string>
```

A bool from a JSON boolean; an Err otherwise.

### `string_to_json`

```
pub fn string_to_json(s: string) -> Json
```

`s` as a JSON string.

### `string_from_json`

```
pub fn string_from_json(j: Json) -> Result<string, string>
```

A string from a JSON string; an Err otherwise.
