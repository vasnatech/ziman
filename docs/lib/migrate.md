# `migrate`

Versioned schema migrations: a directory of plain `.sql` files, applied in order, once each.

A migration is a file named `V<version>__<name>.sql` (`V001__create_users.sql`): the version is a whole number, the
name says what it does, and the versions of a directory run 1, 2, 3, ... with no gap and no repeat. The file holds
ordinary SQL, one or more statements separated by `;` (a `;` inside a quoted string, a `"quoted"` or `` `quoted` ``
identifier, a `--` or `/* */` comment or a `$$ ... $$` body does not end one). **Anything that opens and never
closes is an error**, naming the line it opened on: an unterminated `/*` used to swallow the rest of the file and
let the half-applied migration be recorded as done.

Block comments do not nest, which is SQLite's and MySQL's reading; PostgreSQL's do. A nested one is therefore
mis-split rather than silently truncated -- the text after the inner `*/` is handed to the server, which refuses
it -- so the failure is loud, but it is a known difference rather than a feature. Files in the directory that do not end in `.sql` are
ignored; a `.sql` file that does not fit the name is an error, so a typo cannot silently skip a migration.


```
val applied = try migrate.run(conn, "migrations")
```

SQL that has to differ by engine comes in two sizes, and they have different answers.

A whole migration that differs -- `ALTER TABLE ... ADD CONSTRAINT`, which SQLite lacks, or a data conversion one
engine needs and another does not -- goes in a subdirectory named for the engine, `migrations/postgres/`,
`migrations/sqlite/` or `migrations/mysql/`. `run` applies the files of the directory and of the subdirectory of
the engine the connection is on, as one list: each version is in the shared directory or in the engine's, never
both, so every engine's database has 1, 2, 3, ... and a version whose SQL differs has one file per engine.

A migration that differs only in a *column type* -- a key that is `uuid` on PostgreSQL and `TEXT` on SQLite --
does not need two files. Write `{{uuid}}` and `load_for` substitutes the engine's spelling; see "engine type
tokens" below. Two files for one column type is how a schema drifts, and the drift is not hypothetical: the
health-record backend kept nine such pairs, 342 duplicated lines, and the duplication caused a two-commit
PostgreSQL outage before anything ran both engines.

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

## Example

```
use datasource
use db.DbError
use db.db_error_message
use fs
use memdb
use migrate

// Versioned schema migrations: a directory of plain `.sql` files, applied in order, once each. A
// file is named `V<version>__<name>.sql`, the versions run 1, 2, 3 with no gap and no repeat, and
// what has already been applied is recorded in the database -- so running twice is not running
// twice.
val drivers = datasource.registry([memdb.driver()])

fn write_migrations(dir: string) -> Result<unit, string> {
    if Err(e) = fs.make_dirs(dir) {
        return err(fs.fs_error_message(e))
    }
    if Err(e) = fs.write_text(
        fs.path_join(dir, "V001__create_people.sql"),
        """
        -- Two statements in one file, separated by `;`.
        CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL);
        INSERT INTO people (id, name) VALUES (1, 'Ada');
        """
    ) {
        return err(fs.fs_error_message(e))
    }
    if Err(e) = fs.write_text(
        fs.path_join(dir, "V002__add_city.sql"),
        "ALTER TABLE people ADD COLUMN city TEXT"
    ) {
        return err(fs.fs_error_message(e))
    }
    return unit
}

fn demo(dir: string) -> Result<unit, DbError> {
    val conn = try datasource.open(drivers, "memdb:")

    // The answer is how many ran, so a deploy can say what it did.
    print(try migrate.run(conn, dir))

    // Running again applies nothing: each version is recorded as it is applied.
    print(try migrate.run(conn, dir))

    print(len(try datasource.query(conn, "SELECT id, name, city FROM people")))

    // The directory can be read without a database, which is what a check in CI does.
    for m in try migrate.load(dir) {
        printf("V{} {}\n", m.version, m.name)
    }

    // A checksum over the text, so an edit to a file that has already run can be detected.
    print(migrate.checksum("CREATE TABLE a (b INT)") == migrate.checksum("CREATE TABLE a (b INT)"))
    print(migrate.checksum("CREATE TABLE a (b INT)") == migrate.checksum("CREATE TABLE a (c INT)"))
    datasource.close(conn)
    return unit
}

fn main() {
    val dir = "/tmp/ziman_manual_migrate_example"
    if Err(e) = write_migrations(dir) {
        printf("migrate example failed: {}\n", e)
        return
    }
    if Err(e) = demo(dir) {
        printf("migrate example failed: {}\n", db_error_message(e))
    }
    if Err(e) = fs.remove_all(dir) {
        printf("could not remove {}: {}\n", dir, fs.fs_error_message(e))
    }
}
```

Output:

```
2
0
1
V1 create_people
V2 add_city
true
false
```

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

