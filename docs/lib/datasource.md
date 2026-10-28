# `datasource`

The module a program calls to use a database, the shape of JDBC: a connection
handle plus execute/query taking SQL text and *bound* parameters (a
value is never spliced into the SQL string), transactions as plain
begin/commit/rollback, and rows handed back as `db.Row`. Which
engine sits behind a handle is chosen by the URL given to `open`:
`sqlite::memory:`, `sqlite:<path>` or
`postgres://user:password@host:port/database` or
`mysql://user:password@host:port/database` -- the API above doesn't
change, so repositories written against it don't either.

**This module names no engine.** An application says which it links:


```
val reg = datasource.registry([sqlite.driver(), pg.driver()])
val db = try datasource.open_pool(reg, url, 10)
```

and everything after that takes the `Connection` and never mentions a
driver again. A `db.Driver` is an engine (a name, the URL prefixes it
claims, whether a URL of its can be pooled, whether an idle connection
of its can go stale, and how to open one); a `db.Conn` is one open
connection as four closures that already hold it, which is what lets
this module work an engine it has no type for. Each engine has its own
driver module: `sqlite` (a thin layer over the C library), `pg` and
`mysql` (both written in Ziman, on top of `net`), sharing the value,
row and error types and URL parsing through `db`.

It used to hold an enum of the three concrete connection types and
switch over it in five places, plus the URL-scheme map, the rule that
`sqlite::memory:` cannot be pooled and the rule that a SQLite
connection is never probed -- so adding a fourth engine meant editing
this file in eight places. Now it means writing a `driver()` and
putting it in the registry.

Concurrency: sharing one `Connection` between threads is safe
statement by statement -- SQLite takes the connection's own lock, and a
Postgres connection is owned by one thread that serialises requests. A
transaction is NOT safe to share -- another thread's statements can
interleave inside it -- so run a transaction from one thread. Use
`with_transaction` (below), or by hand with `defer`:


```
try datasource.begin(conn)
defer datasource.rollback(conn)
try datasource.execute(conn, ...)     // an early return rolls back
try datasource.commit(conn)           // after a commit the deferred rollback
                           // finds nothing to undo (its error
                           // result is discarded, as `defer` does)
```

Without the `defer`, a `try` that returns early from inside a
transaction leaves it open.

`use datasource` -- or `use datasource.Name` to bring one item in bare.

## Example

```
use datasource
use db.DbError
use db.SqlValue
use db.db_error_message
use db.row_get
use db.sql_as_int
use db.sql_as_text
use sqlite

// The engines this program links. `datasource` names none of them itself, so a program that
// talks to SQLite pulls in SQLite and nothing else; add `pg.driver()` to reach Postgres too.
val drivers = datasource.registry([sqlite.driver()])

// Bound parameters, rows, and a transaction that rolls back on an error. `sqlite::memory:` is a
// private in-memory database; `postgres://...` and `mysql://...` URLs work the same way.
//
// Every call here answers the same DbError, so the work propagates with `try` and `main` reports
// once at the end.
fn demo() -> Result<unit, DbError> {
    val conn = try datasource.open(drivers, "sqlite::memory:")
    try datasource.execute(
        conn,
        """
        CREATE TABLE person (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL
        )
        """
    )
    try datasource.execute(conn, "INSERT INTO person (name) VALUES (?)", SqlValue.Text("Ada"))

    val rows = try datasource.query(conn, "SELECT id, name FROM person")
    print(len(rows))
    print(try sql_as_text(try row_get(rows[0], "name")))

    // A large result is read one row at a time with rows, instead of building the whole array.
    // Each row arrives as a Result, since the read can fail after the first one has been handed out.
    for row in try datasource.rows(conn, "SELECT id, name FROM person") {
        if Ok(found) = row {
            print(try sql_as_text(try row_get(found, "name")))
        }
    }

    // A constraint violation is its own kind of error, and switch names the kinds apart from the
    // statements that succeed.
    if Err(e) = datasource.execute(conn, "INSERT INTO person (name) VALUES (NULL)") {
        print(db_error_message(e))
    }

    // with_transaction commits when the work returns Ok and rolls back when it returns Err.
    val failed = datasource.with_transaction<int>(conn, fn(tx: datasource.Connection) -> Result<int, DbError> {
        try datasource.execute(tx, "INSERT INTO person (name) VALUES (?)", SqlValue.Text("Grace"))
        return err(DbError.Sql("changed my mind"))
    })
    print(is_ok(failed))
    print(len(try datasource.query(conn, "SELECT id FROM person")))
    datasource.close(conn)
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("datasource example failed: {}\n", db_error_message(e))
    }
}
```

Output:

```
1
Ada
Ada
constraint violation: NOT NULL constraint failed: person.name
false
1
```

### `Registry`

```
pub struct Registry {
    drivers: [Driver]
}
```

The drivers an application has chosen to link, and the only thing here that knows an engine
exists. Built once at startup -- `datasource.registry([sqlite.driver(), pg.driver()])` -- and
handed to `open`; a program that talks to SQLite alone names SQLite alone.

### `registry`

```
pub fn registry(drivers: [Driver]) -> Registry
```

A registry of `drivers`. Order matters only if two drivers claim the same prefix, in which case
the first wins.

### `Connection`

```
pub enum Connection {
    Direct(Conn)
    Pool(PoolState)
}
```

One database connection, or a pool of them; every `datasource` function takes this one type, so code
written against it does not care which engine is behind it or whether it is pooled.

### `dialect`

```
pub fn dialect(conn: Connection) -> string
```

Which engine `conn` is on: `sqlite`, `postgres` or `mysql`; for a pool, the one its URL names. For SQL that has to be written differently per engine
(see `migrate`).

### `open`

```
pub fn open(reg: Registry, url: string) -> Result<Connection, DbError>
```

`sqlite::memory:` (a private in-memory database, gone when the
process exits), `sqlite:<path>` (a file, created if missing), or
`postgres://user:password@host:port/database` (`postgresql://` too;
no TLS yet -- a `sslmode=require` URL is refused, not downgraded).

