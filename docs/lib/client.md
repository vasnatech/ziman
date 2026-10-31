# `client`

An HTTP/1.1 *client*, written in Ziman on top of `net`, so a Ziman program can
call another service (the server side is `server`; the types they share are in `http`).


```
val r = client.get("https://example.com/api/items?limit=5")
val resp = client.request(method: HttpMethod.Post, url: u, body: "{\"a\":1}",
                              headers: [http.Header { name: "Content-Type", value: "application/json" }])
```

`http://` and `https://` URLs (TLS with full certificate verification). What it does for you:
  * keep-alive: an idle connection is reused for the next request to the same place (and a request
    that finds the parked connection dead is sent again on a fresh one);
  * compression: it asks for `gzip`/`deflate` and decodes the body (see `compress`);
  * redirects: 301/302/303/307/308, up to `max_redirects` (default 5; 0 = don't follow); 303 and a
    301/302 answering a POST are re-sent as GET without a body, and an `Authorization` header is not
    sent to a different host;
  * proxies: `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY` from the environment, or the `proxy` argument;
    https goes through a CONNECT tunnel;
  * cookies: with a `CookieJar` (`request_with_jar`), cookies a server sets are sent back;
  * credentials in the URL (`https://user:pass@host/`) become a Basic `Authorization` header.
Bodies are read by `Content-Length`, `Transfer-Encoding: chunked`, or until the server closes.

A response comes back as an `http.Response` whose `body` is `[byte]`: images, files and other binary data as they are,
and text through `http.response_text` (or `bytes_to_string`). **Any** status, including 404 or 500, is `Ok` -- an `Err`
means the exchange itself failed (bad URL, network, timeout, malformed response, too large). Bodies are capped at 8 MiB
after decompression (`TooLarge`). Response bodies can be streamed block by block with `stream`.
Not yet: request bodies streamed from a file, HTTP/2, and mutual TLS.

Mutual TLS is absent here rather than in `net`, which has the client-certificate surface: this
module threads a lone `ca_file` through six call layers and would have to carry that options
struct instead, which is mechanical but wide (docs/reviews/tasks/ F64).

`use client` -- or `use client.Name` to bring one item in bare.

## Example

```
use client
use http
use http.HttpMethod
use net
use server

// A tiny server and a client in one program, talking over the loopback interface.

// `route` answers Some only when the method and the pattern both match, and the match carries the
// path parameters -- so one `if` tests the route and names the parameters.
fn handle(req: http.Request) -> http.Response {
    if Some(hit) = server.route(req, HttpMethod.Get, "/hello/{name}") {
        return server.text_response(200, concat("hello ", server.param(hit, "name")))
    }
    return server.not_found()
}

// The exchanges, which all answer the same ClientError. The body decode answers a plain string
// instead, so it is destructured where it happens.
fn exchanges(base: string) -> Result<unit, client.ClientError> {
    val ok = try client.get(concat(base, "/hello/ada"))
    print(ok.status)
    if Ok(body) = bytes_to_string(ok.body) {
        print(body)
    }

    // Any status is Ok; only a failed exchange is an Err.
    val nowhere = try client.get(concat(base, "/nowhere"))
    print(nowhere.status)
    print(is_ok(client.get("http://127.0.0.1:1/")))

    // A response read as a stream, block by block, instead of all at once. Each block is a Result,
    // because the connection can drop partway through the body.
    val streamed = try client.stream(method: HttpMethod.Get, url: concat(base, "/hello/ada"))
    var bytes = 0
    for block in streamed.body {
        if Ok(chunk) = block {
            bytes = bytes + len(chunk)
        }
    }
    print(bytes)
    return unit
}

fn main() {
    val listener = switch net.listen("127.0.0.1", 0) {
        Ok(l) { l }
        Err(e) {
            printf("listen failed: {}\n", net.net_error_message(e))
            return
        }
    }
    val port = net.listener_port(listener)
    val options = with server.serve_options() {
        idle_timeout_ms: 2000,
        header_timeout_ms: 2000,
        max_connections: 4,
        log_requests: false,
        fault: None,
        metrics: false,
        max_connections_per_client: 0
    }
    spawn server.serve(listener, handle, options)

    if Err(e) = exchanges(format("http://127.0.0.1:{}", port)) {
        printf("client example failed: {}\n", client.client_error_message(e))
    }
}
```

Output:

```
200
hello ada
404
false
9
```

## `client.zm`

### `ClientError`

```
pub enum ClientError {
    BadUrl(string)
    Network(string)
    Timeout
    BadResponse(string)
    TooLarge
    TooManyRedirects
}
```

Why an exchange failed (any HTTP status, even 500, is a success).

### `client_error_message`

```
pub fn client_error_message(e: ClientError) -> string
```

The error as one line of text, for logging or showing to a user. Named `client_error_message`,
matching `net_error_message`/`db_error_message`/`jwt_error_message`'s own convention (and
freeing the bare `error_message` name for the builtin of that name, which dispatches to this
exact function for a `ClientError` -- see docs/design/journal/'s "A unified error_message builtin").

### `Url`

```
pub struct Url {
    secure: bool
    host: string
    port: int
    target: string
    userinfo: string
}
```

A parsed `http://` or `https://` URL. `target` is the path and query as they go on the request line
("/" if the URL has none); a #fragment is dropped; `userinfo` is the raw `user:password` before an
`@` in the authority ("" if none).

### `parse_url`

```
pub fn parse_url(url: string) -> Result<Url, string>
```

Parses an `http://` or `https://` URL. An Err says what is wrong: another scheme, a missing host, an
unterminated `[` in an IPv6 host, a bad port.

## `read.zm`

### `response_header`

```
pub fn response_header(r: Response, name: string) -> Option<string>
```

The first header called `name` (any case) of a response, or None.

## `send.zm`

### `close_idle`

```
pub fn close_idle()
```

Closes every parked keep-alive connection.

## `cookie.zm`

### `Cookie`

```
pub struct Cookie {
    name: string
    value: string
    domain: string
    path: string
    secure: bool
    host_only: bool
    expires: int
}
```

One stored cookie. `expires` is seconds since the epoch, -1 for a session cookie.

### `CookieJar`

```
pub struct CookieJar {
    cookies: [Cookie]
}
```

The cookies a client is holding, passed to `request_with_jar` and returned updated. Make an
empty one with `cookie_jar()`.

### `cookie_jar`

```
pub fn cookie_jar() -> CookieJar
```

A jar with no cookies.

### `JarResponse`

```
pub struct JarResponse {
    response: Response
    jar: CookieJar
}
```

A response together with the cookie jar as the exchange left it.

### `jar_store`

```
pub fn jar_store(jar: CookieJar, u: Url, headers: [Header]) -> CookieJar
```

The jar with the cookies in a response's `Set-Cookie` headers stored (replacing ones of the same
name, domain and path; an already expired one deletes it). `u` is the URL that was requested.

