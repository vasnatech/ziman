# `derive_json`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Json)]`, written in Ziman: `<name>_to_json`, `<name>_from_json` and the accumulating
`<name>_from_json_all`.

**The largest of the five and the last ported** -- 418 lines of C against `Row`'s 128 -- and
the only one that reaches arrays, maps, sets, generics and type variables. Struct declarations
are handled here; an enum still falls through to `src/derive.c`, which is what
`ziman_generate_in_ziman` answering NULL means.

Keys are camelCase (`middle_name` -> `middleName`), which is the one transform here; `Row`'s
column names are the field names as written, and the difference is deliberate -- JSON is read
by other systems and a SQL column is not.

`use derive_json` -- or `use derive_json.Name` to bring one item in bare.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```

**This port is not finished, and refuses rather than answering half of it.**

`#[derive(Json)]` writes three functions for a struct -- `_to_json`, `_from_json` and the
accumulating `_from_json_all` -- and two for an enum. Only `to_json` is here. A `generate`
that answered it alone would look like success and produce a module missing its decoder, so
it refuses by name instead, and `src/generators.c` does not list this module: `#[derive(Json)]`
still goes to `src/derive.c`.

Wiring it up means finishing `from_json` first. The refusal is what makes doing that in the
wrong order fail loudly.

### `to_json`

```
pub fn to_json(d: meta.Decl) -> Result<string, string>
```

The half that is written, exported so `tests/lang/derive_json_in_ziman` can hold it to the
bytes `src/derive.c` emits.
