# `derive_ord`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Ord)]`, written in Ziman.

**The first generator to move out of `src/derive.c`**, and the port is meant to be read against
the C it replaced: same input, same output text, byte for byte. A generator is a function from
a declaration to source, and `meta.Decl` is what makes that expressible here -- so this file is
ordinary Ziman that `--fmt`, `--lint` and `ziman test` can all see, where the C was outside
every one of them. See docs/ROADMAP.md's "Derives should be written in Ziman, not in C".

**The bodies are templates**, so the shape of what this writes is the shape of what you read.
The two it needs were measured against the `push`/`concat` version first
(docs/design/templates.md §12): the struct half is close to a wash and the enum half is where
it pays, its control flow having been eleven string fragments. Neither needs an `@if` -- every
decision is made here, in Ziman, where the type checker reads it.

`use derive_ord` -- or `use derive_ord.Name` to bring one item in bare.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```

A struct compares field by field in declaration order, the first difference deciding; an enum
compares by variant position first and then by payload when both are the same variant. Each
member goes through the `compare` builtin, so a member whose own type is not `Ord` is reported
by the type checker against the generated text -- which is the same place the C version put it.

The `Result` is the shape a generator owes the compiler: a refusal it can report at the
declaration. This one cannot fail on any input `meta` can build, so what the `Result` carries
today is a mistake in the templates above, caught by `tests/lang/derive_ord_in_ziman`.
