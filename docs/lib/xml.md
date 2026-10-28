# `xml`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

XML 1.0 as a document tree: a reader, a writer, and a small path finder.
Written in pure Ziman, like `toml` and `yaml`; `json` and `csv` are native because they
sit on hot paths (a JSON body on every request), and XML does not.

Marked `#![experimental]` below: new, and no Ziman program depends on it yet.

Unlike `yaml` and `toml`, this does **not** parse into a `Json` tree, because XML does not
fit one. `<p>Take <b>2</b> tablets</p>` interleaves text and elements, so children are an
ordered list rather than a map; `<dose value="2"/>` and `<dose><value>2</value></dose>` are
different documents, so attributes are not children; and a name's identity is its namespace
URI, not its prefix. Flattening any of that into `Json` is the usual way an XML library
becomes a one-way street -- it can write a document it cannot read back.

## What it will not do, on purpose

**There is no DTD.** `<!DOCTYPE ...>` is a parse error, not an option that defaults to off.
That single decision removes the billion-laughs entity expansion (a kilobyte of input
becoming gigabytes of output) and XXE (`<!ENTITY x SYSTEM "file:///etc/passwd">`, and the
same trick aimed at an internal URL) by construction rather than by configuration, so there
is no flag for a caller to turn off and no default for a future refactor to get wrong. The
five predefined entities and numeric character references still work: those are XML itself,
not DTD. Nothing here reads a file or opens a socket.

The other limits are in `XmlLimits` and apply to every parse: input size, nesting depth and
attribute count. A byte cap alone would not be enough -- 100 KB of `<a>` is fifty thousand
levels deep -- so depth is counted separately, at the same 512 `parse` already uses.

Also absent, and not planned: XSD or any other validation, XPath (`find` below is child-name
steps, nothing more), and XML 1.1.

## Namespaces

`xmlns` and `xmlns:p` stay in `attributes` where they were written, *and* are used to resolve
names while reading, so `Name.namespace` holds the URI a name resolved to. An unprefixed
attribute is in no namespace, which is what the spec says and what trips people up -- a
default `xmlns` applies to elements only.

The writer does not consult `namespace`: it emits `prefix:local` when a prefix is set and
`local` otherwise, and never invents a declaration. So a tree that came from `parse`
writes back unchanged, and a tree built by hand needs its `xmlns` written as an ordinary
attribute, once, on the element that scopes it. `namespace` is what `==` and `find_ns`
compare; `prefix` is only ever a hint to the writer.

`use xml` -- or `use xml.Name` to bring one item in bare.

## Example

```
use xml

// XML is a tree of its own, not a Json tree: text and elements interleave, attributes are not
// children, and a name is identified by its namespace rather than its prefix.
//
// Navigation answers Option, because any step can be absent; parsing and writing answer Result.
fn demo() -> Result<unit, string> {
    val doc = try xml.parse("""
        <?xml version="1.0" encoding="UTF-8"?>
        <record xmlns="urn:hl7-org:v3">
          <patient><name>Ann Lee</name></patient>
          <section code="allergies">
            <entry substance="penicillin" severity="high"/>
            <entry substance="latex" severity="low"/>
          </section>
          <note>Take <em>2</em> tablets &amp; rest.</note>
        </record>
        """)

    // `find` follows child names; namespaces are resolved but do not have to be spelled out.
    // Two attributes, two Options, one `if` -- a later part of the condition sees the earlier binds.
    for entry in xml.find(doc.root, "section/entry") {
        if Some(substance) = xml.attr(entry, "substance") && Some(severity) = xml.attr(entry, "severity") {
            print(format("{}: {}", substance, severity))
        }
    }
    if Some(section) = xml.find_first(doc.root, "section") && Some(code) = xml.attr(section, "code") {
        print(code)
    }

    // Mixed content keeps its order, and `text_of` flattens it.
    if Some(note) = xml.child(doc.root, "note") {
        print(xml.text_of(note))
    }
    if Some(namespace) = doc.root.name.namespace {
        print(namespace)
    }

    // Building and writing. Most fields of a record are optional, so `opt_element` makes one
    // that may be absent, `req_element` one that is always there, and `filter_present` keeps
    // whichever turned up -- no `if` per field. Escaping is the writer's job, so the `&` below
    // cannot break the output.
    val missing = Option<string>.None
    val built = xml.document(xml.element(xml.name("visit"), [xml.attribute("id", "7")], filter_present<xml.Node>(
        xml.req_element("reason", "cough & fever"),
        xml.opt_element("temperature", Option<string>.Some("38.1")),
        xml.opt_element("notes", missing)
    )))
    print(trim(try xml.format(built)))

    // There is no DTD, so the shapes that make XML dangerous simply do not parse.
    print(is_ok(xml.parse("<!DOCTYPE x [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><x>&e;</x>")))

    // The limits are a value you can tighten for input you do not control.
    val strict = xml.XmlLimits { max_bytes: 4096, max_depth: 8, max_attributes: 16 }
    print(is_ok(xml.parse("<a><b><c/></b></a>", strict)))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("xml example failed: {}\n", e)
    }
}
```

