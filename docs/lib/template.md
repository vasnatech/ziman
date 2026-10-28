# `template`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

Templates for generated code: the shape of the output, visible in the source.

Written for the derive generators (`lib/core/derive_*.zm`), which assemble Ziman source. A
generator built from `push`/`concat` hides the thing it exists to produce. See
docs/design/templates.md -- §12 for the comparison this module was built on, §15 for why a
source is a *sheet* of named templates rather than one anonymous one.

```
@template from_row
pub fn @{snake}_from_row(row: Row) -> Result<@{name}, DbError> {
@for f in fields
    val field_@{f.name}_raw = try row_get(row, "@{f.name}")
@yield f.read
@end
}
@end

@template read
@choose shape
@when simple
    val @{dst} = try @{call}(@{src})
@when option
    var @{dst} = Option<@{elem}>.None
@end
@end
```

**`@` and not `{}`**, which is where this parts company with `format`: a code template's output
is mostly braces, and under `format`'s `{{`/`}}` rule every literal brace would double -- the
common case paying for the rare one. `@` is not a token in Ziman's grammar at all.

**There are no expressions.** `@choose` reads a bound string and `@when` compares it to a
literal tag; a loop runs over bound rows; a hole holds a bound string. Anything that needs
computing is computed in Ziman, where the type checker reads it. That rule is the whole design
-- docs/design/templates.md §4 argues it against the generator that wants the most, and §15
keeps it while making the alternatives visible.

**This module carries no `#[derive(..)]` and uses no generated code, deliberately**: a
generator that needs a generator cannot be loaded. See the design's §8.

`use template` -- or `use template.Name` to bring one item in bare.

## The parsed sheet

### `Sheet`

```
pub struct Sheet {
    templates: [Named]
}
```

A source's templates, by name. **A source is always a sheet**, never one anonymous template:
a generator's output vocabulary belongs in one literal, read top to bottom, which is what §15
of the design is about.

## What a template is given

### `Row`

```
pub struct Row {
    cells: [Binding]
}
```

### `Binding`

```
pub struct Binding {
    name: string
    value: Bound
}
```

A value under a name, as `eql.bind` is -- the same problem, so the same shape.

### `text`

```
pub fn text(name: string, value: string) -> Binding
```

A string for `@{name}`, for `@yield name`, and for the tag a `@choose` reads. **Everything a
template can hold is a string or a list of rows**, so there is no number to format and no
question about how one is formatted.

### `rows`

```
pub fn rows(name: string, value: [Row]) -> Binding
```

The rows a `@for name` repeats over.

### `row`

```
pub fn row(cells: [Binding]) -> Row
```

One row of a `@for`: its cells are reached qualified, as `@{binder.cell}`.

## Parsing

### `parse`

```
pub fn parse(source: string) -> Result<Sheet, string>
```

Reads a sheet, or says what is wrong with it and where.

### `must_parse`

```
pub fn must_parse(source: string) -> Sheet
```

`parse` for a module-level `val`, where a broken sheet is a mistake in a string literal in the
source rather than a condition any caller could act on -- the same reasoning
`repository.query` records for its own `unwrap`.

## Rendering

### `render`

```
pub fn render(sheet: Sheet, name: string, bindings: [Binding]) -> Result<string, string>
```

One of the sheet's templates, with its holes filled.

**Both directions are checked**: a hole naming nothing bound, and a binding the template never
mentions. The second is the more likely one after a template is edited, and it is a question
about the text rather than about this render -- so an unused binding is reported even when the
loop that would have read it has no rows, or the `@when` that would have is not the one chosen.
