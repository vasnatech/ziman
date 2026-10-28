# `template`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

Templates for generated code: the shape of the output, visible in the source.

Written for the derive generators (`lib/core/derive_*.zm`), which assemble Ziman source. A
generator built from `push`/`concat` hides the thing it exists to produce; the same generator
built from a template shows it. See docs/design/templates.md for the design, and its §12 for
the comparison this module was built on.

```
pub fn @{snake}_compare(a: @{self}, b: @{self}) -> int {
@for m in members
    val c_@{m.name} = compare(a.@{m.name}, b.@{m.name})
@end
    return 0
}
```

**`@` and not `{}`**, which is where this parts company with `format`: a code template's output
is mostly braces, and under `format`'s `{{`/`}}` rule every literal brace would double -- the
common case paying for the rare one. `@` is not a token in Ziman's grammar at all.

**There are no expressions.** `@if` takes a bound boolean; a loop runs over bound rows; a hole
holds a bound string. Anything that needs computing is computed in Ziman, where the type
checker reads it. That rule is the whole design and the reason this is 300 lines rather than a
second language -- docs/design/templates.md §4 argues it against the generator that wants the
most.

**This module carries no `#[derive(..)]` and uses no generated code, deliberately**: a
generator that needs a generator cannot be loaded. See the design's §8.

`use template` -- or `use template.Name` to bring one item in bare.

## The parsed template

### `Template`

```
pub struct Template {
    nodes: [Node]
    // Every top-level name the text mentions -- holes, `@for` sources and `@if` flags. Collected
    // at parse time so that `render` can refuse an unused binding, which is a *static* question
    // about the text: counting what a render touched would call a binding unused whenever its
    // loop ran zero times.
    refs: [string]
}
```

A template, parsed. **Parse once, in a module-level `val`** -- `repository.query`'s pattern, and
the same gain: a malformed template fails when the module loads rather than on the first call
that happens to reach it.

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

A string for `@{name}`. **Everything a hole can hold is already a string**, so there is no
number to format and no question about how one is formatted.

### `flag`

```
pub fn flag(name: string, value: bool) -> Binding
```

A boolean for `@if name`, and nothing else reads it.

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
pub fn parse(source: string) -> Result<Template, string>
```

Reads a template, or says what is wrong with it and where.

### `must_parse`

```
pub fn must_parse(source: string) -> Template
```

`parse` for a module-level `val`, where a broken template is a mistake in a string literal in
the source rather than a condition any caller could act on -- the same reasoning
`repository.query` records for its own `unwrap`.

## Rendering

### `render`

```
pub fn render(t: Template, bindings: [Binding]) -> Result<string, string>
```

The template's text with its holes filled.

**Both directions are checked**: a hole naming nothing bound, and a binding the text never
mentions. The second is the more likely one after a template is edited, and it is a question
about the text rather than about this render -- so an unused binding is reported even when the
loop that would have read it has no rows.
