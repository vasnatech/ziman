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

`@if` and `@else` pair with each other and nothing else. The case that wants them is a field
whose two readings are genuinely different text -- `derive(Row)`'s `Option` column against a
required one, which `emit_from_sql` writes by hand today:

```
@if optional
    var @{dst} = Option<@{elem}>.None
    if !SqlValue.is_Null(@{src}) {
        @{dst} = Option<@{elem}>.Some(@{inner})
    }
@else
    val @{dst} = try sql_as_@{kind}(@{src})
@end
```

**There is no `@elif`**, and that is the line rather than an omission: a second condition is a
second flag, and a template needing three of them is a template that should have been two
templates chosen in Ziman. `@elif` is also the first construct that cannot be read without
scanning upward, which is the property `@if`/`@else`/`@end` on their own lines exists to keep.

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
* `@else` with no `@if` open, a second `@else` for one `@if`, or an `@else` closing a `@for`
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
3. **`@else` exists; `@elif` does not.** Given up: a three-way choice written in one place.
   Taken because `@else` adds no expression grammar and removes the `@if flag` / `@if not_flag`
   pair, which is two bindings and two chances for them to disagree -- where `@elif` is the first
   construct a reader cannot understand without scanning upward for what it continues. A third
   branch is two templates, chosen in Ziman.
4. **Bindings are an array of constructor calls, not a map.** Given up: map-literal brevity for
   the simplest case. Taken for nesting, for the three kinds, and because `eql` already answers
   this question the same way.
5. **Parsed at load, rendered to a `Result`.** Given up: compile-time checking of holes, which a
   module cannot have. Taken because load time is where `repository.query`'s mistakes already
   surface.

## 11. What is left before building

1. **Superseded by §12**, which does the comparison on `derive_ord` instead and reaches a
   narrower answer: the no-expressions rule holds, and whether templates pay at scale is still
   open. The order it recommends is to build the module against `derive_ord` -- whose output is
   already pinned byte for byte by `tests/lang/derive_ord_in_ziman.out` -- and port `derive_row`
   after, with that fixture as the guard.
2. **Then port the generators**, and delete each C version as its Ziman replacement lands.

## 12. The same generator, both ways

§11 originally said to write `derive_row` both ways and judge from that. **That was the wrong
experiment**, and `derive_ord` is the right one: it already exists in `push`/`concat` form *and*
`tests/lang/derive_ord_in_ziman.out` pins its exact output, so rewriting it changes one variable
and the answer is checkable. Two fresh implementations of `derive_row` would compare two pieces
of new code, written by someone who wants one of them to win.

What follows is the rewrite on paper. It has not been built.

### The struct half

Today, 10 lines:

```ziman
fn struct_compare(d: meta.Decl) -> string {
    val out = mutable_array_new<string>()
    push(out, signature(d))
    for m in d.members {
        push(out, concat("    val c_", m.name, " = compare(a.", m.name, ", b.", m.name, ")\n"))
        push(out, concat("    if c_", m.name, " != 0 {\n        return c_", m.name, "\n    }\n"))
    }
    push(out, "    return 0\n}\n\n")
    return concat(pieces: snapshot(out))
}
```

With a template, 9 lines of template and 8 of marshalling:

```
pub fn @{snake}_compare@{bounds}(a: @{self}, b: @{self}) -> int {
@for m in members
    val c_@{m.name} = compare(a.@{m.name}, b.@{m.name})
    if c_@{m.name} != 0 {
        return c_@{m.name}
    }
@end
    return 0
}
```

```ziman
fn struct_compare(d: meta.Decl) -> Result<string, string> {
    val members = mutable_array_new<template.Row>()
    for m in d.members {
        push(members, template.row([template.text("name", m.name)]))
    }
    return template.render(struct_body, [
        template.text("snake", d.snake_name),
        template.text("bounds", meta.type_params_bounded(d, ": Ord")),
        template.text("self", meta.type_text(d)),
        template.rows("members", snapshot(members))
    ])
}
```

**The template version is longer.** 17 lines against 10, and it gives the function a `Result` the
caller has to thread. What it buys is that the eight lines of generated code are visible as eight
lines. For a shape this small that is close to a wash, and pretending otherwise would be the
easiest way to get this decision wrong.

### The enum half, which is where it pays

