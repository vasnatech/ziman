# `meta`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A declaration, as a generator sees it.

**This is the one piece of the derive mechanism that stays in C**: `src/derive.c` builds a
`Decl` from a `StructDecl` or an `EnumDecl` and hands it to a generator written in Ziman, which
answers the source text to splice in. The conversion is data, not code generation -- which is
the whole point of the split, because everything on the other side of it can then be read by
`--fmt`, `--lint`, `ziman test` and a person.

**It is deliberately small.** What is here is what the generators ported so far actually read;
a field nobody reads is a promise about the compiler's internals that cannot be taken back. See
docs/ROADMAP.md's "Derives should be written in Ziman, not in C" for the shape of the rest.

`use meta` -- or `use meta.Name` to bring one item in bare.

### `Member`

```
pub struct Member {
    name: string
    // The member's type as the compiler renders it -- `int`, `[string]`, `Option<Uuid>`. For an
    // enum variant it is the payload's type, and is empty when there is no payload.
    type_name: string
    // Enum variants only: whether this one carries a payload. Always false for a struct's field.
    has_payload: bool
}
```

One member of a declaration: a struct's field, or an enum's variant.

### `Decl`

```
pub struct Decl {
    // The declaration's own name, as written: `LocalDate`.
    name: string
    // The same name in the spelling generated functions use: `local_date`, so a generator writes
    // `local_date_compare` without repeating the compiler's conversion rule.
    snake_name: string
    // `true` for an `enum`, `false` for a `struct`. Two kinds, so a bool rather than a tag -- a
    // third kind would be a reason to change this, and there is no third kind.
    is_enum: bool
    // The declaration's own type parameters, bare: `["T", "U"]` for `struct Box<T, U>`, empty for
    // a declaration that is not generic.
    type_params: [string]
    // A struct's fields in declaration order, or an enum's variants in declaration order.
    members: [Member]
}
```

A `struct` or an `enum` that carries a `#[derive(..)]`.

### `type_params_bounded`

```
pub fn type_params_bounded(d: Decl, bound: string) -> string
```

`<T: Ord, U: Ord>` for a generic declaration, or "" -- what a generated function's own
signature repeats, with whatever bound the generator needs.

### `type_text`

```
pub fn type_text(d: Decl) -> string
```

`Box<T, U>` for a generic declaration and `Point` for a plain one: the type as a parameter list
has to spell it.
