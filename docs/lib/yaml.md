# `yaml`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A strict YAML 1.2 subset on top of `json`: a document is a `Json` tree, so any
`#[derive(Json)]` type decodes from it with its own `_from_json`. Written in pure Ziman.

Marked `#![experimental]` below: unlike `json`, no real Ziman program depends on this
module yet, so treat a gap or an edge case as more likely here than elsewhere in the
library.

Reads: block mappings and sequences (indentation, spaces only), `- key: v` items, flow
`[a, b]` / `{a: b}` (nesting, multi-line), plain / 'single' / "double" quoted scalars,
literal `|` and folded `>` block scalars (with `-` and `+` chomping), comments, and an
optional leading `---`. Scalars follow the YAML 1.2 core schema: `null`/`~`/empty, `true`/`false`,
integers (`42`, `0x1f`, `0o17`), floats (`1.5`, `2e3`); anything else is a string. `yes`/`no`/`on`/`off`
are strings. A number with a leading zero (`007`, a zip code) stays a string.
Not supported, and reported as errors rather than misread: anchors and aliases (`&a`, `*a`),
tags (`!!str`), the `<<` merge key, complex keys (`? `), multiple documents, tab indentation,
block-scalar indentation indicators, `.inf` / `.nan`.

Writes block style; a string that would read back as another type (`"42"`, `"true"`, `""`,
`"null"`) or that needs it is double-quoted, so the output re-reads as the same tree. A
`JNull` is written `null`; a whole-valued float keeps a `.0`.

`use yaml` -- or `use yaml.Name` to bring one item in bare.

## Example

```
use json
use json.Json
use yaml

// A YAML document becomes a Json tree, the same as TOML or JSON, so one set of helpers reads all three.
fn main() {
    val doc = unwrap(yaml.parse("""
        name: demo
        ports:
          - 80
          - 443
        limits: {cpu: 2, memory: 512}
        """))

    print(unwrap(json.require_string(doc, "name")))
    print(json.format(doc))
    print(trim(yaml.format(doc)))

    // Anchors, tags and multiple documents are errors, not silently misread.
    print(is_ok(yaml.parse("a: &x 1\nb: *x")))
}
```

Output:

```
demo
{"name":"demo","ports":[80,443],"limits":{"cpu":2,"memory":512}}
name: demo
ports:
  - 80
  - 443
limits:
  cpu: 2
  memory: 512
false
```

## Block structure

### `parse`

```
pub fn parse(text: string) -> Result<Json, string>
```

Parses a YAML document (the subset described above) into a `Json` tree; an empty document is
`JNull`. An Err carries `line N: ...` for a syntax error, a duplicate key, bad indentation, or
something unsupported (anchors, aliases, tags, the `<<` merge key, multiple documents, tab
indentation), so nothing is silently misread.

## Writing

### `format`

```
pub fn format(j: Json) -> string
```

Writes `j` as block-style YAML: maps as `key: value`, sequences as `- item`, empty containers
as `{}` / `[]`. A string that would read back as another type (`"42"`, `"true"`, `""`, `"null"`)
or that needs it (a leading `-`, a `: ` inside) is double-quoted; floats are written with full
precision and keep a `.0`. Reading the output back gives the same tree.
