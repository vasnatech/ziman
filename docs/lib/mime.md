# `mime`

Media types (MIME types, RFC 6838): `application/json`, `text/html; charset=utf-8`. A
`MediaType` is a type, a subtype and parameters; `parse` reads the text form, `to_string` writes it, and
`matches` compares one against a pattern with wildcards (`*/*`, `text/*`). `of_extension` and `of_path`
give the type of a file by its extension. The module knows nothing about HTTP: the `Accept` header with its
weights lives in `http`, which builds on this, and so can any other module that names a kind of content.

Types, subtypes and parameter names are case-insensitive and are kept in lower case. A parameter value is
kept as written, so `charset=UTF-8` stays `UTF-8`. A `MediaType` has no `==` (its parameters are a `Map`);
compare `essence` for the type and subtype alone.

`use mime` -- or `use mime.Name` to bring one item in bare.

## Example

```
use mime

// Parsing a media type answers a Result; looking one up by file extension answers an Option, since
// a path simply may not have a known type.
fn demo() -> Result<unit, string> {
    // Parse the text form; type, subtype and parameter names are lower-cased.
    val m = try mime.parse("Text/HTML; Charset=UTF-8")
    print(mime.essence(m))
    print(mime.charset(m))
    print(mime.format(mime.with_param(m, "charset", "iso-8859-1")))

    // A malformed type is an Err that says what is wrong -- this one is meant to fail, so it is
    // taken apart here instead of propagating.
    if Err(e) = mime.parse("json") {
        print(e)
    }

    // Match against a range, ignoring parameters.
    val texts = try mime.parse("text/*")
    print(mime.matches(texts, m))
    print(mime.matches(texts, mime.application_json()))

    // A `+json` suffix counts as JSON.
    print(mime.is_json(try mime.parse("application/vnd.api+json")))

    // The type of a file, by its extension.
    if Some(by_path) = mime.of_path("/srv/report.csv") {
        print(mime.essence(by_path))
    }
    print(Option.is_None(mime.of_path("Makefile")))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("mime example failed: {}\n", e)
    }
}
```

Output:

```
text/html
Some(UTF-8)
text/html; charset=iso-8859-1
invalid media type 'json': there is no '/' between the type and the subtype
true
false
true
text/csv
true
```

### `MediaType`

```
pub struct MediaType {
    type: string
    subtype: string
    params: Map<string, string>
}
```

## Well-known types

### `application_json`

```
pub fn application_json() -> MediaType
```

`application/json`.

### `application_ndjson`

```
pub fn application_ndjson() -> MediaType
```

`application/x-ndjson`, newline-delimited JSON.

### `application_yaml`

```
pub fn application_yaml() -> MediaType
```

`application/yaml`.

### `application_toml`

```
pub fn application_toml() -> MediaType
```

`application/toml`.

### `application_xml`

```
pub fn application_xml() -> MediaType
```

`application/xml`.

### `application_form`

```
pub fn application_form() -> MediaType
```

`application/x-www-form-urlencoded`, an HTML form body.

### `application_octet_stream`

```
pub fn application_octet_stream() -> MediaType
```

`application/octet-stream`, bytes of no known kind.

### `application_pdf`

```
pub fn application_pdf() -> MediaType
```

`application/pdf`.

### `application_zip`

```
pub fn application_zip() -> MediaType
```

`application/zip`.

### `application_gzip`

```
pub fn application_gzip() -> MediaType
```

`application/gzip`.

### `text_plain`

```
pub fn text_plain() -> MediaType
```

`text/plain`.

### `text_html`

```
pub fn text_html() -> MediaType
```

`text/html`.

### `text_csv`

```
pub fn text_csv() -> MediaType
```

`text/csv`.

### `image_png`

```
pub fn image_png() -> MediaType
```

`image/png`.

### `image_jpeg`

```
pub fn image_jpeg() -> MediaType
```

`image/jpeg`.

### `image_gif`

```
pub fn image_gif() -> MediaType
```

`image/gif`.

### `image_svg`

```
pub fn image_svg() -> MediaType
```

`image/svg+xml`.

## Parsing

### `parse`

```
pub fn parse(text: string) -> Result<MediaType, string>
```

The media type in `text`: `type/subtype` optionally followed by `; name=value` parameters. Surrounding
blanks are ignored; type, subtype and parameter names are lower-cased. An Err says what is wrong.

## Writing and reading

### `essence`

```
pub fn essence(m: MediaType) -> string
```

`type/subtype` alone, without parameters: `application/json`.

### `format`

```
pub fn format(m: MediaType) -> string
```

The text form: `text/html; charset=utf-8`. A parameter value that is not a token is written quoted.

### `param`

```
pub fn param(m: MediaType, name: string) -> Option<string>
```

The value of parameter `name` (lower case), or None.

### `charset`

```
pub fn charset(m: MediaType) -> Option<string>
```

The `charset` parameter, or None.

### `with_param`

```
pub fn with_param(m: MediaType, name: string, value: string) -> MediaType
```

`m` with parameter `name` set to `value` (replacing any existing one); the name is lower-cased.

### `suffix`

```
pub fn suffix(m: MediaType) -> Option<string>
```

The structured-syntax suffix of the subtype, if it has one: `json` for `application/vnd.api+json`.

### `is_json`

```
pub fn is_json(m: MediaType) -> bool
```

True for `application/json` and any `+json` type.

### `is_text`

```
pub fn is_text(m: MediaType) -> bool
```

True for `text/*`.

## Matching

### `matches`

```
pub fn matches(pattern: MediaType, candidate: MediaType) -> bool
```

True when `candidate` is an instance of `pattern`, ignoring parameters: `*/*` matches everything,
`text/*` any text type, and a full `type/subtype` only itself.

## Files

### `of_extension`

```
pub fn of_extension(extension: string) -> Option<MediaType>
```

The media type for a file extension (`json`, `.PNG`; case and a leading dot do not matter), or None when the
extension is not one of the common ones this module knows.

### `of_path`

```
pub fn of_path(path: string) -> Option<MediaType>
```

The media type of the file at `path`, by its extension: `of_path("/srv/report.csv")` is `text/csv`.
