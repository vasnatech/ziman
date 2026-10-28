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

### `MetaType`

```
pub struct MetaType {
    // `int`, `float`, `bool`, `string`, `decimal`, `bigint`, `byte`, `unit`, `array`, `struct`,
    // `enum`, `map`, `set`, `channel`, `func`. The spelling the compiler's own `TypeKind` uses,
    // lowercased and without its prefix.
    kind: string
    // `struct` and `enum` only: the type's bare name, with no module qualifier. Empty otherwise.
    name: string
    // The type arguments, in order: one for `Option<T>` and `[T]`, two for `Map<K, V>`, none for
    // a scalar.
    args: [MetaType]
}
```

A member's type, as much of it as a generator needs to write a conversion for one.

**It was a bare `type_name: string` until `derive_row` needed it**, and that is the argument
for keeping this file small rather than guessing: a string was enough for nothing, because the
first generator to read a type at all switches on its kind, asks whether it is an `Option` and
then needs the element. A field nobody reads is a promise about the compiler's internals that
cannot be taken back -- and, as here, is usually the wrong shape anyway.

### `Member`

```
pub struct Member {
    name: string
    // For an enum variant, the payload's type; `unit` when it carries none.
    type: MetaType
    // Enum variants only: whether this one carries a payload. Always false for a struct's field.
    has_payload: bool
}
```

One member of a declaration: a struct's field, or an enum's variant.

### `scalar`

```
pub fn scalar(kind: string) -> MetaType
```

`int`, `string` -- a type with no name and no arguments of its own.

### `named`

```
pub fn named(kind: string, name: string) -> MetaType
```

`Uuid`, `Instant` -- a struct or enum with no type arguments.

### `generic`

```
pub fn generic(kind: string, name: string, args: [MetaType]) -> MetaType
```

`Option<string>`, `[int]` -- a type with arguments.

### `is_option`

```
pub fn is_option(t: MetaType) -> bool
```

Whether `t` is an `Option<T>`, which every generator that writes a conversion asks first: a
nullable column, an absent JSON key and an omitted property are all this one question.

### `type_text`

```
pub fn type_text(t: MetaType) -> string
```

The type as source spells it: `Option<string>`, `[Uuid]`, `int`.

### `snake_case`

```
pub fn snake_case(name: string) -> string
```

`LocalDate` -> `local_date`: the spelling generated function names use, so a generator writing
`<type>_to_sql` does not repeat the compiler's own conversion rule. A capital starts a new word
when it follows a lower-case letter or a digit, or when it begins the last capital of a run
(`HTTPServer` -> `http_server`).

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
