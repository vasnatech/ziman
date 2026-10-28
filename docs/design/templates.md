# A template module: the shape of generated code, visible in the source

Status: **built** (`lib/core/template.zm`), and in use by three generators. Written because
`#[derive(Ord)]` had just been ported out of C and the port made the real problem visible: a
generator written as `push`/`concat` hides the thing it exists to produce.

**How to read this.** §§1-11 are the design as first written, kept as written. §§12-16 are what
building it changed, in the order it changed them -- including two conclusions that were wrong
(§14's "conversions cannot be templates", §15's seven scattered vals) and are corrected there
rather than quietly edited above. The three sections a reader wants *current* rather than
historical -- the syntax, the data model and the API -- have been brought up to date in place,
each saying what moved and where the reasoning for the move lives.

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
sheet      := { template }
template   := "@template" name NEWLINE { line } "@end" NEWLINE
hole       := "@{" name { "." name } "}"
directive  := "@for" ident "in" name | "@choose" name | "@when" tag | "@otherwise"
            | "@yield" name | "@end"
escape     := "@@"
```

A directive **occupies its whole line** and emits nothing, not even the newline; every other line
of a template's body carries its own, including the last. Everything else is literal text.

**This is the grammar as built.** The first version had no `@template` (a source was one
anonymous template), no `@choose`/`@when`/`@otherwise` and no `@yield`, and it had `@if`/`@else`
in their place -- which were built, went unused by two generators, and were removed. §15 has the
argument and §16 the evidence that removing them was right.

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
pub fn rows(name: string, value: [Row]) -> Binding
pub fn row(cells: [Binding]) -> Row
```

There was a `flag(name, value: bool)` too, read only by `@if`, and it went when `@if` did (§15):
a `@choose` reads a *tag*, which is a `text`. **The binding kinds are `text` and `rows`**, which
is a simplification of the data model and not only of the grammar.

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

```ziman
pub fn parse(source: string) -> Result<Sheet, string>
pub fn must_parse(source: string) -> Sheet
pub fn render(sheet: Sheet, name: string, bindings: [Binding]) -> Result<string, string>
```

with the parse living in a module-level `val`:

```ziman
val sheet = template.must_parse("""...""")
```

`render` naming its template is what lets one `val` serve a whole generator; §15 is why a source
is a *sheet* rather than one anonymous template.

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
* `@for` over a name bound to text, or `@choose` over one bound to rows
* `@{r.cell}` where `r` is not a loop binder, or `cell` is not in the row
* `@end` with nothing open, or a template ending with something open
* a name the sheet has no template for -- listing the ones it has
* **a tag matching no `@when`, with no `@otherwise`** -- at render, naming the tag and the cases.
  This one cannot be a parse-time check: the tag is a value.
* `@when` or `@otherwise` outside a `@choose`; a second `@otherwise`; a `@choose` with no `@when`
* text between `@choose` and its first `@when`
* `@template` inside a `@template`; two templates sharing a name; a bare `@template` with no
  name; text outside any `@template`

The first two are the reason `render` answers a `Result` rather than a `string`. The last group
arrived with sheets (§15); `@if`'s own refusals went with it.

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
   Three of five are ported (§§13, 14, 16) and **none of the C is deleted yet**, because nothing
   calls the Ziman ones -- see §17.

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

## 14. `derive_row`: the scale question, answered and narrower than expected

§13 left one thing open -- whether templates pay for a generator bigger than `Ord`. `Row` is
that generator: two functions, every scalar kind, the short-spelled bundled types, and `Option`.
It was ported on 2026-10-28 and is **byte-identical to the C on the first run**, over a ten-field
struct covering a `Uuid`, all six scalars, an `Instant` and three shapes of `Option` (over a
string, over a flat-named type, over a user type).

### The answer: every emitted shape is a template, including the recursive ones

**The first version of this section said the conversions could not be templates, and it was
wrong.** It argued that `Option<U>` emits *U's own conversion inside it*, so the text is
recursive and a template cannot hold a shape that contains itself. The second half of that does
not follow from the first: **the recursion is in the call, not in the shape.** Each level of an
`Option` has a perfectly fixed shape --

