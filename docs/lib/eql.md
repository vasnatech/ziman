# `eql`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
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

**No `SELECT new Dto(..)`**: a projection answers rows, and the caller reads them. **No bulk
`UPDATE`/`DELETE`**: writes stay SQL, where the statement being explicit is worth more than the
brevity. **No implicit joins, no `FETCH`, no `TREAT`, no polymorphism** -- there is no
inheritance between entity structs to be polymorphic over.

`use eql` -- or `use eql.Name` to bring one item in bare.

### `Entity`

```
pub struct Entity {
    name: string
    table: string
    columns: [string]
}
```

One entity: the name a query calls it, the table it is stored in, and its columns.

The columns are `#[derive(Entity)]`'s `<name>_columns`, which is the struct's own field list, so
what a query may name and what `from_row` reads are the same list by construction. The table is
hand-written beside the struct, because no pluralisation rule survives a real schema -- see the
derive's own comment.

### `entity`

```
pub fn entity(name: string, table: string, columns: [string]) -> Entity
```

### `Registry`

```
pub struct Registry {
    entities: [Entity]
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

### `Translated`

```
pub struct Translated {
    sql: string
    statement: Statement
    bindings: [string]
    lists: [string]
    query: string
    registry: Registry
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

### `rows`

```
pub fn rows(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<[Row], DbError>
```

The rows a query answers, for a projection -- a few fields, an aggregate, a count. The caller
reads them with `db.row_get` and the `sql_as_*` converters, as it would any other result.

### `count`

```
pub fn count(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>
```

The one number a counting query answers: one row of one column, read as an integer.

`SELECT count(*) AS n FROM Visit v WHERE v.patient_id = :id` is the commonest projection there
is, and reading it by hand is four lines of row and converter every time. A query that answers
more than one row, or a row of more than one column, is an error naming what it answered --
because a count that quietly read the first of several numbers would be worse than no helper.
