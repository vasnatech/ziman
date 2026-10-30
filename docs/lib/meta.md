# `meta`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A declaration, as a generator sees it.

**This is the one piece of the derive mechanism that stays in C**: `src/derive.c` builds a
`Decl` from a `StructDecl` or an `EnumDecl` and hands it to a generator written in Ziman, which
answers the source text to splice in. The conversion is data, not code generation -- which is
the whole point of the split, because everything on the other side of it can then be read by
`--fmt`, `--lint`, `ziman test` and a person.

**The rule is totality, not smallness.** An earlier version of this file said it was
deliberately small, because a field nobody reads is a promise about the compiler's internals
that cannot be taken back. That is still true of *data*. It was the wrong rule for *shape*:
the type model was a `kind` string beside a flat argument list, faithful for five of the
compiler's nineteen type kinds and silently flat for the rest, and three bugs shipped through
the gap in one week. A `switch` over a Ziman enum is exhaustive and has no default arm, so a
kind nobody handles is now a compile error in every generator rather than a wrong answer at
run time. See docs/design/meta.md; `tests/lang/meta_boundary` is what this file's output
actually looks like.

`use meta` -- or `use meta.Name` to bring one item in bare.

### `Type`

```
pub enum Type {
    Int
    Float
    Bool
    String
    Bigint
    Decimal
    Byte
    // `unit` is the type with one value; `void` is a function that declares no return type at
    // all. Two variants because they are two things, even though most generators treat them
    // alike.
    Unit
    Void
    Array(Type)
    // **`SetOf`/`MapOf`/`ResultOf`/`ChannelOf` carry the suffix because `Set`, `Map`, `Result`
    // and `Channel` are builtin type names and the language will not take one as a variant
    // name.** The four that collide are the four that are renamed; `Array`, `Func`, `Optional`
    // and `Named` do not, and are left alone rather than renamed for symmetry.
    SetOf(Type)
    ChannelOf(Type)
    MapOf(MapType)
    ResultOf(ResultType)
    Func(FuncType)
    // `Option<T>`. **A variant of its own, which the compiler has no equivalent of**: it
    // represents an `Option` as a struct in some positions and an enum in others, so the old
    // model's `is_option` had to test name and arity and deliberately ignore kind. `meta` is an
    // interface rather than a mirror and may normalise; every generator asks this question
    // first, and now it is a pattern rather than a guess.
    Optional(Type)
    // A user-declared struct or enum. Which of the two it is, is not here: no generator reads
    // the distinction for a type it merely *refers* to.
    Named(NamedType)
    // `T` in `struct Box<T>` -- a type parameter in the body of the declaration that binds it.
    Var(string)
}
```

A type, as much of it as a generator needs to write a conversion for one.

**Every kind the compiler can hand a generator is a variant, and each carries its own parts
under their own names.** `Map`'s are `key`/`value` and `Result`'s are `ok`/`err`, where the
flat list they replaced made both `args[1]` and left a reader to know which was which.

### `MapType`

```
pub struct MapType {
    key: Type
    value: Type
}
```

### `ResultType`

```
pub struct ResultType {
    ok: Type
    err: Type
}
```

### `FuncType`

```
pub struct FuncType {
    params: [Type]
    returns: Type
}
```

### `NamedType`

```
pub struct NamedType {
    name: string
    args: [Type]
}
```

**`name` is the name the source wrote**, module qualifier and all: `time.Instant`, not
`Instant`. That is what resolves when a generator writes it back into the user's file, and
stripping it was one of the three bugs. A generator that wants the bare name for a *lookup* --
a table of bundled types keyed by `Instant` -- calls `bare_name`.

## What the compiler calls to build one

### `int_type`

```
pub fn int_type() -> Type { return Type.Int }
```

### `float_type`

```
pub fn float_type() -> Type { return Type.Float }
```

### `bool_type`

```
pub fn bool_type() -> Type { return Type.Bool }
```

### `string_type`

```
pub fn string_type() -> Type { return Type.String }
```

### `bigint_type`

```
pub fn bigint_type() -> Type { return Type.Bigint }
```

### `decimal_type`

```
pub fn decimal_type() -> Type { return Type.Decimal }
```

