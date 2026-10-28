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

**The bundled five move onto the same footing**: `lib/core/derive_row.zm` gains `#![generator(Row)]`
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
pub fn generate(d: meta.Decl, arg: Option<string>) -> Result<string, string>
```

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

* **Module-level attributes of a project's own**, the `#![repository(E)]` shape. Declaration-level
  `#[derive(X)]` is one mechanism; a user-defined module attribute is a second, and the first
  should ship before the second is designed.
* **Attributes on functions**, the `#[repository]` derived-query shape. Same reason.
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

## 11. What is left before building

1. **The bridge, which this shares with the bundled five** and which is the larger half: a C
   function building a `meta.Decl` value (`struct_obj_new` is the primitive, and no native builds
   a struct today), and per-generator-module initialisation as §6 describes.
2. **Then `#![generator(X)]` and `use`-list discovery**, which is small once the bridge exists --
   the lookup is `resolve_repository`'s walk with a different attribute.
3. **Measure `--lsp`** before deciding §8 for good. One generator call per declaration, 140
   declarations in the sample backend, an interpreter rather than C.
