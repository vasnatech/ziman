# `db`

What every database module shares: the value, row and error types, the readers that turn a
`SqlValue` into an ordinary value, and the parser for database URLs. `sqlite`, `pg` and `mysql` are
built on it, and so is `datasource`, the module a program calls (`datasource.open`, `datasource.query`, ...), which
resolves a URL to one of the `Driver`s an application registered and pools connections. `Driver` and `Conn` below are
that contract: they are why `datasource` names no engine. Anything that maps its own types to and from SQL (`uuid`, `time`,
`#[derive(Row)]`-generated code) uses this module alone, so it needs no driver.

Only primitives cross the driver boundary: NULL, integer, float, text, bool, and bytes. Uuid,
LocalDate, Instant, decimal and bigint all travel as `Text` and are converted by the mapping layer --
that sidesteps every dialect difference in how SQLite and Postgres type those (SQLite has no UUID or
timestamp type at all).

The URL parser reads `scheme://user[:password]@host[:port]/database[?k=v&...]`. User, password,
database and query names *and* values are percent-decoded, so a password may contain `@`, `:` or `/`
when written as %40, %3A, %2F (a literal `@` in the password also works: the LAST `@` separates
userinfo from host). With no database, the user's name stands in, as Postgres itself does.

**A host may be a bracketed IPv6 literal**, `[::1]:5432`; the brackets keep the address's own colons
out of the host/port split and are not part of the address, so `host` holds the bare `::1` -- which is
what `net.connect` resolves (it answers "host not found" for the bracketed form).

**A port must be in 1..65535, and a query parameter must have a `=`.** Both are refused here rather
than carried into a driver: a port of `0` or `70000` reaches `net.connect` and comes back as a
connection problem rather than a typo, and a parameter with no value is dropped in silence -- which
matters because both drivers read `sslmode` from these and default it to `disable` when it is absent,
so `?sslmode` instead of `?sslmode=require` would be an unencrypted connection with nothing said.

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
    val row = Row {
        columns: ["id", "name", "active"],
        values: [SqlValue.Int(1), SqlValue.Text("Ada"), SqlValue.Int(1)]
    }

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

## The driver contract

### `Conn`

```
pub struct Conn {
    dialect: string
    close: fn()
    execute: fn(string, [SqlValue]) -> Result<int, DbError>
    query: fn(string, [SqlValue]) -> Result<[Row], DbError>
    rows: fn(string, [SqlValue]) -> Result<Iter<Result<Row, DbError>>, DbError>
}
```

One open connection, as the four functions `datasource` needs from it.

**Every field is a closure that already holds the driver's own connection**, and that is what
lets `datasource` drive an engine it cannot name: there is no type here for a
`sqlite.Connection` or a `pg.Connection` to be, so none has to exist. Before this, `datasource`
held an enum of the three concrete connection types and switched over it in five places, which
made it the one module that had to change whenever an engine was added.

**`close` belongs here and not on `Driver`** -- that is the one forced move in the shape. A
`close(connection)` on the driver would need a parameter type for the connection, which is
exactly the concrete type the closure is hiding; as a closure with no arguments it needs none.

`dialect` rides along because `migrate` asks for it, and reading it from the connection is what
lets a pool answer without re-parsing its URL.

### `Driver`

```
pub struct Driver {
    name: string
    prefixes: [string]
    poolable: fn(string) -> bool
    idle_check: bool
    open: fn(string) -> Result<Conn, DbError>
}
```

One engine, as an application hands it over: `datasource.registry([sqlite.driver(), pg.driver()])`.
A program that talks to SQLite alone names SQLite alone, and nothing it links pulls in `pg` or
`mysql`.

`name` is the dialect too, deliberately one field rather than two: `migrate` compares it against
"sqlite", "postgres" and "mysql", and a separate `dialect` could drift from the name.

`prefixes` is how a URL finds its engine, so each engine owns its own URL spelling -- `sqlite`
strips its own `sqlite:` rather than having `datasource` do it with a `substring(url, 7, ..)`.

`poolable` answers whether several connections to this URL would be the same database. Only
SQLite says no, and only for `:memory:`, where each connection is a private database and a pool
of four would be four different ones. Asking the driver puts that where the knowledge is.

`idle_check` answers whether a connection that has sat idle is worth testing before it is lent
out again. A connection over a socket can be closed from the far end while nobody is looking;
an open file cannot, so SQLite says no. It is a plain `bool` where `poolable` is a function
because it is a fact about the engine, while poolability is a fact about the URL.

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

A scan of the row's own names, which is the right shape for one lookup and **is most of what it
costs to read a wide result**. Measured on a 12-column row: 290 ns for the first column, 790 for
the last, and 8 microseconds to read every column of one row -- so a thousand rows of twelve
columns spend about 8 milliseconds here, which for a query that takes 5 to 20 on the wire is a
third of the client's own work.

The structural answer is an index built once per result and shared by its rows, the way
`columns` already is: measured at 215 ns a lookup against 790, paying for itself after five
lookups in a result set and saving about 9 milliseconds on a thousand-row read. It is not here
because the callers that would benefit are the `from_row` functions `#[derive(Row)]` generates,
so adopting it means changing what that emits -- a deliberate increment with these numbers in
hand, not a change to make in passing.

Reading the length once rather than per iteration was tried and is **not** a win: interleaved
A/B, it takes 17-20% off a scan that walks twelve columns or more and adds 3-7% to one that
stops at the first or second, and reading every column of a 12-column row -- which is what a
`from_row` does -- came out the same either way. The cost is the call and the string comparisons,
not the loop condition, so the loop stays in its clearer form.

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

### `sql_as_decimal`

```
pub fn sql_as_decimal(v: SqlValue) -> Result<decimal, DbError>
```

A decimal column: TEXT holding decimal text (how `#[derive(Row)]` writes one, keeping its scale), or a whole-number or float
column read as a decimal.

### `sql_lift`

```
pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>
```

Lifts a domain type's own parse result (Result<T, string>, e.g.
uuid.parse) into the DbError error type a from_sql needs.

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

Decodes `%XX` escapes in `s`; an Err for a malformed escape or one that yields a NUL byte. The loop is
`url.percent_decode`, shared with `http`; this wraps its message in the `DbError` the rest of this module
answers with, and says which URL was at fault, which a caller cannot work out from "bad %-escape" alone.
`+` is a literal plus here: a database URL is not a query string.

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

## What the network drivers share

### `QueryResult`

```
pub struct QueryResult {
    rows: [Row]
    affected: int
}
```

What a statement produced: its rows and how many rows it changed.

### `open_rows`

```
pub fn open_rows(
    requests: Channel<Request>,
    sql: string,
    params: [SqlValue]
) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Opens a cursor for `sql` on the connection whose owning thread reads `requests`, and returns its rows
lazily as an `Iter`: the first page is fetched before this returns (so a failing statement is an Err here),
and later pages as the loop asks for them. Each item is a `Result<Row, DbError>`: a row, or the error that
ended the result part-way (after which the iteration ends). Leaving the loop early or `iter_close` ends the
cursor, and a cursor dropped unfinished is ended when it is garbage collected.
