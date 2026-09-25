# `db`

A database driver layer, the shape of JDBC: a connection
handle plus execute/query taking SQL text and *bound* parameters (a
value is never spliced into the SQL string), transactions as plain
begin/commit/rollback, and rows handed back as `sql.Row`. Which
engine sits behind a handle is chosen by the URL given to `open`:
`sqlite::memory:`, `sqlite:<path>` or
`postgres://user:password@host:port/database` or
`mysql://user:password@host:port/database` -- the API above doesn't
change, so repositories written against it don't either. There are no
interfaces in Ziman to abstract over drivers with; dispatching by URL
scheme inside this module is the substitute. Each engine has its own
driver module: `sqlite` (a thin layer over the C library), `pg` and
`mysql` (both written in Ziman, on top of `net`), sharing URL parsing
through `dburl`.

Concurrency: sharing one `Connection` between threads is safe
statement by statement -- SQLite takes the connection's own lock, and a
Postgres connection is owned by one thread that serialises requests. A
transaction is NOT safe to share -- another thread's statements can
interleave inside it -- so run a transaction from one thread. Use
`with_transaction` (below), or by hand with `defer`:


```
db.begin(conn)?
defer db.rollback(conn)
db.execute(conn, ...)?     // an early return rolls back
db.commit(conn)?           // after a commit the deferred rollback
                           // finds nothing to undo (its error
                           // result is discarded, as `defer` does)
```

Without the `defer`, a `?` that returns early from inside a
transaction leaves it open.

`use db` -- or `use db.Name` to bring one item in bare.

## Example

```
use db
use sql.DbError
use sql.SqlValue
use sql.db_error_message
use sql.row_get
use sql.sql_as_int
use sql.sql_as_text

// Bound parameters, rows, and a transaction that rolls back on an error. `sqlite::memory:` is a
// private in-memory database; `postgres://...` and `mysql://...` URLs work the same way.
fn main() {
    val conn = unwrap(db.open("sqlite::memory:"))
    unwrap(db.execute(conn, "CREATE TABLE person (id INTEGER PRIMARY KEY, name TEXT NOT NULL)", []))
    unwrap(db.execute(conn, "INSERT INTO person (name) VALUES (?)", [SqlValue.Text("Ada")]))

    val rows = unwrap(db.query(conn, "SELECT id, name FROM person", []))
    print(len(rows))
    print(unwrap(sql_as_text(unwrap(row_get(rows[0], "name")))))

    // A constraint violation is its own kind of error.
    val bad = db.execute(conn, "INSERT INTO person (name) VALUES (NULL)", [])
    print(db_error_message(unwrap_err(bad)))

    // with_transaction commits when the work returns Ok and rolls back when it returns Err.
    val failed = db.with_transaction<int>(conn, fn(tx: db.Connection) -> Result<int, DbError> {
        db.execute(tx, "INSERT INTO person (name) VALUES (?)", [SqlValue.Text("Grace")])?
        return err(DbError.Sql("changed my mind"))
    })
    print(is_ok(failed))
    print(len(unwrap(db.query(conn, "SELECT id FROM person", []))))
    db.close(conn)
}
```

Output:

```
1
Ada
constraint violation: NOT NULL constraint failed: person.name
false
1
```

### `PoolState`

```
pub struct PoolState {
    idle: Channel<Connection>
    size: int
    url: string
}
```

The state of a pool: the channel of idle connections, how many were opened and the URL to
reopen one from (see `open_pool`). Statements run on the pool borrow a connection for that one
statement, and `acquire` hands one out exclusively. `Connection` is recursive through this
`Channel`, like `Json` is through an array.

### `Connection`

```
pub enum Connection {
    Sqlite(sqlite.Connection)
    Postgres(pg.Connection)
    Mysql(mysql.Connection)
    Pool(PoolState)
}
```

One database connection, or a pool of them; every `db` function takes this one type, so code
written against it does not care which engine is behind it or whether it is pooled.

### `open`

```
pub fn open(url: string) -> Result<Connection, DbError>
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
forever; `defer db.release(...)` is what prevents that.

### `execute`

```
pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>
```

Runs a statement (or, for DDL, a `;`-separated script) and returns the
number of rows the last statement changed.

### `query`

```
pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>
```

Runs a query and returns its rows, each a `sql.Row`; `params` are bound to the placeholders
(`?` for SQLite and MySQL, `$1`, `$2` for Postgres), never spliced into `sql`.

### `query_one`

```
pub fn query_one(conn: Connection, sql: string, params: [SqlValue]) -> Result<Option<Row>, DbError>
```

The common lookup-by-key shape: the first row, or None.

### `begin`

```
pub fn begin(conn: Connection) -> Result<int, DbError>
```

Starts a transaction on `conn`. Refused on a pool: `acquire` an exclusive connection first, or
use `with_transaction`.

### `commit`

```
pub fn commit(conn: Connection) -> Result<int, DbError>
```

Commits the transaction open on `conn`.

### `rollback`

```
pub fn rollback(conn: Connection) -> Result<int, DbError>
```

Rolls back the transaction open on `conn`. Called with none open it is harmless, which is what
lets `defer db.rollback(conn)` stand guard after a commit.

## Pooling

### `open_pool`

```
pub fn open_pool(url: string, size: int) -> Result<Connection, DbError>
```

Opens `size` connections to `url` up front (failing fast if any can't
connect) and returns a pool-backed `Connection`: the same type as
`open` returns, so repository code written against a `Connection` works
unchanged. Statements run on it (`execute`/`query`) each borrow a
connection for that one statement and give it back; a unit of work that
spans several statements -- a transaction above all -- borrows one
exclusively with `acquire`:


```
val tx = db.acquire(conn)?
defer db.release(conn, tx)
db.begin(tx)?
defer db.rollback(tx)
...
db.commit(tx)?
```

The pool is a channel of idle connections, so a borrow blocks while
every connection is in use -- natural backpressure, no extra locking.
`tx` is an ordinary, non-pool `Connection`, so a repository called with
it uses it directly and never borrows again (borrowing while holding one
could deadlock a small pool).

A connection that dies (server restart, dropped network) is replaced: when a statement run
on the pool fails with `DbError.Connection`, that connection is closed and a fresh one opened
from the same URL in its place (if the server is still down the dead one goes back, and the
next statement tries again). The statement that met the failure is NOT re-sent -- it may or may
not have run, so retrying an INSERT could repeat it -- and returns its error. For a connection
taken with `acquire`, use `discard` in place of `release` after a connection error. A borrow
blocks while every connection is in use; `acquire_timeout` gives up after a while instead, and a
leaked borrow blocks other callers forever -- `defer release` is the guard. A pool over `sqlite::memory:` is a single connection whatever
`size` says: every in-memory connection is its own private database, so
N of them would be N different databases.

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
a `defer db.release(...)`. A connection released while a transaction is
still open goes back dirty -- roll it back first (`defer db.rollback`
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
val moved = db.with_transaction<int>(conn, fn(tx: db.Connection) -> Result<int, DbError> {
    db.execute(tx, "UPDATE ...", [SqlValue.Int(amount)])?
    return ok(amount)
})?
```

`work` is a function value: a lambda capturing what it needs (`amount` above), or a named
function. If the transaction ends in a connection error the connection is replaced rather than
returned to the pool. A `work` that itself calls `db.begin`/`db.commit` on `tx` breaks the nesting -- it
must not.
