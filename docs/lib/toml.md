# `toml`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

TOML 1.0 on top of `json`: a document is a `Json` object, so any
`#[derive(Json)]` type decodes from it with its own `_from_json`. Written in pure Ziman.

Marked `#![experimental]` below: unlike `json`, no real Ziman program depends on this
module yet, so treat a gap or an edge case as more likely here than elsewhere in the
library.

Reads tables (`[a.b]`), arrays of tables (`[[a]]`), dotted keys, inline tables, arrays
(any nesting, multi-line, trailing commas), basic / literal / multi-line strings with all
escapes, integers (decimal, `0x` `0o` `0b`, `_` separators), floats, booleans and comments.

**Refuses what the spec refuses, which took a pass of its own.** Appending to a statically
defined array, adding a key or a header to a sealed inline table, redefining a dotted key's
table with a header, an underscore not between two digits, a float with no digit on one side
of its point or in its exponent, a date or time whose components are out of range (`1979-02-30`,
`25:00:00`, `+99:00`) or a time with no seconds, and a raw control character in any string.
A leap second (`23:59:60`) is *allowed*, because RFC 3339 allows one and toml.io is silent.

**Two deliberate deviations, both from the `Json` representation.** Date and time values are
kept as `JString` text, since Json has no date type -- so a document that goes through `parse`
and back through `format` comes out with its dates quoted, as strings, and only the *tree*
round-trips. And `inf`/`nan` are refused (as is any float that overflows to one, such as
`1e400`), because no Json number can hold them.

Defining the same key or table twice is an error.

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
    val config = unwrap(toml.parse("""
        title = "demo"

        [server]
        port = 8080
        hosts = ["a.example", "b.example"]

        [[job]]
        name = "backup"

        [[job]]
        name = "report"
        """))

    val server = unwrap(json.require(config, "server"))
    print(unwrap(json.require_int(server, "port")))
    print(json.format(unwrap(json.require(config, "job"))))

    // An error says where the text went wrong.
    print(is_ok(toml.parse("port = ")))
    print(trim(unwrap(toml.format(server))))
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

### `parse`

```
pub fn parse(text: string) -> Result<Json, string>
```

Parses a TOML document into a `Json` object (tables and inline tables become objects, arrays
arrays, integers `JNumber(Int)`, floats `JNumber(Float)`, dates and times `JString`). An Err carries
`line N: ...` for a syntax error, a duplicate key, a table defined twice, a table redefined as a
value (or the reverse), a value whose components are out of range, or an unsupported value
(`inf`, `nan`).

**Validating costs about a fifth of the parse**, measured: a 203 KB document went from 130 ms to
162 ms with no dates in it, and from 194 ms to 237 ms with 2000 of them. Most of that is not the
dates (about 5 microseconds each) but the control-character test, which every character of every
string must pass -- `parse_basic` went from 5.9% to 7.7% of the profile and `is_control` is 3.3%
of it. There is no cheaper way to know that a string holds no raw control character than to look
at each one.

## Writing

### `format`

```
pub fn format(j: Json) -> Result<string, string>
```

Writes a `Json` object as TOML: scalars and arrays first as `key = value`, then sub-tables
(`[a.b]`), then arrays of objects (`[[name]]`). Floats are written with full precision and always
look like floats (`150.0`). An Err if `j` is not an object, or a null sits inside an array or an
inline table (TOML has no null; a null field of a table is simply left out).
