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
`DROP TABLE [IF EXISTS]`, `INSERT INTO ... VALUES`, `SELECT [DISTINCT]` with `WHERE`,
`GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT` and `OFFSET`, `UPDATE ... SET ... WHERE`,
`DELETE FROM ... WHERE`, `RETURNING` on any of the three writes, and
`BEGIN`/`COMMIT`/`ROLLBACK`.

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
to right and with table aliases. `RIGHT` and `FULL` are read by `sql` and refused here, with an
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
the way the standard does, and the two differ in exactly five places -- `LIKE`'s case
sensitivity, what division by zero answers, whether a number may be compared with text, whether
a boolean in a select list comes back as a boolean or as the integer 0 or 1 (SQLite has no
boolean type; PostgreSQL and the standard do), and whether a `RETURNING` clause may use a
qualified star.

**The fifth one runs the other way**, and is the only one that does. The first four are places
the strict reading *refuses* what SQLite accepts. `RETURNING t.*` is refused by SQLite in those
words and answered by PostgreSQL, so there the lax reading refuses and the strict one runs it --
which means a statement failing on `memdb:` and passing on `memdb-ansi:` is PostgreSQL-only SQL
that will break on SQLite. Both directions are worth knowing; only the direction of the arrow
changes.

**The strict one is a portability check.** A query that passes on `memdb:` and fails on
`memdb-ansi:` is leaning on SQLite and will behave differently on PostgreSQL, which matters when
one application ships on both. `tests/lang/memdb_ansi_strictness.zm` holds the list and asserts
both halves: the lax reading still matches real SQLite, and the strict reading still differs.

Measured against the sample application before any of it was written: its 191 SQL strings use no
joins and no subqueries, because its repositories query one table each and compose in Ziman. The
20,365 characters of SQL in it are almost all of the shape above. Joins and subqueries are here
anyway, because a test suite is not restricted to the queries the application happens to issue
and an engine that refuses them would push every such test onto a container.

Constraint *enforcement*, set operations (`UNION`, `INTERSECT`, `EXCEPT`), derived tables in a
`FROM` and per-test forking are the next increments. `count(t.*)` is not read: SQLite refuses it,
PostgreSQL counts whole rows with it, and nothing needs it. Anything it does not understand
is an error naming the token, never a quiet partial execution.

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

The same engine reading SQL the way the **standard** does, at `memdb-ansi:`. Five things differ,
and the first four are places where SQLite departs from ANSI:


```
- `LIKE` is case-sensitive, as the standard and PostgreSQL have it, where SQLite folds ASCII;
- division by zero is an error (SQLSTATE 22012's rule), where SQLite answers NULL;
- comparing a number with text is a type error, where SQLite orders them by a type rank;
- a boolean in a select list stays a boolean, where SQLite has no boolean type and answers 0
  or 1.
```

The fifth is the one exception, and it points the other way: `RETURNING t.*` is **refused** by
SQLite and answered by PostgreSQL (measured, not assumed), so the lax reading refuses it and
this one runs it.

**It is a portability check, not a better engine.** A suite that passes on `memdb:` and fails on
`memdb-ansi:` has found SQL that leans on SQLite, which matters when the same application also
ships on PostgreSQL -- and finding it costs a second local run rather than a Postgres container.
A statement that fails the other way round, on `memdb:` only, is the mirror of that: SQL only
PostgreSQL will take.
Both report the dialect `memdb`, so migrations and `migrate`'s type tokens work for either.
