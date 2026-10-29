# `meta`: what a generator is allowed to know

Status: **design, not built.** Written because the derives-in-Ziman port finished on 2026-10-28
and the thing it left behind is not the generators -- those are done and proved -- but `meta`,
which was grown one field at a time by whichever generator needed it next and is now the only
part of the mechanism nobody designed.

**How to read this.** §1 is the evidence, which is three shipped bugs with one cause. §2 is the
principle the current file states and §3 is where that principle actually failed. §§4-8 are the
model. §9 is the testing change, which matters more than the model. §§10-12 are staging, cost and
open decisions.

## 1. The evidence: three bugs, one cause

Wiring `derive(Json)` to the bridge on 2026-10-28 found three bugs, all shipped in `ace87675`,
none caught by 1,293 fixtures:

1. **A field's module qualifier was stripped.** `Option<time.Instant>` generated
   `Option<Instant>`, which does not resolve. A type's *name* only reaches generated text through
   an `Option`, an array or a map, and no fixture had a qualified type inside one.
2. **A map's and a set's element types were dropped.** They fell through `reify_type`'s scalar
   tail, so `Map<string, int>` arrived as a bare `map` with no arguments and the generator
   indexed `args[0]` of an empty array.
3. **The reified `Decl` was built where the collector could not see it**, so a collection partway
   through freed a finished `Member`. This one is not about the model -- it is in §9 anyway,
   because the reason it survived is the same.

The cause they share is not carelessness about any one kind. It is that **a fixture builds its
`MetaType` by hand**:

```ziman
f("scores", meta.generic("map", "Map", [meta.scalar("string"), meta.scalar("int")]))
```

That line asserts what the generator does with a map. It cannot assert what `reify_type` *produces*
for a map, because it is the thing standing in for `reify_type`. Every fixture in the suite is
built this way, so the entire C-to-Ziman boundary -- the one piece of the derive mechanism that is
still C, by design -- **has no test at all**. Three bugs is what that costs, and the number is only
that low because `derive(Entity)` has not been ported yet.

## 2. What the file says about itself, and why it was right

`lib/core/meta.zm` opens with a rule:

> **It is deliberately small.** What is here is what the generators ported so far actually read; a
> field nobody reads is a promise about the compiler's internals that cannot be taken back.

That rule is good and this document does not repeal it. "Redesign `meta` to cover most Ziman
features" is a reasonable instruction that would, taken literally, produce exactly what the rule
warns against: a `Decl` carrying visibility, doc comments, attribute lists, line numbers and
bounds, most of it read by nothing, every field a commitment.

So the proposal below adds **what a named, concrete generator needs** and nothing else -- and the
named generator is `derive(Entity)` plus `#[repository]`, which is the last thing `src/derive.c`
owns and the only derive target not yet ported. §4 is that list, derived from reading the C.

**§5 then revises this rule rather than obeying it.** The rule is about *data*: a field nobody
reads is a speculative promise. It says nothing against a *shape* that is total over what the
compiler can actually produce -- and §5 makes the model an enum, where totality is enforced by
exhaustive `switch` and a missing kind is a compile error rather than a silent flattening. The
rule survives in a sharper form, stated at the end of §6: **`meta` is total over what the
compiler can hand a generator, and silent about everything else.**

## 3. Where "small" actually failed

The rule says *a field nobody reads is a promise*. The three bugs were not about extra fields.
They were about fields that **exist and lie**.

`MetaType` has `kind`, `name` and `args`. For 5 of the 19 `TypeKind`s those three are faithful.
For the rest, `args` is empty whether or not the type has parts, and until the fix on 2026-10-28
`name` was empty too. Today:

