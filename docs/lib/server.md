# `server`

An HTTP/1.1 *server* toolkit, written in Ziman on top of
`net`: parse a request off a socket, route it, write a response.
Two ways to use it. `serve(listener, ctx, handler, options)` runs the accept
loop and the per-connection loop for you and calls your handler -- a
function value -- for each request (see "serve" at the end of this file).
Or write your own accept loop and connection loop and call the pieces:
`read_request`, `write_response`, the response builders, and route matching
that extracts path parameters (`route`, `param`). A handler cannot capture
(function values are references to named functions or lambdas), so what it needs
besides the request travels as an explicit context value.

What it speaks: HTTP/1.1 and 1.0 requests with a request line, headers
and an optional body (`Content-Length`, or `Transfer-Encoding: chunked`, decoded); keep-alive
across requests on one connection (including pipelined requests already in the buffer);
`Expect: 100-continue`; `HEAD` (headers only). Every response carries `Date` and `Server`. What it
does not: other transfer encodings (answered 501), HTTP/2, multipart bodies, compression, and
request trailers (read and ignored). TLS is the socket's business: hand `serve` a listener from
`net.listen_tls` and every connection is HTTPS. Request and response bodies are `string`, so they can't carry
a 0 byte (a request that tries gets a 400).

Bounded by design: headers may total at most 16 KiB and a body at most
1 MiB, so a client can't make a handler thread buffer without limit;
exceeding them is `HeadersTooLarge`/`BodyTooLarge` (431/413). Set a read
timeout on the socket (`net.set_timeout`) so an idle or stalled client
can't hold a thread forever: a timed-out read is `Err(Timeout)`. A per-read
timeout alone does not stop a client that sends a byte just inside it, again
and again (slowloris), so `open_session_with_timeout` (which `serve` uses,
with `idle_timeout_ms`) also bounds the whole request -- head and body --
from its first byte.

Strict where a proxy in front might be lenient, because a difference between the
two is how requests get smuggled: a request whose `Content-Length` values differ,
or is not plain digits (`+5`), or that has two `Transfer-Encoding` or `Host`
headers, or an HTTP/1.1 request with no `Host`, or a header name that is not a
token, or a header line with a bare CR or LF or another control character, is a
400. Response headers are checked on the way out: `write_response` refuses a
header whose name is not a token or whose value has a control character (CRLF
injection through a value an application took from a client) and sends a 500.

`use server` -- or `use server.Name` to bring one item in bare.

## Example

```
use client
use http
use http.HttpMethod
use net
use server
use server.ServeOptions

// Routes with path parameters, a JSON reply, and a 404 for everything else. The client module's
// example shows the same server from the other side.
fn handle(req: http.Request) -> http.Response {
    val user = server.route(req, HttpMethod.Get, "/users/{id}")
    if Option.is_Some(user) {
        val id = server.param(Option.unwrap_Some(user), "id")
        return server.json_response(200, concat(concat("{\"id\":\"", id), "\"}"))
    }
    return server.not_found_or_method_not_allowed(req, ["GET /users/{id}"])
}

fn main() {
    val listener = unwrap(net.listen("127.0.0.1", 0))
    val options = ServeOptions { idle_timeout_ms: 2000, max_connections: 4, log_requests: false }
    spawn server.serve(listener, handle, options)
    val base = format("http://127.0.0.1:{}", net.listener_port(listener))

    print(unwrap(bytes_to_string(unwrap(client.get(concat(base, "/users/42"))).body)))
    print(unwrap(client.get(concat(base, "/other"))).status)
}
```

Output:

```
{"id":"42"}
404
```

### `Params`

```
pub struct Params {
    names: [string]
    values: [string]
}
```

The `{name}` captures of a matched route, by position.

### `HttpError`

```
pub enum HttpError {
    BadRequest(string)
    BodyTooLarge
    HeadersTooLarge
    NotImplemented(string)
    Timeout
    Closed
    Io(string)
}
```

