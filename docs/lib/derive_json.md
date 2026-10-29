# `derive_json`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Json)]`, written in Ziman: `<name>_to_json`, `<name>_from_json` and the accumulating
`<name>_from_json_all`.

**The largest of the five and the last ported** -- 418 lines of C against `Row`'s 128 -- and
the only one that reaches arrays, maps, sets, generics and type variables. All three struct
functions are here; an enum still falls through to `src/derive.c`, which is what
`ziman_generate_in_ziman` answering NULL means.

Keys are camelCase (`middle_name` -> `middleName`), which is the one transform here; `Row`'s
column names are the field names as written, and the difference is deliberate -- JSON is read
by other systems and a SQL column is not.

`use derive_json` -- or `use derive_json.Name` to bring one item in bare.

## The decoder

### `from_json`

```
pub fn from_json(d: meta.Decl) -> Result<string, string>
```

`<name>_from_json`: stops at the first bad field.

### `from_json_all`

```
pub fn from_json_all(d: meta.Decl) -> Result<string, string>
```

`<name>_from_json_all`: reports every bad field, as `Result<T, [string]>`.

Each field decodes inside a **lambda**, which is what makes this cheap to generate: the
emitters above end a failing field with `return err(..)`, and inside a lambda that returns
from the lambda -- so `field_read` is reused exactly, with no second recording mode to keep in
step with it. The cost is one closure per field, paid only by callers of this function.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```

**This port is not finished, and refuses rather than answering half of it.**

`#[derive(Json)]` writes three functions for a struct -- all three are above -- and two for an
enum, which are not. A `generate` that answered a struct and failed an enum would wire up
cleanly and then break on the first `#[derive(Json)] enum`, so it refuses by name for both
instead, and `src/generators.c` does not list this module: `#[derive(Json)]` still goes to
`src/derive.c`.

The refusal is what makes wiring this up before the enum halves exist fail loudly.

### `to_json`

```
pub fn to_json(d: meta.Decl) -> Result<string, string>
```

`<name>_to_json`, exported -- as `from_json` and `from_json_all` are -- so
`tests/lang/derive_json_in_ziman` can hold each to the bytes `src/derive.c` emits while
`generate` still refuses.
