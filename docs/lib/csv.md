# `csv`

CSV (RFC 4180) as `Json`: a table is a `Json` array of objects, one per data row and
keyed by the header row, so any `#[derive(Json)]` type decodes from a row with its own
`_from_json` and encodes back. Splitting the text into cells is a native scanner
(`scan_cells`); the rest is Ziman.

Every function that takes text and returns a table comes in two forms: the plain one uses
the comma, and the `_with` form takes a one-character delimiter (`"\t"` for TSV). They are
separate functions because Ziman can only omit an optional parameter in a call that names
its arguments. A delimiter that is not exactly one character, or is a quote or a newline,
is an `Err`.

Cells are text. A reader returns them as strings (`parse_csv`), or infers a type for the
cells that look like one (`parse_csv_typed`); see each function. The writer quotes exactly
the strings that would otherwise read back as something else, so `parse_csv_typed` of what
`csv_to_string` wrote gives the same table.

`use csv` -- or `use csv.Name` to bring one item in bare.

## Example

```
use csv
use json
use json.Json

fn main() {
    val table = unwrap(csv.parse_csv("""
        name,age
        Ada,36
        Grace,45
        """))
    print(json.json_to_string(table))

    // Typed parsing reads 36 as a number; writing it back gives the same text.
    val typed = unwrap(csv.parse_csv_typed("name,age\nAda,36\n"))
    print(json.json_to_string(typed))
    print(trim(unwrap(csv.csv_to_string(typed))))
}
```

Output:

```
[{"name":"Ada","age":"36"},{"name":"Grace","age":"45"}]
[{"name":"Ada","age":36}]
name,age
Ada,36
```

## Reading

### `parse_csv_rows`

```
pub fn parse_csv_rows(text: string) -> Result<[[string]], string>
```

The raw table as strings, header row included: `[["name", "age"], ["Ada", "36"]]`. Quotes
are removed and doubled quotes collapsed; nothing else is interpreted. Blank lines are
skipped and a leading byte order mark is ignored.

### `parse_csv_rows_with`

```
pub fn parse_csv_rows_with(text: string, delimiter: string) -> Result<[[string]], string>
```

parse_csv_rows with another one-character delimiter, e.g. "\t" for TSV.

### `parse_csv`

```
pub fn parse_csv(text: string) -> Result<Json, string>
```

The first row names the columns; every later row becomes a JSON object of strings keyed by
them, so `#[derive(Json)]` types with `string` fields decode straight from it. An Err for a
duplicate column name, or for a row whose cell count differs from the header's (the message
names the line). An empty file is an empty array.

### `parse_csv_with`

```
pub fn parse_csv_with(text: string, delimiter: string) -> Result<Json, string>
```

parse_csv with another one-character delimiter.

### `parse_csv_typed`

```
pub fn parse_csv_typed(text: string) -> Result<Json, string>
```

parse_csv, but an *unquoted* cell becomes a number (`42` an int, `-1.5` or `2e3` a float; not
`007`, `+1` or `1_000`, which stay strings), `true` / `false` a boolean, and an empty cell
null. A quoted cell is always a string. This is what lets `#[derive(Json)]` types with `int`,
`float`, `bool` and `Option` fields decode from a row.

### `parse_csv_typed_with`

```
pub fn parse_csv_typed_with(text: string, delimiter: string) -> Result<Json, string>
```

parse_csv_typed with another one-character delimiter.

### `parse_csv_columns`

```
pub fn parse_csv_columns(text: string, columns: [string]) -> Result<Json, string>
```

For a file with no header row: every row is data, and `columns` names the cells. Each row
must have `len(columns)` cells.

### `parse_csv_columns_typed`

```
pub fn parse_csv_columns_typed(text: string, columns: [string]) -> Result<Json, string>
```

parse_csv_columns with the cell typing of `parse_csv_typed`.

## Writing

### `csv_rows_to_string`

```
pub fn csv_rows_to_string(rows: [[string]]) -> string
```

A table of raw strings as text, one row per line, quoting where a cell needs it. Nothing
is inferred: an empty string is written as an empty cell.

### `csv_rows_to_string_with`

```
pub fn csv_rows_to_string_with(rows: [[string]], delimiter: string) -> Result<string, string>
```

csv_rows_to_string with another one-character delimiter. An Err for an invalid delimiter.

### `csv_to_string`

```
pub fn csv_to_string(rows: Json) -> Result<string, string>
```

A JSON array of objects as CSV: a header row of the union of the rows' keys in first-seen
order, then one line per object; a key a row lacks is an empty cell. An object value is
flattened one level into dotted columns (`address.city`); an array, or an object nested
deeper, is an Err (CSV cannot hold it), as is a row that is not an object.

### `csv_to_string_with`

```
pub fn csv_to_string_with(rows: Json, delimiter: string) -> Result<string, string>
```

csv_to_string with another one-character delimiter.
