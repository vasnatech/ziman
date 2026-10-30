# `eql`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

An **entity query language**: JPQL's shape, over a Ziman struct instead of a Java
class. A query names entities and their fields, and this module turns it into the SQL those
names stand for.


```
val reg = eql.registry([patient.meta, visit.meta])
val q = try eql.translate(reg, "SELECT p FROM Patient p WHERE p.last_name = :name ORDER BY p.created_at DESC")
// q.sql       -> SELECT p.id AS id, p.last_name AS last_name, .. FROM patients AS p
//                WHERE p.last_name = $1 ORDER BY p.created_at DESC
// q.bindings  -> ["name"]
```

**It is a translator, not an engine.** The answer is a `sql.Statement` and the SQL text that
tree writes, so everything downstream is what it already was: `datasource` runs the text,
`memdb` can run it in a test, and `sql.format` decides how it is spelled. Nothing here talks to
a database -- `eql.list` and friends, which do, are a layer above this one.

## Why a translation rather than a string you write yourself

Three things the repositories it is for do by hand today, and each is a mistake the type checker
cannot see:


```
- **a field name is just text in a SQL string.** `WHERE last_nmae = $1` compiles, deploys, and
  fails on the first request. Here a path is resolved against the entity's own column list, so
  the error arrives at translation and names the entity and what it does have.
- **parameters are numbered by hand**, in one function, with the values assembled in another,
  in the same order. `repository/paging.zm` builds ` AND type IN ($2, $3, ...)` that way, and
  `service/crud.zm`'s E9 note is about a hand-numbered INSERT "passing 24 parameters to 8
  holes". Here a parameter has a *name*, `arrange` lays the values out in whatever order the
  SQL ended up numbering them, and a missing or unused one is an error rather than a shift.
- **the column list of a `SELECT *` is implicit**, so a row read by name depends on the
  engine's column order. `SELECT p` expands to the entity's own columns, each aliased to its
  bare name, which is what `<name>_from_row` reads by.
```

## How it works, and why at the token level

The lexer is `sql.tokenize`, and the rewrite happens on the token run: entity names become
tables, `alias.field` is checked and passed through, `:name` becomes `$N`. **A text-level
substitution could not do this correctly** -- a `:` inside a quoted string or a `--` comment is
text and not a parameter, and only the lexer knows the difference. The rewritten run is then
handed to `sql.parse`, so the output is checked by the SQL parser before it can reach a
database, and anything the expression grammar already reads -- `CASE`, `BETWEEN`, `IN`,
subqueries, function calls, `CAST` -- works here without this module knowing it exists.

## What it reads

`SELECT` (an alias on its own, or any expression over paths, aliases and literals, with
`AS` names), `FROM Entity alias` with more entities after a comma, `[INNER|LEFT|RIGHT|FULL|
CROSS] JOIN Entity alias [ON ..|USING (..)]`, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY` with
`ASC`/`DESC`, `LIMIT` and `OFFSET`. Named parameters are `:name`; the SQL forms `?` and `$1` are
refused, because a query that mixes the two numbering schemes is a bug waiting for a reader.

## What it does not read, and what that costs

**No associations.** The entities this was built for declare no relationships -- a `patient_id`
is a `Uuid` field, not a reference -- so there are no path expressions across entities
(`v.patient.last_name`) and no fetch joins. A join names the second entity and its condition
explicitly, which is what the repositories write today. Adding relationships is a modelling
increment of its own, and the one that brings the N+1 question with it.

**No `SELECT new Dto(..)`**: a projection answers rows, and the caller reads them.

**`DELETE` is translated and `UPDATE` is not**, which is a line drawn by what each one needs
rather than by how much work it is. A bulk delete is a `WHERE` over one entity and nothing else,
so it is the same translation a `SELECT`'s `WHERE` already gets, and the reason to write it here
is the same: a misspelt `v.patinet_id` fails when the module loads rather than on the request
that would have run it. A bulk update additionally needs a `SET` list, which is the half that has no entity to check it
against -- and a one-row update is `eql.update_one`, which builds its statement from the
entity's own columns and so cannot disagree with them at all. There is nothing an
`UPDATE .. SET ..` entity query would add except a second way to get a column list wrong.

**No implicit joins, no `FETCH`, no `TREAT`, no polymorphism** -- there is no inheritance
between entity structs to be polymorphic over.

`use eql` -- or `use eql.Name` to bring one item in bare.

## Example

```
use datasource
use db.DbError
use db.SqlValue
use db.db_error_message
use eql
use sqlite

