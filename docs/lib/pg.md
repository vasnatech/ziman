# `pg`

A PostgreSQL client, the second backend behind the `datasource`
module (`datasource.open("postgres://user:password@host:port/database")`).
Written in Ziman on top of `net` (TCP), byte arrays (message framing) and
`crypto` (SCRAM-SHA-256); nothing here is native but the exact float
formatting. It speaks protocol version 3.0.

What it supports: trust, cleartext-password and SCRAM-SHA-256
authentication (the default since Postgres 14); the simple query
protocol for parameterless statements (which also lets `execute` take a
`;`-separated DDL script) and the extended protocol (Parse/Bind/
Describe/Execute/Sync) for statements with bound `$1, $2, ...`
parameters -- values always travel as parameters, never spliced into the
SQL. Everything is exchanged in text format, so a value's meaning comes
from the result column's type OID: integers, floats, booleans and bytea
become the matching `SqlValue`, and everything else (text, uuid, dates
and timestamps, numeric, json, ...) arrives as `Text` and is converted
by the mapping layer, exactly as it is for SQLite.

TLS is negotiated the way Postgres does it: an SSLRequest packet, the
server's one-byte answer ('S' to proceed, 'N' if it has no TLS), then
the same connection is upgraded in place (`net.start_tls_with`) before
the startup message. The URL's `sslmode` follows libpq's meanings:
`disable` (no TLS -- the default here, unlike libpq's `prefer`),
`prefer` (use TLS if the server offers it, else plaintext; no
verification), `require` (TLS or fail, but the certificate is NOT
verified -- encrypted, yet open to a man in the middle, exactly as in
libpq), `verify-ca` (also verify the certificate chain) and
`verify-full` (also that the certificate is valid for the host you
asked for -- the one to use for a hosted database). `sslrootcert=<path>`
names a PEM file of trusted CAs; without it the system CA store is
used (libpq reads ~/.postgresql/root.crt instead). `allow` is not
supported, and if a `prefer` handshake fails there is no retry over a
fresh plaintext connection.

What it does not: MD5 authentication (deprecated; removed as the
default in Postgres 14), Kerberos/GSS, client certificates, IPv6
literals in the URL, the COPY protocol, LISTEN/NOTIFY,
prepared-statement reuse (every parameterised statement is parsed
afresh), or SASLprep normalisation of a non-ASCII password. `[byte]` is an array of interpreter values, so
very large result sets are slow -- see docs/ROADMAP.md.

`rows` reads a result lazily through an extended-protocol portal, 500 rows at a time (see `rows`).

One connection is owned by one spawned thread and driven through a
channel of requests -- Ziman has no mutex, and the protocol is strictly
request/response, so a lone owner thread is what serialises concurrent
callers (each blocks on its own reply channel). That also means there
is no lock a `?` returning early can leave held. A *transaction* is
still per-connection state on the server: statements from other
threads interleave inside it, as documented for `datasource`.

`use pg` -- or `use pg.Name` to bring one item in bare.

### `float_to_text`

```
pub native fn float_to_text(f: float) -> string
```

A float as the shortest text that reads back as the same float, for binding as a parameter.

## Owner thread API

### `Connection`

```
pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}
```

An open Postgres connection: the channel to the thread that owns its
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

Runs a statement and returns its rows lazily, `cursor_fetch_rows` at a time, so a result of any size
uses memory for one page. Each item is a `Result<Row, DbError>`: a row, or the error that ended the
result part-way (after which the iteration ends). The Err on the call is the statement failing. The
connection carries no other statement until the cursor is read to the end, left early or closed
with `iter_close`: a statement sent meanwhile is an Err. A cursor dropped unfinished is ended when
it is garbage collected.
