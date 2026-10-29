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

That last one is the biggest change: today `meta` describes **types**, and `#[repository]`
needs it to describe **a function signature**. Nothing in the current model can hold one.

## 5. The model: `MetaType` becomes honest

Keep `kind`/`name`/`args`, and make `args` mean the same thing for every kind that has parts:

| kind      | `name`              | `args`                                   |
| --------- | ------------------- | ---------------------------------------- |
| `array`   | `""`                | `[elem]`                                 |
| `set`     | `"Set"`             | `[elem]`                                 |
| `map`     | `"Map"`             | `[key, value]`                           |
| `result`  | `"Result"`          | `[ok, err]`                              |
| `channel` | `"Channel"`         | `[elem]`                                 |
| `func`    | `""`                | `[param.., ret]` -- **return type last** |
| `struct`  | the name as written | the type arguments                       |
| `enum`    | the name as written | the type arguments                       |
| `typevar` | the variable        | empty                                    |
| scalars   | `""`                | empty                                    |

`func` putting its return type last is the one arbitrary choice; the alternative is a separate
field that only one kind uses. Last is where the source writes it.

**And one new field**, which is what makes the model honest rather than merely wider:

```ziman
pub struct MetaType {
    kind: string
    name: string
    args: [MetaType]
    // The type exactly as the source spelled it. Set for every kind.
    text: string
}
```

`text` is what `meta.type_text` reconstructs today. Reconstruction is why bug 1 existed: the
generator rebuilt `Option<time.Instant>` out of parts after the qualifier had been dropped from
one of them. **Carrying the written form removes a whole class of bug** -- a generator that wants
the type as written never recomputes it -- and leaves `type_text` as a convenience that must
agree with it, which is a thing a test can check on every real declaration (§9).

## 6. Members gain their attributes

```ziman
pub struct Member {
    name: string
    type: MetaType
    has_payload: bool
    // The attributes written on this member, bare names, in source order: `["pk"]`.
    attrs: [string]
}

pub fn has_attr(m: Member, name: string) -> bool
```

A **list of bare names**, not a `pk: bool`. `#[pk]` is the only field attribute today, and a bool
would mean this file changes shape for the second one. A list costs nothing extra and says what it
is. An attribute with arguments is deferred until one exists (§12).

## 7. The declaration gains its context

```ziman
pub struct Decl {
    name: string
    snake_name: string
    is_enum: bool
    type_params: [string]
    members: [Member]
    // The argument the derive was written with: `#[derive(Entity("medications"))]` -> "medications".
    arg: Option<string>
    // The module the declaration lives in, and its repository attributes when it has them.
    module: Module
}

pub struct Module {
    // `#![repository(Entity)]`: the entity's name, or None.
    repository_entity: Option<string>
    repository_label: Option<string>
    repository_crud: bool
    // Bodyless `pub fn`s awaiting a generated body, in declaration order.
    queries: [Function]
}

pub struct Function {
    name: string
    params: [Param]
    returns: MetaType
}

pub struct Param {
    name: string
    type: MetaType
}
```

`arg` is the second parameter the user-generators design specified, folded into `Decl` instead.
Reason: `generate(d: meta.Decl)` is one parameter and four generators already have that signature,
and an argument *about the declaration* belongs on the declaration rather than beside it. The
alternative -- changing every generator's signature for the benefit of one -- is what §2's rule
exists to prevent.

`Module` is new and is the part to be most suspicious of: it is the first time a generator sees
anything outside the declaration it was handed. It is here because `#[repository]` genuinely is a
module-level feature -- the entity, the label and the CRUD flag are module attributes, and the
queries are sibling declarations. A generator for it cannot work from a struct alone.

## 8. What is still deliberately absent

* **Visibility, doc comments, line and column.** Nothing generates from them. A refusal is
  reported at the declaration by the bridge, which already has the position.
* **Expressions, statements, bodies.** `meta` describes declarations. A generator that wanted to
  read a body would be a macro system, which is a different document.
* **Attribute arguments.** `attrs: [string]` holds names. The first attribute that takes one
  changes this, and that is a smaller change than guessing the shape now.
* **Bounds on type parameters.** `type_params: [string]` stays bare. `derive_ord` is the only
  generator that writes bounds and it writes them unconditionally.

## 9. The change that matters most: test the boundary, not the model

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

## 10. Staging

1. **The `Reflect` generator and its fixture, against today's `meta`.** It will pin the current
   behaviour including whatever is still wrong, which is the point: the diff in step 2 is then the
   proof.
2. **`MetaType` gains `text` and the three missing kinds** (§5). The step-1 fixture changes, and
   every change in it is reviewable as a line.
3. **`Member.attrs`** (§6) and `Decl.arg` (§7), both additive; no generator reads them yet.
4. **`Module` and `Function`** (§7), likewise.
5. **Port `derive(Entity)` and `#[repository]`**, which is the consumer that proves 2-4 were right
   and the thing that takes the last generator out of `src/derive.c`.

Steps 2-4 each change `src/generators.c`'s reification and the constructors in `meta.zm`; no
generator breaks, because `meta`'s constructors are what the C calls and adding a field changes
the constructor rather than every caller -- the property §7 of user-generators.md was designed for
and that step 3 of this plan is the first real use of.

## 11. Cost

`meta.zm` roughly doubles, and `reify_*` in `src/generators.c` grows by about the same. Against
that: `src/derive.c` loses the last ~1,000 lines it owns once step 5 lands, which is the whole
derived-query machinery, and the boundary gets its first test.

The risk worth naming is **step 4**. `Module` hands a generator sibling declarations, which is a
larger grant than anything `meta` has given so far and is hard to take back. If `#[repository]`
turns out not to need all of it, the parts it does not use should not ship.

## 12. Open decisions

* **Does `Decl` carry `module`, or does `generate` take a second parameter?** §7 chose the field.
  The counter-argument is that a `Decl` is then not self-contained and a test fixture has to build
  a `Module` to build a `Decl`. A second parameter keeps `Decl` pure and changes five signatures.
* **Should `text` on `MetaType` be authoritative, with `type_text` deleted?** Two ways to spell the
  same thing is how they drift. Keeping both and checking they agree (§9) is the proposal, but
  deleting `type_text` is tidier.
* **Does `func` really need its parts**, or is text enough? No generator handles a function-typed
  field today and `derive(Json)` refuses one. Including it is for the model's honesty, which is an
  argument §2 is suspicious of.
* **Attribute arguments** (§8): deferred, but `#[pk]` is unlikely to stay the only field
  attribute, and `#[column("name")]` is the obvious next one.
