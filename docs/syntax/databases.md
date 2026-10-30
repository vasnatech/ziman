# Databases, validation and transactions

Part of the [v0 syntax reference](README.md).

## Databases

Two modules: `db` (shared by the drivers -- `SqlValue`, `Row`, `DbError`, `sql_as_*` readers,
the URL parser, and the `Driver`/`Conn` contract) and `datasource` (what a program calls --
connections, `execute`, `query`, `rows`, pools, transactions). SQLite (`sqlite::memory:`/
`sqlite:<path>`), PostgreSQL (`postgres://user:password@host:port/database`) and MySQL
(`mysql://...`) are the backends, behind the same calls.

`sql` is the statement tree on its own -- `sql.parse` reads one statement, `sql.parse_many` a
script, `sql.parse_file` a file, and `sql.format` writes a `sql.Statement` back as SQL. It holds no
database and no connection, so a program that wants to read, rewrite or generate SQL uses it
without an engine; `memdb` is built on it. `parse` then `format` then `parse` gives the same tree,
which is the property it is tested against over the sample application's real migrations.

**An application says which engines it links**, and `datasource` names none of them: a `db.Driver`
is an engine (its name, the URL prefixes it claims, and how to open a connection), a `db.Conn` is
one open connection as a handful of closures that already hold it. So a program that talks to
SQLite registers SQLite and contains no Postgres driver at all; a fourth engine is a new module
rather than an edit to `datasource`.

```ziman
use datasource
use db.SqlValue
use db.row_get
use db.sql_as_text
use sqlite

// the one place an engine is named
val drivers = datasource.registry([sqlite.driver()])

fn main() {
    val conn = unwrap(datasource.open(drivers, "sqlite::memory:"))
    unwrap(datasource.execute(conn, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"))
    unwrap(datasource.execute(conn, "INSERT INTO t (id, name) VALUES ($1, $2)", SqlValue.Int(1), SqlValue.Text("Ada")))
    val rows = unwrap(datasource.query(conn, "SELECT name FROM t WHERE id = $1", SqlValue.Int(1)))
    print(unwrap(sql_as_text(unwrap(row_get(rows[0], "name")))))   // Ada
}
```

- **Bound parameters only** -- SQL text carries `$1, $2, ...` (the SQLite driver rewrites to
  its own `?N`), values go in a `[SqlValue]` argument, nothing is ever spliced into SQL text.
- **`SqlValue`** is `Null | Int(int) | Float(float) | Text(string) | Bool(bool) | Bytes([byte])`.
  Nothing dialect-specific crosses the driver: `Uuid`/`LocalDate`/`Instant`/`decimal`/`bigint`
  all travel as `Text`, converted by the mapping layer. A `bool` column reads back as an
  integer under SQLite; `sql_as_bool` accepts either.
- **`DbError`** is `NotFound | Constraint | Connection | Sql`. `Constraint` is a unique/
  foreign-key/not-null/check violation (SQLite turns foreign keys on, matching Postgres).
  `NotFound` is never raised by the driver itself -- a repository returns it for an empty
  lookup, so one error type covers the whole data-access layer. `db_error_message(e)` renders any.
