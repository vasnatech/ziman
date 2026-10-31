# `derive_xml`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Xml)]`, written in Ziman: a struct's `<name>_to_xml`/`<name>_from_xml` over
`xml.Document`, and the `_to_element`/`_from_element` pair a nested struct composes from.

**Why this is not `#[derive(Json)]` with a different writer.** XML is not a `Json`-shaped tree.
A field can be an attribute or a child element, elements are ordered and repeat under one name,
and a name can be namespaced -- none of which a `Json` object carries. Routing XML through
`Json` would have to pick one convention for all of that and then be wrong for every schema
that chose the other, which is why `codec.TreeFormat` deliberately excludes XML. So this
generator writes the `Document` model directly, and `codec.encoder`/`codec.decoder` take the
result: `T -> [byte]` and `[byte] -> T`, the interface every format joins by.

**The wire names are `#[derive(Json)]`'s.** A field's element or attribute is `camel_case` of
its name, which is the key the JSON derive uses -- so one model serialised either way names its
parts the same, and a client switching `Accept` sees one vocabulary rather than two.
`#[xml(name: "...")]` overrides one field's, `#[xml(attribute: true)]` makes a scalar field an
attribute rather than a child element, and `#[xml(skip: true)]` leaves it off the wire
altogether -- one attribute, every choice an argument of it.

**A deriving module must `use xml`**, as a `#[derive(Json)]` one must `use json`: the spliced
code names `xml.Element` and `xml.text_element`.

Deliberately narrower than `#[derive(Json)]`, and **every gap is refused by name rather than
mis-generated**: an enum, a map, a set, a generic parameter and a non-scalar attribute each get
their own message. The JSON derive grew its coverage over many commits; this one starts with
the shapes a request or response body actually has, and says where it stops.

`use derive_xml` -- or `use derive_xml.Name` to bring one item in bare.

### `attributes`

```
pub fn attributes() -> [meta.AttrSpec]
```

**One attribute, named after the format, and every choice an argument of it** -- this was
`#[xml_attribute]` plus `#[xml_name("..")]` for a day, and the pair was wrong twice over. The
name said what one of them did, so renaming a *child element* (the common case:
`code` -> `<product-code>`) had nowhere to live once `name:` belonged to `xml_attribute`; and
the next option would have been a third name rather than a third argument. `#[xml(..)]` has
room for all of it, and a reader meets one name instead of counting them.

`attribute:` and `element:` are **two names for one choice**, so either may be written with
either value and giving both must agree; a child element is the default and `element: true` is
its explicit spelling. The three `skip` arguments are separate booleans rather than one
`skip: "out"`, because an argument has one kind in the spec and a `skip` that took a bool here
and a string there could not be declared at all.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```