```
    var @{dst} = Option<@{elem}>.None
    if !SqlValue.is_Null(@{src}) {
@{nested}
    @{dst} = Option<@{elem}>.Some(@{inner})
    }
```

-- with a hole where the nested conversion's text goes, which is the same arrangement
`from_row_body` already used for `@{f.read}`. The recursive call happens in Ziman and hands its
result in as a string.

The mistake was visible in the file and I did not look: the `Option` branch was a sixteen-line
`concat` of fragments after `--fmt` broke it up, which is precisely the shape this module exists
to remove. It took a reader asking *"why is there code in a string?"*.

| | C | Ziman |
|---|---|---|
| the two function bodies | 42 lines of `cb_appendf` | 23 lines of template |
| the five conversion shapes | 86 lines | **18 lines of template** |
| dispatch, recursion, temp names | included above | 60 lines of ordinary Ziman |

So `derive_row` holds **six templates and thirteen `concat` calls**, and all thirteen build an
*expression* -- a function name, a `str(..)` wrapper, a qualified `Option.unwrap_Some(..)` --
never a line of output. That is the rule the port actually found, and it is sharper than the one
§4 guessed at:

> **Every line of generated output comes from a template. `concat` builds the values that go into
> the holes, and nothing else.**

Which also answers the scale question without qualification: templates paid for the whole of
`Row`, not half of it.

### `@if` is still unused, and §4's own example was wrong about why

§4 argued the no-expressions rule using `derive(Row)`'s `Option` column, and §13 predicted that
`derive_row` was where `@if` would finally be exercised. **It was not.** An `Option` field and a
required one do not share a template with a branch in it: they are two different shapes, so they
are two templates, chosen by an `if` in Ziman. The example in §4 is a fair picture of the
*output* and a misleading picture of where the choice lives -- the same confusion, in a smaller
form, as the one corrected above.

**Two generators in, `@if` and `@else` have no users.** They are not thereby wrong -- a generator
whose output varies without recursing is an ordinary thing to expect, and `Properties` with its
required-versus-optional keys is the next candidate -- but the construct is currently paid for
and unused, which is the kind of thing that should be said out loud rather than discovered later.
If `derive_json` and `derive_properties` also come back without using it, that is an argument for
taking it out, not for finding it a user.

### What `meta` learned

`Member.type_name: string` was replaced by `Member.type: MetaType`. The string had been
speculative -- §1's "a field nobody reads is a promise that cannot be taken back" -- and the
first generator to read a type at all proved it the wrong shape: a conversion switches on the
kind, asks whether it is an `Option`, and then needs the element. Keeping `meta` small is what
made that a one-fixture change rather than a migration.

## 15. Sheets, `@choose` and `@yield`: making a generator's templates one document

Status within this document: **design only.** §§1-14 are built; nothing in this section is.

### What is wrong now

`lib/core/derive_row.zm` holds **seven `must_parse` vals scattered over a hundred lines**, with
helper functions between them. Every line of output comes from a template, which §14 was pleased
about, and the file is still hard to read. Three separate faults, which want three different
answers:

1. **Scatter.** Seven `val`s, none adjacent to the others, so the output vocabulary is assembled
   in a reader's head.
2. **Composition is invisible.** `@{f.read}` says *something* goes here; it does not say what can.
3. **Dispatch is invisible.** Which of the five shapes applies is decided in `emit_from_sql`, 150
   lines away from any of them.

**The tell is in the file's own comments.** Each template carries a pasted copy of the assembled
output, guarded by `tools/check_template_examples.py`. That guard is worth keeping, but needing
the comment at all is the admission: *if a file has to show what its templates produce, the
templates are not showing it.*

### Three changes

```
sheet      := { template }
template   := "@template" name NEWLINE { line } "@end" NEWLINE
directive  := "@for" ident "in" name | "@choose" name | "@when" tag | "@otherwise"
            | "@yield" name | "@end"
hole       := "@{" name { "." name } "}"
```

**`@template name` … `@end`** -- a source is a **sheet** of named templates, never a lone
anonymous one. One `must_parse` per generator, and the vocabulary reads top to bottom as a
document.