Why reading a request failed: malformed (`BadRequest`), too large (`BodyTooLarge`,
`HeadersTooLarge`), unsupported (`NotImplemented`), a stalled client (`Timeout`), a peer that
went away (`Closed`) or a socket error (`Io`). `serve` answers each with the matching status
where the peer is still there.

### `Session`

```
pub struct Session {
    sock: Socket
    buffer: [byte]
    timeout_ms: int
}
```

One client connection's read state: the socket, plus any bytes already
read past the end of the last request (the start of the next, when a
client pipelines). Values are immutable, so `read_request` returns the
updated session rather than changing this one.

### `Exchange`

```
pub struct Exchange {
    request: Request
    session: Session
}
```

One request read from a connection, with the session to read the next one from.

## Helpers

### `open_session`

```
pub fn open_session(sock: Socket) -> Session
```

A new connection's read state: the socket and nothing buffered yet. Reads are bounded only by
the socket's own timeout, one read at a time.

### `open_session_with_timeout`

```
pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session
```

Like `open_session`, and the whole of each request (head and body, from its first byte) must
arrive within `timeout_ms` (a stalled or dripping client gets `Err(Timeout)`, answered 408), on top
of the socket's per-read timeout, which it sets and resets itself: set none yourself. `serve` opens
its sessions this way with `idle_timeout_ms`.

## Reading

### `read_request`

```
pub fn read_request(session: Session) -> Result<Exchange, HttpError>
```

Reads the next request off the session. `Err(Closed)` when the client
closed the connection between requests (the normal way a keep-alive
connection ends); other errors mean the request itself was unusable --
`error_response` maps one to the HTTP response to send before closing.

## Responses

### `response`

```
pub fn response(status: int, body: [byte]) -> Response
```

A response with `status`, no headers and `body`.

### `with_header`

```
pub fn with_header(r: Response, name: string, value: string) -> Response
```

`r` with one more header (added after any it already has; nothing is replaced).

### `media_response`

```
pub fn media_response(status: int, body: [byte], media: mime.MediaType) -> Response
```

A response with `status` and `body`, `Content-Type` set to `media` (parameters such as a charset included).

### `json_response`

```
pub fn json_response(status: int, body: string) -> Response
```

A response with `status` and `body`, `Content-Type: application/json`.

### `text_response`

```
pub fn text_response(status: int, body: string) -> Response
```

A response with `status` and `body`, `Content-Type: text/plain; charset=utf-8`.

### `bad_request`

```
pub fn bad_request(message: string) -> Response
```

A 400 response with a JSON body `{"status":400,"error":message}`.

### `not_found`

```
pub fn not_found() -> Response
```

A 404 response with a JSON body `{"status":404,"error":"not found"}`.

### `method_not_allowed`

```
pub fn method_not_allowed(allowed: string...) -> Response
```

A 405 response with an `Allow` header listing `allowed`, and a JSON error body.

### `error_response`

```
pub fn error_response(e: HttpError) -> Response
```

The response to send for a request that couldn't be read. (`Closed` and
`Io` mean there is nobody to answer; the caller just closes.)

### `write_response`

```
pub fn write_response(sock: Socket, req: Request, response: Response) -> Result<bool, HttpError>
```

Serialises and sends `resp` (adding Content-Length and Connection), and
says whether the connection stays open for another request: HTTP/1.1
keeps alive unless either side asked to close, HTTP/1.0 only if the
client asked to keep it. A `HEAD` request gets the headers, with the
Content-Length the body would have had, and no body.

### `write_error`

```
pub fn write_error(sock: Socket, e: HttpError) -> Result<bool, HttpError>
```

Sends the response for a request that couldn't be read (see
`error_response`) and closes the exchange: there is no parsed Request to
consult, so it always says `Connection: close`. Nothing to send for
`Closed`/`Io` -- the peer is gone -- so it succeeds without writing.

