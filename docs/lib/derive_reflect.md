# `derive_reflect`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Reflect)]`: prints the declaration a generator was handed, and generates nothing.

**This is a test instrument, not a feature.** Every fixture that exercises a generator builds
its own `meta.Decl` by hand, which means it asserts what the *generator* does with a type and
can say nothing about what *reification* produced -- it is the thing standing in for
reification. So the C-to-Ziman boundary, the one piece of the derive mechanism still written
in C, has never had a test. Three bugs shipped through it in one week: a stripped module
qualifier, a map and a set arriving with no element types, and a `Decl` built where the
collector could not see it. None was caught by 1,293 fixtures and all three were found by
hand, late.

This generator closes that. Write `#[derive(Reflect)]` on a real declaration, run
`ziman --emit-derived`, and the output is exactly what `src/generators.c` built -- pinned by
`tests/lang/derive_reflect`. See docs/design/meta.md §10.

**It emits only comments**, so a declaration that derives it is unchanged. That is deliberate:
the instrument must not be able to break the program it is measuring.

`use derive_reflect` -- or `use derive_reflect.Name` to bring one item in bare.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```