**`@choose tag` / `@when value` / `@otherwise`** -- dispatch on a bound **string**. This is the
one that answers fault 3, and it keeps the rule §4 holds: `@choose` reads a bound value and
`@when` compares it to a literal tag. No operators, no arithmetic, no calls. Ziman still decides;
the template shows **what the alternatives are**, which is the half a reader wants and the half
that was missing.

**`@yield name`** -- a hole holding a *block* rather than a value, and the difference is not only
documentation: **`@yield` prefixes every line of the block with its own indentation.** Written at
column 0 it is exactly what `@{name}` does today; written indented it produces correctly indented
output, which is how the mis-indentation inside the generated `if` gets fixed -- opt-in, so
byte-identity with the C survives until the bridge lands and the improvement is taken
deliberately.

### `derive_row`, whole

Seven vals and a hundred lines become **one sheet of four templates**:

```
@template from_row
pub fn @{snake}_from_row(row: Row) -> Result<@{name}, DbError> {
@for f in fields
    val field_@{f.name}_raw = try row_get(row, "@{f.name}")
@yield f.read
@end
    return @{name} {
@for f in fields
        @{f.name}: field_@{f.name}@{f.comma}
@end
    }
}

@end

@template read
@choose shape
@when simple
    val @{dst} = try @{call}(@{src})
@when option
    var @{dst} = Option<@{elem}>.None
    if !SqlValue.is_Null(@{src}) {
@yield nested
    @{dst} = Option<@{elem}>.Some(@{inner})
    }
@end
@end

@template to_params
pub fn @{snake}_to_params(v: @{name}) -> [SqlValue] {
    val __derive_params = mutable_array_new<SqlValue>()
@for f in fields
@yield f.write
    push<SqlValue>(__derive_params, field_@{f.name}_sql)
@end
    return snapshot(__derive_params)
}

@end

@template write
@choose shape
@when value
    val @{dst} = SqlValue.@{ctor}(@{src})
@when converted
    val @{dst} = @{call}(@{src})
@when option
    var @{dst} = SqlValue.Null
    if Option.is_Some(@{src}) {
@yield nested
    @{dst} = @{inner}
    }
@end
@end
```

That is the whole of what `derive_row` writes, in forty lines, in the order a reader meets it.
The Ziman beside it keeps exactly what it had: the recursion, the temporary names, the per-kind
tag, and the `flat_names` table. **What moves is not logic -- it is the five shapes, out of five
separate `val`s and into one place.**

Two things fall out that were not the point:

* **The comments showing assembled output stop being necessary**, because the sheet shows it.
  They can go, and `check_template_examples.py` with them -- though keeping one worked example
  per *generator* (rather than per template) is probably still worth it.
* **The trailing-newline trap disappears.** A text block drops the newline before its closing
  `"""`, so `}\n\n` needs two blank lines in the source today -- a bug I shipped and the fixture
  caught (§13). With `@end` as the terminator the body is simply the lines between, each with its
  newline, and the rule is one a reader can see.

### `@if` and `@else` are removed

They have **no users after two generators**, and `@choose` covers them: a boolean condition
becomes a tag the generator computes (`"option"` against `"simple"` above, rather than a flag).
Carrying two dispatch constructs when one is unused is how a language accretes, and the honest
reading of zero users is that `@if` was the wrong shape rather than an unlucky one.

**`template.flag` goes with it.** Nothing else reads a boolean binding, so the binding kinds drop
to **`text` and `rows`**, which is a simplification of the data model and not only of the grammar.

What this gives up: `@if optional` / `@else` reads better at a single site than `@choose shape` /
`@when option` / `@when simple`. The trade is one construct instead of two, and a tag that says
*which* shape rather than merely *whether*.

### The API

A source is always a sheet, so the entry points change shape:

```ziman
pub fn parse(source: string) -> Result<Sheet, string>
pub fn must_parse(source: string) -> Sheet
pub fn render(sheet: Sheet, name: string, bindings: [Binding]) -> Result<string, string>
```

`render` naming its template is what lets one `val` serve a whole generator. An unknown name is a
refusal listing what the sheet holds -- the same shape as every other refusal here.

### Refusals the new constructs bring

* `@when` or `@otherwise` outside a `@choose`; a second `@otherwise` for one `@choose`
* a `@choose` with no `@when` at all
* **a tag matching no `@when`, with no `@otherwise`** -- at render, naming the tag and listing the
  cases the template has. This one cannot be a parse-time check: the tag is a value.