// An entity query names a struct and its fields; eql turns those names into the table and columns
// they stand for. `#[derive(Entity(..))]` writes the description it needs -- `person_meta` below --
// from the struct itself, so the field names in a query are checked against the same list the
// struct declares rather than against a SQL string nobody can type-check.
#[derive(Row, Entity("people"))]
struct Person {
    #[pk]
    id: int
    name: string
    city: string
}

val drivers = datasource.registry([sqlite.driver()])

// One registry names every entity a query may mention.
val people = eql.registry([person_meta])

fn demo() -> Result<unit, DbError> {
    val conn = try datasource.open(drivers, "sqlite::memory:")
    try datasource.execute(
        conn,
        "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, city TEXT NOT NULL)"
    )

    // Translation happens once, not per request: the result holds the SQL and the names it expects
    // to be given. A mistyped field fails here rather than on the first request that reads it.
    val in_city = try eql.translate(
        people,
        "SELECT p FROM Person p WHERE p.city = :city ORDER BY p.name"
    )
    print(in_city.sql)
    print(in_city.bindings)

    // The entity is enough for the ordinary writes; no SQL is written for them at all.
    try eql.insert_one<Person>(
        conn,
        person_meta,
        person_to_params(Person { id: 1, name: "Ada", city: "London" }),
        person_from_row
    )
    try eql.insert_one<Person>(
        conn,
        person_meta,
        person_to_params(Person { id: 2, name: "Grace", city: "New York" }),
        person_from_row
    )
    try eql.insert_one<Person>(
        conn,
        person_meta,
        person_to_params(Person { id: 3, name: "Alan", city: "London" }),
        person_from_row
    )
    print(try eql.count_all(conn, person_meta))

    // Running the translated query: the binding is named, so the order of the SQL parameters is
    // eql's problem rather than the caller's.
    for found in try eql.list<Person>(conn, in_city, [eql.bind("city", "London")], person_from_row) {
        print(format("{} of {}", found.name, found.city))
    }

    // The primary key is declared by `#[pk]`, so reading one row needs no query at all.
    val grace = try eql.find_by_key<Person>(conn, person_meta, [SqlValue.Int(2)], person_from_row)
    print(grace.name)
    print(try eql.exists(conn, person_meta, [SqlValue.Int(9)]))

    // A mistyped field is caught by translation, against the struct's own column list.
    switch eql.translate(people, "SELECT p FROM Person p WHERE p.citty = :city") {
        Ok(_) { print("unexpected: a misspelled field translated") }
        Err(e) { print(db_error_message(e)) }
    }

    print(try eql.delete_where(conn, person_meta, [eql.bind("city", "London")]))
    print(try eql.count_all(conn, person_meta))
    datasource.close(conn)
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("eql example failed: {}\n", db_error_message(e))
    }
}
```

Output:

```
SELECT p.id AS id, p.name AS name, p.city AS city FROM people AS p WHERE p.city = $1 ORDER BY p.name
[city]
3
Ada of London
Alan of London
Grace
false
database error: entity Person has no field 'citty' (it has: id, name, city)
2
1
```

### `Entity`

```
pub struct Entity {
    name: string
    table: string
    columns: [string]
    // The columns that identify one row, in field order: the fields marked `#[pk]`.
    //
    // **Empty is allowed and means "no key was declared"**, not "the first column". A reader that
    // needs one -- anything addressing a single row -- refuses an empty `keys` by name rather than
    // guessing, because the guess was right for all 27 entities in the North Star application and
    // would have been silently wrong for the first table that is keyed on something else.
    //
    // More than one is a compound key, which is simply two fields marked. Nothing here objects to
    // that; a *reader* that can only address a single-column key is the thing that has to say so.
    keys: [string]
    // The struct field each column belongs to, in the same order -- what a *query* names where
    // `columns` is what the *database* has.
    //
    // **Equal to `columns` unless a field carries `#[column(name: "..")]`** (2026-10-29), and the
    // paragraph above this struct is why that matters: field name, column name, what a query may
    // write and what `from_row` reads were one list "by construction", and an override makes them
    // two. So a query still says `e.start`, this pairs it with `start_date`, and the column name
    // appears in the SQL and nowhere a reader of the struct has to think about it.
    //
    // The two lists are parallel. Nothing here enforces that, because the only thing that builds a
    // mismatched pair is a caller writing one by hand, and `column_for` falls back to the field's
    // own name rather than reading past the end of `columns`.
    fields: [string]
}
```

One entity: the name a query calls it, the table it is stored in, and its columns.

The columns are `#[derive(Entity)]`'s `<name>_columns`. **They were the struct's own field list,
so what a query may name and what `from_row` reads were the same list by construction** -- and a
field carrying `#[column(name: "..")]` makes them two lists (2026-10-29), which is what `fields`
below pairs. Everything a *reader* writes is still the field: a query says `e.start`, this maps
it to `start_date`, and the column name appears in the SQL and in the attribute and nowhere
else. The table is hand-written beside the struct, because no pluralisation rule survives a real
schema -- see the derive's own comment.

