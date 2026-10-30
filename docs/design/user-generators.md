# A derive a project can write itself

Status: **design only.** Nothing below is built. Written because the five generators that exist
are now Ziman (`lib/core/derive_*.zm`) and nothing calls them: the question *"what links
`#[derive(Properties)]` to `derive_properties.zm`?"* has no answer yet, and answering it for the
bundled five and for a project's own derive is the same question.

## 1. What exists today

`#[derive(X)]` is a fixed set of five names, recognised by `strcmp` in `src/derive.c`:

```c
int is_props = strcmp(sd->derives[d], "Properties") == 0;
```

Adding a sixth means editing `derive.c`, rebuilding the compiler and shipping a binary. So
`#[derive(Entity("table"))]` exists because *this* repository's sample backend wanted it, and an
application with its own repeated shape has no way to write the equivalent. That is the roadmap
entry's complaint, and it is the half the Ziman port did not touch -- `derive_ord.zm`,
`derive_row.zm` and `derive_properties.zm` are proved byte-identical to the C and **nothing in
`src/` references them**.

## 2. What a project writes

The use site is a `use` and an attribute:

```ziman
// model/customer.zm
use derive/csv

#[derive(Csv)]
pub struct Customer {
    id: Uuid
    name: string
    balance: decimal
}
```

The generator is an ordinary module:

```ziman
// derive/csv.zm
//
// `#[derive(Csv)]` -> `<name>_to_csv_row`, one string per field in declaration order.
#![generator(Csv)]

use meta
use template

val sheet = template.must_parse("""
@template to_csv_row
pub fn @{snake}_to_csv_row(v: @{name}) -> [string] {
    return [@{cells}]
}

@end
""")

pub fn generate(d: meta.Decl, arg: Option<string>) -> Result<string, string> {
    val cells = mutable_array_new<string>()
    for m in d.members {
        push(cells, concat("str(v.", m.name, ")"))
    }
    return template.render(sheet, "to_csv_row", [
        template.text("snake", d.snake_name),
        template.text("name", d.name),
        template.text("cells", join(separator: ", ", pieces: snapshot(cells)))
    ])
}
```

```
myapp/
  main.zm
  derive/
    csv.zm            #![generator(Csv)]
  model/
    customer.zm       use derive/csv  +  #[derive(Csv)]
  test/
    csv_test.zm       #[test] over generate(meta.Decl { .. })
```

**The last line is the point that is easy to miss.** A generator is a function from a declaration
to a string, so a project's own derive is unit-testable the way `tests/lang/derive_ord_in_ziman`
tests ours: build a `meta.Decl`, call `generate`, compare the text. A generator written in C can
only ever be exercised end to end.

## 3. Discovery: the `use` list, and nothing else

When generation meets `#[derive(Csv)]` it walks **this module's own `use` list** for a loaded
module carrying `#![generator(Csv)]`, and falls back to the bundled generators.

**This is not a new mechanism.** `resolve_repository` already walks `mod->uses` and `find_loaded`
to turn `#![repository(VitalSign)]` into a `StructDecl` in another file, for the same reason: at
the point generation runs, the loader is the only thing that knows which files are loaded.

What it buys is that **there is no manifest, no search path and no registry to invent**. The file
that wants a derive imports its generator, exactly as it imports anything else, and a reader of
`model/customer.zm` can see where `Csv` comes from without leaving the file. Every alternative
considered -- a project manifest, a well-known `derive/` directory, a compiler flag -- adds a
second place for the answer to live.

**The bundled five move onto the same footing**: `lib/db/derive_row.zm` gains `#![generator(Row)]`
and renames its entry point to `generate`. One rule, with corelib as the fallback that keeps
`#[derive(Row)]` working with no import -- the way the prelude keeps `print` working with no
import. A project's own generator is not vocabulary and says so by being imported.

## 4. The link is declared, not spelled

The convention the bundled modules fell into is `derive(X)` -> module `derive_<snake(X)>` ->
function of the same name. It is tidy and it is **a name built by string concatenation and
checked by nothing**, which is roadmap question 5 and the exact shape F168 found in `src/lsp.c`'s
hand-kept builtin list and F212 found in two name lists that asserted nothing at all.