* `@choose` or `@yield` over a name bound to the wrong kind
* `@template` inside a `@template`; two templates sharing a name; text outside any `@template`

### Built, 2026-10-28

All three, with both generators ported in the same change and both fixtures unchanged byte for
byte. `derive_ord` is one sheet of two templates, `derive_row` one sheet of four; seven
`must_parse` vals across the two files became two.

Three things the build settled that the design above left open or got wrong:

* **`@yield`'s re-indentation shipped**, and is used at column 0 everywhere, so the generated
  code is byte-for-byte what the C writes and the mis-indentation inside the `if` is one edit
  away rather than a rewrite. Indenting the directive is the whole change.
* **Every body line carries its newline, including the last** -- which is what the section above
  said, and is simpler than it sounds: `@end` is a terminator rather than a string delimiter, so
  the rule needs no exception. It also deleted a helper. The *first implementation* kept the old
  "last line carries none" rule out of habit and was off by a blank line per generated function,
  which the fixtures caught immediately.
* **A bare `@template` with no name needed its own refusal.** Without one it fell through to
  "every line outside a '@template' must be blank", which is true and unhelpful -- the kind of
  message that is correct about the grammar and silent about the mistake.

And one thing worth recording because it reads like a bug and is not: an `@for` that swallows its
template's `@end` is reported as *the template* being unclosed, not the `@for`. That is the
honest diagnosis -- the `@end` did close the `@for` -- and the fixture pins it so the next
reader does not try to "fix" it.

## 16. `derive_properties`: `@choose` holds, and `@for` cannot build a list

The third generator, ported 2026-10-28 and byte-identical to the C. §15 named it as the test of
`@choose`, because its two field shapes are a **genuine either/or with no recursion under them**
-- a required key reads in one line, an optional one tests and then reads -- which is precisely
the case the removed `@if` was designed for.

**`@choose` serves it, and reads better than `@if` would have.** `@when required` and
`@when optional` *name* the two shapes; `@if optional` / `@else` only said whether. Removing
`@if` was the right call, and this is the generator that could have argued otherwise.

### The new limit: a directive owns its whole line

`<name>_keys` returns its list on one line:

```ziman
pub fn server_keys(prefix: string) -> [string] {
    return [properties.key(prefix, "port"), properties.key(prefix, "host")]
}
```

**`@for` cannot build that.** A directive occupies its whole line -- which is what keeps the
grammar line-oriented and readable -- so a loop emits whole lines and never a comma-separated run
inside one. The generator joins the entries in Ziman and binds the result as a single
`@{entries}` hole, so the list's *shape* is the one thing in this generator a reader cannot see
in the template.

That is a real gap, and the obvious fix is an inline form -- `@{join entries with ", "}` or a
`@list` directive. **Not proposed yet**: one occurrence across three generators is not enough to
design against, and the wrong inline construct is how a line-oriented grammar stops being one.
Worth revisiting when a second generator wants it; `derive_json`'s object literal is the likely
candidate.

## 17. Where this leaves the generators

Three of the five are Ziman and byte-identical to the C: `Ord` (§13), `Row` (§14),
`Properties` (§16). Each carries **`#![generator(X)]`** and exports **`generate`**, so the link
from a derive target to its generator is a declaration the compiler checks rather than a name it
concatenates -- see `docs/design/user-generators.md`, and note that the attribute is *checked*
and not yet *dispatched on*.

**Nothing calls any of them.** `#[derive(X)]` is still a `strcmp` over five names in
`src/derive.c`, and no C references `lib/core/derive_*.zm`. The templates and these three
generators are a verified replacement waiting for one thing, and it is worth being plain that
until it lands they are three bundled modules of dead weight whose only consumer is their own
fixtures.

What is left, in order:

1. **`derive_json`**, the last and largest of the five, and the one that says whether §16's
   inline-list gap is a one-off or wants a construct.
2. **The bridge** -- `docs/design/user-generators.md` §12: a C function building a `meta.Decl`
   value, and per-generator-module initialisation. It is shared with user-defined derives, which
   is why it is written up there rather than here.
3. **Then delete the C**, one generator at a time, each as its replacement starts running.
