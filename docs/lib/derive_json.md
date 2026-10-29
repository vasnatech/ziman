# `derive_json`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Json)]`, written in Ziman: `<name>_to_json`, `<name>_from_json` and the accumulating
`<name>_from_json_all`.

**The largest of the five and the last ported** -- 418 lines of C against `Row`'s 128 -- and
the only one that reaches arrays, maps, sets, generics and type variables. Structs and enums
are both here, so with this module `src/derive.c` no longer generates any JSON at all.

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

## The enum halves

### `enum_to_json`

```
pub fn enum_to_json(d: meta.Decl) -> Result<string, string>
```

`<name>_to_json` for an enum: a chain of variant tests, each returning its own object.

The encoder tests variants and the decoder compares tags, so only the encoder has a shape for
"the last one" -- the decoder always needs its `else` for a tag that matches nothing.

### `enum_from_json`

```
pub fn enum_from_json(d: meta.Decl) -> Result<string, string>
```

`<name>_from_json` for an enum: read the tag, then build the variant it names.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```

Three functions for a struct, two for an enum -- the order `src/derive.c` spliced them in.

### `to_json`

```
pub fn to_json(d: meta.Decl) -> Result<string, string>
```

`<name>_to_json` for a struct. The five pieces are exported individually as well as through
`generate`, so `tests/lang/derive_json_in_ziman` can hold each to the bytes the C emitted.