Today: **36 lines** of `push`/`concat`, the generated control flow spread across eleven string
fragments. With a template the shape is one block:

```
pub fn @{snake}_compare@{bounds}(a: @{self}, b: @{self}) -> int {
    var ra = 0
    var rb = 0
@for r in ranked
    if @{name}.is_@{r.variant}(a) {
        ra = @{r.rank}
    }
    if @{name}.is_@{r.variant}(b) {
        rb = @{r.rank}
    }
@end
    if ra == 0 && !@{name}.is_@{first}(a) {
        ra = @{last}
    }
    if rb == 0 && !@{name}.is_@{first}(b) {
        rb = @{last}
    }
    if ra < rb {
        return -1
    }
    if ra > rb {
        return 1
    }
@for p in payloaded
    if @{name}.is_@{p.variant}(a) {
        return compare(@{name}.unwrap_@{p.variant}(a), @{name}.unwrap_@{p.variant}(b))
    }
@end
    return 0
}
```

with marshalling that is all list-building and no text:

```ziman
val ranked = mutable_array_new<template.Row>()
for i in 0..prelude.len(d.members) - 1 {
    push(ranked, template.row([
        template.text("variant", d.members[i].name),
        template.text("rank", str(i))
    ]))
}
val payloaded = mutable_array_new<template.Row>()
for m in d.members {
    if m.has_payload {
        push(payloaded, template.row([template.text("variant", m.name)]))
    }
}
```

That is the comparison the decision should rest on: **the half with real shape is the half that
wins**, and it wins on readability before it wins on size. A reader of the template sees the
function `compare` will dispatch to; a reader of the 36 lines sees eleven fragments and has to
run it.

### What the exercise actually settled

**Neither half of `derive_ord` needs `@if`.** Not one conditional, in the generator that §4 used
to argue the no-expressions rule. That is worth being precise about, because it is easy to
misread as the rule being untested: §4's argument is about what *the template* would need **if
the logic lived there** -- an index, `len()`, a subtraction, a filter. With the logic in Ziman,
`@{...}` and `@for` are the whole of it, and `@if` is not reached.

So `derive_ord` answers one of the two questions and not the other:

* **Does the no-expressions rule survive?** Yes, comfortably, on the generator chosen as its
  hardest case. The index, the `count - 1` and the `has_payload` filter are four lines of
  ordinary Ziman that the type checker reads.
* **Do templates pay for themselves?** Not proven. The struct half is a wash and the enum half is
  one data point. `derive_row` and `derive_json` are where the answer is, and they are also where
  `@if` finally gets exercised -- an `Option` column reads differently from a required one, which
  is §4's example and is not hypothetical.

**The revised order**: build the module against `derive_ord`, whose expected output is already
pinned byte for byte, and only then port `derive_row` -- with the fixture as the guard rather
than the judgement.

## 13. Built, 2026-10-28

`lib/core/template.zm`, and `lib/core/derive_ord.zm` rewritten on it. The guard is the one §12
asked for: `tests/lang/derive_ord_in_ziman.out` pinned the generator's output byte for byte
before the rewrite, and it is unchanged after -- both halves, struct and enum, generic and plain,
with and without payloads.

What the build confirmed and what it corrected:

* **§12's measurements held.** The struct half is 17 lines against 10 and the enum half's eleven
  string fragments are one readable block. Neither needs `@if`, so the construct ships unused by
  its first client and exercised only by `tests/lang/template_refusals`.
* **The trailing newline is the template's**, and it is easy to get wrong: a text block drops the
  newline before its closing `"""`, so the `}\n\n` every generated function ends with needs
  *two* blank lines in the source. The first attempt had one and differed from the C by a blank
  line per function -- caught by the fixture, which is the whole reason it was written first.
* **A module whose strings contain Ziman broke `tools/gen_lib_manual.py`.** It reads declarations
  off raw lines, which was safe while no module's strings looked like code, and
  `pub fn @{snake}_compare..` inside a template was read as a declaration. Fixed by honouring the
  one rule the syntax gives -- a `"""` ending a line opens a block, the next closes it -- rather
  than by loosening the regex, which would have put template text in the manual. **Any module
  holding a code template will hit this**, so it is the generators' problem and not this one's.

Still open, unchanged: whether templates pay at scale. `derive_row` and `derive_json` are where
that is answered, and where `@if` is first used in anger.