`#![generator(Csv)]` makes the link a declaration. A declaration can be checked -- a module
claiming a target with no `generate`, two modules claiming one target, a target nobody claims are
all answerable at the declaration. A concatenated name can only be looked up and missed.

`generate` is a fixed name, and **one module serves one target**, mirroring "one module, one
entity" in `#![repository(..)]`. Two targets in one file would need two entry points and a rule
for which is which; two files is the same thing without the rule.

## 5. What a generator is given and answers

```ziman
pub fn generate(d: meta.Decl) -> Result<string, string>
```

**Built as one parameter, not two.** The signature above read `generate(d, arg: Option<string>)`
when this was written, for `#[derive(Entity("table"))]`'s string -- and `Entity` is the one of
the five not yet ported, so every generator that exists would carry a parameter none of them
reads. That is the mistake `meta.Member.type_name` already made once in this same work
(templates §14): a field nobody reads is a promise about the shape that is usually wrong anyway.
The parameter arrives with `derive_entity`, which is the first thing that can say what it should
be.

`d` is `lib/core/meta.zm`'s reified declaration -- name, snake name, struct-or-enum, type
parameters, and the members with their types. `arg` carries the string a target may take:
`#[derive(Entity("vital_signs"))]` is the one that does today.

The answer is **source text**, lexed and parsed and spliced by the same pipeline a C generator's
output goes through, so a generated function is indistinguishable from a hand-written one by the
time the type checker sees it. An `err` is reported at the declaration carrying the attribute,
naming the generator -- the same place and shape as `derive(Entity) on 'X' needs derive(Row) too`.

A generator that *fails* rather than refusing -- an index out of range, an unwrap of None -- is
caught at a `ThreadBoundary` and reported the same way, with the generator's own frame in the
trace. A generator is code, and code has bugs; what it must not do is take the compiler down
without saying which declaration it was working on.

## 5a. A generator's own field attributes

`#[derive(Csv)]` alone does not let a project write the derive it wants. The bundled generators
read **field attributes** — `#[pk]`, `#[column(name:)]`, `#[one_of(values:)]` — and a project's
would want the same: a `Csv` derive with no `#[csv_column(name: "ID")]` is a derive that can only
use the field's own name.

Today that is a hard stop, and not a deferral: the attribute is refused by the *parser*.

```ziman
use derive/csv

#[derive(Csv)]
struct Customer {
    #[csv_column(name: "ID")]
    id: int
}
```

> `syntax error: unknown field attribute 'csv_column' -- a struct field takes one of: pk, column,
> one_of, not_blank, not_empty, email, positive, min, max, size, valid`

**This section exists because §9 did not mention it.** §9 names module-level attributes and
function attributes as out of scope, so field attributes read as covered — and they are not
covered anywhere. Found 2026-10-30 by asking the obvious question of a design that had shipped
eleven field attributes and a generator written in Ziman: *the user will not have access to C code
to add the new annotation.*

### What makes it tractable