### `close`

```
pub fn close(conn: Connection)
```

Closes a connection. For a pool, waits until every connection has been
released (`receive` blocks until each is back), then closes them all --
so a borrowed connection that was never released makes this wait
forever; `defer datasource.release(...)` is what prevents that.

### `execute`

```
pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>
```

Runs a statement (or, for DDL, a `;`-separated script) and returns the
number of rows the last statement changed.

### `query`

```
pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>
```

Runs a query and returns its rows, each a `db.Row`; `params` are bound to the placeholders
(`?` for SQLite and MySQL, `$1`, `$2` for Postgres), never spliced into `sql`.

### `rows`

```
pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>
```

Runs a query and returns its rows lazily, one at a time as a `for` loop asks for them:


```
val rows = try datasource.rows(conn, "SELECT * FROM lab_result")
for row in rows {
    val r = try row                       // Result<Row, DbError>
    ...
}
```

On SQLite the rows come from a cursor, so a result of any size uses memory for one row; the cursor
is closed when the rows are used up, when the loop is left early, or with `iter_close`. On a pool the
connection is borrowed for the whole iteration and given back the same way, so finish or close the
iterator (a pooled one is not closed by the garbage collector, and a forgotten one keeps its
connection). On Postgres the rows come from a portal, 500 at a time; the connection carries no other
statement until the cursor is read to the end or closed (one sent meanwhile is an Err), which a
pool takes care of by lending another connection. On MySQL the rows come from a server-side cursor,
500 at a time; other statements may run between fetches, but a connection holds one cursor at a time.
A row that fails to arrive is an Err item and ends the iteration; the Err on the call is the statement
failing to run.

### `query_one`

```
pub fn query_one(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Option<Row>, DbError>
```

The common lookup-by-key shape: the first row, or None. It runs `sql` as written and keeps the first
row of whatever comes back -- it does not add `LIMIT 1`, so a query that could match many rows fetches
and discards them all. Write the `LIMIT 1` yourself when that matters; for a lookup by primary key or
unique column, where the engine returns one row anyway, it makes no difference.

### `begin`

```
pub fn begin(conn: Connection) -> Result<unit, DbError>
```

Starts a transaction on `conn`. Refused on a pool: `acquire` an exclusive connection first, or
use `with_transaction`.

### `commit`

```
pub fn commit(conn: Connection) -> Result<unit, DbError>
```

Commits the transaction open on `conn`.

### `rollback`

```
pub fn rollback(conn: Connection) -> Result<unit, DbError>
```

Rolls back the transaction open on `conn`. Called with none open it is harmless, which is what
lets `defer datasource.rollback(conn)` stand guard after a commit.

## Pooling

### `open_pool`

```
pub fn open_pool(reg: Registry, url: string, size: int) -> Result<Connection, DbError>
```

