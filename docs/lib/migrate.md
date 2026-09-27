# `migrate`

Versioned schema migrations: a directory of plain `.sql` files, applied in order, once each.

A migration is a file named `V<version>__<name>.sql` (`V001__create_users.sql`): the version is a whole number, the
name says what it does, and the versions of a directory run 1, 2, 3, ... with no gap and no repeat. The file holds
ordinary SQL, one or more statements separated by `;` (a `;` inside a quoted string, an identifier, a `--` or
`/* */` comment or a `$$ ... $$` body does not end one). Files in the directory that do not end in `.sql` are
ignored; a `.sql` file that does not fit the name is an error, so a typo cannot silently skip a migration.


```
val applied = try migrate.run(conn, "migrations")
```

SQL that has to differ by engine (`ALTER TABLE ... ADD CONSTRAINT`, which SQLite lacks; a native `UUID` or `TIMESTAMPTZ`; a key length on MySQL) goes in
a subdirectory named for the engine, `migrations/postgres/`, `migrations/sqlite/` or `migrations/mysql/`. `run` applies the files of the directory and of the
subdirectory of the engine the connection is on, as one list: each version is in the shared directory or in the engine's, never both, so every engine's
database has 1, 2, 3, ... and a version whose SQL differs has one file in each engine's directory.

`run` keeps a table, `schema_migrations`, with a row per applied migration: its version, name, a checksum of its
text and when it ran. Each start it:


```
- refuses a database that has a migration the directory does not (the database is newer than the program);
- refuses one whose stored checksum differs from the file's, because an applied migration has been edited: a
  database built from the edited file would differ from one that ran the original, and nothing would say so. To
  change a schema, add a migration; never edit one that has run somewhere;
- applies the pending ones, in version order.
```

All the pending migrations and their `schema_migrations` rows go in one transaction, so a failure leaves the
database as it was (on SQLite and PostgreSQL; MySQL commits DDL statement by statement, so there a failure in the
middle leaves the earlier statements done). On PostgreSQL the run also takes an advisory lock for the length of
the transaction, so several servers starting at once migrate one after the other, not all at once.

There are no down migrations: a mistake is fixed by the next migration.

`use migrate` -- or `use migrate.Name` to bring one item in bare.

### `Migration`

```
pub struct Migration {
    version: int
    name: string
    file: string
    sql: string
    checksum: string
}
```

One migration: its `version`, its `name`, the `file` it came from (for messages), its `sql` text and the
`checksum` of that text.

### `checksum`

```
pub fn checksum(sql: string) -> string
```

The checksum of a migration's text: SHA-256, base64, over the text with its line endings normalised, so a file
checked out with CRLF on another machine has the same checksum.

### `migration`

```
pub fn migration(version: int, name: string, sql: string) -> Migration
```

A migration made from its parts, with its checksum worked out: for migrations that are written in the program
rather than read from a directory.

## Splitting a file into statements

### `split_statements`

```
pub fn split_statements(sql: string) -> [string]
```

The statements of `sql`, without their `;`, comments or surrounding blanks; blank ones are dropped.

## Reading a directory

### `load`

```
pub fn load(dir: string) -> Result<[Migration], DbError>
```

The migrations in `dir`, in version order. An Err for an unreadable directory, a `.sql` file with a bad name, or
versions that repeat or skip.

### `load_for`

```
pub fn load_for(dir: string, dialect: string) -> Result<[Migration], DbError>
```

The migrations for a database on `dialect` (`sqlite`, `postgres`, `mysql`): those in `dir`, which run everywhere, and those in `dir/<dialect>/`, which
run only there, as one list in version order. A version may be in the shared directory or in a dialect's, never in both -- the versions of one
database must be 1, 2, 3, ... whichever engine it is, so each dialect directory holds its own file for a version the shared directory leaves out. A
missing dialect directory is no migrations of its own.

## Applying

### `apply`

```
pub fn apply(conn: Connection, migrations: [Migration]) -> Result<int, DbError>
```

Brings the database to the latest of `migrations`, given in version order; the number applied by this call. See
the top of this file for what it checks and how it is kept safe.

### `run`

```
pub fn run(conn: Connection, dir: string) -> Result<int, DbError>
```

`apply` for the migrations in the directory `dir`.
