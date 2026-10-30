# `#[column(name: "..")]`: a field stored in a column of another name

Status: **built, 2026-10-29.** Both halves shipped: the attribute in `meta` (F245) and the SQL
behaviour through `eql`'s field-to-column pairing (F244), with the North Star's 12,542 generated
lines byte-identical across the change. `required:` was asked for and deliberately left out; §4 says why. The compiler stopped reading the attribute at all in F249 --
`Param.column_name` turned out to have been write-only.

Asked for on 2026-10-29: *"let add a new annotation. `[column(name: "start_date"), required: "true"]`
— both name and required are optional — can be placed on entity fields. optional. used for default
override."*

Shipped: `name:`. `required:` was left out, with a reason (§4). Everything below is the reasoning
the implementation rests on; the attribute itself is one line in `docs/syntax/`.

## 1. What it is for

A schema you do not control, or one whose spelling you would not choose:

```ziman
#[derive(Row, Entity("events"))]
struct Event {
    #[pk]
    id: int
    #[column(name: "start_date")]
    start: string
}
```

The struct reads the way the application thinks; the database keeps the name it has.

## 2. The invariant this breaks, and why that is the whole design question

`eql.zm` said of its column list:

> The columns are `#[derive(Entity)]`'s `<name>_columns`, which is the struct's own field list, **so
> what a query may name and what `from_row` reads are the same list by construction.**

Field name = column name = what a query writes = what `row_get` reads. **Four** things rode on that
identity: `<name>_columns`, `from_row`, a query's `e.x`, and a derived query's *name*
(`find_by_start` reads its terms against field names). An override makes one list into two, so the
only real question was where the mapping lives. Three answers were put side by side:

| | a query names | cost |
| --- | --- | --- |
| **A** columns only | `e.start_date` | nothing in `eql`; the rename leaks into every query and every derived-query name |
| **B** `eql` learns the pairing | `e.start` | a field on `eql.Entity`, and `translate` maps a path before emitting it |
| **C** don't add it | — | rename the Ziman field; a schema you don't control then dictates your types |

**B was chosen** (by the user, on that comparison). It is JPA's model and it keeps the identity true
at the level a person works at: you name the struct's field everywhere, and the column name appears
in the attribute and in the emitted SQL and nowhere else.

What B bought, concretely: **`derive_repository` needed no change at all.** It writes `e.<field>`
and `:<field>` exactly as before and `eql` does the rest, so the nine CRUD functions, the derived
queries and their names were untouched by this feature.

## 3. Where each piece ended up

* **The parser** records `column_name` on `Param`, beside `is_pk` -- two plain fields, because the
  AST is the compiler's business.
* **`meta`** models attributes *generally*: `attrs: [Attr]`, `Attr { name, args: [AttrArg] }`. That
  settles the decision `meta.md` §13 deferred -- "an attribute that takes an argument ...  is not
  modelled yet, and will be when one exists rather than guessed at now", naming `#[column("name")]`
  as the obvious next one. `has_attr` reads the same as it always did.
* **`meta.attr_value(f, attr, arg)`** is generic; a `column_of(f)` belongs to whatever generates
  SQL, since `meta` models a declaration and knows nothing about columns. Both SQL generators
  write that one line themselves rather than one exporting it to the other.
* **`eql.Entity`** gains `fields: [string]`, parallel to `columns`. `entity`/`keyed_entity` keep
  their old arities meaning "field == column", so every existing call -- and every entity without
  an override -- is unchanged.
* **`column_for(e, field)`** replaces `check_field`: it matches on `fields` and answers from
  `columns`, so a path is mapped in the one place a path is resolved.
* **`keys` are column names.** `eql` puts them straight into SQL and `update_one` compares them
  against `columns`, so a `#[pk]` field with an override is keyed on its column.
* **`derive_entity`** emits the pairing **only when some field overrides**, so the generated text
  for every entity that does not is byte-identical -- verified across the North Star's 12,542
  generated lines.
* **`derive_row`** reads the row by column and names its local for the field:
  `val field_start_raw = try row_get(row, "start_date")`.

## 4. Why `required:` was left out

Nullability already comes from the type: `Option<T>` *is* the nullable column, `from_row` reads
NULL into `None`, and a non-Option field refuses NULL. There is no second source of truth for
required-ness and nothing that reads one -- no DDL is generated, and the validation use that might
want it has no home yet (`expressions.md` §17's open decision is literally whether rules live in a
properties file, a struct attribute, or a database row; `required:` would *be* the struct-attribute
answer, so it should follow that decision rather than pre-empt it).

`meta.zm`'s own rule decides it: *a field nobody reads is a promise about the compiler's internals
that cannot be taken back.* `required: true` on an `Option<T>` field would also put the type and
the attribute in disagreement about one column, with the type losing silently to a reader.

When something reads it, it arrives -- and the attribute's arguments are **named** precisely so
that adding a second one costs nothing. A positional `#[column("start_date")]` would have read
fine today and stopped reading the moment `required:` existed.

## 5. What is refused

Five shapes, each by name rather than ignored -- a silently-ignored argument would be an override
that does not happen:

```
#[column]                        ( takes named arguments )
#[column("x")]                   ( positional )
#[column(nmae: "x")]             ( unknown argument )
#[column(name: "")]              ( names no column )
#[column(name: "a", name: "b")]  ( two name arguments )
```

And at the query layer: naming the *column* where a field belongs is refused with the field list --
*"entity Event has no field 'start_date' (it has: id, start)"*.

## 6. What is not covered

* **A column name is not validated as an identifier.** It reaches SQL as written, and `eql` quotes
  nothing, so a name needing quoting is not supported. Nothing checks for it either; the first
  schema that needs `"order"` will say so.
* **No `#[table]` on a struct.** The table is `#[derive(Entity("t"))]`'s argument and stays there.
* **Nothing maps a column back for `--emit-derived` readers.** The generated text shows the column,
  which is the point, but a reader of a long generated file sees `start_date` without being told
  which field it belongs to.
