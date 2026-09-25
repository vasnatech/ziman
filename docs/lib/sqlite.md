# `sqlite`

The SQLite driver behind `datasource`. A connection is an integer
handle to a native connection object (src/native_sqlite.c, linked against
libsqlite3), closed by `close` or, failing that, when it is garbage collected. This
module encodes `SqlValue` parameters into that native's primitive-only shape and decodes
result cells back. Use it
through `datasource.open("sqlite:...")`; `pg` is the Postgres counterpart.

The natives only ever exchange primitives (ints, strings, arrays of
*tagged strings* -- one tag character then a payload: N null,
I<int>, F<float>, T<text>, B<hex>, b0/b1 bool).

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
pub fn execute(conn: Connection, sql: string, params: SqlValue...) -> Result<int, DbError>
```

Runs a statement, or a `;`-separated script of them, and returns the rows the last statement
changed. `params` bind to the `?` placeholders.

### `query`

```
pub fn query(conn: Connection, sql: string, params: SqlValue...) -> Result<[Row], DbError>
```

Runs a statement and returns its rows; `params` bind to the `?` placeholders.

### `rows`

```
pub fn rows(conn: Connection, sql: string, params: SqlValue...) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Runs a statement and returns its rows lazily, one at a time as a `for` loop asks for them, so a
result of any size uses memory for one row. Each item is a `Result<Row, DbError>`: a row, or the
error that ended the result part-way (after which the iteration ends). The Err on the call is the
statement failing to prepare or bind. The cursor is finalized when the rows are used up, when the
loop is left early, with `iter_close`, or when it is garbage collected. Close cursors before
closing the connection.