| kind                                  | parts in the compiler        | in `MetaType`                  |
| ------------------------------------- | ---------------------------- | ------------------------------ |
| `array`, `set`                        | `elem`                       | `args[0]` ✓                    |
| `map`                                 | `elem`, `elem_err`           | `args[0]`, `args[1]` ✓         |
| `struct`, `enum`                      | `type_args`                  | `args` ✓                       |
| `typevar`                             | `type_var_name`              | `name` ✓                       |
| **`result`**                          | `elem` (Ok), `elem_err`(Err) | **neither; text only**         |
| **`channel`**                         | `elem`                       | **neither; text only**         |
| **`func`**                            | `elem` (return), `type_args` | **neither; text only**         |
| scalars                               | --                           | ✓                              |

So `Result<T, E>`, `Channel<T>` and `fn(A) -> B` reach a generator as an opaque name. That is the
same shape as the map/set bug, still present, and it is why `derive(Json)` refuses a `Result`
field with "does not support field type" rather than handling it: the generator could not see
inside one if it wanted to.

**The failure was not that `meta` was small. It was that `MetaType` claimed to be a tree and was
one only sometimes.** A model that is faithful for 5 of 19 kinds and silently flat for the rest is
worse than one that is honestly partial, because nothing in the shape says which you have.

A wider struct would fix this table and not the cause: `kind: string` with `args: [MetaType]` can
always be given a kind it has no arguments for, and nothing complains. §5's enum cannot represent
that state at all, which is why the fix is the shape rather than the contents.

## 4. What `derive(Entity)` and `#[repository]` need that is not there

Read out of `src/derive.c` rather than imagined:

* **`#[pk]` on a field.** `is_pk` on `Param`, read in six places, used to build the key list, to
  refuse an `Option` key, and to generate `find_by_key`/`delete`. **A generator cannot see it at
  all today**, and no amount of type information substitutes.
* **The derive's argument.** `#[derive(Entity("medications"))]` -- `derive_args`, parallel to
  `derives`. The design for user generators already specified `generate(d, arg: Option<string>)`
  and the four ported generators were built with one parameter because none needed it. Entity is
  the one that does.
* **The module's repository attributes**: `repository_entity`, `repository_label`,
  `repository_no_crud` -- `#![repository(Entity, label: "Medication", crud: false)]`.
* **Function declarations.** A derived query is a bodyless `pub fn` whose *name* is the query and
  whose parameters bind by name. To generate it in Ziman, a generator needs the function's name,
  parameters (name and type, in order) and return type.
* **The names the module already defines**, because a repository that writes `find_by_id` by hand
  must not also have one generated (`module_defines_function` in the C).

Two things to notice about that list, because they decide the shape rather than just lengthening
it. **Function signatures are a new kind of thing**: `meta` describes types today and nothing in
it can hold a signature. And **the last three entries are not properties of the declaration being
derived at all** -- they belong to a *different module* from the entity struct. §8 is where that
leads.

## 5. The model: a `Type` is an enum, and that is the whole argument

Replace `MetaType { kind: string, name: string, args: [MetaType] }` with a tagged enum:

```ziman
pub enum Type {
    Int  Float  Bool  String  Bigint  Decimal  Byte  Unit  Void
    Array(Type)
    Set(Type)
    Channel(Type)
    Map(MapType)
    Result(ResultType)
    Func(FuncType)
    Optional(Type)
    Named(NamedType)
    Var(string)
}

pub struct MapType    { key: Type     value: Type }
pub struct ResultType { ok: Type      err: Type }
pub struct FuncType   { params: [Type]  returns: Type }
pub struct NamedType  { name: string  args: [Type] }
```

A variant carries one payload, so the three multi-part kinds take a small struct each, named for
their own parts -- `key`/`value` and `ok`/`err` rather than one shared `Pair` whose field names
would be wrong half the time. Recursion is direct: `Array(Type)` works, as does `switch` over it.

### A worked example, because three fields here look alike

`NamedType.args` is the one field whose meaning is easy to guess wrong. It is **the type
arguments a *use* supplies**, and the thing to hold on to is that a declaration and a reference
to one are different values with different fields.

The North Star has both next to each other (`model/paging.zm`):

```ziman
pub struct Page<T> {
    content: [T]
    page: int
}

pub enum Listing<T> {
    All([T])
    Paged(Page<T>)
}
```

