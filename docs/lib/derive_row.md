# `derive_row`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Row)]`, written in Ziman: `<name>_from_row` and `<name>_to_params`, which map a
struct to and from one SQL result row.

**The second generator ported out of `src/derive.c`, and the one that answers whether
templates pay.** `Ord`'s two halves were a wash and a win; this one is bigger, nests, and has
to handle `Option`. What it found is in docs/design/templates.md §14, and the short version is
in this file: **the two function bodies are templates and the per-field conversions are not**,
because a conversion is recursive and a template can only show a fixed shape.

Column names are the struct's own field names, as written -- already snake_case, which is also
how SQL columns are conventionally named, so unlike Json's camelCase keys there is no transform
here.

`use derive_row` -- or `use derive_row.Name` to bring one item in bare.

### `emit_to_sql`

```
pub fn emit_to_sql(t: meta.Type, src: string, dst: string, n: Mutable<int>) -> Result<string, string>
```

The same the other way: a value to a `SqlValue`. An absent `Option` is `SqlValue.Null`, which
is why this one opens with a `var` rather than returning the inner conversion directly.

**`pub`, and the only part of this file that is** (2026-10-29): `derive_repository` converts a
repository's key columns and a derived query's bound parameters, and those have to reach SQL by
exactly the route the entity's own `to_params` writes them. The C shared one `emit_to_sql` for
that reason; two copies of this rule could disagree, and a key bound differently from the
column it addresses finds no row and says nothing. If a third generator ever needs it, the
conversion layer below is what moves into a `derive_sql` of its own.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```

Both functions, in the order `src/derive.c` splices them.
