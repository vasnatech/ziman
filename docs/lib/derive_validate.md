# `derive_validate`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Validate)]`, written in Ziman.

**The generator writes calls and nothing else.** Every rule, and every word of every message,
is in `lib/core/validate.zm`, where a person can call it and a fixture can test it without a
generator in the way. What is here is the one thing a hand-written check cannot be: the
*mapping* from a field's attributes to the calls, in declaration order, with the field's wire
name attached. See docs/design/validation.md.

**One violation per field.** The response is a map keyed by field name, so a second message on
one field would be lost; a field with several constraints goes through `validate.first`, which
reports the first rule that failed. §4 of the design argues this from the wire shape.

**The type decides whether absence is allowed**, not the attribute: `#[not_blank]` on a
`string` requires text, and on an `Option<string>` requires text only when the value is there.
That is Jakarta's semantics, and it is why one attribute replaces the `not_one_of` /
`optional_not_one_of` / `list_not_one_of` trio the North Star wrote by hand.

`use derive_validate` -- or `use derive_validate.Name` to bring one item in bare.

### `attributes`

```
pub fn attributes() -> [meta.AttrSpec]
```

**The nine attributes this generator owns**, which is where their names live: `src/parser.c`
held a table of them until 2026-10-30, so adding a constraint meant editing the compiler. The
compiler now asks every loaded generator for this list and refuses a name no generator claims
(docs/design/user-generators.md §5a).

`message:` on every constraint is Jakarta's `message` attribute: the rule still decides whether
the field is in violation and the author decides what the reader is told. `valid` has none --
it answers violations about the *nested* struct's fields rather than about this one.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```
