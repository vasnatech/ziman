# `http`

What an HTTP server and an HTTP client have in common: the request and response
types, the method enum, status codes by name, header lookup and percent-encoding. `server`
builds on it to answer requests, `client` to make them.

`use http` -- or `use http.Name` to bring one item in bare.

## Example

```
use http
use http.Header
use http.HttpMethod
use http.status_not_found

// The types a server and a client share.
fn main() {
    print(http.method_name(HttpMethod.Post))
    print(http.method_name(http.method_parse("PATCH")))
    print(http.method_parse("get") == HttpMethod.Other("get"))

    val req = http.Request {
        method: HttpMethod.Get,
        target: "/a?x=1",
        path: "/a",
        query: "x=1",
        version: "HTTP/1.1",
        headers: [Header { name: "Content-Type", value: "text/plain" }],
        body: []
    }
    print(http.header(req, "content-type"))
    print(status_not_found)

    print(http.percent_encode("a b&c/é", false))
    print(unwrap(http.percent_decode("a+b%26c", true)))
}
```

Output:

```
POST
PATCH
true
Some(text/plain)
404
a%20b%26c%2F%C3%A9
a b&c
```

### `is_token`

```
pub native fn is_token(s: string) -> bool
```

True if `s` is a non-empty run of the characters RFC 7230 allows in a "token": letters, digits and
``!#$%&'*+-.^_`|~``. What a header name or a method must be; a space, a colon, a control character
or anything else makes it false.

### `is_field_value`

```
pub native fn is_field_value(s: string) -> bool
```

True if `s` holds no control character except a tab (so no CR or LF, which would end a header
line and let the rest of the value pass for another header) and no DEL. What a header value must be.

### `status_continue`

```
pub val status_continue = 100
```

HTTP status codes by name.

### `status_ok`

```
pub val status_ok = 200
```

### `status_created`

```
pub val status_created = 201
```

### `status_accepted`

```
pub val status_accepted = 202
```

### `status_no_content`

```
pub val status_no_content = 204
```

### `status_moved_permanently`

```
pub val status_moved_permanently = 301
```

### `status_found`

```
pub val status_found = 302
```

### `status_not_modified`

```
pub val status_not_modified = 304
```

### `status_bad_request`

```
pub val status_bad_request = 400
```

### `status_unauthorized`

```
pub val status_unauthorized = 401
```

### `status_forbidden`

```
pub val status_forbidden = 403
```

### `status_not_found`

```
pub val status_not_found = 404
```

### `status_method_not_allowed`

```
pub val status_method_not_allowed = 405
```

### `status_not_acceptable`

```
pub val status_not_acceptable = 406
```

### `status_request_timeout`

```
pub val status_request_timeout = 408
```

### `status_conflict`

```
pub val status_conflict = 409
```

### `status_payload_too_large`

```
pub val status_payload_too_large = 413
```

### `status_unsupported_media_type`

```
pub val status_unsupported_media_type = 415
```

### `status_unprocessable_entity`

```
pub val status_unprocessable_entity = 422
```

### `status_header_fields_too_large`

```
pub val status_header_fields_too_large = 431
```

### `status_internal_server_error`

```
pub val status_internal_server_error = 500
```

### `status_not_implemented`

```
pub val status_not_implemented = 501
```

### `status_service_unavailable`

```
pub val status_service_unavailable = 503
```

### `HttpMethod`

```
pub enum HttpMethod {
    Get
    Head
    Post
    Put
    Patch
    Delete
    Options
    Other(string)
}
```

A request method. The common ones are variants (so a route's method is
checked by the compiler and a `switch` over it is exhaustive); anything else
a client sends (PROPFIND, TRACE, a made-up verb) is `Other` with the text as
received. Method names are case-sensitive: "get" is `Other("get")`.

### `method_parse`

```
pub fn method_parse(name: string) -> HttpMethod
```

The method named `name`: a known one (`GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE`, `OPTIONS`) or
`Other(name)`. Case-sensitive.

### `method_name`

```
pub fn method_name(m: HttpMethod) -> string
```

The text that goes on the request line ("GET", "POST", ...).

### `Header`

```
pub struct Header {
    name: string
    value: string
}
```

One header line. `name` keeps the case it had; compare names with `equals_ignore_case`.

### `Request`

```
pub struct Request {
    method: HttpMethod
    target: string
    path: string
    query: string
    version: string
    headers: [Header]
    body: [byte]
}
```

`target` is the raw request target ("/patients/7?x=1"); `path` and
`query` are its two halves, still percent-encoded (`route` and
`query_param` decode what they hand back). `headers` keep their
original case; look one up with `header`, which ignores case.

### `Response`

```
pub struct Response {
    status: int
    headers: [Header]
    body: [byte]
}
```

What a handler returns: a status code, headers and a body of bytes. `write_response` adds
`Content-Length`, `Connection`, and (unless set) `Date` and `Server`.

### `response_text`

```
pub fn response_text(r: Response) -> Result<string, string>
```

The body of `r` as text: an Err when it holds a NUL byte (it is binary, not text).

### `request_text`

```
pub fn request_text(req: Request) -> Result<string, string>
```

The body of `req` as text: an Err when it holds a NUL byte.

### `header`

```
pub fn header(req: Request, name: string) -> Option<string>
```

The value of the header called `name` in `req` (case-insensitive), or None.

### `header_value`

```
pub fn header_value(headers: [Header], name: string) -> Option<string>
```

The value of the first header called `name` (case-insensitive), or None.

### `content_type`

```
pub fn content_type(headers: [Header]) -> Option<mime.MediaType>
```

The `Content-Type` in `headers` as a media type, or None when there is none or it is not a valid media type.

### `accepts`

```
pub fn accepts(headers: [Header], media: mime.MediaType) -> bool
```

Whether a client whose headers are `headers` accepts a response of type `media`: true when there is no
`Accept` header, or when one of its ranges (`application/json`, `text/*`, `*/*`) matches `media`. A range
naming `media` exactly with `q=0` refuses it even when a wildcard would allow it. Other quality weights
are not ranked; any positive one counts as acceptance.

### `percent_decode`

```
pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>
```

%XX escapes; `plus_is_space` is for query strings ("a+b" is "a b"
there, but a literal plus in a path).

### `percent_encode`

```
pub fn percent_encode(s: string, plus_for_space: bool) -> string
```

The inverse of `percent_decode`: every byte except the unreserved characters (letters, digits,
`-` `_` `.` `~`) as `%XX` (uppercase hex). With `plus_for_space` a space is written `+` (form and query
encoding) instead of `%20`.