### `byte_type`

```
pub fn byte_type() -> Type { return Type.Byte }
```

### `unit_type`

```
pub fn unit_type() -> Type { return Type.Unit }
```

### `void_type`

```
pub fn void_type() -> Type { return Type.Void }
```

### `array_type`

```
pub fn array_type(elem: Type) -> Type { return Type.Array(elem) }
```

### `set_type`

```
pub fn set_type(elem: Type) -> Type { return Type.SetOf(elem) }
```

### `channel_type`

```
pub fn channel_type(elem: Type) -> Type { return Type.ChannelOf(elem) }
```

### `optional_type`

```
pub fn optional_type(elem: Type) -> Type { return Type.Optional(elem) }
```

### `var_type`

```
pub fn var_type(name: string) -> Type { return Type.Var(name) }
```

### `map_type`

```
pub fn map_type(key: Type, value: Type) -> Type
```

### `result_type`

```
pub fn result_type(ok: Type, err: Type) -> Type
```

### `func_type`

```
pub fn func_type(params: [Type], returns: Type) -> Type
```

### `no_label`

```
pub fn no_label() -> Option<string> { return Option<string>.None }
```

The derive's argument, or its absence. Two constructors because the C cannot build an
`Option` without knowing its layout, which is the thing this boundary exists to avoid.
`Target.label`'s two cases. These are **not** derive arguments -- a label is one optional string
a module attribute carries, which is exactly what an `Option` is -- so they stayed when
`Decl.arg` became a list (§5b).

### `some_label`

```
pub fn some_label(value: string) -> Option<string> { return Option<string>.Some(value) }
```

### `no_args`

```
pub fn no_args() -> [AttrArg]
```

No arguments, for a target written bare -- `meta.no_args()` rather than an empty literal, since
`[]` cannot say what it holds.

### `decl_arg`

```
pub fn decl_arg(d: Decl) -> Option<string>
```

The one unnamed argument a target was written with, as `#[derive(Entity("medications"))]` has
it: the argument called `value`, which is where `parse_attr_args` puts a lone value.

```
pub fn decl_arg(d: Decl, name: string) -> Option<string>
```

A named argument of the derive: `#[derive(Endpoint(method: POST))]` answers `Some("POST")` for
`"method"`.

### `decl_attr_arg`

```
pub fn decl_attr_arg(d: Decl, name: string) -> Option<AttrArg>
```

The whole argument rather than its text, for a generator that cares **how it was written**: the
lexer takes the quotes off a string, so `Entity("patients")` and `Entity(patients)` arrive with
the same value and only `kind` tells them apart. The parser used to make that distinction for
`Entity` alone; a target's arguments have no spec of their own yet, so the generator that cares
is the one that checks (user-generators.md §5b).

### `named_type`

```
pub fn named_type(name: string, args: [Type]) -> Type
```

## The declaration

### `AttrArg`

```
pub struct AttrArg {
    name: string
    value: string
    // How it was written: `"string"`, `"int"` or `"name"`.
    //
    // **Added after the design said it was unnecessary** (docs/design/validation.md §5f), which
    // claimed the value's text was enough because "the compiler hands text over and the generator
    // decides what it means". True of a generator -- and false of anything that has to *show* an
    // attribute, because the lexer takes the quotes off a string, so `name: "start_date"` and
    // `values: type_words` arrive identical. `attr_text` printed one of them wrongly whichever way
    // it guessed, and the generator that exists to show exactly what it was handed
    // (`derive_reflect`) is the wrong place for a cosmetic lie.
    kind: string
}
```

An argument written on an attribute: `#[column(name: "start_date")]` has one, whose `name` is
"start_date".

**The value is a `string` because every argument an attribute takes is written as one.** A
future `#[column(required: true)]` would arrive here as `"true"`, and the generator that reads
it decides what that means -- which is the same division `Decl.arg` already uses for
`#[derive(Entity("medications"))]`.

### `Attr`

```
pub struct Attr {
    name: string
    args: [AttrArg]
}
```

An attribute written on a field, with its arguments: `#[pk]` has none,
`#[column(name: "start_date")]` has one.

### `Field`

