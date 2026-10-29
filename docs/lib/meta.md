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
a field nobody reads is a promise about the compiler's internals that cannot be taken back.

**A redesign is written and not built: docs/design/meta.md.** It sharpens the rule above rather
than repealing it -- *total over what the compiler can hand a generator, silent about
everything else* -- and the central change is that `MetaType` becomes a **tagged enum**. A
`switch` over a Ziman enum is exhaustive with no default arm, so a kind nobody handles becomes
a compile error instead of the silent flattening that shipped three times (`Result`, `Channel`
and `fn` are still flat here today). `Decl` follows: `is_enum` and `has_payload` both describe
half their values, and `has_payload`'s own comment below says so. Its §10 is the other half:
every fixture builds a `MetaType` by hand, so the boundary this file *is* has never been
tested at all.

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

**The kind is not consulted, deliberately.** The compiler represents `Option<T>` as a struct
in some positions and an enum in others -- `is_option_type_here` in `src/derive.c` tests both
for that reason -- so a generator that asked for `enum` would be right about hand-built
declarations and wrong about the ones the compiler passes it. That gap was invisible until
the bridge ran a generator for real: every fixture here builds `Option` as an enum and
passed, while `#[derive(Row)]` on an `Option<string>` field generated `option_from_sql`.
Name and arity are what the question is actually about.

### `type_text`

```
pub fn type_text(t: MetaType) -> string
```

The type as source spells it: `Option<string>`, `[Uuid]`, `int`.

### `camel_case`

```
pub fn camel_case(name: string) -> string
```

`middle_name` -> `middleName`: the spelling JSON keys use, so a generator writing one does not
repeat the compiler's own conversion rule. An underscore is dropped and capitalises the letter
after it; nothing else changes.

### `fresh`

```
pub fn fresh(n: Mutable<int>, hint: string) -> string
```

A fresh `__derive_<hint>_<n>`, from a counter a whole generation shares. The `__derive_`
prefix is the convention `src/derive.c` writes too, so a generated temporary is recognisable
as one wherever it turns up.

**A cell, not a value threaded through every call.** The first generators returned the next
counter alongside every piece of text, so each emitter answered a pair, every caller unpacked
one, and the offsets -- `tmp + 3` here, `tmp + 5` there -- were written out by hand. That was
the one part of those files that had to be exactly right and could not be read off the page.
The numbering depends on call order either way; a cell just stops restating it, and the order
is now the order the `fresh` calls appear in. `src/derive.c` threads an `int *` for the same
reason.

### `bare_name`

```
pub fn bare_name(name: string) -> string
```

`time.Instant` -> `Instant`: a type name with its module qualifier dropped.

**For looking a name up, never for writing one out.** A field's type reaches a generator as
the source spells it, qualifier and all, because that is the text that resolves when it is
written back into the user's file. But a table of bundled types -- the short-converter lists
in `derive_row` and `derive_json` -- is keyed by the bare name, and `time.Instant` must find
`Instant` in it. Writing `bare_name(..)` into generated code instead of looking up with it is
the bug this exists to make unlikely.

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

### `member`

```
pub fn member(name: string, type: MetaType, has_payload: bool) -> Member
```

One member, built by the compiler. **A constructor rather than a struct literal**, so that the
C that reifies a declaration never knows this file's field order or field names: adding a
field here changes this function and nothing in `src/`. See docs/design/user-generators.md.

### `decl`

```
pub fn decl(name: string, snake_name: string, is_enum: bool, type_params: [string], members: [Member]) -> Decl
```

A declaration, built by the compiler, for the same reason.

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
