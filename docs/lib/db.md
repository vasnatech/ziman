# `db`

What every database module shares: the value, row and error types, the readers that turn a
`SqlValue` into an ordinary value, and the parser for database URLs. `sqlite`, `pg` and `mysql` are
built on it, and so is `sql`, the module a program calls (`sql.open`, `sql.query`, ...), which picks
the driver and pools connections. Anything that maps its own types to and from SQL (`uuid`, `time`,
`#[derive(Row)]`-generated code) uses this module alone, so it needs no driver.

Only primitives cross the driver boundary: NULL, integer, float, text, bool, and bytes. Uuid,
LocalDate, Instant, decimal and bigint all travel as `Text` and are converted by the mapping layer --
that sidesteps every dialect difference in how SQLite and Postgres type those (SQLite has no UUID or
timestamp type at all).

The URL parser reads `scheme://user[:password]@host[:port]/database[?k=v&...]`. User, password,
database and query values are percent-decoded, so a password may contain `@`, `:` or `/` when written
as %40, %3A, %2F (a literal `@` in the password also works: the LAST `@` separates userinfo from host).

`use db` -- or `use db.Name` to bring one item in bare.

## Example

```
use db
use db.DbError
use db.Row
use db.SqlValue
use db.db_error_message
use db.row_get
use db.sql_as_bool
use db.sql_as_int
use db.sql_as_text

// What the database modules share: a value, a row, the error type, and the URL parser.
fn main() {
    val row = Row { columns: ["id", "name", "active"],
        values: [SqlValue.Int(1), SqlValue.Text("Ada"), SqlValue.Int(1)] }

    print(unwrap(sql_as_int(unwrap(row_get(row, "id")))))
    print(unwrap(sql_as_text(unwrap(row_get(row, "name")))))
    // SQLite has no boolean type, so a 0 or 1 integer reads as one.
    print(unwrap(sql_as_bool(unwrap(row_get(row, "active")))))

    // Asking for the wrong type, or a column that is not there, is a DbError.
    print(db_error_message(unwrap_err(sql_as_int(SqlValue.Text("x")))))
    print(db_error_message(unwrap_err(row_get(row, "email"))))

    // A database URL, with its credentials decoded (`%40` is `@`); a missing port takes the default.
    val u = unwrap(db.parse_url("postgres://ada:p%40ss@db.example:6543/health?sslmode=require", ["postgres://"], 5432))
    print(u.user)
    print(u.password)
    print(u.host)
    print(u.port)
    print(u.database)
    print(db.url_param(u, "sslmode"))
    print(unwrap(db.parse_url("postgres://ada@localhost/health", ["postgres://"], 5432)).port)
    print(is_ok(db.parse_url("mysql://x@h/d", ["postgres://"], 5432)))
}
```

Output:

```
1
Ada
true
database error: expected INTEGER, found TEXT
database error: no such column in result row: email
ada
p@ss
db.example
6543
health
Some(require)
5432
false
```

## Values, rows, errors

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

## URLs

### `UrlParam`

```
pub struct UrlParam {
    name: string
    value: string
}
```

One `name=value` pair from the query part of a database URL, already decoded.

### `DbUrl`

```
pub struct DbUrl {
    user: string
    password: string
    host: string
    port: int
    database: string
    params: [UrlParam]
}
```

A parsed database URL: the decoded credentials, host, port (0 when the URL names none), database
and query parameters.

### `percent_decode`

```
pub fn percent_decode(s: string) -> Result<string, DbError>
```

Decodes `%XX` escapes in `s`; an Err for a malformed escape or one that yields a NUL byte.

### `url_param`

```
pub fn url_param(u: DbUrl, name: string) -> Option<string>
```

The query parameter `name`, if the URL has it.

### `parse_url`

```
pub fn parse_url(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>
```

`prefixes` are the accepted `scheme://` starts (e.g. "postgres://").
The database defaults to the user name when the URL has none.
