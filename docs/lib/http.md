# `http`

What an HTTP server and an HTTP client have in common: the request and response
types, the method enum, status codes by name, header lookup and percent-encoding. `server`
builds on it to answer requests, `client` to make them.

`use http` -- or `use http.Name` to bring one item in bare.

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
    body: string
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
    body: string
}
```

What a handler returns: a status code, headers and a text body. `write_response` adds
`Content-Length`, `Connection`, and (unless set) `Date` and `Server`.

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