`Page` reified contains **no `NamedType` at all** -- it is the thing being declared, not
referenced:

```ziman
Decl {
    name: "Page"
    snake_name: "page"
    type_params: ["T"]                 // the declaration BINDS T
    arg: Option<string>.None
    body: Body.Struct([
        Field { name: "content", type: Type.Array(Type.Var("T")), attrs: [] },
        Field { name: "page",    type: Type.Int,                  attrs: [] }
    ])
}
```

`Listing` references it, and that is where `args` appears:

```ziman
Decl {
    name: "Listing"
    type_params: ["T"]
    body: Body.Enum([
        Variant { name: "All",
                  payload: Some(Type.Array(Type.Var("T"))) },
        Variant { name: "Paged",
                  payload: Some(Type.Named(NamedType {
                      name: "Page"               // the type referred to
                      args: [Type.Var("T")]      // what it SUPPLIES for Page's own T
                  })) }
    ])
}
```

At a concrete use, `Listing<Patient>` is
`Named(NamedType { name: "Listing", args: [Named(NamedType { name: "Patient", args: [] })] })`.
**`args` is empty for every non-generic named type**, which is the common case: `Uuid` is
`Named(NamedType { name: "uuid.Uuid", args: [] })`.

| field                | holds                                   | in the example               |
| -------------------- | --------------------------------------- | ---------------------------- |
| `Decl.type_params`   | the names a declaration **binds**       | `["T"]`                      |
| `NamedType.args`     | the types a use **supplies**            | `[Var("T")]`, `[Named Patient]` |
| `Type.Var(name)`     | a bound parameter **referred to**       | `Var("T")` in `content: [T]` |

**What reads it.** `derive_json` emits, for a generic field, a call carrying one converter per
type argument:

```ziman
val field_page_json = page_to_json<Patient>(v.page, patient_to_json)
//                                 ^^^^^^^^^          ^^^^^^^^^^^^^^^
//                                 from args          one per args entry
```

That is the whole of its job: spell the `<..>` and supply a converter per element.

**And `Named` covers only user-declared structs and enums.** `[T]`, `Map<K, V>`, `Option<T>` and
`Result<T, E>` each have their own variant with named parts, so none of them is ever a `Named`
with positional `args`. Under today's `MetaType` they all share one `args` list and a reader has
to know by convention that `args[1]` is a map's *value* but a result's *error* -- which is the
§5 change in one sentence.

### Why this is better than a wider struct, and it is not "more fields"

**A `switch` over an enum is exhaustive and has no default arm.** That is the compiler's rule, not
a convention:

> this 'switch' over T is not exhaustive: missing Str (add an arm for each variant; there is no
> default arm on purpose, so a new variant is a compile error here)

So **adding a kind to `meta` becomes a compile error in every generator that does not handle it.**
Set against §1: the map/set bug was a kind falling through a scalar tail in the C and then through
an `if` ladder in the generator, visible to nothing. Under this model the C cannot produce a
`Type` without choosing a variant, and a generator cannot ignore one. The failure mode that
shipped three times stops being possible rather than becoming better tested.

That is a different argument from §2's, and it is why §2 does not block this. §2 is about *data*:
a field nobody reads is a promise about internals. An enum variant for a kind the compiler can
actually produce is not a speculative promise -- it is the truth about what can arrive, and
leaving it out is the lie. **Totality is cheap to maintain precisely because it is enforced.**

### Three things the enum settles that the struct could not

**`Optional(Type)` is its own variant.** Today `is_option` must test name *and* arity and
deliberately ignore kind, because the compiler represents `Option<T>` as a struct in some
positions and an enum in others -- the comment in `meta.zm` is an apology for an ambiguity
inherited from the C. `meta` is an **interface, not a mirror**: it can normalise. Every generator
asks "is this an Option" first; making it a variant means asking it by pattern-matching and never
getting it wrong.

