# `derive_properties`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Properties)]`, written in Ziman: `<name>_from_properties` and `<name>_keys`, which
bind a struct to the keys under a prefix in `lib/core/properties.zm`'s `Properties`.

Field `idle_timeout_ms` reads key `<prefix>.idle-timeout-ms`: the name is kebab-cased, which is
the one transform in this generator and the reason `snake_to_kebab` exists. An `Option<T>`
field is an optional key; every other field is required, and a missing one is the `Result`'s
error.

**The third generator ported, and the one §15 named as the test of `@choose`** -- its two
field shapes are a genuine either/or with no recursion under them, which is exactly the case
the removed `@if` was designed for. `@choose` serves it and reads better: `@when required` and
`@when optional` name the two shapes where `@if optional` only said *whether*.

`use derive_properties` -- or `use derive_properties.Name` to bring one item in bare.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```