```
pub struct Field {
    name: string
    type: Type
    // The attributes written on this field, in source order.
    //
    // **A list of attributes, not a `pk: bool` and a `column: Option<string>`.** `#[pk]` was the
    // only field attribute when this was `[string]`, and that version said an attribute with an
    // argument "is not modelled yet, and will be when one exists rather than guessed at now".
    // One exists: `#[column(name: "start_date")]` (2026-10-29), so the names became
    // `Attr`s with arguments -- the shape the §13 note deferred, settled by a real reader rather
    // than guessed. `has_attr` reads the same as it always did, so a generator asking only
    // whether `#[pk]` is there did not change.
    attrs: [Attr]
}
```

A struct's field.

### `Variant`

```
pub struct Variant {
    name: string
    payload: Option<Type>
}
```

An enum's variant. **`payload` is an `Option<Type>`**, where the old model carried a
`has_payload: bool` beside a `type` that was `unit` when there was none -- two spellings of
absence, and a bool that was always false for a struct's field because a struct and an enum
shared one `Member` type.

### `Body`

```
pub enum Body {
    Struct([Field])
    Enum([Variant])
}
```

What a declaration declares. A struct and an enum are different shapes, so a generator that
handles one writes one arm and is refused the other by the type checker rather than by a
runtime check it had to remember to write.

### `Decl`

```
pub struct Decl {
    // The arguments the derive was written with, in source order.
    //
    // **On the declaration rather than a second parameter to `generate`.** Four generators
    // already take one parameter and exactly one target takes an argument; an argument *about*
    // the declaration belongs on it rather than beside it.
    //
    // **A list of named arguments, not one string** (2026-10-30, user-generators.md §5b). It was
    // `arg: Option<string>`, because `Entity` was the only target that took anything and took one
    // bare table name -- which meant `#[derive(Endpoint(method: POST, path: "/orders"))]` could
    // not be written at all. A target now goes through the same reader a field attribute does, so
    // `#[derive(Entity("medications"))]` is one argument named `value` (`#[min(8)]`'s convention)
    // and `decl_arg(d)` is how a generator asks for it.
    args: [AttrArg]
    // The declaration's own name, as written: `LocalDate`.
    name: string
    // The same name in the spelling generated functions use: `local_date`, so a generator writes
    // `local_date_compare` without repeating the compiler's conversion rule.
    snake_name: string
    // The declaration's own type parameters, bare: `["T", "U"]` for `struct Box<T, U>`, empty
    // for one that is not generic. A bound (`<T: Hash>`) is not here -- no generator reads one.
    type_params: [string]
    body: Body
}
```

### `field`

```
pub fn field(name: string, type: Type, attrs: [Attr]) -> Field
```

### `attr`

```
pub fn attr(name: string) -> Attr
```

### `attr_with`

```
pub fn attr_with(name: string, args: [AttrArg]) -> Attr
```

### `AttrSpec`

```
pub struct AttrSpec {
    name: string
    args: string
}
```

One field attribute a generator declares: its name and the grammar of its arguments.

**The compiler holds no list of field attributes.** It parses `#[name(a: 1, b: "x")]` into a
generic list and then asks every loaded generator which names exist, by calling its
`attributes()`. So `#[not_blank]` is `derive_validate`'s business and `#[pk]` is
`derive_entity`'s, and neither is a name `src/` knows -- which is what lets a project's own
generator have attributes of its own (docs/design/user-generators.md §5a).

`args` is the spec: `name:kind` entries separated by commas, `?` for one that may be left out,
and an empty string for an attribute that takes none. The kinds are `string`, `int` and `name`
(an identifier the *generated code* will emit, as `#[one_of(values: type_words)]` does).

### `attr_spec`

```
pub fn attr_spec(name: string, args: string) -> AttrSpec
```

### `attr_arg`

```
pub fn attr_arg(name: string, value: string, kind: string) -> AttrArg
```

### `has_attr`

```
pub fn has_attr(f: Field, name: string) -> bool
```

Whether `f` carries `#[<name>]`, whatever arguments it was written with.

### `attr_value`

```
pub fn attr_value(f: Field, attr: string, arg: string) -> Option<string>
```