Output:

```
penicillin: high
latex: low
allergies
Take 2 tablets & rest.
urn:hl7-org:v3
<?xml version="1.0" encoding="UTF-8"?>
<visit id="7"><reason>cough &amp; fever</reason><temperature>38.1</temperature></visit>
false
true
```

## The model

### `Name`

```
pub struct Name {
    namespace: Option<string>
    local: string
    prefix: Option<string>
}
```

An expanded name: the namespace URI it resolved to (None when in no namespace), the local
part, and the prefix as written. Two names are the same name when `namespace` and `local`
match, whatever the prefixes; `same_name` says so, since `==` on the struct would compare
the prefix too.

### `Attribute`

```
pub struct Attribute {
    name: Name
    value: string
}
```

### `Instruction`

```
pub struct Instruction {
    target: string
    data: string
}
```

One piece of an element's content. `CData` is kept apart from `Text` so a document survives
a read/write round trip; the two mean the same thing to a reader of the data.
A `<?target data?>` processing instruction. `data` is everything after the target, verbatim,
and is "" when there is none.

### `Node`

```
pub enum Node {
    Element(Element)
    Text(string)
    CData(string)
    Comment(string)
    ProcessingInstruction(Instruction)
}
```

### `Element`

```
pub struct Element {
    name: Name
    attributes: [Attribute]
    children: [Node]
}
```

### `Declaration`

```
pub struct Declaration {
    version: string
    encoding: Option<string>
    standalone: Option<bool>
}
```

The `<?xml ... ?>` declaration, when the document has one.

### `Document`

```
pub struct Document {
    declaration: Option<Declaration>
    prolog: [Node]
    root: Element
    epilog: [Node]
}
```

`prolog` and `epilog` are the comments and processing instructions before and after the root
element, kept so that writing a parsed document back gives the same text.

### `XmlLimits`

```
pub struct XmlLimits {
    max_bytes: int
    max_depth: int
    max_attributes: int
}
```

The reader's limits. `xml_limits()` has the defaults; pass your own to `parse`.
There is deliberately no `allow_doctype`: see the module header.

### `xml_limits`

```
pub fn xml_limits() -> XmlLimits
```

16 MiB, 512 levels, 256 attributes per element. The depth matches `parse`'s, so the two
readers behave the same way on input built to exhaust a stack.

## Building

### `name`

```
pub fn name(local: string) -> Name
```

A name in no namespace: what you want for an ordinary document.

### `ns_name`

```
pub fn ns_name(uri: string, prefix: string, local: string) -> Name
```

A name in namespace `uri` written with `prefix`. Pass "" for `prefix` to write it unprefixed,
which is right when the element is covered by a default `xmlns`.

### `element`

```
pub fn element(n: Name, attributes: [Attribute], children: [Node]) -> Element
```

An element with attributes and children.

### `empty`

```
pub fn empty(local: string) -> Element
```

An empty element: `<name/>`.

### `text_element`

```
pub fn text_element(local: string, content: string) -> Element
```

An element holding one run of text: `<name>text</name>`. Empty text gives an element with no
children at all rather than one holding an empty `Text`, because that is what reading
`<name></name>` back produces -- so a tree built here and the same tree parsed from its own
output compare equal, which they would not if one carried a child the other never makes.

### `opt_element`

```
pub fn opt_element(local: string, value: Option<string>) -> Option<Node>
```

A child element holding `value`, or nothing at all when there is no value -- the shape of
almost every field in an exported record, where an absent one is left out rather than written
empty. Pair it with `filter_present`, which drops the nothings:


```
xml.element(xml.name("patient"), [], filter_present<Node>(
    xml.opt_element("firstName", p.first_name),
    xml.opt_element("dateOfBirth", p.date_of_birth)
))
```

### `req_element`

```
pub fn req_element(local: string, value: string) -> Option<Node>
```

`opt_element` for a value that is always there, so a required field and an optional one can
stand side by side in one `filter_present` list instead of the required one being lifted into
an `Option` by hand at the call site.

### `attribute`

```
pub fn attribute(local: string, value: string) -> Attribute
```

### `document`

```
pub fn document(root: Element) -> Document
```

A document with no declaration and nothing around the root. `format` writes a
`<?xml version="1.0" encoding="UTF-8"?>` line for a document whose declaration is None, so
the output is still a well-formed file.

## Reading a tree

### `same_name`

```
pub fn same_name(a: Name, b: Name) -> bool
```

Whether two names are the same name: same namespace, same local part, prefixes ignored.

### `attr`

```
pub fn attr(e: Element, local: string) -> Option<string>
```

The value of attribute `local` (in no namespace), or None.

