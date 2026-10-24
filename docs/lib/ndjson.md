# `ndjson`

Newline-delimited JSON: one JSON value per line, written on top of `json`.
The format of the FHIR bulk-data files and of most log pipelines.

`use ndjson` -- or `use ndjson.Name` to bring one item in bare.

## Example

```
use fs
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
    print(json.format(items[1]))
    print(trim(ndjson.ndjson_to_string(items)))
    print(is_ok(ndjson.parse_ndjson("{\"id\": 1}\nnot json")))

    // A large file is read one record at a time instead of all at once.
    val path = "/tmp/ziman_manual_ndjson_example.ndjson"
    unwrap(fs.write_text(path, ndjson.ndjson_to_string(items)))
    var ok_records = 0
    val records = unwrap(ndjson.records(path))
    for record in records {
        if unwrap(json.require_bool(unwrap(record), "ok")) {
            ok_records = ok_records + 1
        }
    }
    print(ok_records)
    unwrap(fs.remove(path))
}
```

Output:

```
2
{"id":2,"ok":false}
{"id":1,"ok":true}
{"id":2,"ok":false}
false
1
```

### `parse_ndjson`

```
pub fn parse_ndjson(text: string) -> Result<[Json], string>
```

Every non-blank line as a JSON value, in order. A line that is not valid JSON is an
error naming its (1-based) line number. A trailing CR (CRLF files) is ignored.

### `records`

```
pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>
```

The JSON values of a newline-delimited file, read lazily as a `for` loop asks for them, so a log
of any size uses memory for one record: `val records = try ndjson.records(path)` then
`for record in records { val value = try record ... }`. Each item is a `Result<Json, string>`; blank
lines are skipped, a line that is not valid JSON is an Err item naming its (1-based) line number
(`line 3: unexpected character ...`) and the iteration carries on, and a read error is a last Err
item. The Err on the call itself is the file failing to open. Leaving the loop early closes the file.

### `ndjson_to_string`

```
pub fn ndjson_to_string(items: [Json]) -> string
```

One compact JSON value per line, each line ended by "\n" (so the text ends with a newline).