## Engine type tokens

### `TypeToken`

```
pub struct TypeToken {
    token: string
    // What each engine spells this token as, keyed by the dialect `datasource.dialect` answers.
    //
    // **A map rather than a field per engine.** It was `sqlite`/`postgres`/`mysql` as three
    // fields with a three-branch lookup, so a fourth engine meant editing this struct, the table
    // and the lookup -- which is exactly the shape `datasource` lost when a driver became a value.
    // The first thing `memdb` did on meeting a real migration directory was fail here, with
    // "uses {{uuid}} and there is no spelling of it for 'memdb'", which is the right error and
    // was being raised by the wrong design.
    spellings: Map<string, string>
}
```

Two engines often want the same table and disagree about one column type -- a key is `uuid` on PostgreSQL and
`TEXT` on SQLite, and PostgreSQL will not create a foreign key between the two. Writing the whole file twice for
that is how a schema drifts: the health-record backend had nine such pairs, 342 lines of DDL maintained in
duplicate, and the duplication had already caused a two-commit PostgreSQL outage.

So a shared migration may write a token, and `load_for` substitutes the engine's spelling before anything reads
the SQL:


```
CREATE TABLE audit_events (
    id {{uuid}} PRIMARY KEY,
    actor_id {{uuid}} NOT NULL REFERENCES users(id),
    ...
```

This is deliberately not a template language. There are no expressions, no conditionals and no way to add a
token from outside this file: a migration either differs in a column type, which a token covers, or it differs in
its statements, which is what the `postgres/`, `sqlite/` and `mysql/` subdirectories are for. An unknown
`{{...}}` is an error naming the file, never text passed through to the database, so a typo fails at startup
rather than creating a column called `{{uudi}}`.

**The checksum is of the substituted SQL, not of the template.** It has to be: the checksum's job is to notice
when the SQL a database actually ran stops matching the SQL the program now has, and a template whose token
table changed meaning would otherwise slip past. The consequence is worth stating plainly -- collapsing an
already-applied pair of files into one token file changes that migration's checksum, and `run` refuses a
database whose stored checksum moved. That is the correct refusal; it just means the collapse belongs in the
same change as a fresh database, or needs the stored checksums updated deliberately.

### `type_tokens`

```
pub val type_tokens = [
```

Every token, and what each engine gets. `CHAR(36)` rather than MySQL's `BINARY(16)`: the text form is what the
other two store, and a migration that moves to the packed form can be written when something needs it.

### `substitute`

```
pub fn substitute(sql: string, dialect: string, file: string) -> Result<string, DbError>
```

`sql` with every `{{token}}` replaced by `dialect`'s spelling. An Err if a `{{...}}` is left over -- an unknown
token, or a known one on an engine this file has no spelling for -- naming `file` so the message says where.

## Splitting a file into statements

### `split_statements`

```
pub fn split_statements(sql: string) -> Result<[string], DbError>
```

The statements of `sql`, without their `;`, comments or surrounding blanks; blank ones are dropped.

**An unterminated string, identifier, comment or dollar body is an error**, and that is the whole
reason this answers a `Result`. Each of them used to run to the end of the text and stop there,
which for a comment meant *silently discarding the rest of the file*:


```
CREATE TABLE kept (x int);
/* a note that forgets to close
CREATE TABLE lost (y int);
```

applied one statement, never created `lost`, and reported success -- then recorded a row in
`schema_migrations` holding a checksum of the *whole* file, so nothing would ever flag it, the
migration would never run again, and a developer who closed the comment would be met with a
checksum mismatch instead of a fix. For a module whose stated purpose includes "a typo cannot
silently skip a migration", a half-applied migration recorded as done is the worst thing it
could do, and it took one missing `*/`.

**Backticks count as identifier quotes** here too. The header has always said a `;` inside an
identifier does not end a statement, and `"a;b"` was honoured while MySQL's `` `a;b` `` was not
-- in a module that has a `migrations/mysql/` directory and asks the connection for its dialect.

A doubled quote inside a quoted run (`'it''s'`, `` `a``b` ``) needs no special case: the run ends
at the first of the pair and the second immediately opens another, so the text between them stays
quoted either way.

## Reading a directory

### `load`

```
pub fn load(dir: string) -> Result<[Migration], DbError>
```

The migrations in `dir`, in version order. An Err for an unreadable directory, a `.sql` file with a bad name, or
versions that repeat or skip.

The SQL comes back as written, with any `{{token}}` still in it, because a directory has no engine of its own --
`load_for` is what substitutes, and its checksums are the ones a database stores. Use this to list or inspect
the files; use `load_for` to get what will actually run.

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

`apply` for the migrations in the directory `dir`, resolved through `ZIMAN_RESOURCE_PATH` first (see
`fs.resolve`) -- with the variable unset, `dir` is used exactly as given, unchanged.