### `jar_header`

```
pub fn jar_header(jar: CookieJar, u: Url) -> Option<string>
```

The `Cookie` header value for a request to `u` -- `a=1; b=2` -- from the jar's matching cookies (longer
paths first), or None if none match.

## `request.zm`

### `request`

```
pub fn request(
    method: HttpMethod,
    url: string,
    headers: Option<[Header]>,
    body: Option<[byte]>,
    timeout_ms: Option<int>,
    max_redirects: Option<int>,
    ca_file: Option<string>,
    proxy: Option<string>,
    compress: Option<bool>,
    reuse: Option<bool>
) -> Result<Response, ClientError>
```

Sends one request and returns the (final) response, its body as bytes.
  method        an `http.HttpMethod` (`HttpMethod.Get`, `HttpMethod.Post`, ...)
  headers       extra request headers; Host, Content-Length and Connection are always set by the client
  body          the request body, if any, as bytes (`to_bytes(text)` for text)
  timeout_ms    per read/write, default 30000
  max_redirects redirects to follow, default 5 (0 = return the 3xx as it is)
  ca_file       a PEM file of trusted CAs for https, instead of the system store (a private CA, a test
                certificate); the certificate is always verified
  proxy         a proxy URL (`http://host:port`, optionally `user:pass@`); "" for none; if omitted, the
                `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY` environment variables decide
  compress      ask for gzip/deflate and decode the body (default true)
  reuse         keep the connection for the next request to the same place (default true)