- **Postgres** (module `pg`, pure Ziman on `net`/`number`/`crypto`) speaks protocol 3.0:
  trust/cleartext-password/**SCRAM-SHA-256** auth, simple query protocol for parameterless
  statements (`execute` takes a `;`-separated DDL script), extended protocol for `$1, $2, ...`.
  Result values travel as text and are interpreted by column type (int/float/bool/bytea map to
  the matching `SqlValue`, everything else -- text, uuid, dates, numeric, json -- arrives as
  `Text`), so the same repository SQL/schema/`#[derive(Row)]` runs on both backends. A `Bool`
  parameter sends `1`/`0` (Postgres accepts it for `BOOLEAN` or `INTEGER`). One connection is
  owned by one thread serializing requests, so sharing across threads is safe statement by
  statement. **TLS**: an `SSLRequest` then in-place upgrade, chosen by `sslmode` with libpq's
  meanings -- `disable` (the default here, unlike libpq's `prefer`), `prefer` (TLS if offered,
  unverified), `require` (TLS or fail, certificate **not** verified), `verify-ca` (+ chain),
  `verify-full` (+ hostname -- use this for a hosted database); `sslrootcert=<path>` names a
  trusted-CA PEM, else the system store. `allow` unsupported; a failed `prefer` doesn't retry
  plaintext. **Not supported:** MD5 auth, Kerberos/GSS, client certificates, IPv6 URL literals,
  COPY, LISTEN/NOTIFY, prepared-statement reuse, SASLprep of a non-ASCII password.
- **`datasource.rows(conn, sql, params)`** is `query` as a lazy `Iter<Result<Row, DbError>>`:
  SQLite streams from a cursor (a million rows: 72 MB vs. 1.7 GB materialized), closed when
  exhausted, left early, or via `iter_close`; on a pool the connection is borrowed for the whole
  iteration (finish or close it -- not GC-closed). Postgres/MySQL stream 500 rows at a time
  (Postgres: a portal, connection carries no other statement until done/closed; MySQL: a
  server-side cursor, other statements may interleave but one cursor at a time); an unfinished
  cursor ends when garbage collected. 300k rows: 51 MB vs. 609 MB on Postgres, 43 MB vs. 706 MB
  on MySQL.
- `datasource.query` returns `[Row]`; `query_one` returns `Option<Row>`; `execute` returns the
  last statement's row count, and takes a `;`-separated script when there are no parameters.
- **Connection pools.** `datasource.open_pool(url, size)` opens `size` connections up front
  (fails fast, closing any already opened) and returns a pool-backed `Connection` -- the same
  type `open` returns, so repository code is unchanged. Statements each borrow and return a
  connection; a multi-statement unit of work (a transaction, above all) borrows one exclusively:
  ```
  val tx = datasource.acquire(conn)?
  defer datasource.release(conn, tx)      // returned on every path out
  datasource.begin(tx)?
  defer datasource.rollback(tx)           // runs before the release
  datasource.commit(tx)?
  ```
  The pool is a channel of idle connections (a borrow blocks while all are busy -- natural
  backpressure). `tx` is an ordinary non-pool `Connection`, so a repository called with it never
  borrows again (avoiding a small-pool deadlock). `begin`/`commit`/`rollback` on the pool itself
  are refused (each statement would borrow a different connection). On a plain `Connection`,
  `acquire` returns it and `release` no-ops, so the same code runs against either.
  `datasource.close(pool)` waits for every connection to be released then closes them --
  `defer datasource.release` is what prevents an unreleased borrow hanging it forever;
  `acquire_timeout(pool, ms)` gives up with `DbError.Connection` instead. **A dying connection
  is replaced**: a pool statement failing with `DbError.Connection` closes and reopens from the
  same URL (not re-sent, since it may have run); `datasource.discard(pool, tx)` does the same
  after a failure on an acquired connection (`with_transaction` does it automatically). **Health
  check**: a connection idle 30+ seconds is tested with `SELECT 1` when lent out and replaced on
  failure (`open_pool_with(url, size, check_after_seconds)`, 0 to disable; SQLite never tested).
  **Limits**: no min/max sizing or idle timeout, a released connection isn't reset (goes back
  dirty if released mid-transaction), and a pool over `sqlite::memory:` is always a single
  connection (every in-memory connection is its own private database; SQLite itself waits up to
  five seconds for the writer lock instead of failing, making a file-backed pool usable).
- **Transactions** are plain `begin`/`commit`/`rollback` on the connection. Every call takes the
  connection's own lock (safe to share a `Connection` across threads statement by statement, not
  mid-transaction -- other threads' statements can interleave inside it). A `try` returning
  early inside one leaves it open unless the caller uses `defer datasource.rollback(tx)` or
  `with_transaction` (see "Transactions").
- **`#[derive(Row)]`** on a struct generates `<name>_from_row(row) -> Result<Name, DbError>` and
  `<name>_to_params(v) -> [SqlValue]` (field declaration order, matching a hand-written
  `INSERT`'s column order; field names map directly, already `snake_case`). `int`/`float`/
  `bool`/`string` map directly; a struct/enum field dispatches to `<name>_to_sql`/`_from_sql`
  (provided for `Uuid`; a `time` type's pair is the short `to_sql`/`to_<type>`); `Option<U>` is NULL-or-value. An enum, generic
  struct, array field, or `bigint`/`decimal`/`byte` field is a compile error. The derive imports
  what its generated code calls; a field type of your own needs its conversion imported by the
  deriving module.
- **`#[derive(Entity)]`** on a struct generates `<name>_columns: [string]` -- the column names in
  `to_params` order, which is the order a hand-written `INSERT INTO t (a, b) VALUES ($1, $2)` lists
  its columns in, so the two cannot disagree. It needs `Row` beside it (`#[derive(Row, Entity)]`),
  since an entity is something read back out of a row and `<name>_from_row` is what reads it --
  checked **by name**, so a hand-written `<name>_from_row` satisfies it as well as the derive does;
  a fieldless struct and an enum are both refused.
  The table name is never **guessed**, because no pluralisation rule survives real schemas
  (`FamilyHistory` is stored in `family_history`, `MedicalHistory` in `medical_histories`,
  `UserNotificationPreference` in `user_notification_prefs`) and a wrong guess would build SQL
  against a table that does not exist. It can be **given**, though:
- **`#[derive(Row, Entity("patients"))]`** adds `pub val table: string` and
  `pub val <name>_meta: eql.Entity`, the latter being
  `eql.entity("<StructName>", "<table>", <name>_columns)` -- so a struct that is stored in a table
  says so once, where it used to say it three times (a `table` val, the table again inside an
  `eql.entity` call, and the struct's own name a third time inside the same call). The `eql` import
  is added for you, as it is for `time` and `uuid` behind the other derives.
  The **entity's name is the struct's**, not the table's: a query writes `FROM Patient p`, naming
  the type the reader has in hand, and taking it from the table would make `FROM patients p` the
  spelling -- SQL leaking back through the layer whose job is to hide it. The argument must be a
  string; a bare word is refused, since `Entity(patients)` could plausibly name a type.
- **`#[pk]` on a struct field** marks it as part of the row's key, and `#[derive(Entity("t"))]`
  puts the marked fields in the metamodel as `keys`, in field order. An unknown field attribute is
  refused rather than ignored, and a function parameter cannot carry one, since field attributes
  are read in the struct-field loop and nowhere else.
  **The compiler knows no field attribute at all.** It parses `#[name(a: 1, b: "x")]` into a
  generic list, hands it to `meta.Field.attrs`, and asks every loaded generator which names exist
  — `derive_entity` declares `pk` and `column`, `derive_validate` the nine constraints — so an
  unknown name is still refused by name, with the list, on its own line. Neither the names nor
  their meanings were the generators' until 2026-10-30: `#[pk]` and `#[column]` were read by
  `src/derive.c` to pick a repository's imports, and all eleven were a table in `src/parser.c`.
  This is what lets a project's own generator have attributes of its own; see
  [docs/design/user-generators.md](../design/user-generators.md) §5a.
  `keys` is **empty when nothing is marked**, which means "no key was declared" and not "the first
  column". A reader that addresses a single row refuses an empty `keys` by name rather than
  guessing -- the guess would have been right for all 27 entities in the North Star application,
  which is the kind of right that stops being true without anything failing.
  **More than one field may be marked**, and that is the whole syntax for a compound key. Nothing
  about the attribute objects; a reader that can only address a single-column key is what has to
  say so.
- **`#[column(name: "start_date")]` on a struct field** says the field is stored in a column of
  another name -- for a schema you do not control, or one whose spelling you would not choose.
  `<name>_columns` carries the column, `<name>_from_row` reads the column, `keys` uses the column
  for a `#[pk]` field, and `<name>_meta` carries the pairing so that **everything you write still
  names the field**: a query says `e.start`, `eql` emits `start_date`, and a derived query is
  `find_by_start`. Naming the *column* in a query is refused, with the field list.
  The argument is **named**, not positional: `#[column("start_date")]` would read fine today and
  stop reading the moment a second argument exists. A misspelled argument is refused, since
  ignoring one would be a column override that silently does not happen.

## Validation

Constraints are field attributes, and `#[derive(Validate)]` turns them into one function. The rules
live in the `validate` core module, so every one of them is also a function you can call.

```ziman
val type_words: [string] = ["WEIGHT", "HEIGHT"]

#[derive(Validate)]
struct Reading {
    #[one_of(values: type_words)]
    type: string

    #[not_blank]
    #[size(min: 4, max: 64)]
    note: string

    #[positive]
    value: float

    #[email]
    reported_by: Option<string>
}

fn main() {
    val bad = Reading { type: "DEPTH", note: "  ", value: 0.0,
                        reported_by: Option<string>.Some("nobody@") }
    for v in reading_violations(bad) {
        printf("{}: {}\n", v.field, v.message)    // type: must be one of WEIGHT, HEIGHT
    }
}
```

- **Nine constraints**: `#[not_blank]`, `#[not_empty]`, `#[email]`, `#[positive]`,
  `#[one_of(values: <name>)]`, `#[size(min: <n>, max: <n>)]`, `#[min(<n>)]`, `#[max(<n>)]` and
  `#[pattern(regex: "…")]`, plus `#[valid]` below.
  They are named after Jakarta Validation's where the two coincide, so a reader who knows Bean
  Validation needs no table. `#[one_of]`'s `values:` is **a name** -- a `val` of the module's own,
  which the generated code reads -- because a word list is not something an attribute can hold.
  `#[size]`'s two bounds are both optional and at least one must be given.
- **The type decides whether absence is allowed**, not the attribute. On a `string`, `#[not_blank]`
  requires text; on an `Option<string>`, it requires text only when the value is there, as Jakarta's
  constraints pass on a null and only `@NotNull` rejects one. `#[one_of]` on a `[string]` checks
  every element. So one attribute covers what a hand-written check needs three of.
- **`#[derive(Validate)]`** on a struct generates `<snake>_violations(v) -> [validate.Violation]`,
  where `Violation { field, message }` names the field as the **wire** does -- `newPassword`, not
  `new_password`, computed exactly as `#[derive(Json)]` computes a key. An empty list is the
  success case. `use validate` is added for you. A struct with no constraint is refused, as is a
  generic struct and an enum.
- **One violation per field**, the first rule that failed in the order they were written: a
  response keyed by field name cannot carry a second. A field with several constraints goes
  through `validate.first`, which is an ordinary function doing exactly that.
- **The messages are Hibernate Validator's** -- `"must not be blank"`,
  `"must be a well-formed email address"`, `"size must be between 3 and 5"` -- so a Spring
  application ported field by field keeps telling its clients what it told them before. Two
  deliberate departures: `#[size(min: 8)]` with no maximum says `"size must be at least 8"` rather
  than Java's `"size must be between 8 and 2147483647"`, which is an annotation default leaking;
  and `#[one_of]` has no Jakarta counterpart at all, where Java reaches for an enum type or a
  `@Pattern`. Every constraint takes `message:` to override its own text.
- **`#[pattern(regex: "…")]`** is Jakarta's `@Pattern`: the value must match, in `regex`'s syntax,
  and the message names the pattern because nothing else can say what was wanted. The argument is
  named rather than positional, for `#[column(name:)]`'s reason -- `message:` is already a second
  one. A pattern that will not compile is a violation naming it, not a crash: the pattern comes
  from an attribute, so a bad one is a mistake in the source and the field is where a reader looks.
- **`#[valid]` cascades** into a field whose own type is validated, producing a dotted path:
  `#[valid] ingredients: [IngredientRequest]` reports `ingredients[0].name`. It takes a struct, a
  list of structs, or an `Option` of either, and finds the nested function **by name** --
  `<snake>_violations` of the field's type -- so a type in another module needs that imported, as
  `#[derive(Json)]` needs its `_to_json`. Explicit rather than automatic, as Jakarta's `@Valid` is:
  a nested struct validated silently would surprise a reader of the outer one, and a dotted path
  becomes a key the client sees. A cascade reports a *different* field, so it sits outside the
  one-message-per-field rule -- three bad elements are three violations, and `#[not_empty]` and
  `#[valid]` on the same list do not collide.
- **`validate` is callable on its own**: `validate.email(address)`, `validate.size_at_least(s, 8)`
  and the rest answer `Option<string>` -- the message, or `None`. `validate.is_email` answers a
  `bool` for a caller who wants the question rather than the message. See
  [docs/design/validation.md](../design/validation.md).
