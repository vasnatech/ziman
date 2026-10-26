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
`DROP TABLE [IF EXISTS]`, `INSERT INTO ... VALUES`, `SELECT` with `WHERE`, `ORDER BY`, `LIMIT`
and `OFFSET`, `UPDATE ... SET ... WHERE`, `DELETE FROM ... WHERE`, and
`BEGIN`/`COMMIT`/`ROLLBACK`. Expressions: literals, column references, bound parameters (`?`,
`$1`), `AND`/`OR`/`NOT`, the six comparisons, `IS [NOT] NULL`, `+ - * /`, and parentheses.

Measured against the sample application before any of it was written: its 191 SQL strings use no
joins and no subqueries, because its repositories query one table each and compose in Ziman. The
20,365 characters of SQL in it are almost all of the shape above.

Aggregates (`COUNT`, `SUM`, `MIN`, `MAX`), `GROUP BY`, `LIKE`, `BETWEEN`, `IN`, `CASE WHEN`,
`COALESCE`, the string functions, `DISTINCT` and `RETURNING` are the next increment; constraint
*enforcement*, `JOIN`, subqueries and per-test forking are the one after. Anything it does not
understand is an error naming the token, never a quiet partial execution.

## Transactions are free, because a database is a value

A `Database` is an immutable `Map` of immutable tables, so `BEGIN` keeps the current one,
`ROLLBACK` puts it back and `COMMIT` forgets it. There is no undo log and no copying: the saved
database *is* the old state. That is also what will make per-test forking free later -- seed a
schema and a dataset once, then hand every test its own copy of the same value.

## Known divergences from SQLite and PostgreSQL


```
- **No type affinity.** A column declared `INTEGER` holds whatever is inserted; SQLite would
  coerce `'7'` to 7. Insert the right type.
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
