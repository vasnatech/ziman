# `sql`

The value/row/error vocabulary shared by the `db` driver
module and by anything that maps its own types to and from SQL (uuid,
time, and `#[derive(Row)]`-generated code). Pure Ziman, no natives.
Split out of `db` so a type like Uuid can provide its own
uuid_to_sql/uuid_from_sql without dragging in a whole database driver.

Only primitives cross the driver boundary: NULL, integer, float, text,
bool, and bytes. Uuid, LocalDate, Instant, decimal and bigint all
travel as `Text` and are converted by the mapping layer -- that
sidesteps every dialect difference in how SQLite and Postgres type
those (SQLite has no UUID or timestamp type at all).

`use sql` -- or `use sql.Name` to bring one item in bare.

### `SqlValue`

```
pub enum SqlValue {
    Null
    Int(int)
    Float(float)
    Text(string)
    Bool(bool)
    Bytes([byte])
}
```

One database value: SQL NULL, an integer, a float, text, a boolean or a blob.

### `DbError`

```
pub enum DbError {
    NotFound(string)
    Constraint(string)
    Connection(string)
    Sql(string)
}
```

What a database call can fail with. `NotFound` isn't something a
driver raises -- it's for a repository whose lookup came back empty,
kept here so one error type covers the whole data-access layer.
`Constraint` is a unique/foreign-key/not-null/check violation (the
caller did something the schema forbids); `Connection` is failing to
reach or open the database at all; `Sql` is everything else (bad
syntax, wrong parameter count, a value of the wrong SQL type).

### `Row`

```
pub struct Row {
    columns: [string]
    values: [SqlValue]
}
```

One result row: `values[i]` belongs to column `columns[i]`.

### `db_error_message`

```
pub fn db_error_message(e: DbError) -> string
```

`e` as a sentence for a log line or a response, prefixed by its kind (`NotFound` is the bare
message).

### `row_get`

```
pub fn row_get(row: Row, column: string) -> Result<SqlValue, DbError>
```

The value of column `column` in `row`, or a DbError.Sql naming the missing column.

### `sql_as_text`

```
pub fn sql_as_text(v: SqlValue) -> Result<string, DbError>
```

The text of a TEXT value, or a DbError.Sql saying what was found instead.

### `sql_as_int`

```
pub fn sql_as_int(v: SqlValue) -> Result<int, DbError>
```

The number in an INTEGER value, or a DbError.Sql saying what was found instead.

### `sql_as_bool`

```
pub fn sql_as_bool(v: SqlValue) -> Result<bool, DbError>
```

SQLite hands an integer column back for a value stored as 0/1 where
Postgres would say Bool -- accept an integer that's exactly 0 or 1 too.

### `sql_as_float`

```
pub fn sql_as_float(v: SqlValue) -> Result<float, DbError>
```

A whole-number column read as a float, or a float column as itself.

### `sql_lift`

```
pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>
```

Lifts a domain type's own parse result (Result<T, string>, e.g.
uuid_from_string) into the DbError error type a from_sql needs.