## Routing

### `path_params`

```
pub fn path_params(pattern: string, path: string) -> Option<Params>
```

Matches `path` against `pattern`: literal segments must be equal, and a
`{name}` segment captures any non-empty segment (percent-decoded). A
trailing slash is ignored on both sides. `None` if it doesn't match.

### `route`

```
pub fn route(req: Request, method: HttpMethod, pattern: string) -> Option<Params>
```

`Some(params)` if the request's method is `method` and its path matches
`pattern`, else `None` -- the one-line test a route function is built
from: `val m = server.route(req, HttpMethod.Get, "/patients/{id}")`.

### `param`

```
pub fn param(p: Params, name: string) -> string
```

A captured parameter by name. Asking for a name the pattern doesn't
have is a bug in the route function, so it is a fatal error rather than
a value to check.

### `not_found_or_method_not_allowed`

```
pub fn not_found_or_method_not_allowed(req: Request, routes: [string]) -> Response
```

What to answer when no route matched. `routes` lists every route as
"METHOD /pattern": if the path matches some route under a *different*
method the answer is 405 with an Allow header, otherwise 404.

### `query_param`

```
pub fn query_param(req: Request, name: string) -> Option<string>
```

A query-string value by name, percent-decoded ("+" is a space); the
first one if it is repeated. `None` if absent.

## Typed endpoints

### `Endpoint`

```
pub struct Endpoint<C> {
    method: HttpMethod
    pattern: string
    handler: fn(C, Request, Params) -> Response
}
```

A route: the handler, already adapted to requests and responses, for `method` on `pattern`.

### `Decoder`

```
pub struct Decoder<I> {
    media: mime.MediaType
    decode: fn([byte]) -> Result<I, string>
}
```

How to decode a request body of type `media` into an `I`; an `Err` message is answered 400. Build one with `decoder`
(a function over the body's bytes) or `text_decoder` (over its text).

### `Encoder`

```
pub struct Encoder<O> {
    media: mime.MediaType
    encode: fn(O) -> [byte]
}
```

How to encode an answer `O` as `media`. Build one with `encoder` (to bytes) or `text_encoder` (to text).

### `decoder`

```
pub fn decoder<I>(media: mime.MediaType, decode: fn([byte]) -> Result<I, string>) -> Decoder<I>
```

A decoder for bodies of `media`, from the body's bytes.

### `text_decoder`

```
pub fn text_decoder<I>(media: mime.MediaType, decode: fn(string) -> Result<I, string>) -> Decoder<I>
```

A decoder for bodies of `media` that are text: the body is turned into a string first (an `Err`, so a 400, when it
holds a NUL byte) and `decode` reads that.

### `encoder`

```
pub fn encoder<O>(media: mime.MediaType, encode: fn(O) -> [byte]) -> Encoder<O>
```

An encoder for answers of `media`, to bytes.

### `text_encoder`

```
pub fn text_encoder<O>(media: mime.MediaType, encode: fn(O) -> string) -> Encoder<O>
```

An encoder for answers of `media` that are text: `encode` writes a string and the encoder turns it into bytes.

### `Denied`

```
pub enum Denied {
    Unauthenticated(string)
    Forbidden(string)
    Failed(string)
}
```

Why a caller was refused before an endpoint ran: not identified (`Unauthenticated`, answered 401), identified but not
allowed (`Forbidden`, 403), or identifying them failed for a reason that is the server's (`Failed`, 500).

## The three input kinds, each in a `secure_` form (the caller is identified first) and a `public_` one.

### `secure_handler`

```
pub fn secure_handler<C, U, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, U) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A secured endpoint with no input: `run(ctx, caller)` returns the answer or an error; the answer is written with the
first of `encoders` the client's `Accept` allows, with `status`.

### `secure_path_handler`

```
pub fn secure_path_handler<C, U, P, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    path: fn(Params) -> Result<P, string>,
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, U, P) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A secured endpoint whose input is in the path: `path` turns the captured parameters into a `P` (an `Err` message is
answered 400, so a bad id never reaches `run`), and `run(ctx, caller, input)` returns the answer or an error.