One argument of one attribute: `attr_value(f, "column", "name")` is the `"start_date"` of
`#[column(name: "start_date")]`, and `None` when the attribute or the argument is absent.

**Generic on purpose.** A `column_of(f)` belongs to whatever generates SQL, not here: `meta` is
the model of a declaration and knows nothing about columns. A caller writes
`get_or(meta.attr_value(f, "column", "name"), f.name)` and the SQL meaning stays in the SQL
generator.

### `attr_text`

```
pub fn attr_text(a: Attr) -> string
```

An attribute as source spells it: `pk`, `column(name: "start_date")`, `min(value: 1)`. For a
generator that prints what it was handed (`derive_reflect`), and for a refusal that quotes an
attribute back.

Quoted exactly as the source wrote it, from `AttrArg.kind` -- see that field for why inferring
it from the text does not work.

### `unit_variant`

```
pub fn unit_variant(name: string) -> Variant
```

### `payload_variant`

```
pub fn payload_variant(name: string, payload: Type) -> Variant
```

### `struct_decl`

```
pub fn struct_decl(
    name: string,
    snake_name: string,
    args: [AttrArg],
    type_params: [string],
    fields: [Field]
) -> Decl
```

### `enum_decl`

```
pub fn enum_decl(
    name: string,
    snake_name: string,
    args: [AttrArg],
    type_params: [string],
    variants: [Variant]
) -> Decl
```

## Reading one

### `type_text`

```
pub fn type_text(t: Type) -> string
```

The type as source spells it: `Option<string>`, `[Uuid]`, `Map<string, int>`, `int`.

## Names

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

## A module generator's input

### `Param`

```
pub struct Param {
    name: string
    type: Type
}
```

A function's parameter, as the author wrote it.

### `Function`

```
pub struct Function {
    name: string
    params: [Param]
    returns: Type
}
```

A signature without its body: what a generator checks a name against, and repeats when it
writes the body it was declared without.

### `Target`

```
pub struct Target {
    // The entity, reified from wherever it is declared: its fields are the columns, and the ones
    // carrying `#[pk]` are what addresses one row.
    entity: Decl
    // `#![repository(Medication, label: "Medication")]`'s label -- the entity as a person says
    // it, for a `NotFound` message that reaches an API response. `None` when none was given, and
    // then the entity's own name is the label.
    label: Option<string>
    // `#![repository(Notification, crud: false)]` makes this false: name the entity, generate
    // none of the nine CRUD functions. A module with only derived queries wants exactly that.
    crud: bool
    // The functions this module already defines, so a generator can decline to replace one.
    // **A hand-written function wins**, which is what makes the attribute safe to add to a
    // repository that has already been written.
    defined: [string]
}
```

One `#![repository(Entity)]`, as its generator sees it.

**Named for what it is rather than `Module`**, so that it cannot become the place every later
module attribute accretes: a second module-level feature gets its own type, or a reason is
written down for sharing this one.

**It carries only what the port reads.** The design (§8) also proposed the entity's `use` path
and the list of bodyless functions: the first is read by nothing -- generated text spells the
entity's bare name, and the import that puts it in scope is the compiler's own work -- and the
second is a parameter to `generate_query` instead, which is how a refusal keeps its position.
A grant this wide is hard to take back once a generator depends on it, so it starts narrow.

### `param`

```
pub fn param(name: string, type: Type) -> Param
```

### `function`

```
pub fn function(name: string, params: [Param], returns: Type) -> Function
```

### `target`

```
pub fn target(entity: Decl, label: Option<string>, crud: bool, defined: [string]) -> Target
```

### `label_of`

```
pub fn label_of(t: Target) -> string
```

The label a `NotFound` message carries: the attribute's, or the entity's own name.

### `fields_of`

```
pub fn fields_of(t: Target) -> [Field]
```

The entity's fields, or an empty list for an entity that is somehow an enum -- which
`src/derive.c` refuses before a generator runs, and a `switch` still has to answer.

### `keys_of`

```
pub fn keys_of(t: Target) -> [Field]
```

The fields carrying `#[pk]`, in declaration order: what addresses one row. More than one is a
compound key.

### `defines`

```
pub fn defines(t: Target, name: string) -> bool
```

Whether this module writes `name` itself.
