# `codec`

A value as bytes, in a named media type, and back: the typed `Encoder`/`Decoder` pair and the
`Format`s they can be built from.

**This is serialization, not HTTP**, which is why it is here. The pair lived in
`lib/http/server/` until 2026-10-31 and could only be reached by something building an endpoint
-- so a command-line tool with a `--format` flag, a job writing a file, or a queue payload had
to write its own. Nothing in either type mentions a request or a response; what *is* HTTP is
choosing between several of them from an `Accept` header, and that stayed in `server`.

**`Json` is the pivot, and it has to be.** Ziman has no runtime reflection, so there is no
`encode(value, SomeType)` that inspects a value at run time -- the Java interface this is
answering to passes a `Class` token for exactly that and cannot be translated. What ziman has
instead is `#[derive(Json)]`, which gives every type a `<snake>_to_json` and
`<snake>_from_json` at compile time; `encoder_of` takes one of those and a `Format`, and that
composition is what "the same value in any of five media types" means here. The type is checked
where the encoder is built rather than hoped for at run time.

A format that is not a tree -- a PDF, a spreadsheet -- is not a `Format` and should not be bent
into one: build its `Encoder` with `encoder` directly, from whatever writes it.

`use codec` -- or `use codec.Name` to bring one item in bare.

### `Decoder`

```
pub struct Decoder<I> {
    media: mime.MediaType
    decode: fn([byte]) -> Result<I, string>
}
```

How to decode bytes of type `media` into an `I`. An `Err` message is for whoever asked -- the
HTTP server answers it 400 -- so it names what was wrong with the input rather than how.

### `Encoder`

```
pub struct Encoder<O> {
    media: mime.MediaType
    encode: fn(O) -> [byte]
}
```

How to encode an `O` as `media`.

### `Format`

```
pub struct Format {
    // What to call it to a person: `JSON`, not `application/json`. The media type is the wire's
    // name for a format and a poor word for a message -- "malformed application/json" reads as
    // machinery where "malformed JSON" reads as a sentence. The Java interface this answers to
    // draws the same distinction, `MediaType.name()` beside `mimeType()`.
    name: string
    media: mime.MediaType
    parse: fn(string) -> Result<Json, string>
    format: fn(Json) -> Result<string, string>
}
```

A tree format: its media type, and the two directions over `Json`.

**`format` answers a `Result` even for the three formats that cannot fail.** `toml.format` can
(a table cannot hold every tree a JSON document can), and a `Format` whose shape changed with
the format would not be one type. The infallible ones wrap in `ok`, which costs nothing and
keeps one signature.

### `format_of`

```
pub fn format_of(
    name: string,
    media: mime.MediaType,
    parse: fn(string) -> Result<Json, string>,
    format: fn(Json) -> Result<string, string>
) -> Format
```

### `json_format`

```
pub fn json_format() -> Format
```

### `yaml_format`

```
pub fn yaml_format() -> Format
```

### `toml_format`

```
pub fn toml_format() -> Format
```

### `ndjson_format`

```
pub fn ndjson_format() -> Format
```

Newline-delimited JSON. `parse` answers the array of documents as one `JArray`, so that a reader
sees the same shape the other formats give; `ndjson.parse` itself answers `[Json]`.

### `csv_format`

```
pub fn csv_format() -> Format
```

CSV, which is a table rather than a tree: `csv.format` accepts the `Json` shapes it can flatten
and refuses the rest, which is why this one's `Result` is load-bearing.

### `formats`

```
pub fn formats() -> [Format]
```

Every format, for looking one up by media type or listing what `--format` takes. JSON first,
which is the one to pick when a client expresses no preference.

### `tree_formats`

```
pub fn tree_formats() -> [Format]
```

**Which formats can write which shape is a fact about the format**, so it belongs here rather
than in a message at encode time. Three sets, and they are not nested:


```
* `tree_formats` writes a lone object. NDJSON is a sequence of documents and CSV is a table,
  so neither can.
* `sequence_formats` writes an array. TOML cannot: a document is a table at the top.
* `formats` is everything, for looking one up by media type.
```

Found by offering all five for both shapes and reading what came back: NDJSON and CSV refused
the object, and TOML refused the array.

### `sequence_formats`

```
pub fn sequence_formats() -> [Format]
```

### `format_for`

```
pub fn format_for(media: mime.MediaType) -> Option<Format>
```

The format whose media type matches `media`, or `None`. Compared by `mime.essence`, so a
`Content-Type: application/json; charset=utf-8` finds the JSON format -- the parameters are not
part of which format it is.

## Building a typed pair

### `decoder`

```
pub fn decoder<I>(media: mime.MediaType, decode: fn([byte]) -> Result<I, string>) -> Decoder<I>
```

A decoder for bodies of `media`, from their bytes.

### `text_decoder`

```
pub fn text_decoder<I>(media: mime.MediaType, decode: fn(string) -> Result<I, string>) -> Decoder<I>
```

A decoder for bodies of `media` that are text: the bytes are turned into a string first (an
`Err` when they hold a NUL byte) and `decode` reads that.

### `encoder`

```
pub fn encoder<O>(media: mime.MediaType, encode: fn(O) -> [byte]) -> Encoder<O>
```

An encoder for `media`, to bytes.

### `text_encoder`

```
pub fn text_encoder<O>(media: mime.MediaType, encode: fn(O) -> string) -> Encoder<O>
```

An encoder for `media` whose output is text.

### `encoder_of`

```
pub fn encoder_of<O>(f: Format, to_json: fn(O) -> Json) -> Encoder<O>
```

**The composition this module exists for**: a type's own derived `to_json`, plus a format, is an
encoder for that type in that format. `#[derive(Json)]` wrote the first half, so offering a
value as YAML or CSV is one call rather than a closure spelled out per type and per format.

A format that refuses the tree -- CSV given something it cannot flatten -- fails when the value
is encoded, not when the encoder is built, because only then is there a tree to judge. The
message is the format's own.

### `list_encoder_of`

```
pub fn list_encoder_of<O>(f: Format, to_json: fn(O) -> Json) -> Encoder<[O]>
```

The same for a list, which is what a collection endpoint answers.

### `decoder_of`

```
pub fn decoder_of<I>(f: Format, from_json: fn(Json) -> Result<I, string>) -> Decoder<I>
```

A decoder from a type's own derived `from_json`.

### `encoders_of`

```
pub fn encoders_of<O>(to_json: fn(O) -> Json) -> [Encoder<O>]
```

Every format that can write a lone value, as encoders for one type: what an endpoint lists when
it is happy to answer in whichever the client prefers.

### `list_encoders_of`

```
pub fn list_encoders_of<O>(to_json: fn(O) -> Json) -> [Encoder<[O]>]
```

The same for a list: NDJSON and CSV join, TOML drops out.

## JSON, which is the common case and worth its own names

### `json_decoder`

```
pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> Decoder<I>
```

### `json_encoder`

```
pub fn json_encoder<O>(to_json: fn(O) -> Json) -> Encoder<O>
```

### `json_list_encoder`

```
pub fn json_list_encoder<O>(to_json: fn(O) -> Json) -> Encoder<[O]>
```

### `count_encoder`

```
pub fn count_encoder() -> Encoder<int>
```

A bare JSON number as the whole output, which is what a count endpoint answers: Spring returns a
`Long` there rather than an object, and a client reading it wants the scalar.

### `json_value_encoder`

```
pub fn json_value_encoder() -> Encoder<Json>
```

A `Json` value as the whole body, for something answering a tree it passes through rather than a
model of its own.