It looks blocked by the reason `FIELD_ATTRS` is in C at all — **the parser is the only thing that
sees every field**, so a misspelling on a struct that derives nothing can be caught nowhere else
(`src/parser.c`'s own comment, and the `#[deprecated]` precedent). It is not.

**The parsing is already generic.** `field_attr_list_push` and `field_attr_arg` accept any
`IDENT(name: value, ...)` shape and record each argument's `kind`; `check_field_attribute` is a
separate pass over the table. And `FieldAttr`/`FieldAttrArg` already carry `line` and `col`.

So the check can move from parse time to **after module loading**, when the `use` list is known,
and still report at the field's own position. The thing being checked becomes:

> every struct field in the module, against the built-in names **union** the names declared by the
> generators this module imports.

Which keeps the whole protection. A typo is still refused on a struct that derives nothing, because
the check is keyed on the *module*, not on the derive. And `#[csv_column]` in a module that does
not import `derive/csv` is still refused, which is the case the table was built for.

### How a generator declares them

Symmetrically with `#![generator(X)]` — a `pub val` the compiler reads once the module is loaded,
in the spec grammar `FIELD_ATTRS` already uses:

```ziman
// derive/csv.zm
#![generator(Csv)]
use meta

pub val attributes = [
    meta.attr_spec("csv_column", "name:string"),
    meta.attr_spec("csv_skip", ""),
    meta.attr_spec("csv_order", "value:int,header:string?")
]

pub fn generate(d: meta.Decl) -> Result<string, string> { ... }
```

Most of the machinery is already built: the spec grammar (`"min:int?,max:int?,message:string?"`),
`field_attr_spec_text` turning a spec into `name: "<text>", value: <number>` for the refusal, the
three argument kinds (`string`, `int`, `name`), and `resolve_repository`'s walk of the `use` list.
What is missing is the deferral, a `meta.attr_spec`, and the collision rule below.

### What this gives up

* **The check happens one phase later.** Same file, same line, same column — a different phase.
  A `--fmt` or a `--lsp` pass that stops before module loading no longer sees a bad field
  attribute, where today it does.
* **Two generators may claim one name.** Two imported generators both declaring `csv_column` has to
  be refused at the importing module, naming both — the same shape as `#![generator(..)]` twice in
  one file. A generator cannot be asked to namespace its attributes (`#[csv.column]` is not a thing
  the grammar has, and a prefix convention is not a rule).
* **A generator must be loaded to check a field it has nothing to do with.** A module importing
  `derive/csv` pays the generator's initialisation (§6) even if the mistake is in a struct that
  derives nothing. That cost is already paid by anything that derives at all.

### Where it sits in the order

**After the bridge and `use`-list discovery (§12.1, §12.2), and before a project would call any of
this usable.** The three are one feature: a target a project can name, a generator that runs, and
the attributes that generator reads. Shipping the first two without this would mean a project's
derive can be triggered and can only ever see field *names* — which is `#[derive(Csv)]` without
`#[csv_column]`, the thing this section started from.

## 5b. `derive` is the only target mechanism, at every position

Decided by the user 2026-10-30, after a survey of every annotation `src/` knows by name:

> `#[repository]` should not stay. We can replace it something similar to `#[derive(Repository)]`
> `#[derive(Endpoint(method: POST, path:"..."))]` — not exactly. derive part can be in C.
> `#![repository(Entity)]` should not stay.

So the rule is: **C knows `derive`, and knows nothing about any target.** `#[repository]` on a
function and `#![repository(Entity)]` on a module are the two places a *domain* name is wired into
the compiler, and both become ordinary targets resolved through the `use` list (§3).

| position | today | after |
| --- | --- | --- |
| module | `#![repository(Note)]` | `#![derive(Repository(Note))]` |
| function | `#[repository]` on a bodyless fn | `#[derive(Repository)]` |
| struct/enum | `#[derive(Entity("medications"))]` | unchanged in shape |
| field | `#[pk]`, `#[one_of(..)]` — a table in C | declared by the generator (§5a) |

**The two generator kinds need no attribute of their own.** Which kind a generator is — one
declaration (`generate`) or a whole module (`generate_module`/`generate_query`) — is read off its
exported entry point already (meta.md §13), and that is a property of the *generator module*, not
of the attribute at the use site. So the module form needs no second name: C resolves `Repository`,
finds `generate_module`, and knows what it is being asked for.

### What `derive` has to grow

**Named arguments with kinds.** `meta.Decl.arg` is `Option<string>` and the grammar takes one
positional string — `#[derive(Entity("medications"))]` is the only use. `Endpoint(method: POST,
path: "/orders")` needs a name, a value and a *kind* per argument, which is exactly
`FieldAttrArg {name, value, kind}` and the `"name:kind,other:kind?"` spec grammar that
`FIELD_ATTRS` already uses for fields. So this is the same upgrade §5a needs, applied one position
up:

* `meta.Decl.arg: Option<string>` becomes `args: [meta.AttrArg]`, which `meta.Field.attrs` already
  carries for fields. `#[derive(Entity("medications"))]` keeps working as one unnamed argument
  under the name `value`, which is the convention `#[min(8)]` already follows.
* A generator declares its target's argument spec the way §5a has it declare its field attributes
  — one `pub val`, one grammar, read after the module loads.
* Validation defers past module loading for the same reason and with the same guarantee: the spec
  is not knowable until the generator is resolved, and `line`/`col` on the attribute mean the
  refusal still lands on the attribute.

### What is left in C

24 annotation names are wired in today. §5a removes the eleven field attributes; this removes
`repository` from both of its positions. **Twelve remain, and not one of them is a library or
domain name:**

> **Partly done, 2026-10-30.** The eleven field attributes are still *named* in `FIELD_ATTRS` --
> that is §5a's work -- but the compiler no longer reads what any of them **mean**. `#[pk]` and
> `#[column]` were pulled out of the attribute list into `Param.is_pk`/`column_name` and read by
> `src/derive.c` to decide a repository's imports; `#[valid]` was read to require a nested
> `_violations` in scope. All three reads are gone: importing a converter for every field type is a
> superset of what the keys need, the `uuid` import keys on the *type* rather than on a key, and the
> `#[valid]` requirement is reported by the generated call that does not resolve. `Param` carries
> nothing but the generic list now. The North Star's 34,800 lines of derived output are
> byte-identical across the change.

`derive`, `from`, `test`, `deprecated`, `experimental`, `generator`, `thread_bound`, `thread_key`,
`at_least_one`, `effectful`, `compiler_resolved`, `when_unmatched`.

Every one changes how the language itself compiles, loads or type-checks — `#[from]` is `try`'s
error conversion, `#[effectful]` is an overload rule, `#![experimental]` is a diagnostic,
`#![generator(X)]` is a module's claim about itself. That is the line: **C knows the annotations
that are facts about the language, and none that are facts about a library.**

### Two questions this does not settle

1. **Does a bodyless function still need a marker?** `#[derive(Repository)]` on each one keeps the
   explicitness `#[repository]` has, and repeats a word the module attribute already said. The
   alternative — a bodyless function in a module with a module generator is that generator's to
   fill — needs no attribute at all, and makes "I forgot a body" indistinguishable from "generate
   this". The current design chose the marker; nothing has argued the other side yet.
2. **Is `derive` the right word at module level?** `#![derive(Repository(Note))]` reads as a
   declaration being derived when what is derived is the *file*. Keeping one word holds C's
   vocabulary to one entry; a second word (`#![generated_from(..)]`) reads better and costs C a
   name. Not decided.

## 6. Initialisation and ordering, which is the real cost

A generator's `val sheet = template.must_parse(..)` has to be initialised before `generate` is
called. Module-level vals are initialised **once, in `interpret()`, after all loading** -- and
generation runs *during* loading. So the bridge needs its own initialisation.

With a manifest the set of generators would be known up front and could be loaded, checked and
initialised as one program before the user's program is read. **`use`-based discovery gives that
up**: the set is discovered incrementally, one importing module at a time. So the rule is

> a generator module's globals are initialised on first use, in dependency order, into an
> environment that outlives the load.

Which is more intricate than loading one generator program at startup, and is the price of not
inventing a manifest. The machinery is the same either way -- `interpret_tests` already shows the
shape: set `g_program` and `g_global_env`, push a `<module initialisation>` frame,
`init_param_defaults` and `init_global_vals`, then `call_function` under a `ThreadBoundary`.

The ordering that makes it work is already guaranteed: generation for a module runs **after its
dependencies are loaded to completion**, so a generator named in the `use` list is fully parsed
and spliced before anything asks it to generate.

## 7. Cycles

Two kinds, and only the second is new:

* **A `use` cycle** -- the loader already refuses one by name.
* **A derive cycle** -- `derive/csv.zm` carrying `#[derive(Csv)]`, or two generators each using
  the other's target. The `use` graph can be acyclic while this is not. Refused by name, with the
  chain: *"generating 'Csv' needs 'Csv'"*.

A generator using a *different*, already-loaded derive is fine and should stay fine: a generator
with a `#[derive(Row)]` struct of its own is not a cycle, just a dependency.

## 8. `--check` stops being execution-free, and that is the thing to decide

Today `ziman --check` type-checks and runs nothing, and `--lsp` the same. **A generator is user
code the compiler executes**, so both would start running it -- `--lsp` potentially on every
reparse of a file whose imports include a generator.

For `ziman file.zm` this changes nothing: the compiler was going to run that project's code
anyway. For `--check` and `--lsp` it is a real change and it should be stated rather than
discovered. Three ways:

1. **Accept it and say so.** A generator is part of the program's meaning; a check that skips it
   cannot report what the program compiles to. The honest position, and the one every macro
   system takes.
2. **A flag** (`--no-generators`), which gives a check that is fast and incomplete, and a second
   mode for everything downstream to get wrong.
3. **Restrict what a generator may reach** -- no `fs`, no `net`, no `process`. Buildable (the
   native surface is the lever) and a bigger design than this one, since it needs a story for
   what a restricted module even is.

Recommended: **(1)**, with `--lsp` the part to measure rather than assume -- a generator runs once
per declaration carrying the attribute, and the sample backend has 140 of them.

## 9. What it deliberately does not cover

* ~~**Module-level attributes of a project's own**, the `#![repository(E)]` shape.~~ Overtaken by
  §5b: there is to be no second mechanism. `#![repository(E)]` itself becomes a `derive` target, so
  a project's module-level generator is the same feature as its declaration-level one rather than a
  thing to design after it.
* **Attributes on functions**, the `#[repository]` derived-query shape. Also §5b now: the function
  marker becomes a `derive` target too.
* ~~**Field attributes of a generator's own.**~~ This list said nothing about them, which read as
  coverage rather than as an omission — §5a now covers them, and they are part of the feature
  rather than outside it: a derive that cannot read `#[csv_column(name:)]` can only ever see a
  field's own name.
* **Determinism.** A generator reading the clock or a file produces a different program per
  build. Nothing here polices that; it is a documented expectation, as it is in every language
  with compile-time code.
* **Generating into another module.** A generator writes declarations into the module carrying
  the attribute, as all five do today. Roadmap question 2 stays open.
* **Ordering between two generators on one declaration.** `#[derive(Json, Csv)]` runs them in the
  order written and they may not read each other's output, which is the existing rule (DESIGN.md,
  2026-10-28) and the reason ordering has not mattered so far.

