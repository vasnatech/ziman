# `csv`

CSV (RFC 4180) as `Json`: a table is a `Json` array of objects, one per data row and
keyed by the header row, so any `#[derive(Json)]` type decodes from a row with its own
`_from_json` and encodes back. Splitting the text into cells is a native scanner
(`scan_cells`); the rest is Ziman.

**The delimiter is an argument, not a name.** Every reader and writer here has a one-argument
form that uses the comma and a two-argument form taking a one-character delimiter (`"\t"` for
TSV) -- two declarations under one name, told apart by arity. They cannot be one declaration
with an `Option<string>`: Ziman omits an optional parameter only in a call that names *every*
argument, so `parse(text)` would have to be written `parse(text: text)`. A delimiter that is
not exactly one character, or is a quote or a newline, is an `Err`.

**`typed` is a name, not an argument**, for the mirror-image reason: `parse(text, true)` tells
a reader nothing, and an `Option<bool>` would cost the common call its positional form. So the
typing reader is `parse_typed`.

Cells are text. A reader returns them as strings (`parse`), or infers a type for the cells that
look like one (`parse_typed`); see each function. The writer quotes exactly the strings that
would otherwise read back as something else, so `parse_typed` of what `format` wrote gives the
same table.

Nothing here carries the module's own name: a caller writes `csv.parse(text)` and
`csv.format(rows)`, where `csv.parse_csv` and `csv.csv_to_string` stuttered.

`use csv` -- or `use csv.Name` to bring one item in bare.

## Example

```
use csv
use fs
use json
use json.Json

fn main() {
    val table = unwrap(csv.parse("""
        name,age
        Ada,36
        Grace,45
        """))
    print(json.format(table))

    // Typed parsing reads 36 as a number; writing it back gives the same text.
    val typed = unwrap(csv.parse_typed("name,age\nAda,36\n"))
    print(json.format(typed))
    print(trim(unwrap(csv.format(typed))))

    // A big file is read one record at a time.
    val path = "/tmp/ziman_manual_csv_example.csv"
    unwrap(fs.write_text(path, "name,note\nAda,\"likes maths\"\nGrace,\"two\nlines\"\n"))
    val records = unwrap(csv.records(path))
    for record in records {
        print(json.format(unwrap(record)))
    }
    unwrap(fs.remove(path))
}
```

Output:

```
[{"name":"Ada","age":"36"},{"name":"Grace","age":"45"}]
[{"name":"Ada","age":36}]
name,age
Ada,36
{"name":"Ada","note":"likes maths"}
{"name":"Grace","note":"two\nlines"}
```

## Reading

### `parse_rows`

```
pub fn parse_rows(text: string) -> Result<[[string]], string>
```

The raw table as strings, header row included: `[["name", "age"], ["Ada", "36"]]`. Quotes
are removed and doubled quotes collapsed; nothing else is interpreted. Blank lines are
skipped and a leading byte order mark is ignored.

```
pub fn parse_rows(text: string, delimiter: string) -> Result<[[string]], string>
```

The same with another one-character delimiter, `"\t"` for TSV.

### `parse`

```
pub fn parse(text: string) -> Result<Json, string>
```

The first row names the columns; every later row becomes a JSON object of strings keyed by
them, so `#[derive(Json)]` types with `string` fields decode straight from it. An Err for a
duplicate column name, or for a row whose cell count differs from the header's (the message
names the line). An empty file is an empty array.

```
pub fn parse(text: string, delimiter: string) -> Result<Json, string>
```

The same with another one-character delimiter.

### `parse_typed`

```
pub fn parse_typed(text: string) -> Result<Json, string>
```

`parse`, but an *unquoted* cell becomes a number (`42` an int, `-1.5` or `2e3` a float; not
`007`, `+1` or `1_000`, which stay strings), `true` / `false` a boolean, and an empty cell
null. A quoted cell is always a string. This is what lets `#[derive(Json)]` types with `int`,
`float`, `bool` and `Option` fields decode from a row.

**A name of its own rather than `parse(text, typed: true)`**, which the argument list cannot
say: a bare `parse(text, true)` tells a reader nothing, and an `Option<bool>` parameter would
make the common `parse(text)` have to be written `parse(text: text)` -- an `Option` parameter
is omittable only in an all-named call.

```
pub fn parse_typed(text: string, delimiter: string) -> Result<Json, string>
```

`parse_typed` with another one-character delimiter.

### `parse_columns`

```
pub fn parse_columns(text: string, columns: [string]) -> Result<Json, string>
```

For a file with no header row: every row is data, and `columns` names the cells. Each row
must have `len(columns)` cells.

```
pub fn parse_columns(text: string, columns: [string], delimiter: string) -> Result<Json, string>
```

`parse_columns` with another one-character delimiter. There was no way to say this before:
the headerless readers took the comma and nothing else.

### `parse_columns_typed`

```
pub fn parse_columns_typed(text: string, columns: [string]) -> Result<Json, string>
```

`parse_columns` with the cell typing of `parse_typed`.

```
pub fn parse_columns_typed(text: string, columns: [string], delimiter: string) -> Result<Json, string>
```

`parse_columns_typed` with another one-character delimiter.

## Reading a file lazily

### `records`

```
pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>
```

A CSV file read lazily, one record at a time as a `for` loop asks for them, so a file of any size uses
memory for one record: `val records = try csv.records(path)` then `for record in records { val row =
try record ... }`. The first record names the columns and every later one is a JSON object of strings keyed
by them, as in `parse`. Each item is a `Result<Json, string>`: an object, or an Err for a record
with the wrong number of cells, a stray quote, a quote never closed, a line with a NUL byte or a read
error; each message names the line the record started on, and the iteration carries on after all but a
read error or an unclosed quote. The Err on the call is the file failing to open, an invalid header
or a duplicate column name. A quoted cell may contain newlines (the record then spans lines). Leaving the
loop early closes the file.

```
pub fn records(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>
```

`records` with another one-character delimiter.

### `records_typed`

```
pub fn records_typed(path: string) -> Result<Iter<Result<Json, string>>, string>
```

`records` with the cell typing of `parse_typed`: unquoted numbers, booleans and empty cells.

```
pub fn records_typed(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>
```

`records_typed` with another one-character delimiter.

## Writing

### `format`

```
pub fn format(rows: [[string]]) -> string
```

A table of raw strings as text, one row per line, quoting where a cell needs it. Nothing
is inferred: an empty string is written as an empty cell.

**This is the one member of `format` that is not a `Result`**, and the arity is why: a comma
is a known-good delimiter and raw strings need no conversion, so with nothing to validate
there is nothing to fail. The form below takes a delimiter, which can be invalid, so it
answers a `Result` -- a difference in return type that only two separate declarations can
state, and `Option` could not.

```
pub fn format(rows: [[string]], delimiter: string) -> Result<string, string>
```

The same with another one-character delimiter. An Err for an invalid delimiter.

```
pub fn format(rows: Json) -> Result<string, string>
```

A JSON array of objects as CSV: a header row of the union of the rows' keys in first-seen
order, then one line per object; a key a row lacks is an empty cell. An object value is
flattened one level into dotted columns (`address.city`); an array, or an object nested
deeper, is an Err (CSV cannot hold it), as is a row that is not an object.

```
pub fn format(rows: Json, sep: string) -> Result<string, string>
```

The same with another one-character delimiter.
