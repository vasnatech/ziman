# `pg`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A PostgreSQL client, the second backend behind the `datasource`
module, which reaches it through the `db.Driver` that `driver()` below answers:
`datasource.open(datasource.registry([pg.driver()]), "postgres://user:password@host:port/database")`.
Written in Ziman on top of `net` (TCP), byte arrays (message framing) and
`crypto` (SCRAM-SHA-256); nothing here is native but the exact float
formatting. It speaks protocol version 3.0.

Marked `#![experimental]` below: a real wire protocol reimplemented end to end, the
largest externally-specified surface in the library, so treat a protocol-conformance
gap as more likely here than elsewhere.

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
the same connection is upgraded in place (`net.start_tls`) before
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
very large result sets are slow -- see docs/roadmap/.

`rows` reads a result lazily through an extended-protocol portal, 500 rows at a time (see `rows`).

One connection is owned by one spawned thread and driven through a
channel of requests -- Ziman has no mutex, and the protocol is strictly
request/response, so a lone owner thread is what serialises concurrent
callers (each blocks on its own reply channel). That also means there
is no lock a `try` returning early can leave held. A *transaction* is
still per-connection state on the server: statements from other
threads interleave inside it, as documented for `datasource`.

`use pg` -- or `use pg.Name` to bring one item in bare.

## Example

```
use datasource
use db.DbError
use db.db_error_message
use pg

// The PostgreSQL driver. Most programs never name it: they register it and reach Postgres through
// `datasource`, so the same code runs against SQLite in a test and Postgres in production with
// only the URL changing. What is here is what is specific to Postgres.
//
// Everything past a connection needs a live server, so that half lives in
// `tests/integration/pg/`, which the suite runs when one is available. This example is the half
// that does not: the registration, the URL, the value formatting, and what a failure looks like.
val drivers = datasource.registry([pg.driver()])

fn demo() -> Result<unit, DbError> {
    // Postgres writes a float with enough digits to read back the same value, which is why this
    // is not the shortest text that looks right: 0.1 is not 0.1.
    print(pg.float_to_text(0.1))
    print(pg.float_to_text(1.0))
    print(pg.float_to_text(1.0 / 3.0))

    // A URL carries the host, the database and the options; `db.parse_url` is what reads one, and
    // `datasource.open` is what hands it to this driver.
    if Err(e) = datasource.open(drivers, "postgres://nobody@127.0.0.1:1/none") {
        // A connection failure is a DbError like any other, so a caller handles it the same way
        // it handles a constraint violation -- there is nothing driver-shaped to catch.
        print(db_error_message(e))
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("pg example failed: {}\n", db_error_message(e))
    }
}
```

Output:

```
0.10000000000000001
1
0.33333333333333331
database connection error: connection refused: Connection refused
```

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
pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>
```

The number of rows the last statement changed.

### `query`

```
pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>
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
pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Runs a statement and returns its rows lazily, `cursor_fetch_rows` at a time, so a result of any size
uses memory for one page. Each item is a `Result<Row, DbError>`: a row, or the error that ended the
result part-way (after which the iteration ends). The Err on the call is the statement failing. The
connection carries no other statement until the cursor is read to the end, left early or closed
with `iter_close`: a statement sent meanwhile is an Err. A cursor dropped unfinished is ended when
it is garbage collected.

## The driver

### `driver`

```
pub fn driver() -> Driver
```

This engine as a `db.Driver`, for `datasource.registry([..])`. The closures in the `Conn` hold
the connection, so nothing above here needs a name for it.