### `request_with_jar`

```
pub fn request_with_jar(
    jar: CookieJar,
    method: HttpMethod,
    url: string,
    headers: Option<[Header]>,
    body: Option<[byte]>,
    timeout_ms: Option<int>,
    max_redirects: Option<int>,
    ca_file: Option<string>,
    proxy: Option<string>,
    compress: Option<bool>,
    reuse: Option<bool>
) -> Result<JarResponse, ClientError>
```

`request`, with cookies: the jar's matching cookies are sent (on every redirect hop too), and the
cookies the responses set are stored. Returns the response and the updated jar; pass that jar to the
next call to stay "logged in". Other arguments as for `request`.

### `get`

```
pub fn get(url: string) -> Result<Response, ClientError>
```

GET `url`.

### `head`

```
pub fn head(url: string) -> Result<Response, ClientError>
```

HEAD `url`: the status and headers, no body.

### `delete`

```
pub fn delete(url: string) -> Result<Response, ClientError>
```

DELETE `url`.

### `post_json`

```
pub fn post_json(url: string, body: string) -> Result<Response, ClientError>
```

POST with a JSON body: sets `Content-Type: application/json`.

### `put_json`

```
pub fn put_json(url: string, body: string) -> Result<Response, ClientError>
```

PUT with a JSON body: sets `Content-Type: application/json`.

### `form_encode`

```
pub fn form_encode(fields: Map<string, string>) -> string
```

The fields as an `application/x-www-form-urlencoded` body: `a=1&b=x+y`, names and values
percent-encoded, in the map's insertion order.

### `post_form`

```
pub fn post_form(url: string, fields: Map<string, string>) -> Result<Response, ClientError>
```

POST an HTML-form-style body built from `fields` (`Content-Type: application/x-www-form-urlencoded`).

## `stream.zm`

### `StreamResponse`

```
pub struct StreamResponse {
    status: int
    headers: [Header]
    body: Iter<Result<[byte], ClientError>>
}
```

A response whose body has not been read yet: the status and headers, and the body as an `Iter` of blocks
of bytes read as a `for` loop asks for them.

### `stream`

```
pub fn stream(
    method: HttpMethod,
    url: string,
    headers: Option<[Header]>,
    body: Option<[byte]>,
    timeout_ms: Option<int>,
    ca_file: Option<string>,
    proxy: Option<string>
) -> Result<StreamResponse, ClientError>
```

Sends one request and returns the status and headers as soon as they arrive, with the body left on the
connection to be read lazily -- for a large download, or a response that never ends. The body is an
`Iter` of `Result<[byte], ClientError>` blocks of at most 8192 bytes, read as a `for` loop asks for
them: `val r = try client.stream(method: HttpMethod.Get, url: u)` then `for block in r.body { val bytes =
try block ... }`. A read error, or a timeout waiting for the next block, is a last Err item. The body comes
as sent: this asks for no compression (`Accept-Encoding: identity`), and a server that compresses anyway
is not decoded. No redirects are followed and no cookies are sent (a 3xx is returned as it is), nothing
is limited to 8 MiB, and the connection is not reused: it is closed when the body is finished, when the
loop is left early or with `iter_close(r.body)`, and a stream dropped unfinished is closed when its
socket is collected. Arguments as for `request`: `headers`, `body`, `timeout_ms`, `ca_file`,
`proxy`.
