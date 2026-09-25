# `sqlite`

The SQLite driver behind `db`. A connection is an integer
handle to a native connection object (src/native_sqlite.c, linked against
libsqlite3), closed by `close` or, failing that, when it is garbage collected. This
module encodes `SqlValue` parameters into that native's primitive-only shape and decodes
result cells back. Use it
through `db.open("sqlite:...")`; `pg` is the Postgres counterpart.

The natives only ever exchange primitives (ints, strings, arrays of
*tagged strings* -- one tag character then a payload: N null,
I<int>, F<float>, T<text>, B<hex>, b0/b1 bool).

`use sqlite` -- or `use sqlite.Name` to bring one item in bare.

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
pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>
```

Runs a statement, or a `;`-separated script of them, and returns the rows the last statement
changed. `params` bind to the `?` placeholders.

### `query`

```
pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>
```

Runs a statement and returns its rows; `params` bind to the `?` placeholders.
