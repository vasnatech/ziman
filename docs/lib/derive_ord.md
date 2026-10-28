# `derive_ord`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Ord)]`, written in Ziman.

**The first generator to move out of `src/derive.c`**, and the port is meant to be read against
the C it replaced: same input, same output text, byte for byte. A generator is a function from
a declaration to source, and `meta.Decl` is what makes that expressible here -- so this file is
ordinary Ziman that `--fmt`, `--lint` and `ziman test` can all see, where the C was outside
every one of them. See docs/ROADMAP.md's "Derives should be written in Ziman, not in C".

`use derive_ord` -- or `use derive_ord.Name` to bring one item in bare.

### `derive_ord`

```
pub fn derive_ord(d: meta.Decl) -> string
```

A struct compares field by field in declaration order, the first difference deciding; an enum
compares by variant position first and then by payload when both are the same variant. Each
member goes through the `compare` builtin, so a member whose own type is not `Ord` is reported
by the type checker against the generated text -- which is the same place the C version put it.
