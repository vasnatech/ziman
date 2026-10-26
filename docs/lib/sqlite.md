# `sqlite`

The SQLite driver behind `datasource`. A connection is an integer
handle to a native connection object (src/native_sqlite.c, linked against
libsqlite3), closed by `close` or, failing that, when it is garbage collected. This
module encodes `SqlValue` parameters into that native's primitive-only shape and decodes
result cells back. Use it
through `datasource` -- `datasource.open(datasource.registry([sqlite.driver()]), "sqlite:...")`
-- which drives this module through the `db.Driver` that `driver()` below answers, and so never
names it; `pg` is the Postgres counterpart.

The natives only ever exchange primitives (ints, strings, arrays of
*tagged strings* -- one tag character then a payload: N null,
I<int>, F<float>, T<text>, B<hex>, b0/b1 bool, E<message> a cell the driver will not hand over).

**`F` carries `%.17g` in both directions**, which is what makes a float survive the round trip
exactly -- Ziman's own `str(float)` is six significant digits and would quietly round a value on
its way to a column.

**`E` exists because SQLite's TEXT is not a Ziman string.** SQLite stores whatever bytes it is
given and still calls them text, so a TEXT value may hold a NUL byte or bytes that are not valid
UTF-8; a Ziman string may hold neither. Those are refused in the native, where the bytes are
still in hand, and arrive here as an ordinary `DbError` naming `CAST(col AS BLOB)` as the
lossless way to read such a column.

`use sqlite` -- or `use sqlite.Name` to bring one item in bare.

## Example

```
use db.SqlValue
use db.row_get
use db.sql_as_int
use fs
use sqlite

// The SQLite driver on its own. Most programs use it through `datasource.open("sqlite:...")` instead.
fn main() {
    val path = "/tmp/ziman_manual_sqlite_example.db"
    val conn = unwrap(sqlite.open(path))
    unwrap(sqlite.execute(conn, "CREATE TABLE t (n INTEGER, label TEXT)"))
    unwrap(sqlite.execute(conn, "INSERT INTO t VALUES (?, ?)", SqlValue.Int(7), SqlValue.Text("seven")))
    val rows = unwrap(sqlite.query(conn, "SELECT n FROM t"))
    print(unwrap(sql_as_int(unwrap(row_get(rows[0], "n")))))
    sqlite.close(conn)
    unwrap(fs.remove(path))
}
```

Output:

```
7
```

### `Connection`

```
pub struct Connection {
    handle: int
}
```

An open SQLite connection: a handle to the native connection. Opaque to
callers -- use `execute`/`query`/`close`.

### `open`

```
pub fn open(path: string) -> Result<Connection, DbError>
```

`path` is a SQLite filename, or `:memory:`.

### `close`

```
pub fn close(conn: Connection)
```

Closes the database. Using the connection afterwards is an error, not a crash; one never closed
is closed when it is garbage collected, but a file database stays locked until then.

### `execute`

```
pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>
```

Runs a statement, or a `;`-separated script of them, and returns the rows the last statement
changed. `params` bind to the `?` placeholders.

### `query`

```
pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>
```

Runs a statement and returns its rows; `params` bind to the `?` placeholders.

### `rows`

```
pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Runs a statement and returns its rows lazily, one at a time as a `for` loop asks for them, so a
result of any size uses memory for one row. Each item is a `Result<Row, DbError>`: a row, or the
error that ended the result part-way (after which the iteration ends). The Err on the call is the
statement failing to prepare or bind. The cursor is finalized when the rows are used up, when the
loop is left early, with `iter_close`, or when it is garbage collected. Close cursors before
closing the connection.

## The driver

### `driver`

```
pub fn driver() -> Driver
```

SQLite as a `db.Driver`, for `datasource.registry([sqlite.driver(), ..])`. The closures in the
`Conn` hold the connection, so nothing above here needs a name for it.
