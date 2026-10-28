# `memdb`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A small SQL engine in pure Ziman, holding its tables in memory, for tests.

A `db.Driver` like `sqlite`, `pg` and `mysql`, so `datasource` reaches it the same way and
nothing above it knows the difference:


```
val drivers = datasource.registry([memdb.driver()])
val conn = try datasource.open(drivers, "memdb:")
try migrate.run(conn, "migrations")          // or run schema.sql and data.sql by hand
```

**It is a third target, not a replacement.** A test that passes here and fails on PostgreSQL
certifies nothing, so the point of this engine is to run the *same* suite that sqlite and
postgres run, where a divergence shows up as a failure instead of hiding as a false green. The
divergences it knows about are listed at the bottom of this comment; run the real engine before
believing anything surprising.

## What it understands

`CREATE TABLE` (with `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, `CHECK`, `DEFAULT` and a column type
that is recorded but not enforced), `CREATE [UNIQUE] INDEX`, `ALTER TABLE ... ADD [COLUMN]`,
`DROP TABLE [IF EXISTS]`, `INSERT INTO ... VALUES`, `SELECT [DISTINCT]` with `WHERE`,
`GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT` and `OFFSET`, `UPDATE ... SET ... WHERE`,
`DELETE FROM ... WHERE`, `RETURNING` on any of the three writes, and
`BEGIN`/`COMMIT`/`ROLLBACK`.

Set operations: `UNION [ALL]`, `INTERSECT [ALL]` and `EXCEPT [ALL]`, chained, with the
compound's own `ORDER BY`, `LIMIT` and `OFFSET`. A compound's `ORDER BY` may name an output
column or its position and nothing else, because there is no single source to resolve anything
else against. Two of the five divergences between the two readings live here -- how a chain
groups, and whether the `ALL` forms exist at all.

`ORDER BY 2` and `ORDER BY alias` work everywhere, not only in a compound. They did not before
set operations needed them: an integer term was evaluated per row, so `ORDER BY 1 DESC` compared
1 with 1 for every pair and quietly left the rows as they came, and an output name was "no such
column". Five such queries now agree with SQLite that did not, including the out-of-range
refusal, which is SQLite's wording down to the `1st`.

`RETURNING a, b`, `RETURNING *` or an expression with an alias, on `INSERT`, `UPDATE` and
`DELETE`. A qualified star, `t.*`, in a select list -- naming one table's columns by the name or
alias the `FROM` gave it, which is what makes `p.*` work and `patient.*` not once `patient AS p`
has been written. In a `RETURNING` clause it is one of the two readings' divergences, below. An `INSERT` answers what it stored with defaults filled in, an `UPDATE` the row as it
now stands, a `DELETE` the row as it was -- each what a real engine answers. A `*` expands
anywhere in the list, so `RETURNING *, id` is two copies of `id`, and the same is now true of a
select list.

**`RETURNING` is not ANSI SQL and both readings run it anyway.** The standard's form is a data
change delta table -- `SELECT * FROM FINAL TABLE (INSERT ..)`, SQL:2011 -- which essentially only
Db2 implements; `RETURNING` is PostgreSQL's, SQLite has had it since 3.35 and MySQL has nothing
of the kind. So whether a connection has it is a question about the *engine*, which
`datasource.dialect` answers, and not about which reading of SQL's semantics is in force. **A
caller that gates on the dialect has to name `memdb` in its list** -- the sample application's
`returns_written_row` does -- or its tests take a path production does not.

Joins: `[INNER] JOIN`, `LEFT [OUTER] JOIN` and `CROSS JOIN`, with `ON` or `USING`, chained left
to right and with table aliases. A comma in the `FROM` list is the `CROSS JOIN` it means.

**Derived tables**: a `(SELECT ..)` in the `FROM`, run once and read like a table, with or
without an alias, joined, nested, or holding a compound query. A derived table sees the scopes
*enclosing the statement it is in* and not the `FROM` entry beside it -- so
`FROM t, (SELECT .. WHERE u.id = t.id) AS x` is an error, while the same derived table inside a
correlated subquery is legal. SQLite draws the line in exactly that place, which was measured
rather than reasoned about: a derived table is not `LATERAL`, but it is still inside whatever
encloses it. `RIGHT` and `FULL` are read by `sql` and refused here, with an
error naming the engine to use instead -- they are a mirror and a union of what this does, and
shipping them without a test that proves them against a real engine would be guessing.

Subqueries: a scalar `(SELECT ..)`, `IN (SELECT ..)`, `NOT IN (SELECT ..)` and `EXISTS`, each of
which may be correlated -- a name a subquery does not have resolves outward through the scopes
enclosing it, which is what makes `WHERE visit.patient_id = patient.id` inside one mean what it
says.

Expressions: literals, column references (qualified or not), bound parameters (`?`, `$1`),
`AND`/`OR`/`NOT`, the six comparisons, `IS [NOT] NULL`, `LIKE`, `IN`, `BETWEEN`, `CASE WHEN`,
`CAST(x AS type)`, `+ - * /`, `||`, parentheses, the scalar functions `substr`, `length`,
`lower`, `upper`, `trim`, `abs` and `coalesce`, and the aggregates `COUNT` (including
`COUNT(DISTINCT x)`), `SUM`, `MIN`, `MAX` and `AVG`.

**Constraints are enforced**, not merely remembered: `NOT NULL`, `PRIMARY KEY`, `UNIQUE`,
`CHECK` and `FOREIGN KEY`, at column level and at table level, on `INSERT`, `UPDATE` and
`DELETE`, with SQLite's own messages. Each reads NULL the way SQL says to, and the three readings differ: `NOT NULL` refuses
a NULL; `UNIQUE` and `PRIMARY KEY` *ignore* a key with a NULL in it, because uniqueness asks
whether two keys are equal and a NULL is equal to nothing; and a `CHECK` fails only when it is
false, NULL being unknown and passing. A statement that violates a constraint leaves the
database exactly as it was, which costs nothing here because a `Database` is a value.

Referential integrity: a child key must name a row of the parent unless it has a NULL in it
(the standard's `MATCH SIMPLE`, which is the default in both engines), and the parent's side
obeys each key's `ON DELETE` and `ON UPDATE` -- `NO ACTION` and `RESTRICT` refuse, `CASCADE`
follows, `SET NULL` and `SET DEFAULT` rewrite the child's key and then check the child's *own*
constraints, which is how a `SET NULL` into a `NOT NULL` column is caught. A cascade recurses
and cannot loop: deleting only ever shrinks the database, so a cycle runs out of rows.

**The referenced columns must be a key of the parent**, a `PRIMARY KEY` or a `UNIQUE`, not merely
columns that exist -- otherwise "the row it points at" need not be one row. Both engines refuse
the shape; SQLite does it when the statement runs, which is the wording here
(`foreign key mismatch - "child" referencing "parent"`), and PostgreSQL when the table is made.

**SQLite enforces foreign keys only with `PRAGMA foreign_keys = ON`**, which is off by default
and which this library's driver sets at every connection. An application whose suite runs
against a plain `sqlite3` therefore tests no referential integrity at all, which is worth knowing
before trusting any suite that is green on one engine.

A single-column `INTEGER PRIMARY KEY` is **SQLite's rowid under another name**: a NULL in it, or
leaving the column out, takes the next value rather than being refused. The declared type must be
exactly `INTEGER` -- `INT` and `BIGINT` are ordinary columns in SQLite and keep the NULL, which
was measured rather than assumed. PostgreSQL has no rowid, so the strict reading does none of
this and refuses a NULL in a key.

**Columns are resolved before a statement runs**, as a real engine resolves them when it
prepares one. Looking them up per row instead meant a query whose `WHERE` matched nothing never
looked, so `SELECT count(age) FROM visit WHERE reason = 'nothing'` answered 0 where SQLite
answers "no such column: age" -- a typo passing here and failing there is the exact false green
this engine exists to avoid, and the cross-check fixture caught it within a minute of the
aggregates being written. It caught the same thing a second time when joins arrived: validation
had been loosened to let a correlated name through, which let every typo through with it, so it
now checks against the shape the `FROM` produces *and* the scopes enclosing it -- which is also
where an unqualified `id` across two joined tables becomes "ambiguous column name" rather than
silently the left one.

## Two readings of SQL

`driver()` at `memdb:` reads SQL the way SQLite does. `strict_driver()` at `memdb-ansi:` reads it
the way the standard does, and the two differ in exactly eleven places -- `LIKE`'s case
sensitivity, what division by zero answers, whether a number may be compared with text, whether
a boolean in a select list comes back as a boolean or as the integer 0 or 1 (SQLite has no
boolean type; PostgreSQL and the standard do), whether a `RETURNING` clause may use a qualified
star, **how a chain of set operations groups**, whether `INTERSECT ALL`, `EXCEPT ALL` and
`UNION DISTINCT` exist at all, whether a `PRIMARY KEY` column may hold NULL, whether a `UNIQUE`
column may be added to a table that already has rows, whether two columns of one derived table
may share a name, and whether a derived table may rename its columns with `AS x (a, b)`.

The grouping is the one to remember, because nothing in the text of the query says which reading
is in force and both answer rows. SQLite reads `UNION`, `INTERSECT` and `EXCEPT` strictly left to
right; the standard and PostgreSQL bind `INTERSECT` tighter than the other two. So
`a UNION b INTERSECT c` is `(a UNION b) INTERSECT c` on one engine and `a UNION (b INTERSECT c)`
on the other -- a different answer, no error, no warning. Parenthesise it or do not write it.

**Five of the eleven run the other way**, and they are the ones where SQLite is the stricter
engine: `RETURNING t.*`, the `ALL` forms of `INTERSECT` and `EXCEPT`, `UNION DISTINCT`,
`ALTER TABLE ... ADD COLUMN .. UNIQUE` and a derived table's column alias list are each refused
by SQLite and answered by PostgreSQL. There the lax reading refuses and the strict
one runs it, so a statement failing on `memdb:` and passing on `memdb-ansi:` is PostgreSQL-only
SQL that will break on SQLite. Both directions are worth knowing; only the direction of the
arrow changes.

**The strict one is a portability check.** A query that passes on `memdb:` and fails on
`memdb-ansi:` is leaning on SQLite and will behave differently on PostgreSQL, which matters when
one application ships on both. `tests/lang/memdb_ansi_strictness.zm` holds the list and asserts
both halves: the lax reading still matches real SQLite, and the strict reading still differs.

Measured against the sample application before any of it was written: its 191 SQL strings use no
joins and no subqueries, because its repositories query one table each and compose in Ziman. The
20,365 characters of SQL in it are almost all of the shape above. Joins and subqueries are here
anyway, because a test suite is not restricted to the queries the application happens to issue
and an engine that refuses them would push every such test onto a container.

A parenthesised arm of a compound (which SQLite also refuses, so nothing is lost yet), a
parenthesised join expression, `LATERAL`, common table expressions and per-test forking are the
next increments. `count(t.*)` is not read: SQLite refuses it,
PostgreSQL counts whole rows with it, and nothing needs it. Anything it does not understand
is an error naming the token, never a quiet partial execution.

## What a statement costs

Every row of every table is an array in memory and there are **no indexes**, so the shape of a
statement's cost is worth knowing before a fixture grows to thousands of rows. Measured, and
guarded by `tests/bench/memdb_ops.zm` and the cost tests in `tests/stdlib/memdb_test.zm`:


```
- a scan, a filter, an aggregate and a `GROUP BY` are **linear** in the rows;
- an `ORDER BY` sorts with its keys computed once per row, so it is n log n comparisons and n
  evaluations -- it used to evaluate the keys once per *comparison*;
- a join whose `ON` has an equality between a column of each side is **linear**: the right
  side is bucketed by that column once and each left row looks up only what can match. A join
  without such an equality still tries every pair, because nothing else can;
- an **uncorrelated** subquery is answered once per statement, not once per row. A long
  `IN (SELECT ..)` list is then compared against per row, so that part is rows times values;
- a **correlated** subquery runs per row, as it must in any engine without an index to help
  it. `EXISTS (SELECT .. WHERE inner.x = outer.y)` over a thousand rows is a million
  comparisons here, where a real engine would use the index on `x`. A fixture that needs that
  shape at scale wants the real engine;
- a write is **linear in the table** -- a `Database` is an immutable value, so inserting one
  row rebuilds the row array -- which makes seeding n rows one statement at a time quadratic
  in principle. The constant is what matters, and the constants that were there (a uniqueness
  scan per written row, a column-name array built per row) are gone;
- a key's uniqueness is checked through an index built once per statement for a write that
  touched several rows, and by a scan for one that touched one or two, because building a map
  to check a single row costs more than looking.
```

## Transactions are free, because a database is a value

A `Database` is an immutable `Map` of immutable tables, so `BEGIN` keeps the current one,
`ROLLBACK` puts it back and `COMMIT` forgets it. There is no undo log and no copying: the saved
database *is* the old state. That is also what will make per-test forking free later -- seed a
schema and a dataset once, then hand every test its own copy of the same value.

## Known divergences from SQLite and PostgreSQL


```
- **No type affinity.** A column declared `INTEGER` holds whatever is inserted; SQLite would
  coerce `'7'` to 7. Insert the right type.
- **The statement tree and the parser live in `sql`**, not here. This module is the engine:
  storage, evaluation and the two drivers. `sql.parse` reads SQL, `sql.format` writes it back,
  and anything wanting to read or rewrite SQL without a database uses that module alone.
- **`DEFERRABLE` is read and ignored**, so every constraint is checked as each statement runs
  rather than at the end of the transaction. SQLite ignores it too unless
  `PRAGMA defer_foreign_keys` is set, so the two agree; PostgreSQL honours it, and a test that
  needs a deferred check is asking for the real engine.
- **A column constraint with a name** (`a INT CONSTRAINT k CHECK (..)`) is kept as text and
  not enforced, where the same constraint written at table level is. Nothing of it is lost; it
  is simply opaque, and `sql`'s header says which shapes are modelled.
- **`CREATE INDEX` is accepted and ignored.** An index changes how fast a query runs, not what
  it answers, and this engine has no query planner to put one in.
- **NULLs sort first ascending**, which is SQLite's order. PostgreSQL sorts them last. A test
  that depends on either is already engine-specific.
- **No `sqlite_master`, no `information_schema`.** A test that inspects the schema through SQL
  is asking the engine about itself, and this one answers only about data.
```

`use memdb` -- or `use memdb.Name` to bring one item in bare.

## Example

```
use datasource
use db.DbError
use db.SqlValue
use db.db_error_message
use db.row_get
use db.sql_as_int
use db.sql_as_text
use memdb

// A small SQL engine in pure Ziman, holding its tables in memory. It is a `db.Driver` like
// `sqlite`, `pg` and `mysql`, so `datasource` reaches it the same way and the code above it cannot
// tell the difference -- which is the point: a test runs against this, production runs against the
// real engine, and only the URL changes.
val drivers = datasource.registry([memdb.driver()])

fn demo() -> Result<unit, DbError> {
    val conn = try datasource.open(drivers, "memdb:")
    try datasource.execute(conn, "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
    try datasource.execute(conn, "INSERT INTO people (id, name) VALUES (?, ?)", SqlValue.Int(1), SqlValue.Text("Ada"))
    try datasource.execute(conn, "INSERT INTO people (id, name) VALUES (?, ?)", SqlValue.Int(2), SqlValue.Text("Grace"))

    val rows = try datasource.query(conn, "SELECT id, name FROM people ORDER BY name")
    for row in rows {
        print(format("{} {}", try sql_as_int(try row_get(row, "id")), try sql_as_text(try row_get(row, "name"))))
    }

    // It enforces what a real engine enforces, so a test fails here for the same reason it would
    // fail there rather than passing by accident.
    if Err(e) = datasource.execute(
        conn,
        "INSERT INTO people (id, name) VALUES (?, ?)",
        SqlValue.Int(1),
        SqlValue.Text("Alan")
    ) {
        print(db_error_message(e))
    }
    if Err(e) = datasource.execute(
        conn,
        "INSERT INTO people (id, name) VALUES (?, ?)",
        SqlValue.Int(3),
        SqlValue.Null
    ) {
        print(db_error_message(e))
    }
    datasource.close(conn)

    // A template runs its setup once and hands every connection its own copy, so one test cannot
    // see what another wrote.
    val fixture = try memdb.template("memdb:", [
        "CREATE TABLE city (name TEXT PRIMARY KEY)",
        "INSERT INTO city (name) VALUES ('London')"
    ])
    val seeded = datasource.registry([memdb.seeded(fixture)])
    val first = try datasource.open(seeded, "memdb:")
    try datasource.execute(first, "INSERT INTO city (name) VALUES ('Paris')")
    print(len(try datasource.query(first, "SELECT name FROM city")))
    datasource.close(first)

    val second = try datasource.open(seeded, "memdb:")
    print(len(try datasource.query(second, "SELECT name FROM city")))
    datasource.close(second)
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("memdb example failed: {}\n", db_error_message(e))
    }
}
```

Output:

```
1 Ada
2 Grace
constraint violation: UNIQUE constraint failed: people.id
constraint violation: NOT NULL constraint failed: people.name
2
1
```

## The driver

### `driver`

```
pub fn driver() -> Driver
```

This engine as a `db.Driver`, for `datasource.registry([memdb.driver()])`.

The URL is `memdb:` with anything after it ignored, so `memdb:` and `memdb:whatever` are both a
fresh empty database. There is no sharing between connections and no file: like
`sqlite::memory:`, a connection *is* the database, which is why `poolable` says no -- a pool of
four would be four different databases.

### `strict_driver`

```
pub fn strict_driver() -> Driver
```

The same engine reading SQL the way the **standard** does, at `memdb-ansi:`. Eleven things
differ, and the first six are places where SQLite departs from ANSI:


```
- `LIKE` is case-sensitive, as the standard and PostgreSQL have it, where SQLite folds ASCII;
- division by zero is an error (SQLSTATE 22012's rule), where SQLite answers NULL;
- comparing a number with text is a type error, where SQLite orders them by a type rank;
- a boolean in a select list stays a boolean, where SQLite has no boolean type and answers 0
  or 1;
- a `PRIMARY KEY` column may not hold NULL, which makes a key implicitly `NOT NULL` as the
  standard says -- where SQLite allows it, and for a single-column `INTEGER PRIMARY KEY` fills
  in the next rowid instead;
- two columns of one derived table sharing a name make a reference to it ambiguous, where
  SQLite takes the first.
```

The other four point the other way, SQLite being the stricter engine (each measured, not
assumed):


```
- a chain of set operations groups left to right in SQLite and binds `INTERSECT` tighter in the
  standard and in PostgreSQL, so `a UNION b INTERSECT c` is two different queries;
- `INTERSECT ALL`, `EXCEPT ALL` and `UNION DISTINCT` are syntax errors in SQLite and ordinary
  SQL in PostgreSQL;
- `RETURNING t.*` is refused by SQLite and answered by PostgreSQL;
- `ALTER TABLE ... ADD COLUMN u TEXT UNIQUE` is refused by SQLite, which does not rewrite the
  rows already there, and accepted by PostgreSQL, which does;
- a derived table's column alias list, `(SELECT ..) AS x (a, b)`, is a syntax error in SQLite
  and ordinary SQL in PostgreSQL.
```

For those, the lax reading refuses and this one runs them.

**It is a portability check, not a better engine.** A suite that passes on `memdb:` and fails on
`memdb-ansi:` has found SQL that leans on SQLite, which matters when the same application also
ships on PostgreSQL -- and finding it costs a second local run rather than a Postgres container.
A statement that fails the other way round, on `memdb:` only, is the mirror of that: SQL only
PostgreSQL will take.
Both report the dialect `memdb`, so migrations and `migrate`'s type tokens work for either.

## A seeded start

### `Template`

```
pub struct Template {
    database: Database
    strict: bool
}
```

A database seeded once and handed to as many connections as there are tests.

**This is the promise `State` has carried since the engine was written**: "the same property is
what will make per-test forking free: seed a schema and a dataset once, then hand each test its
own copy." A `Database` is an immutable value, so the copy is a pointer and the fork costs
nothing -- the work was never the copying, it was that nothing could *reach* a connection's
database to copy it. A `Conn` is a struct of closures with no way back in, which is why this
starts from the statements rather than from a connection.

### `template`

```
pub fn template(url: string, setup: [string]) -> Result<Template, DbError>
```

Every statement of `setup` run against a fresh database, kept as a value.

`url` picks the reading, the same two prefixes the drivers answer to, so a suite that runs on
`memdb-ansi:` seeds in the reading it will be read back in -- a schema the strict reading refuses
should fail here rather than on the first test.

One statement per element, because this engine runs one statement per call and a `;`-separated
script is the caller's to split (`migrate.split_statements` does it, and does it knowing about
quoted semicolons).

### `seeded`

```
pub fn seeded(t: Template) -> Driver
```

A driver whose every connection starts from `t` instead of from nothing.

Each connection still **is** its own database -- that is unchanged and is why `poolable` stays
false -- but they all start equal, which is what a test suite wants: the schema and the dataset
are built once and each test gets a copy it may write to freely. Rolling back is not a
substitute: a test that leaves a transaction open, or that runs DDL, leaves the next one a
database the seed did not describe.