**A named type keeps the name the source wrote.** `NamedType.name` is `time.Instant`, qualifier
and all -- which is where bug 1 was, and the reason the draft of this section proposed a separate
`text: string` on every type. No longer needed: a primitive has no spelling to vary, an array and
a map are spelled structurally, and the **only** place a written form can differ from a
reconstruction is a named type's qualifier. One field in one variant replaces a field on all of
them, and `type_text` becomes a faithful reconstruction rather than a lossy one.

**`is_enum` is dropped.** The current `kind` distinguishes `struct` from `enum` for a *referenced*
type, and no generator reads the distinction -- `derive_row` and `derive_json` both test
`struct || enum` only to reject everything else, which `Named` now does by being a variant. §2's
rule applies and the field goes.

### What it costs

Every generator's dispatch stops being an `if t.kind == ".."` ladder and becomes a `switch`. That
is a rewrite of the five emitters, and it makes them **shorter and total** rather than longer --
`emit_to_json`'s nine `if`s become nine arms the compiler counts. The fixtures pin the output
byte for byte, so the rewrite is provable.

## 6. A declaration is an enum too, for the same reason

`Decl` carries `is_enum: bool`, and `Member` carries `has_payload: bool` whose own comment says
*"Always false for a struct's field."* That is the same shape as §3's complaint: a field that is
meaningless in half the values it appears in.

```ziman
pub enum Body {
    Struct([Field])
    Enum([Variant])
}

pub struct Decl {
    name: string
    snake_name: string
    type_params: [string]
    body: Body
    // The argument the derive was written with: `#[derive(Entity("medications"))]`.
    arg: Option<string>
}

pub struct Field   { name: string  type: Type  attrs: [string] }
pub struct Variant { name: string  payload: Option<Type> }
```

`Variant.payload` as an `Option<Type>` replaces `has_payload` plus a `unit` type standing in for
absence -- two representations of "no payload" collapse to one. `Field.attrs` is §7.

`arg` is the second parameter the user-generators design specified, folded into `Decl` rather
than sitting beside it: `generate(d: meta.Decl)` is one parameter, four generators already have
that signature, and an argument *about the declaration* belongs on the declaration. The
alternative -- changing five signatures for the benefit of one target -- is what §2's rule exists
to prevent. **An earlier draft also gave `Decl` a `module` field carrying the repository
attributes. That was wrong; §8 is what replaced it.**

A generator that handles only structs writes one arm and is refused for an enum by the type
checker rather than by a runtime check it had to remember to write. `derive_properties`, which
refuses enums today with a hand-written message, gets that for free.

**This is where the "more complete" instruction stops**, and §9 says what is still left out. The
line is: *`meta` is total over what the compiler can hand a generator, and silent about everything
else.* Totality is the new rule; it replaces "deliberately small" without contradicting it,
because every variant above corresponds to something that can actually arrive.

## 7. Members gain their attributes

```ziman
pub struct Field { name: string  type: Type  attrs: [string] }

pub fn has_attr(f: Field, name: string) -> bool
```

A **list of bare names**, not a `pk: bool`. `#[pk]` is the only field attribute today, and a bool
would mean this file changes shape for the second one. A list costs nothing extra and says what
it is. An attribute with arguments is deferred until one exists (§13).

## 8. `#[repository]` is not a derive, and `meta` should stop pretending it could be

The draft above put the repository attributes on `Decl.module` so that a generator for
`#[repository]` could read them. Two things are wrong with it, and the second is fatal.

**It is a general name holding one feature's fields.** `Module` with `repository_*` members is a
dumping ground by construction: the next module attribute lands there too, and a *user-defined*
generator -- the whole point of user-generators.md -- is handed four fields that mean nothing to
it. §6 already took the other road for members, where `#[pk]` is an entry in `attrs: [string]`
rather than a `pk: bool`, and the same argument applies here with more force.

**And the fields would be empty anyway.** In the North Star, `#![repository(Entity)]` lives in
`repository/medication.zm` and `#[derive(Entity("medications"))]` lives in
`entity/medication.zm`. **Different modules.** `Decl.module` for an entity is the entity's module,
which has no repository attributes at all, so every one of those fields would be `None` at every
real site. The draft was not merely inelegant; it did not work.

