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
        body: [],
        peer: None
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

### `status_too_many_requests`

```
pub val status_too_many_requests = 429
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
    // The address at the other end of the connection this request arrived on, as text ("192.0.2.7" or
    // "2001:db8::1"), with no port. `None` on a Request that did not come off a socket -- one built by
    // hand in a test, or by `write_error`'s stub -- which is the honest answer rather than a placeholder.
    //
    // **The transport peer, and only that.** Not `X-Forwarded-For`: that header is written by whatever
    // is in front, and a client that is not behind a proxy can put anything in it, so a server reading
    // it by default would let every caller choose its own identity -- backwards for the thing an
    // address is wanted for, which is telling one client from another in order to rate-limit (B10). An
    // application behind a proxy it trusts reads the header itself, knowing how many hops it owns;
    // `server.forwarded_for` is there to help once that decision has been made explicitly.
    //
    // A field rather than a synthetic header, deliberately: a header could be spoofed by a client
    // sending that exact name unless the parser stripped it, and relying on that is the same subtlety
    // this field exists to avoid. Where the value comes from should be visible in the type.
    peer: Option<string>
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

## Trace context

### `TraceParent`

```
pub struct TraceParent {
    // 32 lowercase hex characters, the same for every hop of one request.
    trace_id: string
    // 16 lowercase hex characters, this hop's own. A callee makes a child rather than reusing it.
    span_id: string
    // The one flag anything acts on: whether the trace was sampled for recording.
    sampled: bool
}
```

W3C Trace Context's `traceparent`, which is how one request is followed across services: the
caller sends it, the callee reads it and sends its own child onward, and every log line in
between carries the same `trace_id` (ROADMAP.md's metrics item 4). The header is
`version-traceid-spanid-flags`, as in
`00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`.

### `traceparent_header_name`

```
pub val traceparent_header_name = "traceparent"
```

### `traceparent_parse`

```
pub fn traceparent_parse(value: string) -> Option<TraceParent>
```

A `traceparent` value, or None when it is not one. Rejecting is the normal path for a malformed
header, not an error: the spec says a receiver that cannot parse it must *restart* the trace, so
callers pair this with `traceparent_new` (which `traceparent_of` does).

The two rules that are easy to miss are both here: an all-zero trace or span id is invalid even
though it is well-formed hex, and version `ff` is reserved and never valid. A version above `00`
is accepted when the rest parses, which is what the spec requires of a forward-compatible
receiver -- refusing it would break against a future sender for no gain.

### `traceparent_new`

```
pub fn traceparent_new() -> TraceParent
```

A new trace: a fresh id for the whole request and a fresh id for this hop. Sampled, because a
service that starts a trace and then marks it unsampled has recorded nothing and told nobody.

### `traceparent_child`

```
pub fn traceparent_child(p: TraceParent) -> TraceParent
```

The same trace, a new hop -- what a service sends onward. The sampling decision is inherited, not
retaken, which is the whole point of propagating it.

### `traceparent_value`

```
pub fn traceparent_value(p: TraceParent) -> string
```

The header value to send. Always version `00`, the only version this writes.

### `traceparent_of`

```
pub fn traceparent_of(req: Request) -> TraceParent
```

The incoming request's trace, or a new one when the header is absent or malformed -- the
restart the spec calls for. A handler's first line, and `trace_id` is what goes to
`log.with_request_id`.

### `traceparent_header_for`

```
pub fn traceparent_header_for(p: TraceParent) -> Header
```

The header to put on an outgoing request so the next service joins this trace rather than
starting its own: a child of `p`, as a `Header` ready for `client.request`'s list.

### `header_value`

```
pub fn header_value(headers: [Header], name: string) -> Option<string>
```

The value of the first header called `name` (case-insensitive), or None.

## Cookies

### `CookiePair`

```
pub struct CookiePair {
    name: string
    value: string
}
```

One `name=value` pair from a request's `Cookie` header.

### `request_cookies`

```
pub fn request_cookies(req: Request) -> [CookiePair]
```

Every pair in the request's `Cookie` header, in the order the client sent them. A malformed pair (no `=`)
is skipped rather than failing the lot: a browser can be made to send almost anything, and one bad pair
should not cost a request its session. Values are returned exactly as sent -- a server that
percent-encoded them is the one that knows to decode them (`url.percent_decode`).

### `request_cookie`

```
pub fn request_cookie(req: Request, name: string) -> Option<string>
```

The value of the cookie called `name`, or None. The first wins when a client sends the name twice, which
it may (different paths or domains produce duplicates and the header does not say which is which).

### `SetCookie`

```
pub struct SetCookie {
    name: string
    value: string
    path: string
    domain: string
    max_age_seconds: int
    secure: bool
    http_only: bool
    same_site: string
}
```

What to put in a `Set-Cookie` header. `max_age_seconds` of -1 leaves the attribute off, which makes a
session cookie; 0 is a valid value and tells the browser to drop it now. `same_site` is written as given
("Lax", "Strict", "None") and left off when blank -- `None` requires `secure`, which this does not check,
since a browser enforcing it is the point of saying it.

### `session_cookie`

```
pub fn session_cookie(name: string, value: string) -> SetCookie
```

A `SetCookie` with the defaults a session cookie wants: path `/`, no domain, no Max-Age, `HttpOnly` and
`Secure` on, `SameSite=Lax`. Override what differs with `with`.

### `set_cookie_header`

```
pub fn set_cookie_header(c: SetCookie) -> Header
```

`c` as a `Set-Cookie` header, ready to add to a response's headers.

### `delete_cookie_header`

```
pub fn delete_cookie_header(name: string, path: string, domain: string) -> Header
```

The header that deletes `name`: the same path and domain it was set with, an empty value and `Max-Age=0`.
A browser only drops a cookie when the path and domain match, which is the part that is easy to forget.

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

%XX escapes; `plus_is_space` is for query strings ("a+b" is "a b" there, but a literal plus in a path).
The loop itself is in `url`, shared with `db.percent_decode`, which needed the same algorithm and a
different error type; these two keep the names this module has always exported.

### `percent_encode`

```
pub fn percent_encode(s: string, plus_for_space: bool) -> string
```

The inverse of `percent_decode`: every byte except the unreserved characters (letters, digits,
`-` `_` `.` `~`) as `%XX` (uppercase hex). With `plus_for_space` a space is written `+` (form and query
encoding) instead of `%20`.