### `entity`

```
pub fn entity(name: string, table: string, columns: [string]) -> Entity
```

An entity with no declared key, for a caller that only names columns.

**The field names are the column names**, which is the case every caller had before an override
existed and still the overwhelmingly common one: a query names what the struct spells.

```
pub fn entity(name: string, table: string, columns: [string], fields: [string]) -> Entity
```

The same where a field is stored in a column of another name: `fields` pairs with `columns`,
position by position. What `#[derive(Entity("table"))]` writes when any field carries
`#[column(name: "..")]`.

### `keyed_entity`

```
pub fn keyed_entity(name: string, table: string, columns: [string], keys: [string]) -> Entity
```

An entity and the columns that identify one of its rows. What `#[derive(Entity("table"))]`
writes when the struct marks a field `#[pk]`.

```
pub fn keyed_entity(
    name: string,
    table: string,
    columns: [string],
    keys: [string],
    fields: [string]
) -> Entity
```

Keyed, and with a field-to-column pairing. `keys` are **column** names, as they are in the form
above: they go into SQL, and `update_one` compares them against `columns`.

### `Registry`

```
pub struct Registry {
    // **Keyed on the folded name**, because SQL identifiers are case-insensitive and every lookup
    // folds what it was given. Folding happens once per entity here instead of once per entity per
    // comparison, which is what a linear scan over this was doing.
    by_name: Map<string, Entity>
    // The names as they were registered, in order, for the refusal message. The keys above are
    // folded and a map has no order, so "registered: vitalsign, bodymeasurement" is neither what
    // the caller wrote nor the order they wrote it in.
    names: [string]
}
```

The entities a program has, and the only thing here that knows one exists -- the same shape as
`datasource.registry`, and for the same reason: this module names no entity, so an application
that has three hands over three.

### `registry`

```
pub fn registry(entities: [Entity]) -> Registry
```

### `Binding`

```
pub struct Binding {
    name: string
    value: SqlValue
    values: [SqlValue]
    list: bool
}
```

A named parameter's value, or the values of a collection one.

A collection is its own kind rather than an array of `SqlValue` everywhere, because the two
cannot be told apart by looking: `:ids` bound to one value and `:ids` bound to a list of one are
different queries, and only the binding knows which was meant.

### `bind`

```
pub fn bind(name: string, value: SqlValue) -> Binding
```

```
pub fn bind(name: string, value: int) -> Binding
```

**The conversion to `SqlValue` belongs here, not at the call site.** Every one of these is the
one conversion its type has -- `uuid_to_sql` for a `Uuid`, `time.to_sql` for an instant -- so
writing it out at the call site was never a choice being made, only a step that could be
forgotten or, worse, got right in a way the reader has to re-verify. What a binding list looked
like before was `[bind("id", uuid_to_sql(id)), bind("patient_id", uuid_to_sql(patient_id))]`,
where the names and the values are what the reader is checking and `uuid_to_sql` is in the way
of both.

The `SqlValue` overload above stays, and is the escape hatch: a value whose SQL representation is
a decision rather than a convention -- a decimal as text at a chosen scale, an `Option` as
`Null` or not -- says so by constructing the `SqlValue` itself. These overloads deliberately do
not cover `Option<T>`, because "absent means NULL" is exactly such a decision.

```
pub fn bind(name: string, value: float) -> Binding
```

```
pub fn bind(name: string, value: string) -> Binding
```

```
pub fn bind(name: string, value: bool) -> Binding
```

```
pub fn bind(name: string, value: [byte]) -> Binding
```

```
pub fn bind(name: string, value: Uuid) -> Binding
```

```
pub fn bind(name: string, value: Instant) -> Binding
```

```
pub fn bind(name: string, value: LocalDate) -> Binding
```

```
pub fn bind(name: string, value: LocalTime) -> Binding
```

```
pub fn bind(name: string, value: LocalDateTime) -> Binding
```

```
pub fn bind(name: string, value: Duration) -> Binding
```

```
pub fn bind(name: string, value: ZoneOffset) -> Binding
```

```
pub fn bind(name: string, value: ZoneId) -> Binding
```