## 10. Decisions

1. **Discovery is the importing module's `use` list, with corelib as the fallback.** Given up: a
   project cannot apply a derive without importing its generator, including in a file that would
   otherwise need no imports. Taken because every alternative invents a second place for the
   answer to live, and because `resolve_repository` already proves the mechanism.
2. **`#![generator(X)]` declares the link; the entry point is `generate`.** Given up: the tidy
   `derive_<snake(x)>` convention the bundled modules already follow. Taken because a declaration
   can be checked and a concatenated name cannot.
3. **One module, one target.** Given up: a file holding a family of related derives. Taken for
   `#![repository(..)]`'s reason -- the alternative needs a rule for which entry point is which.
4. **Generators run during `--check` and `--lsp`.** Given up: an execution-free check. Taken
   because a check that skips generation cannot report what the program compiles to.

## 11. Partly built, 2026-10-28

`#![generator(X)]` is parsed, checked and carried by all three ported generators, whose entry
points are now `generate`. What that buys today is the *declaration* half of §4: a module
claiming a target with no `pub fn generate` is refused at the attribute, and a second
`#![generator(..)]` in one file is refused as "one module, one target".

**Nothing dispatches on it.** `#[derive(X)]` is still a `strcmp` over five names in
`src/derive.c`; the attribute is a claim the compiler checks and does not yet use. That is
deliberate -- the claim is cheap and checkable now, and the bridge below is what makes it
load-bearing.

## 12. What is left before building

1. **The bridge, which this shares with the bundled five** and which is the larger half: a C
   function building a `meta.Decl` value (`struct_obj_new` is the primitive, and no native builds
   a struct today), and per-generator-module initialisation as §6 describes.
2. **Then `#![generator(X)]` and `use`-list discovery**, which is small once the bridge exists --
   the lookup is `resolve_repository`'s walk with a different attribute.
3. **Then the attribute grammar, once, for both positions** (§5a and §5b): `check_field_attribute`
   deferred past module loading, `meta.attr_spec`, the collision rule, and `meta.Decl.arg` becoming
   `args: [meta.AttrArg]`. One grammar serves a generator's field attributes and its target's own
   arguments, and it needs (2)'s `use`-list walk and nothing else.
4. **Then `#![repository(E)]` and `#[repository]` become `derive` targets** (§5b), which is what
   takes the last domain name out of the compiler. It is last because it is a migration of a
   working feature rather than a new one, and because it is the proof that the mechanism carries
   the harder of the two generator kinds.
5. **Measure `--lsp`** before deciding §8 for good. One generator call per declaration, 140
   declarations in the sample backend, an interpreter rather than C.