### `attr_ns`

```
pub fn attr_ns(e: Element, uri: string, local: string) -> Option<string>
```

The value of the attribute named `local` in namespace `uri`, or None.

### `attr_or`

```
pub fn attr_or(e: Element, local: string, fallback: string) -> string
```

`attr`, or `fallback` when the attribute is absent.

### `children`

```
pub fn children(e: Element) -> [Element]
```

Every child element, in order.

### `children_named`

```
pub fn children_named(e: Element, local: string) -> [Element]
```

The child elements with local name `local`, in order.

### `child`

```
pub fn child(e: Element, local: string) -> Option<Element>
```

The first child element with local name `local`, or None.

### `text_of`

```
pub fn text_of(e: Element) -> string
```

All the text under `e`, with its elements flattened away and comments and processing
instructions left out: the usual way to read a leaf whose content may be split by a CDATA
section or an entity. `text_of(<p>Take <b>2</b></p>)` is "Take 2".

### `find`

```
pub fn find(e: Element, path: string) -> [Element]
```

The elements reached by following `path`, a `/`-separated list of child steps starting at
`e`'s children: `find(root, "component/section/entry")`. A step of `*` matches any element.
Steps match the **local** name, ignoring namespaces, which is what makes a namespace-heavy
document (a CDA, a SOAP envelope) readable without spelling out a URI at every step; use
`find_ns` when a document mixes vocabularies and the distinction matters. An empty path
returns `[e]`, an unmatched step an empty array -- never an error, so a chain of `find`s on a
document shaped differently than expected gives nothing rather than blowing up.

### `find_ns`

```
pub fn find_ns(e: Element, uri: string, path: string) -> [Element]
```

`find`, restricted to elements in namespace `uri`.

### `find_first`

```
pub fn find_first(e: Element, path: string) -> Option<Element>
```

The first element `find` reaches, or None.

## Reading

### `parse`

```
pub fn parse(text: string) -> Result<Document, string>
```

Parses one XML document with the default limits (`xml_limits()`): at most 16 MiB, 512 levels
of nesting and 256 attributes on an element, and no DTD -- a `<!DOCTYPE>` is an error whose
message says why. An Err carries `line N: ...`.

A leading byte order mark is ignored. Line endings are normalised to `\n`. Namespaces are
resolved as the document is read, so `Name.namespace` holds a URI and `find`/`same_name`
compare on it. Text that `&...;` references expand to is already expanded in `Node.Text`;
only the five predefined entities and numeric character references exist, there being no DTD
to define any others.

```
pub fn parse(text: string, limits: XmlLimits) -> Result<Document, string>
```

`parse` with limits of your own. Tightening `max_bytes` is the usual reason -- a document
arriving from somewhere you do not control has a size you are willing to spend memory on, and
it is smaller than 16 MiB.

```
pub fn parse(data: [byte]) -> Result<Document, string>
```

`parse` over the bytes of a document -- what `fs.read_bytes` gives, or an HTTP body. The
bytes must be UTF-8; invalid UTF-8 is an Err rather than a tree with broken text in it.

```
pub fn parse(data: [byte], limits: XmlLimits) -> Result<Document, string>
```

## Writing

### `format`

```
pub fn format(doc: Document) -> Result<string, string>
```

`doc` as XML text on one line, with a declaration and a trailing newline. A document that came
from `parse` writes back as the same tree -- reading the output gives an equal `Document`.

Unlike `json.format` this can fail, because XML genuinely cannot carry some strings: a name
that is not a legal XML name, a comment containing `--`, a CDATA section containing `]]>`, and
above all text holding a C0 control character, for which XML 1.0 has no escape at all. JSON
writes that last one as `\u0000`; XML has nothing to write, so refusing is the only honest
answer. The message says which rule the tree broke. (NUL specifically never gets this far: a
Ziman `string` cannot hold one, because `bytes_to_string` refuses it at the boundary.)

```
pub fn format(doc: Document, indent: string) -> Result<string, string>
```

`format` laid out over several lines, each level indented by `indent` (two spaces, a
tab -- whatever you pass). An element with any text in it stays on one line, because
indentation inside mixed content would change the text itself.

**This is not round-trip safe, and cannot be.** The indentation between elements is text like
any other, and nothing in the document says whether it matters -- only a schema this reader
does not have could say. So reading the pretty output back gives a tree with whitespace in it
that the original did not have. Use `format` when the bytes have to come back the
same (a signature, a checksum, a stored document) and this one when a person reads the
output.

### `xml_to_bytes`

```
pub fn xml_to_bytes(doc: Document) -> Result<[byte], string>
```

`format` as UTF-8 bytes: an HTTP body, or what `fs.write_bytes` takes.

### `format`

```
pub fn format(e: Element) -> Result<string, string>
```

One element as XML text, with no declaration: a fragment to embed in a document being built
some other way.