### `bind_list`

```
pub fn bind_list(name: string, values: [SqlValue]) -> Binding
```

A collection parameter, for `IN :names`.

**An empty list is refused rather than guessed at.** `x IN ()` is not SQL, and every spelling
that is has the wrong answer for one of the two directions: `IN (NULL)` is unknown rather than
false, and `NOT IN (NULL)` is unknown rather than true. A caller that can have an empty filter
knows what it means by one -- usually "match nothing", which is an answer it can give without a
round trip -- so it says so at the call site instead of here.

```
pub fn bind_list(name: string, values: [string]) -> Binding
```

The same conversion `bind` does, for a collection. **Three types rather than `bind`'s thirteen**,
and the asymmetry is deliberate: a collection parameter is an `IN` list, and what a query filters
a set of rows by is identifiers, numbers or text. `IN` over a list of durations or zone offsets
is not a shape any query here has, so it is not guessed at -- the `[SqlValue]` overload above
takes one if it ever is.

```
pub fn bind_list(name: string, values: [int]) -> Binding
```

```
pub fn bind_list(name: string, values: [Uuid]) -> Binding
```

### `Translated`

```
pub struct Translated {
    sql: string
    statement: Statement
    bindings: [string]
    lists: [string]
    query: string
    registry: Registry
    // The sizes already worked out, for a query with a collection parameter. See `Shaped`.
    shaped: [Shaped]
}
```

What a query became: the SQL to send, the tree it parsed into, and the parameter names in the
order the SQL numbers them.

It also keeps the query and the registry it came from, because a **collection parameter's** SQL
depends on how many values are bound -- `IN :types` is three placeholders for three values --
and that is not known until the call. A query with no collection parameter is translated once and
its `sql` is used as it stands; one with a collection is re-translated per call, which costs the
140-300 microseconds `translate` costs and is still a fraction of the round trip it saves
hand-numbering for. `lists` is how `prepare` tells the two apart.

### `translate`

```
pub fn translate(reg: Registry, query: string) -> Result<Translated, DbError>
```

A query translated: entity names become tables, paths are checked, `:name` becomes `$N`, and an
alias on its own becomes the entity's columns.

### `arrange`

```
pub fn arrange(t: Translated, bound: [Binding]) -> Result<[SqlValue], DbError>
```

The bound values in the order the translated SQL numbers them.

**This is the half that removes the hand-numbering.** A caller binds by name, in any order, and
a name the query does not use -- or one it uses and nothing bound -- is an error here, where the
hand-written form silently shifted every later parameter by one.

### `prepare`

```
pub fn prepare(query: Translated, bound: [Binding]) -> Result<Prepared, DbError>
```

The SQL to send and the values to send with it.

For a query with no collection parameter this is `t.sql` as it stands and the values laid out by
`arrange` -- the translation was done once and is reused. For one with a collection it is a
second translation, now that the number of values is known, because `IN :types` is three
placeholders for three values and one for one. That costs what a translation costs and still
beats what it replaces: a hand-built ` AND type IN ($2, $3, ...)` in one function with the
values assembled in another.

### `Prepared`

```
pub struct Prepared {
    sql: string
    params: [SqlValue]
}
```

One call's SQL and values.

## Running one

### `list`

```
pub fn list<E>(
    conn: datasource.Connection,
    query: Translated,
    bound: [Binding],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<[E], DbError>
```

The entities a query answers, read by `from_row`.

A row answering **two** entities (`SELECT p, v FROM ..`) has two columns called `id`, and
`row_get` takes the first -- so one `from_row` reads the first entity of such a row and the
second is unreachable through it. Select one entity at a time, or use `rows` and read the tuple.
JPQL answers an array of objects there; doing that here would need a tuple type this layer does
not have.

### `one`

```
pub fn one<E>(
    conn: datasource.Connection,
    query: Translated,
    bound: [Binding],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<Option<E>, DbError>
```

The first entity a query answers, or None.

It runs the query as written and keeps the first row: like `datasource.query_one`, it does
**not** add a `LIMIT 1`, so a query that could match many rows fetches and discards them all.
Write the limit into the query when that matters.

## Writing one row by its key

### `insert_one`

```
pub fn insert_one<E>(
    conn: datasource.Connection,
    e: Entity,
    values: [SqlValue],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<E, DbError>
```

**One row stored, and read back.** The columns and their order are the entity's, so the statement
and the values cannot disagree; the read-back is by the entity's key, which is what `#[pk]` is
for.

### `update_one`

