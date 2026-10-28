# A template module: the shape of generated code, visible in the source

Status: **design only.** Nothing below is built. Written because `#[derive(Ord)]` has just been
ported out of C (`lib/core/derive_ord.zm`) and the port made the real problem visible: a generator
written as `push`/`concat` hides the thing it exists to produce.

## 1. The problem, in one comparison

This is `derive_ord`'s struct half as it stands today. It is correct, it is tested, and you cannot
see the function it writes:

```ziman
push(out, concat("    val c_", m.name, " = compare(a.", m.name, ", b.", m.name, ")\n"))
push(out, concat("    if c_", m.name, " != 0 {\n        return c_", m.name, "\n    }\n"))
```

The same thing with a template:

```
@for m in members
    val c_@{m.name} = compare(a.@{m.name}, b.@{m.name})
    if c_@{m.name} != 0 {
        return c_@{m.name}
    }
@end
```

**That is the entire case.** The output's shape is the source's shape, a reader checks the
generated code by reading it rather than by running it, and the `\n` bookkeeping disappears.

Counted, because the case should be measured rather than felt: `src/derive.c` is 2,486 lines
holding **240 `cb_appendf` calls and 291 `\n` escapes**. Nine generators, and almost every line of
all nine is a string being assembled a fragment at a time.

Worth stating the limit of the case too: this is an argument about **generating code**, where the
output has a shape a reader knows. A template buys much less for a generator whose output is a
flat list, and nothing at all for prose.

## 2. What the language already has

Three facts constrain every decision below.

* **`format("{}", x)` holes are checked at compile time.** `format("{} and {}", 1)` is a type
  error naming the count. Anything this module does with holes is being compared against a
  standard the language already meets for one-line strings.
* **Text blocks strip common indentation**, and `ziman --fmt` leaves them exactly as written. A
  template sitting inside an indented `val` dedents for free, and the formatter will not reflow
  it.
* **`{{`/`}}` is already the brace escape** in a format string, and **`@` is not a token in the
  lexer** -- it appears nowhere in Ziman's grammar.

And one shape worth copying rather than reinventing: `eql` binds values to names in a parsed
query with an array of constructor calls (`[eql.bind("id", id), eql.bind_list("types", xs)]`).
That is the same problem as binding values to holes in a parsed template.

## 3. The syntax

```
hole       := "@{" name { "." name } "}"
directive  := "@for" ident "in" name | "@if" name | "@else" | "@end"
escape     := "@@"
```

A directive **occupies its whole line** and emits nothing, not even the newline. Everything else
is literal text.

**`@` and not `{}`, which is the one place this parts company with `format`.** A code template's
output is mostly braces:

```
pub fn @{snake}_compare(a: @{self}, b: @{self}) -> int {
```

Under `format`'s rule that trailing `{` doubles, and so does every other literal brace in the
file -- the common case paying for the rare one. `format` chose `{}` because its output is usually
prose; this module's output is usually code. The sigil differs because the content differs, and
`@` is free.

**`@{...}` and not `@name`**, so that a hole can abut an identifier character: `@{m.name}_compare`
has one reading and `@m.name_compare` has none.

## 4. Logic lives in Ziman; the template holds shape

**A template interpolates, repeats, and chooses. It does not compute.** No expressions, no
comparisons, no arithmetic, no `len`, no filters, no function calls. `@if` takes a **bound boolean
and nothing else**.

The alternative was measured against the generator that needs the most: `derive_ord`'s enum half
emits a rank block for every variant **but the last**, each with its index as a literal, then a
`count - 1`, then payload arms **only** for variants that carry one. Four kinds of variability. A
template language able to express them needs a loop index, `len()`, `-`, `<` and `@if` with
operands -- an expression language, invisible to the type checker because it lives inside a
string, and one that never stops growing (`@elif`, then `@{name|upper}`, then whitespace control,
then macros).

So Ziman computes and the template repeats:

```ziman
val ranked = mutable_array_new<template.Row>()
for i in 0..prelude.len(d.members) - 1 {
    push(ranked, template.row([
        template.text("variant", d.members[i].name),
        template.text("rank", str(i))
    ]))
}
```

**What this costs, stated plainly**: the marshalling above is four lines to say "a row per variant
but the last", and *"this chunk appears only sometimes"* becomes *"this list is empty"* -- an
indirection a reader follows to a second place. `@if` on a bound flag is what keeps that cost
bounded; everything past it is the slope.

## 5. The data model

One opaque struct for a binding and one for a row, with a constructor per kind -- `eql.bind`'s
shape, for `eql.bind`'s reason:

```ziman
pub fn text(name: string, value: string) -> Binding
pub fn flag(name: string, value: bool) -> Binding
pub fn rows(name: string, value: [Row]) -> Binding
pub fn row(cells: [Binding]) -> Row
```

```ziman
val out = try template.render(compare_template, [
    template.text("name", d.name),
    template.text("snake", d.snake_name),
    template.flag("generic", prelude.len(d.type_params) > 0),
    template.rows("ranked", ranked)
])
```

