# `ndjson`

Newline-delimited JSON: one JSON value per line, written on top of `json`.
The format of the FHIR bulk-data files and of most log pipelines.

`use ndjson` -- or `use ndjson.Name` to bring one item in bare.

## Example

```
use json
use json.Json
use ndjson

// One JSON value per line: a format for logs and streams.
fn main() {
    val items = unwrap(ndjson.parse_ndjson("""
        {"id": 1, "ok": true}
        {"id": 2, "ok": false}
        """))
    print(len(items))
    print(json.json_to_string(items[1]))
    print(trim(ndjson.ndjson_to_string(items)))
    print(is_ok(ndjson.parse_ndjson("{\"id\": 1}\nnot json")))
}
```

Output:

```
2
{"id":2,"ok":false}
{"id":1,"ok":true}
{"id":2,"ok":false}
false
```

### `parse_ndjson`

```
pub fn parse_ndjson(text: string) -> Result<[Json], string>
```

Every non-blank line as a JSON value, in order. A line that is not valid JSON is an
error naming its (1-based) line number. A trailing CR (CRLF files) is ignored.

### `ndjson_to_string`

```
pub fn ndjson_to_string(items: [Json]) -> string
```

One compact JSON value per line, each line ended by "\n" (so the text ends with a newline).