What the C actually does says why:

```c
void generate_repository_functions(Module *module, StructDecl *sd, const char *entity_module);
```

Three inputs -- the **repository** module, a struct declared **elsewhere**, and the path that
struct was imported by -- plus `module_defines_function(module, name)`, because a repository that
writes `find_by_id` by hand must not also have one generated. None of that is a function of one
declaration, which is the only shape `generate(d: Decl)` has.

So `#[repository]` wants a **second generator kind**, declared and dispatched separately:

```ziman
// `#![generator(Repository)]`, a module generator rather than a declaration generator.
pub fn generate_module(m: meta.Target) -> Result<string, string>

pub struct Target {
    // The entity named by `#![repository(Entity)]`, reified from wherever it is declared.
    entity: Decl
    // The `use` path it was found at, which generated code needs in order to name its functions.
    entity_module: string
    label: Option<string>
    crud: bool
    // Bodyless `pub fn`s in this module awaiting a generated body, in declaration order.
    queries: [Function]
    // Functions this module already defines, so the generator can decline to replace one.
    defined: [string]
}

pub struct Function { name: string  params: [Param]  returns: MetaType }
pub struct Param    { name: string  type: MetaType }
```

`Target` is named for what it is -- the target of one `#![repository(..)]` -- rather than
`Module`, precisely so that it cannot become the place other module attributes accrete. A second
module-level feature gets its own type, or a reason is written down for sharing one.

**This is the larger finding of the redesign.** `meta` has one shape because the first four
generators had one shape, and the fifth does not. Discovering that by trying to force it into
`Decl` is cheaper here than after it is built.

## 9. What is still deliberately absent

* **Visibility, doc comments, line and column.** Nothing generates from them. A refusal is
  reported at the declaration by the bridge, which already has the position.
* **Expressions, statements, bodies.** `meta` describes declarations. A generator that wanted to
  read a body would be a macro system, which is a different document.
* **Attribute arguments.** `attrs: [string]` holds names. The first attribute that takes one
  changes this, and that is a smaller change than guessing the shape now.
* **Bounds on type parameters.** `type_params: [string]` stays bare. `derive_ord` is the only
  generator that writes bounds and it writes them unconditionally.

## 10. The change that matters most: test the boundary, not the model

A wider `MetaType` would not have caught any of §1's bugs. **This would:**

> A fixture that declares real types, runs a real `#[derive(..)]`, and asserts what the generator
> was handed.

Concretely, a bundled generator whose only job is to print its input:

```ziman
#![generator(Reflect)]
pub fn generate(d: meta.Decl) -> Result<string, string> {
    // emits a commented dump of `d`: every member, every type, kind / name / args / text
}
```

and a fixture that writes `#[derive(Reflect)]` on declarations covering **every kind in §5's
table** -- a qualified type inside an `Option`, a `Map<string, [int]>`, a `Result<T, E>`, a
`fn(int) -> string`, a generic struct, an enum with and without payloads, a `#[pk]` field -- and
pins the dump with `--emit-derived`.

That fixture is the test the boundary has never had. It is cheap, it is pure data, and each of
§1's three bugs would have failed it loudly on the first run:

* bug 1 would show `Instant` where the source wrote `time.Instant`;
* bug 2 would show `map` with `args: []`;
* bug 3 is an allocation bug and needs volume -- so the fixture declares **enough members to force
  a collection**, which is the cheapest way to put the one failure mode that fixtures structurally
  missed inside a fixture.

**This is the actual deliverable.** The model changes in §§5-7 are what `derive(Entity)` needs;
this is what keeps the next three bugs from shipping.

## 11. Staging

1. **The `Reflect` generator and its fixture, against today's `meta`.** It will pin the current
   behaviour including whatever is still wrong, which is the point: the diff in step 2 is then the
   proof.
