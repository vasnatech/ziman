# `ndjson`

Newline-delimited JSON: one JSON value per line, written on top of `json`.
The format of the FHIR bulk-data files and of most log pipelines.

`use ndjson` -- or `use ndjson.Name` to bring one item in bare.

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
