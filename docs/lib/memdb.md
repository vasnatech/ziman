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

`CREATE TABLE` (with `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, `DEFAULT` and a column type that is
recorded but not enforced), `CREATE [UNIQUE] INDEX`, `ALTER TABLE ... ADD COLUMN`,
`DROP TABLE [IF EXISTS]`, `INSERT INTO ... VALUES`, `SELECT` with `WHERE`, `GROUP BY`, `HAVING`,
`ORDER BY`, `LIMIT` and `OFFSET`, `UPDATE ... SET ... WHERE`, `DELETE FROM ... WHERE`, and
`BEGIN`/`COMMIT`/`ROLLBACK`.

Expressions: literals, column references, bound parameters (`?`, `$1`), `AND`/`OR`/`NOT`, the six
comparisons, `IS [NOT] NULL`, `LIKE`, `IN`, `BETWEEN`, `CASE WHEN`, `+ - * /`, `||`, parentheses,
the scalar functions `substr`, `length`, `lower`, `upper`, `trim`, `abs` and `coalesce`, and the
aggregates `COUNT`, `SUM`, `MIN`, `MAX` and `AVG`.

**Columns are resolved before a statement runs**, as a real engine resolves them when it
prepares one. Looking them up per row instead meant a query whose `WHERE` matched nothing never
looked, so `SELECT count(age) FROM visit WHERE reason = 'nothing'` answered 0 where SQLite
answers "no such column: age" -- a typo passing here and failing there is the exact false green
this engine exists to avoid, and the cross-check fixture caught it within a minute of the
aggregates being written.

## Two readings of SQL

`driver()` at `memdb:` reads SQL the way SQLite does. `strict_driver()` at `memdb-ansi:` reads it
the way the standard does, and the two differ in exactly four places -- `LIKE`'s case
sensitivity, what division by zero answers, whether a number may be compared with text, and
whether a boolean in a select list comes back as a boolean or as the integer 0 or 1 (SQLite has
no boolean type; PostgreSQL and the standard do).

**The strict one is a portability check.** A query that passes on `memdb:` and fails on
`memdb-ansi:` is leaning on SQLite and will behave differently on PostgreSQL, which matters when
one application ships on both. `tests/lang/memdb_ansi_strictness.zm` holds the list and asserts
both halves: the lax reading still matches real SQLite, and the strict reading still differs.

Measured against the sample application before any of it was written: its 191 SQL strings use no
joins and no subqueries, because its repositories query one table each and compose in Ziman. The
20,365 characters of SQL in it are almost all of the shape above.

`DISTINCT` and `RETURNING` are the next increment, then constraint *enforcement*, `JOIN`,
subqueries and per-test forking. Anything it does not understand is an error naming the token,
never a quiet partial execution.

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
- **Constraints are recorded, not enforced** in this increment: `NOT NULL`, `PRIMARY KEY` and
  `UNIQUE` parse and are remembered, and a violation is accepted. The real engines refuse it.
- **`CREATE INDEX` is accepted and ignored.** An index changes how fast a query runs, not what
  it answers, and this engine has no query planner to put one in.
- **NULLs sort first ascending**, which is SQLite's order. PostgreSQL sorts them last. A test
  that depends on either is already engine-specific.
- **No `sqlite_master`, no `information_schema`.** A test that inspects the schema through SQL
  is asking the engine about itself, and this one answers only about data.
```

`use memdb` -- or `use memdb.Name` to bring one item in bare.

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

The same engine reading SQL the way the **standard** does, at `memdb-ansi:`. Three things
differ, and they are the three where SQLite departs from ANSI:


```
- `LIKE` is case-sensitive, as the standard and PostgreSQL have it, where SQLite folds ASCII;
- division by zero is an error (SQLSTATE 22012's rule), where SQLite answers NULL;
- comparing a number with text is a type error, where SQLite orders them by a type rank.
```

**It is a portability check, not a better engine.** A suite that passes on `memdb:` and fails on
`memdb-ansi:` has found SQL that leans on SQLite, which matters when the same application also
ships on PostgreSQL -- and finding it costs a second local run rather than a Postgres container.
Both report the dialect `memdb`, so migrations and `migrate`'s type tokens work for either.