Opens `size` connections to `url` up front (failing fast if any can't
connect) and returns a pool-backed `Connection`: the same type as
`open` returns, so repository code written against a `Connection` works
unchanged. Statements run on it (`execute`/`query`) each borrow a
connection for that one statement and give it back; a unit of work that
spans several statements -- a transaction above all -- borrows one
exclusively with `acquire`:


```
val tx = try datasource.acquire(conn)
defer datasource.release(conn, tx)
try datasource.begin(tx)
defer datasource.rollback(tx)
...
try datasource.commit(tx)
```

The pool is a channel of idle connections, so a borrow blocks while
every connection is in use -- natural backpressure, no extra locking.
`tx` is an ordinary, non-pool `Connection`, so a repository called with
it uses it directly and never borrows again (borrowing while holding one
could deadlock a small pool).

A connection that sat idle for 30 seconds or more is tested with `SELECT 1` when it is lent out and
replaced if that fails (`open_pool_with` changes the time or turns this off). A connection that dies
(server restart, dropped network) is replaced: when a statement run
on the pool fails with `DbError.Connection`, that connection is closed and a fresh one opened
from the same URL in its place (if the server is still down the dead one goes back, and the
next statement tries again). The statement that met the failure is NOT re-sent -- it may or may
not have run, so retrying an INSERT could repeat it -- and returns its error. For a connection
taken with `acquire`, use `discard` in place of `release` after a connection error. A borrow
blocks while every connection is in use; `acquire_timeout` gives up after a while instead, and a
leaked borrow blocks other callers forever -- `defer release` is the guard. A pool over `sqlite::memory:` is a single connection whatever
`size` says: every in-memory connection is its own private database, so
N of them would be N different databases.

### `open_pool_with`

```
pub fn open_pool_with(reg: Registry, url: string, size: int, check_after_seconds: int) -> Result<Connection, DbError>
```

`open_pool`, choosing how long a connection may sit idle before it is tested when borrowed: one idle for
`check_after_seconds` or more gets a `SELECT 1` first, and if that fails with a connection error it is
replaced with a fresh connection before it is lent out, so a server restart overnight does not cost the
first request of the morning. Zero or less turns the test off. SQLite connections are never tested.

### `acquire`

```
pub fn acquire(conn: Connection) -> Result<Connection, DbError>
```

An exclusive connection, for a unit of work that must stay on one (a
transaction). Blocks while the pool is exhausted. On a connection that
isn't a pool it just returns that connection, and `release` does
nothing -- so the same code runs against either.

### `acquire_timeout`

```
pub fn acquire_timeout(conn: Connection, timeout_ms: int) -> Result<Connection, DbError>
```

Like `acquire`, but gives up with a `DbError.Connection` once `timeout_ms` milliseconds pass
with every connection still in use (checked every 2 ms). A `timeout_ms` of zero or less tries
once.

### `release`

```
pub fn release(conn: Connection, taken: Connection)
```

Gives back a connection taken with `acquire`. Pair every `acquire` with
a `defer datasource.release(...)`. A connection released while a transaction is
still open goes back dirty -- roll it back first (`defer datasource.rollback`
registered *after* the release defer runs before it).

### `discard`

```
pub fn discard(conn: Connection, taken: Connection)
```

Gives back a connection taken with `acquire` that has failed with a connection error: it is
closed and a fresh one opened in its place (the failed one goes back if none can be opened).
Use it instead of `release` when the connection is known to be broken. Does nothing on a
connection that isn't a pool.

## Transactions

### `with_transaction`

```
pub fn with_transaction<R>(conn: Connection, work: fn(Connection) -> Result<R, DbError>) -> Result<R, DbError>
```

Runs `work` inside a transaction on one exclusive connection: begin, call
`work(tx)`, then commit if it returned Ok -- or roll back if it returned
Err, or if the commit itself failed. The result of `work` is returned
unchanged. On a pool, `tx` is a borrowed connection that goes back to the
pool on every way out (a `defer`), so this is the whole of the
acquire / begin / rollback-on-error / commit / release dance:


```
val moved = try datasource.with_transaction<int>(conn, fn(tx: datasource.Connection) -> Result<int, DbError> {
    try datasource.execute(tx, "UPDATE ...", SqlValue.Int(amount))
    return ok(amount)
})
```

`work` is a function value: a lambda capturing what it needs (`amount` above), or a named
function. If the transaction ends in a connection error the connection is replaced rather than
returned to the pool. A `work` that itself calls `datasource.begin`/`datasource.commit` on `tx` breaks the nesting -- it
must not.
