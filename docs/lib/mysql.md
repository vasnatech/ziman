# `mysql`

A MySQL / MariaDB client, the third engine behind the `sql`
module (`sql.open("mysql://user:password@host:port/database")`). Written
in Ziman on top of `net` (TCP + TLS), byte arrays (packet framing) and
`crypto` (SHA-1 / SHA-256 for the login scramble); nothing here is
native at all.

What it supports:
  * Authentication: `mysql_native_password` and `caching_sha2_password`
    (the MySQL 8 default), including the server's request to switch
    plugin. caching_sha2's "full authentication" (the first login of a
    user, before the server has cached its hash) sends the password in
    clear, which is only done over TLS -- without TLS the driver refuses
    with a message saying so, rather than fall back to RSA (there is no
    public-key crypto here). Use `sslmode=require` for that first login.
  * TLS, negotiated the MySQL way (an SSLRequest packet, then the same
    connection is upgraded in place). The URL's `sslmode` and
    `sslrootcert` mean what they do for Postgres (see `pg`): `disable`
    (the default), `prefer`, `require` (encrypted, certificate not
    verified), `verify-ca`, `verify-full`.
  * Statements without parameters go through the text protocol
    (COM_QUERY), which also runs a `;`-separated DDL script. Statements
    with `$1, $2, ...` parameters go through the binary protocol
    (prepare / execute / close) -- values always travel as parameters,
    never spliced into the SQL. Ziman's `$N` placeholders are rewritten
    to MySQL's positional `?`, repeating or reordering the values as the
    SQL uses them; quoted strings, backtick names and comments are
    skipped when looking for `$N`.
  * Result values: integers (any width; a BIGINT UNSIGNED above 2^63 is
    an error), floats and doubles become `Int`/`Float`; character
    columns `Text`; binary columns (BLOB, VARBINARY, BIT) `Bytes`;
    DECIMAL, dates, times, JSON, ENUM and SET arrive as `Text`, for the
    mapping layer to convert, exactly as for the other engines. MySQL has
    no boolean type: a BOOLEAN column is a TINYINT and reads back as
    `Int` 0/1.
  * Errors: integrity violations (duplicate key, foreign key, NOT NULL,
    CHECK) are `Constraint`; failing to connect or log in is
    `Connection`; everything else is `Sql`.

What it does not: MySQL's compressed protocol, LOCAL INFILE, the RSA
public-key exchange, client certificates, IPv6 literals in the URL,
stored-procedure OUT parameters, prepared-statement reuse (each
parameterised statement is prepared afresh), multiple result sets
(only the last one is returned) and sending a packet of 16 MiB or more.
`[byte]` is an array of interpreter values, so very large result sets
are slow -- see docs/ROADMAP.md.

`rows` reads a result lazily through a server-side cursor, 500 rows at a time (see `rows`).

One connection is owned by one spawned thread and driven through a
channel of requests, exactly as in `pg`: the protocol is strictly
request/response, so a lone owner thread serialises concurrent callers.

`use mysql` -- or `use mysql.Name` to bring one item in bare.

### `QueryResult`

```
pub struct QueryResult {
    rows: [Row]
    affected: int
}
```

What a statement produced: its rows and how many rows it changed.

### `RunRequest`

```
pub struct RunRequest {
    sql: string
    params: [SqlValue]
    reply_to: Channel<Result<QueryResult, DbError>>
}
```

A statement sent to the thread that owns the connection, with the channel to answer on.

### `Page`

```
pub struct Page {
    rows: [Row]
    finished: bool
    cursor: int
}
```

One batch of a cursor's rows, decoded, whether the server has no more, and the cursor's number (each
cursor a connection opens gets the next one, so a late message about a finished cursor cannot touch
the cursor that replaced it).

### `OpenRequest`

```
pub struct OpenRequest {
    sql: string
    params: [SqlValue]
    reply_to: Channel<Result<Page, DbError>>
}
```

Starts a cursor on the owning thread: run `sql` and answer with its first page.

### `FetchRequest`

```
pub struct FetchRequest {
    cursor: int
    reply_to: Channel<Result<Page, DbError>>
}
```

Asks the owning thread for the next page of the open cursor.

### `EndRequest`

```
pub struct EndRequest {
    cursor: int
    reply_to: Channel<int>
}
```

Asks the owning thread to end the open cursor, if there is one.

### `Request`

```
pub enum Request {
    Run(RunRequest)
    OpenCursor(OpenRequest)
    FetchCursor(FetchRequest)
    EndCursor(EndRequest)
    Close
}
```

A message to a connection's owning thread: run a statement, work a cursor, or close the connection.

## Owner thread API

### `Connection`

```
pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}
```

An open MySQL connection: the channel to the thread that owns its
socket. Opaque to callers -- use `execute`/`query`/`close`.

### `connect`

```
pub fn connect(url: string) -> Result<Connection, DbError>
```

Connects, authenticates, and starts the thread that owns the socket.

### `execute`

```
pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>
```

The number of rows the last statement changed.

### `query`

```
pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>
```

Runs a statement and returns its rows.

### `close`

```
pub fn close(conn: Connection)
```

Asks the connection's thread to say goodbye to the server and close the socket. Returns at once;
the request is queued behind any statement still running.

### `rows`

```
pub fn rows(conn: Connection, sql: string, params: [SqlValue]) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Runs a statement and returns its rows lazily, `cursor_fetch_rows` at a time from a server-side cursor
(a prepared statement opened with CURSOR_TYPE_READ_ONLY), so a result of any size uses memory for one
page. Each item is a `Result<Row, DbError>`: a row, or the error that ended the result part-way (after
which the iteration ends). The Err on the call is the statement failing. A connection holds one cursor
at a time; other statements may run between fetches. A cursor dropped unfinished is ended when it is
garbage collected.
