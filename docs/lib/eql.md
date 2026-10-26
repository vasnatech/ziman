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
}
```

A named parameter's value.

### `bind`

```
pub fn bind(name: string, value: SqlValue) -> Binding
```

### `Translated`

```
pub struct Translated {
    sql: string
    statement: Statement
    bindings: [string]
}
```

What a query became: the SQL to send, the tree it parsed into, and the parameter names in the
order the SQL numbers them.

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