### `secure_query_handler`

```
pub fn secure_query_handler<C, U, Q, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    query: fn(Request) -> Result<Q, string>,
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, U, Q) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A secured endpoint whose input is in the query string (`GET /things?type=x`): `query` reads what it wants from the request, for
which `query_param` is the tool, and turns it into a `Q` (an `Err` message is answered 400, so a bad value never reaches `run`);
`run(ctx, caller, input)` returns the answer or an error.

### `secure_body_handler`

```
pub fn secure_body_handler<C, U, I, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, U, I) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A secured endpoint with a request body: it must arrive as one of `decoders`' media types (else 415), the answer is
negotiated (else 406), the body is decoded with the decoder for its type (a failure is 400), and
`run(ctx, caller, input)` returns the answer or an error.

### `secure_path_body_handler`

```
pub fn secure_path_body_handler<C, U, P, I, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    path: fn(Params) -> Result<P, string>,
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, U, P, I) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A secured endpoint with both an id in the path and a request body (a `PUT /things/{id}`): `path` decodes the parameters
into a `P` and the body is decoded with the decoder for its media type; `run(ctx, caller, path_input, body_input)`
returns the answer or an error. Either input failing to decode is a 400.

### `public_handler`

```
pub fn public_handler<C, O, E>(
    encoders: [Encoder<O>],
    status: int,
    run: fn(C) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A public endpoint with no input: `run(ctx)`. Anyone may call it.

### `public_path_handler`

```
pub fn public_path_handler<C, P, O, E>(
    path: fn(Params) -> Result<P, string>,
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, P) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A public endpoint whose input is in the path: `run(ctx, input)`.

### `public_body_handler`

```
pub fn public_body_handler<C, I, O, E>(
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, I) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A public endpoint with a request body: `run(ctx, input)`.

### `public_path_body_handler`

```
pub fn public_path_body_handler<C, P, I, O, E>(
    path: fn(Params) -> Result<P, string>,
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: int,
    run: fn(C, P, I) -> Result<O, E>,
    failure: fn(E) -> Response,
    fault: fn(int, string) -> Response
) -> fn(C, Request, Params) -> Response
```

A public endpoint with both an id in the path and a request body: `run(ctx, path_input, body_input)`.

### `dispatch`

```
pub fn dispatch<C>(endpoints: [Endpoint<C>], ctx: C, req: Request, fault: fn(int, string) -> Response) -> Response
```

Calls the handler of the first endpoint whose method and pattern match `req`. When none does, the answer is
`fault(404, ...)`, or `fault(405, ...)` with an `Allow` header when the path exists under another method.

## Serve

### `ServeOptions`

```
pub struct ServeOptions {
    idle_timeout_ms: int
    max_connections: int
    log_requests: bool
}
```

The accept loop and the per-connection request loop, so an application only
supplies its handler. A handler is a *function value*: a named function, or a
lambda that captures what it needs (a database connection, a configuration):


```
server.serve(listener, fn(req) { route(conn, req) }, options)
```

One thread per connection, at most `max_connections` at once (the limit is a
channel used as a counting semaphore: a client past it waits in the kernel's
accept queue). `serve` returns when a shutdown has been requested
(`os.install_shutdown_handler()` makes SIGINT/SIGTERM do that) and every
connection already being served has finished -- an idle keep-alive
connection ends at `idle_timeout_ms` at the latest. It does not close the
listener or install the signal handler; the caller owns both.

### `serve`

```
pub fn serve(listener: Listener, handler: fn(Request) -> Response, options: ServeOptions)
```

Serves `listener` until a shutdown is requested, answering each request with `handler`; see
`ServeOptions` for the limits and the description above for how connections and shutdown behave.