**Every value a hole can hold is already a `string`.** That is §4's rule made a type rather than a
runtime refusal: there is no number to format, so there is no question about how a number is
formatted.

**Why not a `Map`.** `Map<string, string>` is a fine *row* and real syntax today. It cannot be the
whole binding set: those need scalars, booleans and lists of rows at once, so a map forces either
three maps and a three-argument `render`, or one recursive `enum Value` in front of every caller.
A `Row` holding `Binding`s gives nesting for free and keeps both out of sight. (Ziman has no tuple
type, so `("variant", x)` is not an option at all.)

**Scoping inside `@for`.** Cells are reached **qualified** -- `@for r in ranked` binds `r`, and the
row's cells are `@{r.variant}`. Unqualified cells would shadow an outer binding silently; this way
nesting two loops is unambiguous and shadowing is impossible by construction.

## 6. Parsed once, at module load

`template.parse(text) -> Result<Template, string>` and `template.render(t, bindings) ->
Result<string, string>`, with the parse living in a module-level `val`:

```ziman
val compare_template = template.must_parse("""...""")
```

This is `repository.query`'s pattern exactly, and it buys the same thing: **a malformed template
fails when the module loads, not on the first call that happens to reach it.** A generator's
templates are fixed text, so every one of them is checked at startup.

It is the honest answer to the standard §2 sets, and it does not meet it. `format`'s holes are
checked by the compiler; a template's are checked when the module loads. The gap is real and the
price of this being a module rather than a language feature. What closes it later, if it is ever
worth closing, is the compiler knowing the template literal -- at which point `@{name}` could be
checked against names in scope.

## 7. Refusals

Each names what was wrong and what was available, as `eql` does for a mistyped field:

* a hole naming nothing bound -- listing what is bound
* a binding nothing uses -- the same mistake from the other side, and the more likely one after a
  template is edited
* `@for` over a name bound to text, or `@if` over a name bound to rows
* `@{r.cell}` where `r` is not a loop binder, or `cell` is not in the row
* `@end` with nothing open, or a template ending with something open
* a directive line with trailing text after it

The first two are the reason `render` answers a `Result` rather than a `string`.

## 8. Where this sits relative to the generators

Generation runs **during loading**, at one point in `load_module`: after a module's dependencies
are loaded, before its own imports are resolved. Nothing is generated at run time. A template
module changes none of that -- it changes how a generator assembles the text it returns.

Two rules keep several generators from needing an ordering between them, and both predate this
document:

* **A generator reads declarations; it may name another generator's output but never inspect it.**
  `#![repository(E)]` reads the entity's columns off its `StructDecl` rather than off
  `derive(Entity)`'s `<snake>_columns`, which looks like duplication and is what makes the order
  irrelevant: generators communicate by name, and names bind at typecheck.
* **A genuine prerequisite is declared and refused, not ordered.** `derive(Entity)` needs
  `derive(Row)` and says so in one error naming both.

**`meta` and `template` must themselves carry no `#[derive(..)]`**, or a generator needs a
generator. `lib/` has zero derive sites today, so this is free -- which makes it luck rather than
design, and worth a check rather than a comment.

## 9. What it deliberately does not cover

* **Expressions of any kind.** §4.
* **Includes, partials, macros, inheritance.** A generator that wants a shared fragment calls a
  Ziman function that renders one and binds the result as text.
* **Escaping.** This module emits Ziman source, not HTML; there is no context to escape for, and a
  generator that needs quoting does it before binding. Guessing would be worse than nothing.
* **Whitespace control** (`@{-name}` and friends). Directives already eat their own line, which is
  the case that matters for code.
* **Formatting a value.** Everything bound is already a string, so `str` and `format` do this
  where they are checked.

## 10. Decisions

1. **`@{name}` for a hole, `@for`/`@if`/`@else`/`@end` on their own lines, `@@` for a literal.**
   Given up: consistency with `format`'s `{}`. Taken because the output is brace-dense code.
2. **`@if` takes a bound boolean, never an expression.** Given up: conditions written where they
   are read. Taken because the alternative is an unchecked language inside a string.
3. **Bindings are an array of constructor calls, not a map.** Given up: map-literal brevity for
   the simplest case. Taken for nesting, for the three kinds, and because `eql` already answers
   this question the same way.
4. **Parsed at load, rendered to a `Result`.** Given up: compile-time checking of holes, which a
   module cannot have. Taken because load time is where `repository.query`'s mistakes already
   surface.

## 11. What is left before building

1. **Decide `@else`.** It adds no expression grammar and removes the `@if a` / `@if not_a` pair,
   and it is still one more construct. Recommended: yes.
2. **Write `derive_row` both ways** -- `push`/`concat` and template -- before committing to either.
   `Row` emits far more shape than `Ord`, with nesting and `Option` handling, and it is the
   generator that will say whether §4's marshalling holds up or whether `@if` has to widen. This
   is the measurement the rest of the design should wait for.
3. **Then port the generators**, and delete each C version as its Ziman replacement lands.