```
pub fn update_one<E>(
    conn: datasource.Connection,
    e: Entity,
    values: [SqlValue],
    scope: [string],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<E, DbError>
```

**Every column of one row replaced by `values`, and the row read back.** The assignments are the
entity's own non-key columns, so a `SET` list cannot drift out of step with the values the way a
hand-written one can.

`scope` names columns the stored row must match *as well as* the key -- for a table whose rows
belong to someone, the owner. Their values come out of `values` like every other column's, so a
scope costs no extra argument to keep aligned; `[]` is the plain shape, keyed and nothing else.

**The read-back goes through the same predicate, not through the key alone**, and with a scope
those differ exactly where it matters: an entity carrying a different owner matches no row, and a
read-back by key would then answer *the other owner's row* -- the update correctly refused and
its subject handed back anyway. Through the predicate it answers `NotFound`.

That is also why this does not consult the number of rows the `UPDATE` affected, which is the
obvious way to tell a miss from a hit: MySQL reports rows *changed* rather than matched unless
`CLIENT_FOUND_ROWS` is set, and `lib/db/mysql.zm` does not set it, so storing the values a row
already has would be indistinguishable from matching nothing.

### `find_by_key`

```
pub fn find_by_key<E>(
    conn: datasource.Connection,
    e: Entity,
    keys: [SqlValue],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<E, DbError>
```

The one row a key addresses.

### `find_maybe`

```
pub fn find_maybe<E>(
    conn: datasource.Connection,
    e: Entity,
    keys: [SqlValue],
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<Option<E>, DbError>
```

The row a key addresses, or `None`. **Absence is an answer here and an error in `find_by_key`**,
and which one a caller wants is not guessable: a request for one record by id is a 404 and wants
the error, while a check before writing wants the `Option` and would otherwise read a `NotFound`
back out of a `Result` to decide nothing is wrong.

### `exists`

```
pub fn exists(conn: datasource.Connection, e: Entity, keys: [SqlValue]) -> Result<bool, DbError>
```

Whether the key addresses a row, without building the entity from it. The projection is the key
rather than `1` or `*`: a constant is a different kind of expression for an engine to plan, and
every column is work thrown away.

### `find_all`

```
pub fn find_all<E>(
    conn: datasource.Connection,
    e: Entity,
    from_row: fn(Row) -> Result<E, DbError>
) -> Result<[E], DbError>
```

**Every row, ordered by the key.** The order is not decoration: without an `ORDER BY` the engine
may answer in any order it likes, and two runs of the same program disagreeing about the order of
a list is the kind of difference that is found in production rather than in a test. The key is
the one ordering every entity is guaranteed to have.

### `count_all`

```
pub fn count_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>
```

How many rows the table has.

### `delete_where`

```
pub fn delete_where(conn: datasource.Connection, e: Entity, matching: [Binding]) -> Result<int, DbError>
```

**Every row matching `matching`, and how many there were.** An empty predicate is refused rather
than read as "all of them": `DELETE FROM t` is a different intention and says so through
`delete_all`.

### `delete_one`

```
pub fn delete_one(
    conn: datasource.Connection,
    e: Entity,
    keys: [SqlValue],
    scope: [Binding]
) -> Result<unit, DbError>
```

**The one row the key addresses, with `scope` as well**, and `NotFound` when nothing matched --
which is also the answer for a row another owner holds, because that is the rule the repository
layer keeps throughout: another owner's row is not found, not forbidden.

Unlike an update, this does consult the number of rows affected. A `DELETE` has no rows-changed
regime to be wrong about: every engine counts the rows it removed.

### `delete_all`

```
pub fn delete_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>
```

Every row of the table, and how many there were. **Spelled out rather than reached by an empty
predicate**, so emptying a table is never what a predicate that came out empty does by accident.

### `execute`

```
pub fn execute(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>
```

**A DELETE, and the rows it removed.** Separate from `rows`/`list`/`one`/`count` rather than
folded into them, because the mistake worth preventing is not a wrong answer: handing a DELETE to
a read would *run* it, delete the rows, and hand back the empty result as though it had found
nothing. So each side refuses the other's statement by name.

### `rows`

```
pub fn rows(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<[Row], DbError>
```

### `count`

```
pub fn count(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>
```

The one number a counting query answers: one row of one column, read as an integer.

`SELECT count(*) AS n FROM Visit v WHERE v.patient_id = :id` is the commonest projection there
is, and reading it by hand is four lines of row and converter every time. A query that answers
more than one row, or a row of more than one column, is an error naming what it answered --
because a count that quietly read the first of several numbers would be worse than no helper.