2. **`Type` becomes an enum** (§5) and `Decl` follows (§6). This is **not additive** -- it
   replaces `MetaType`, rewrites `reify_type`, and turns all five generators' `if t.kind == ".."`
   ladders into exhaustive `switch`es. The step-1 fixture changes, every change in it is
   reviewable as a line, and the generators' own fixtures must not change at all.
3. **`Field.attrs`** (§7), additive; no generator reads it yet.
4. **Port `derive(Entity)`.** It needs only steps 2 and 3, and it is a `generate(d: Decl)` derive
   like the four already done -- so it lands before any of §8's machinery exists and proves that
   much of the model on its own.
5. **`Target`, `Function`, `Param` and the `generate_module` entry point** (§8), then **port
   `#[repository]`** -- the second generator kind, and the thing that takes the last generator out
   of `src/derive.c`.

Step 3 changes `src/generators.c`'s reification and the constructors in `meta.zm` without
breaking a generator, because `meta`'s constructors are what the C calls and adding a field
changes the constructor rather than every caller -- the property user-generators.md was designed
for, and step 3 is its first real use.

**Step 2 does not get that protection and should not pretend to.** Replacing the type model
changes every consumer at once. What makes it safe is not the shape of the change but the
fixtures: five generators whose output is pinned byte for byte, plus step 1's dump of the
boundary. If step 2 is right, the generator fixtures do not move at all.

**Splitting 4 from 5 is the point of this ordering.** `derive(Entity)` and `#[repository]` arrived
in this document as one job and are two: the first fits the existing mechanism, the second needs a
new one. Shipping the first alone is what proves §§5-7 without §8 being right as well.

## 12. Cost

`meta.zm` roughly doubles and `reify_*` grows with it. The five generators are **rewritten, not
adjusted** -- every type dispatch becomes a `switch` -- which is the real bill, though the result
is shorter than the `if` ladders it replaces and is checked for exhaustiveness. Against that:
`src/derive.c` loses the last ~1,000 lines it owns once step 5 lands, the boundary gets its first
test, and the class of bug in §1 stops being expressible.

The risk worth naming is **step 5**. A `Target` hands a generator a declaration from another
module, that module's `use` path, and the list of what the current module already defines --
a far larger grant than anything `meta` gives today, and hard to take back once a user-written
generator depends on it. Two guards: `Target` is one feature's type rather than a general
`Module`, and nothing ships in it that the `#[repository]` port does not read.

## 13. Open decisions

* **Is a second generator kind right, or should `#[repository]` stay in C?** §8 argues it needs
  `generate_module` over a `Target`. The honest alternative is that one feature needing its own
  entry point and its own input type is a sign it is not a derive and should not pretend to be --
  in which case the ~1,000 lines stay in `src/derive.c` and `meta` stops at §7. This is the
  decision the whole document turns on.
* **How does `#![generator(X)]` say which kind it declares?** By which entry point the module
  exports (`generate` vs `generate_module`), or by the attribute. Exporting both should be a
  refusal either way.
* **Should `text` on `MetaType` be authoritative, with `type_text` deleted?** Two ways to spell the
  same thing is how they drift. Keeping both and checking they agree (§9) is the proposal, but
  deleting `type_text` is tidier.
* **Should `Optional` really be a variant** (§5), given that the compiler has no such kind and
  `meta` would be normalising two of its representations into one? This is the clearest case of
  `meta` being an interface rather than a mirror, and the clearest place to object to that.
* **`Void` and `Unit` as separate variants.** `Void` is "no return type written" and `Unit` is a
  real type with one value. Both can arrive; a generator almost certainly wants to treat them
  alike, which is an argument for one variant and a lie.
* **Does `Func` really need its parts**, or would `Named` do? No generator handles a
  function-typed field today and `derive(Json)` refuses one. Including it is for totality, which
  §5 argues is the point and §2 would have argued against.
* **Attribute arguments** (§8): deferred, but `#[pk]` is unlikely to stay the only field
  attribute, and `#[column("name")]` is the obvious next one.
