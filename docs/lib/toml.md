# `toml`

TOML 1.0 on top of `json`: a document is a `Json` object, so any
`#[derive(Json)]` type decodes from it with its own `_from_json`. Written in pure Ziman.

Reads tables (`[a.b]`), arrays of tables (`[[a]]`), dotted keys, inline tables, arrays
(any nesting, multi-line, trailing commas), basic / literal / multi-line strings with all
escapes, integers (decimal, `0x` `0o` `0b`, `_` separators), floats, booleans and comments.
Date and time values are kept as `JString` text (Json has no date type). `inf` and `nan`
are rejected. Defining the same key or table twice is an error.

Writes a `Json` object: scalars and arrays first as `key = value`, then sub-tables, then
arrays of tables (`[[name]]`). TOML has no null, so a `JNull` field is left out; a null
inside an array is an error. The output re-reads as the same tree (a float that is a whole
number keeps its `.0`; an `Int` stays an integer).

`use toml` -- or `use toml.Name` to bring one item in bare.

## Example

```
use json
use json.Json
use toml

// A TOML document becomes a Json tree, so the json helpers read it.
fn main() {
    val config = unwrap(toml.parse_toml("""
        title = "demo"

        [server]
        port = 8080
        hosts = ["a.example", "b.example"]

        [[job]]
        name = "backup"

        [[job]]
        name = "report"
        """))

    val server = unwrap(json.json_require_field(config, "server"))
    print(unwrap(json.json_require_int_field(server, "port")))
    print(json.json_to_string(unwrap(json.json_require_field(config, "job"))))

    // An error says where the text went wrong.
    print(is_ok(toml.parse_toml("port = ")))
    print(trim(unwrap(toml.toml_to_string(server))))
}
```

Output:

```
8080
[{"name":"backup"},{"name":"report"}]
false
port = 8080
hosts = ["a.example", "b.example"]
```

## Reading

### `parse_toml`

```
pub fn parse_toml(text: string) -> Result<Json, string>
```

Parses a TOML document into a `Json` object (tables and inline tables become objects, arrays
arrays, integers `JNumber(Int)`, floats `JNumber(Float)`, dates and times `JString`). An Err carries
`line N: ...` for a syntax error, a duplicate key, a table defined twice, a table redefined as a
value (or the reverse), or an unsupported value (`inf`, `nan`).

## Writing

### `toml_to_string`

```
pub fn toml_to_string(j: Json) -> Result<string, string>
```

Writes a `Json` object as TOML: scalars and arrays first as `key = value`, then sub-tables
(`[a.b]`), then arrays of objects (`[[name]]`). Floats are written with full precision and always
look like floats (`150.0`). An Err if `j` is not an object, or a null sits inside an array or an
inline table (TOML has no null; a null field of a table is simply left out).
