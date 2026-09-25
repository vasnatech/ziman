# `mysql`

A MySQL / MariaDB client, the third engine behind the `db`
module (`db.open("mysql://user:password@host:port/database")`). Written
in Ziman on top of `net` (TCP + TLS), `bytes` (packet framing) and
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

### `Request`

```
pub enum Request {
    Run(RunRequest)
    Close
}
```

A message to a connection's owning thread: run a statement, or close the connection.

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
