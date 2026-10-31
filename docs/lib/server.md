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
`Expect: 100-continue`; `HEAD` (headers only). Every response carries `Date` and `X-Content-Type-Options: nosniff`, and none says
what server it is (a `Server` header only tells an attacker what to look for); a handler that sets either header itself is not overridden. What it
does not: other transfer encodings (answered 501), HTTP/2, and
request trailers (read and ignored). `multipart/form-data` bodies are read on request (see
`multipart`) and responses are gzipped on request (see `compressed`); neither happens unasked. TLS is the socket's business: hand `serve` a listener from
`net.listen_tls` and every connection is HTTPS. Request and response bodies are `[byte]` and may
hold any byte, including a NUL -- only the request line and headers are read as text, so a NUL
there (not in the body) is what gets a 400 (a Ziman `string` cannot hold one).

Bounded by design: headers may total at most 16 KiB and a body at most
1 MiB, so a client can't make a handler thread buffer without limit;
exceeding them is `HeadersTooLarge`/`BodyTooLarge` (431/413). Set a read
timeout on the socket (`net.set_timeout`) so an idle or stalled client
can't hold a thread forever: a timed-out read is `Err(Timeout)`. A per-read
timeout alone does not stop a client that sends a byte just inside it, again
and again (slowloris), so `open_session_with_timeouts` (which `serve` uses,
with `idle_timeout_ms`) also bounds the whole request -- head and body --
from its first byte. A request's head has a shorter limit of its own (`header_timeout_ms`, from its first byte to the blank line), and so does
a new connection's first byte: a client that opens a connection and says nothing, or sends its head a byte at a time, is let go
after that long rather than after `idle_timeout_ms`, so a handful of them cannot keep every connection slot from real clients.

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

// Routes with path parameters, a JSON reply, and a 404 for everything else. The client module's
// example shows the same server from the other side.
//
// `route` answers Some only when method and pattern both match, and the match carries the path
// parameters -- so one `if` both tests the route and names what it captured.
fn handle(req: http.Request) -> http.Response {
    if Some(user) = server.route(req, HttpMethod.Get, "/users/{id}") {
        val id = server.param(user, "id")
        return server.json_response(200, concat(concat("{\"id\":\"", id), "\"}"))
    }
    return server.not_found_or_method_not_allowed(req, ["GET /users/{id}"])
}

// The two requests, which answer the same ClientError; decoding the body answers a plain string.
fn exchanges(base: string) -> Result<unit, client.ClientError> {
    val found = try client.get(concat(base, "/users/42"))
    if Ok(body) = bytes_to_string(found.body) {
        print(body)
    }
    val other = try client.get(concat(base, "/other"))
    print(other.status)
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

    if Err(e) = exchanges(format("http://127.0.0.1:{}", net.listener_port(listener))) {
        printf("server example failed: {}\n", client.client_error_message(e))
    }
}
```

Output:

```
{"id":"42"}
404
```

## `server.zm`

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
    head_timeout_ms: int
    fresh: bool
    // The largest request body `read_request` will accept, or 0 for this module's 1 MiB default.
    // `serve` fills it from `ServeOptions.max_body_bytes`; the `open_session*` constructors leave it
    // 0, so a caller reading requests off its own socket keeps the default until it says otherwise.
    //
    // It is here rather than only on `ServeOptions` because `read_request(session)` is the public
    // entry point and the North Star backend calls it directly -- a limit the server knows has to
    // reach it somehow, and widening that signature would break every such caller (D21, D22).
    max_body_bytes: int
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

### `open_session_with_timeouts`

```
pub fn open_session_with_timeouts(sock: Socket, timeout_ms: int, head_timeout_ms: int) -> Session
```

Like `open_session_with_timeout`, and the head of each request -- from its first byte to the blank line that ends it -- must also arrive within
`head_timeout_ms` (a client that is honest sends it at once). For a session on a new connection the socket's own timeout must already be
`head_timeout_ms`, which is then also how long the first byte may take; once the first request is read, the socket goes back to `timeout_ms`, the wait
between requests of a keep-alive connection. `serve` opens its sessions this way with `ServeOptions.header_timeout_ms`.

## `read.zm`

### `read_request`

```
pub fn read_request(session: Session) -> Result<Exchange, HttpError>
```

Reads the next request off the session. `Err(Closed)` when the client
closed the connection between requests (the normal way a keep-alive
connection ends); other errors mean the request itself was unusable --
`error_response` maps one to the HTTP response to send before closing.

## `respond.zm`

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
pub fn method_not_allowed(allowed: variadic string) -> Response
```

A 405 response with an `Allow` header listing `allowed`, and a JSON error body.

### `error_response`

```
pub fn error_response(e: HttpError) -> Response
```

The response to send for a request that couldn't be read. (`Closed` and
`Io` mean there is nobody to answer; the caller just closes.)

### `error_status`

```
pub fn error_status(e: HttpError) -> int
```

The status a rejection of `e` carries, and the sentence that goes with it. These exist so an application can
render the server's own rejections in its own error shape: `error_response` is the library's rendering of the
same pair, and `ServeOptions.fault` is given these two rather than the `HttpError`, so an application does not
have to match on a library enum to answer a 413.

### `error_text`

```
pub fn error_text(e: HttpError) -> string
```

### `write_response`

```
pub fn write_response(sock: Socket, req: Request, response: Response) -> Result<bool, HttpError>
```

Serialises and sends `resp` (adding Content-Length and Connection), and
says whether the connection stays open for another request: HTTP/1.1
keeps alive unless either side asked to close, HTTP/1.0 only if the
client asked to keep it. A `HEAD` request gets the headers, with the
Content-Length the body would have had, and no body.

### `compressed`

```
pub fn compressed(req: Request, resp: Response, min_bytes: int) -> Response
```

`resp` with its body gzipped, when that is both allowed and worth doing; `resp` unchanged
otherwise. A separate function rather than something `write_response` does for you, for the same
reason the CORS pair is two functions: it composes with `serve`, a hand-written loop or a static
file handler by sitting in front of whichever one writes.

**Off unless asked for** (`ServeOptions.compress_responses`), and that default is a security choice, not
caution about the CPU. Compressing a response that holds a secret alongside attacker-influenced
text leaks the secret through the compressed length -- the BREACH class -- and an API whose login
answer carries a JWT is exactly that shape. A server that wants the bandwidth asks for it; no
existing server gets a compression oracle switched on underneath it.

Four conditions, all of which have to hold:
  * the request's `Accept-Encoding` offers gzip (`identity` and absence both mean no),
  * the handler has not already set `Content-Encoding` -- it may be sending something already
    compressed, and gzipping that twice is slower and larger,
  * the body is at least `min_bytes`; under a packet's worth there is nothing to win and gzip's
    own header can make it bigger,
  * and the result is actually smaller than the input, which incompressible bodies (a JPEG, an
    already-encrypted blob) fail -- checked rather than assumed.

`Vary: Accept-Encoding` goes on every answer this touches, compressed or not, for the same reason
`cors_apply` sets `Vary: Origin`: whether the body is gzipped is what varies by the header, so a
shared cache without it can hand a gzipped body to a client that never asked for one
(tasks.md F61, and F59 for the identical hazard with Origin).

### `write_error`

```
pub fn write_error(sock: Socket, e: HttpError) -> Result<bool, HttpError>
```

Sends the response for a request that couldn't be read (see
`error_response`) and closes the exchange: there is no parsed Request to
consult, so it always says `Connection: close`. Nothing to send for
`Closed`/`Io` -- the peer is gone -- so it succeeds without writing.

### `write_error_with`

```
pub fn write_error_with(
    sock: Socket,
    e: HttpError,
    fault: Option<fn(int, string) -> Response>
) -> Result<bool, HttpError>
```

`write_error`, but rendering the rejection through `fault` when there is one -- see `ServeOptions.fault`. The
application gets the status and the sentence, not the `HttpError`, and what it returns is written as-is, so its
body shape and any header it adds (an HSTS header, say) are on the rejection too.

## `route.zm`

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

## `multipart.zm`

### `Part`

```
pub struct Part {
    name: string
    filename: Option<string>
    content_type: Option<string>
    body: [byte]
}
```

One part of a `multipart/form-data` body: the form field's `name`, a `filename` that is `Some` only for
an upload, the `Content-Type` the part declared, and its bytes.

`filename` and `content_type` are **the client's claims about its own data**, not facts. The filename is
the name some other machine's filesystem had: show it, store it as text, but never open it as a path --
it may be `../../etc/passwd`, an absolute path, or 300 bytes of control characters. The content type is
likewise a claim; sniff the bytes if what they are matters.

`body` is bytes rather than a string because a part may be a file: it can hold a NUL, invalid UTF-8 or a
JPEG, and nothing here decodes or validates it.

### `multipart_max_parts`

```
pub val multipart_max_parts = 128
```

How many parts `multipart` reads before it refuses the body; `multipart_with` takes another limit. A body
of nothing but boundary lines would otherwise make one value per 40 bytes of request.

### `multipart_boundary`

```
pub fn multipart_boundary(req: Request) -> Option<string>
```

The boundary from the request's `Content-Type`, when that is a `multipart/form-data` giving one; `None`
otherwise.

This is separate from `multipart` so a handler can tell two different answers apart: a request that is
not multipart at all is a **415**, while everything `multipart` rejects is about the body and is a
**400**. Check this first when that difference matters -- `multipart` on a JSON body reports a 400,
since a body is all it is ever asked about.

### `multipart`

```
pub fn multipart(req: Request) -> Result<[Part], HttpError>
```

The parts of a `multipart/form-data` request body, up to `multipart_max_parts` of them. Every `Err` is a
`BadRequest`, so `error_response` answers it 400: the `Content-Type` gave no boundary, the body contained
none, it ran out before the closing `--boundary--`, a part had no blank line after its headers or no
`name` in its `Content-Disposition`, or there were more parts than the limit.

The whole body is already in memory -- `read_request` caps it at this module's `max_body_bytes`,
a fixed 1 MiB -- so this is one pass over bytes and streams nothing. The cap applies to the
encoded body, boundaries included.

The cap is **not** configurable today: it is this module's constant, not a `ServeOptions` field,
which is what these two comments used to say it was -- published advice to raise something a
caller has no way to reach (D21). Making it a field is D22, and changes either `read_request`'s
public signature or `Session`'s shape, which is why it is not done here.

What it handles, because a real client sends it: a preamble before the first boundary and an epilogue
after the last (RFC 2046 says to ignore both); parts with no `Content-Type`; empty parts; header and
parameter names in any case; parameter values quoted or bare; and a quoted `filename` holding a `;` or an
escaped `\"`, which is why the `Content-Disposition` line is scanned rather than split on ";". What it
does not: RFC 2231's `filename*=` extended form is a different parameter name, so it reads as absent
rather than being decoded, and a nested `multipart/mixed` part is returned as the bytes it is.

### `multipart_with`

```
pub fn multipart_with(req: Request, max_parts: int) -> Result<[Part], HttpError>
```

`multipart` with its own limit on how many parts to read.

### `part`

```
pub fn part(parts: [Part], name: string) -> Option<Part>
```

The first part with that field name, since a form may repeat one.

### `part_text`

```
pub fn part_text(parts: [Part], name: string) -> Result<Option<string>, string>
```

The first part named `name` as text: `Ok(None)` when no part has that name, and `Err` when the part is
there but its bytes are not text. The error is the outer answer so a caller that is already returning a
`Result` can `try` this and go on with an `Option<string>`; an absent field and a field holding a JPEG
are different things, and neither is silently the other.

## `handler.zm`

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

An endpoint may list several, and **which one answers is the client's choice, not the order here**: each
one's media type gets the request's `Accept` weight (`http.quality`) and the heaviest wins, so an endpoint
offering `text/plain` and `text/csv` answers `Accept: text/csv, text/plain;q=0.5` with CSV. This order
breaks a tie, which makes it the server's preference among types the client likes equally. A type the
client gave `q=0` is refused rather than used as a last resort, and an endpoint whose every encoder is
refused answers 406.

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

### `json_decoder`

```
pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> Decoder<I>
```

`#[derive(Json)]` already gives a type its `<snake_type>_to_json` and `<snake_type>_from_json`;
these three turn one of those into the `Decoder`/`Encoder` an endpoint wants, so a typed JSON
endpoint is one call per direction instead of a closure spelled out at every route.

Promoted out of the reference backend rather than invented here: it had written `json_encoder`,
`list_encoder` and `json_decoder` itself, and its own comment records why -- "every endpoint
below used to spell these out, **105 times** between them", with the list form running to seven
lines each, "which is 105 places for the media type or the JSON writing to drift apart". Any
second JSON service would have written the same three functions, which is what makes them the
library's job (docs/roadmap/'s content-types entry, tasks.md F43).

`media` is `application/json` and not an argument: an endpoint that answers some other media type
wants `encoder`/`decoder` directly, and offering a media parameter here would only let these two
disagree about what they are for.

### `json_encoder`

```
pub fn json_encoder<O>(to_json: fn(O) -> Json) -> Encoder<O>
```

### `json_list_encoder`

```
pub fn json_list_encoder<O>(to_json: fn(O) -> Json) -> Encoder<[O]>
```

The list counterpart, which is the one worth having: written out it is a `map` into a
`JArray` at every route that answers a collection.

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

### `Api`

```
pub struct Api<E> {
    failure: fn(E) -> Response
    fault: fn(int, string) -> Response
}
```

How an application answers what goes wrong, given once and passed to every builder below as `api:`. `failure(e)` builds
the response for an `Err` of the endpoint's own error type `E`, and `fault(status, message)` the one for what the library
itself rejects (400, 404, 405, 406, 415), so an application's error bodies have one shape:
`val api = server.Api<ApiError> { failure: failure, fault: fault }`.

### `secure_handler`

```
pub fn secure_handler<C, U, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    encoders: [Encoder<O>],
    status: Option<int>,
    run: fn(C, U) -> Result<O, E>,
    api: Api<E>
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
    status: Option<int>,
    run: fn(C, U, P) -> Result<O, E>,
    api: Api<E>
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
    status: Option<int>,
    run: fn(C, U, Q) -> Result<O, E>,
    api: Api<E>
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
    status: Option<int>,
    run: fn(C, U, I) -> Result<O, E>,
    api: Api<E>
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
    status: Option<int>,
    run: fn(C, U, P, I) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A secured endpoint with both an id in the path and a request body (a `PUT /things/{id}`): `path` decodes the parameters
into a `P` and the body is decoded with the decoder for its media type; `run(ctx, caller, path_input, body_input)`
returns the answer or an error. Either input failing to decode is a 400.

### `public_handler`

```
pub fn public_handler<C, O, E>(
    encoders: [Encoder<O>],
    status: Option<int>,
    run: fn(C) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A public endpoint with no input: `run(ctx)`. Anyone may call it.

### `public_path_handler`

```
pub fn public_path_handler<C, P, O, E>(
    path: fn(Params) -> Result<P, string>,
    encoders: [Encoder<O>],
    status: Option<int>,
    run: fn(C, P) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A public endpoint whose input is in the path: `run(ctx, input)`.

### `public_body_handler`

```
pub fn public_body_handler<C, I, O, E>(
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: Option<int>,
    run: fn(C, I) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A public endpoint with a request body: `run(ctx, input)`.

### `public_path_body_handler`

```
pub fn public_path_body_handler<C, P, I, O, E>(
    path: fn(Params) -> Result<P, string>,
    decoders: [Decoder<I>],
    encoders: [Encoder<O>],
    status: Option<int>,
    run: fn(C, P, I) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A public endpoint with both an id in the path and a request body: `run(ctx, path_input, body_input)`.

### `Routes`

```
pub struct Routes<C> {
    endpoints: [Endpoint<C>]
    patterns: [Pattern]
    by_method: Map<string, [int]>
    by_length: Map<int, [int]>
    // Whether `dispatch` records a metric per request. Set by `metered_routes`, not by `routes`:
    // the matched pattern is known here and nowhere else, which is the one label worth having and
    // the reason this lives on the table rather than in `ServeOptions`.
    meter: bool
}
```

A table of endpoints compiled for dispatching: each pattern split once, and the endpoints grouped by method
and number of path segments (in the order they were given) so a request only meets the ones that could match.
Build it once with `routes`.

## `ratelimit.zm`

### `rate_allow`

```
pub fn rate_allow(bucket: string, key: string, limit: int, window_seconds: int) -> bool
```

Whether this event is within `limit` for `key` in the last `window_seconds` -- and asking counts it,
so ask exactly once per event. `bucket` separates unrelated limits that share a key: `"login"` and
`"register"` keyed by the same address are two allowances, not one.

What it is for: making an unauthenticated flood expensive. Per-account lockout already covers a
password guessed at one account (and throttles the email tried whether or not the account exists);
what it cannot cover is volume from one source against many accounts, or against an endpoint that
does real work for an anonymous caller -- registration above all, which asks OpenSSL for a ~19 MiB
Argon2id buffer before it knows who is asking (B10).

Fixed window, not a token bucket: a client can spend its whole allowance at the end of one window
and again at the start of the next, so the honest worst case is twice `limit` over a window's width.
That is stated rather than hidden because it is the kind of thing a reader should not have to derive.

At most 4096 distinct keys are tracked at once. Past that, an entry whose window has expired is
reused, and failing that the oldest window is replaced -- so a determined flood can push a
legitimate caller's count out and hand them a fresh allowance. That is a bounded loss of accuracy,
chosen over refusing everyone (a denial of service) or allowing everyone (no protection at all);
src/native_http.c says why at more length.

### `rate_allow_at`

```
pub fn rate_allow_at(bucket: string, key: string, limit: int, window_seconds: int, now_seconds: int) -> bool
```

`rate_allow` with the clock supplied, the same way `jwt.verify_at` takes the time rather than reading
it. A test can then show a window expiring without waiting for one, and a caller with its own idea of
now -- a replayed log, a simulation -- can use that instead. `now_seconds` must not go backwards for a
given key: a window is "has `now` passed `window_start + window_seconds`", so a clock that jumps back
extends the current window rather than opening a new one.

### `rate_reset`

```
pub fn rate_reset()
```

Forgets every counter. For a test that wants a known starting point -- a server calling this would
hand every client a fresh allowance.

### `open_connections_of`

```
pub fn open_connections_of(address: string) -> int
```

How many connections `address` holds open right now. For a test; a server has no reason to ask.

### `forwarded_for`

```
pub fn forwarded_for(req: Request, trusted_hops: int) -> Option<string>
```

The address to rate-limit a request by, for a server that sits behind proxies **it owns**.

`trusted_hops` is how many proxies of your own are in front of this server. Read it that way and
nothing else: the function takes the entry that many places from the *right* of `X-Forwarded-For`,
because each proxy appends the address it saw and only the rightmost entries were written by
machines you control. Everything to the left of those was supplied by the client, which can write
whatever it likes there.

So `trusted_hops: 0` returns `req.peer` and ignores the header entirely, which is the right answer
when nothing is in front. `trusted_hops: 1` behind one proxy of yours returns the address that proxy
saw. Getting the number too *high* is the dangerous direction: it starts trusting entries the client
wrote, which is how a rate limit becomes decoration. Too low merely limits the proxy instead of the
client, which is visible immediately.

There is no default and no auto-detection on purpose. A library cannot know what is in front of a
server, and guessing would mean every deployment that is *not* behind a proxy accepts a header its
clients control.

## `metrics.zm`

### `declare_metrics`

```
pub fn declare_metrics()
```

Declares what `metered_routes` and `ServeOptions.metrics` record. Calling it again is a no-op, so
an application that wants the names documented before any request may call it itself.

The latency buckets are seconds, by Prometheus convention. Note the resolution they are measured
at: `clock.monotonic_millis` is the finest clock Ziman has, so every request faster than a
millisecond lands in the first bucket and a latency histogram cannot tell 100us from 900us. For
a handler doing real work that is unimportant; for one answering from memory it means the
histogram says "fast" and nothing finer.

### `metrics_response`

```
pub fn metrics_response() -> Response
```

The label key for one request. Built by hand rather than through `metrics.series`, which sorts
and escapes: these three values are a method name, a route pattern and a status, none of which can
contain a quote, a backslash or a newline, and the names are already in increasing order.

`route` is the *matched pattern*, never the request's path. A path carries ids, so labelling by it
would grow the registry until the series cap dropped everything after the first 128 -- which is
the usual way a metrics integration takes a service down. A request that matched nothing is
labelled `<unmatched>` for the same reason.
Everything recorded so far, as the body of a `/metrics` endpoint -- the content type Prometheus
expects, and the version it has used since 0.0.4. Mount it in one line:


```
server.Endpoint<Ctx> { method: HttpMethod.Get, pattern: "/metrics", handler: server.metrics_handler<Ctx> }
```

Nothing here restricts who may read it. A metrics page tells a reader the shape of your traffic
and which routes exist, so put it behind the same `who` as the rest of an internal API, or on a
listener that only your network can reach.

### `metrics_handler`

```
pub fn metrics_handler<C>(ctx: C, req: Request, params: Params) -> Response
```

### `routes`

```
pub fn routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>
```

### `metered_routes`

```
pub fn metered_routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>
```

`routes`, and `dispatch` on the result records `http_requests_total` and `http_request_seconds`
for every request, labelled with the method, the pattern that matched and the status. One line to
turn on, and the pattern is a label only something that did the matching can supply.

## `cors.zm`

### `CorsPolicy`

```
pub struct CorsPolicy {
    allowed_origins: [string]
    allowed_methods: string
    allowed_headers: string
    max_age_seconds: int
    allow_credentials: bool
}
```

Which origins may see an answer, and what a preflight is told the browser may then send. `allowed_origins`
is matched exactly; a single `"*"` allows any origin, which is only sound without credentials (a browser
rejects `*` together with `Access-Control-Allow-Credentials`, so `allow_credentials` is ignored then).

### `cors_policy`

```
pub fn cors_policy(origins: [string]) -> CorsPolicy
```

A policy for `origins` with the defaults a JSON API wants: the usual methods, `Authorization` and
`Content-Type` when the browser does not say which headers it needs, a one-hour preflight cache, and
credentials allowed. Override what differs with `with`.

### `cors_preflight`

```
pub fn cors_preflight(policy: CorsPolicy, req: Request, fault: fn(int, string) -> Response) -> Option<Response>
```

The answer to a browser's preflight (`OPTIONS` carrying `Access-Control-Request-Method`), or None when
`req` is not one and should be routed normally. A preflight from an origin not on the list is
`fault(403, ...)` rather than a bare rejection, so it looks like every other refusal this server makes.

### `cors_apply`

```
pub fn cors_apply(policy: CorsPolicy, req: Request, answer: Response) -> Response
```

`answer` with the CORS headers a browser needs to hand it to the page, when the request carried an `Origin`
on the list. Without an `Origin`, or from an origin not on it, `answer` is returned untouched -- which is
what makes the browser withhold it, and is the whole mechanism: a server does not refuse the request, it
declines to say the answer may be read.

### `dispatch`

```
pub fn dispatch<C>(table: Routes<C>, ctx: C, req: Request, fault: fn(int, string) -> Response) -> Response
```

Calls the handler of the first endpoint whose method and pattern match `req`. When none does, the answer is
`fault(404, ...)`, or `fault(405, ...)` with an `Allow` header when the path exists under another method.

A `HEAD` that matches nothing is tried again as `GET`, since RFC 9110 makes HEAD a GET without a body and a
path serving GET has to serve HEAD; the GET handler's body comes back as usual and `write_response` withholds
it on the wire while keeping the `Content-Length` it would have had. `Allow` therefore names HEAD wherever
GET is served.

## `serve.zm`

### `RequestLog`

```
pub struct RequestLog {
    method: HttpMethod
    target: string
    status: int
    duration_ms: int
    response_bytes: int
    peer: Option<string>
}
```

### `ServeOptions`

```
pub struct ServeOptions {
    idle_timeout_ms: int
    header_timeout_ms: int
    // The most requests served at once -- not the most clients connected. A connection holds one
    // of these only while it is actually serving a request; between requests it is parked with the
    // poller and holds nothing, so a server can hold far more keep-alive clients than this number
    // (`net.wait_readable`, and `park_loop` below).
    //
    // It used to mean the other thing, and the difference is worth knowing if you are carrying a
    // size forward: a connection held a slot for its whole life, so under keep-alive an occupant
    // kept one through `idle_timeout_ms` of doing nothing and a 65th client against a cap of 64
    // waited for a *timeout* rather than for a request. 256 keep-alive clients against a cap of 64
    // took one full idle timeout whatever the request count -- 31.5s -- where they now take
    // 1.5-2.0s and a cap of 64 measures the same as a cap of 256 (docs/reviews/tasks/ F139).
    //
    // So size it by the concurrency of *work* you want to admit, which is what a thread-per-request
    // server can usefully run at once, and let the client count follow from sockets.
    max_connections: int
    log_requests: bool
    // The largest request body to accept, or 0 for the library's 1 MiB default. A body over it is
    // refused with 413 before the handler is reached, and the limit applies to what arrives on the
    // wire -- the encoded body, `multipart` boundaries included.
    max_body_bytes: int
    // How to render a rejection the server makes before the handler is ever reached -- a 400 for a malformed
    // request line, a 408 when the client stops mid-request, a 413 for too large a body, a 431 for too large a
    // header block, a 501 for a method the parser will not take. `None` uses the library's own JSON shape.
    //
    // Pass the same renderer the application gives `dispatch`, and a client sees one error shape from the whole
    // server instead of two: a browser that cannot read the body of its own 413 because it arrived in a
    // different shape with none of the application's headers is the reason this exists. Whatever it returns is
    // written as-is, so an HSTS header it adds is on the rejection too.
    //
    // It gets a status and a sentence, not the Request: these failures happen while the request is being read,
    // so there may be no parsed request to pass -- which is also why a per-request header like
    // `Access-Control-Allow-Origin` cannot be set here, there being no `Origin` to answer.
    fault: Option<fn(int, string) -> Response>
    // Called once per finished exchange, with everything the server knows about it. This is the
    // composable half of request logging: `log_requests` gives the library's one fixed line, and
    // this gives the application the fields to write its own -- through the `log` module, as JSON,
    // with a request id, or wherever its logs actually go. The two are independent; setting both
    // prints the built-in line *and* calls this.
    //
    // A field on the options, not a wrapper around `dispatch` as the roadmap first imagined: a
    // wrapper would have to return a closure and allocate one per request, which is the same reason
    // the CORS pair is two plain functions (tasks.md F59). One function value is stored here once.
    //
    // **Only completed exchanges.** A request the server rejects before parsing one -- a malformed
    // request line, a client that stops mid-head, a body over the cap -- never reaches this, because
    // there is no request to describe; the same reason `fault` above takes a status and a sentence
    // rather than a `Request`. Those rejections are `fault`'s to render.
    //
    // Typed `fn(RequestLog)` and not `fn(RequestLog) -> unit`: a void function's type *is* the
    // former, so an application writes `fn note(entry: RequestLog) { ... }` and passes it, with no
    // `-> unit` to remember. This is the library's first callback that returns nothing, so the
    // spelling is set here.
    on_request: Option<fn(RequestLog)>
    // Whether to record `http_open_connections` and the two byte counters. Per-request metrics are
    // separate, on the route table (`metered_routes`), because only something that matched the request
    // knows the pattern to label it with -- and a pattern is the only label worth having.
    metrics: bool
    // Gzip a response when the client offers it and the body is at least 1 KiB -- see `compressed`,
    // which this simply turns on. Off by default on purpose: compressing a response that carries a
    // secret alongside attacker-influenced text leaks it through the length (BREACH), and an API
    // whose login answer holds a JWT is that shape. Ask for it per server rather than inheriting it.
    compress_responses: bool
    // How many connections one address may hold at once. 0 is off, and is the default a server should keep
    // unless it is reached directly by clients: behind a reverse proxy every connection carries the
    // proxy's address, so any cap would be a cap on the proxy.
    //
    // This is the half a request throttle cannot cover. A client that opens connections and never sends a
    // request is counted by no rate limit, and can fill every `max_connections` slot;
    // `header_timeout_ms` frees such a slot within seconds, which bounds how long the flood holds one, not
    // how many it holds at once. A refusal here closes the socket without writing anything -- answering
    // would mean writing to a flooding client from the accept loop, where one slow read stalls every other
    // connection's accept.
    //
    // The address is the socket's, necessarily: at accept time there is no request yet, so there is no
    // `X-Forwarded-For` to consult even if it were trustworthy. That is the same reason the default is off.
    max_connections_per_client: int
}
```

### `serve_options`

```
pub fn serve_options() -> ServeOptions
```

Serves `listener` until a shutdown is requested, answering each request with `handler`; see
`ServeOptions` for the limits and the description above for how connections and shutdown behave.
`ServeOptions` with defaults a small server can take as they are, to be narrowed with `with`:


```
server.serve(listener, handler, with server.serve_options() { log_requests: true })
```

Prefer this to writing the struct out. Every field of a struct literal is required, so a server
that lists all of them stops compiling the day this struct gains one -- which is exactly what
happened to four fixtures and the reference backend when `compress_responses` was added
(tasks.md F61). Built with `with`, the same code keeps working and picks up the new field's
default.

The defaults, and why: a 30-second idle timeout and 10-second header timeout, which are long
enough for a slow mobile client and short enough that a stalled one frees its thread; 64
connections -- which since F139 is a limit on requests in flight rather than on connected
clients, so it no longer caps how many clients a server can hold; no request logging, because a
log line per request is a
decision about volume; no `fault` renderer, so the library's own JSON shape is used; no metrics
and no response compression, both off for the reasons their own comments give; and no per-client
connection cap, which is wrong by default behind a reverse proxy where every connection carries
the proxy's address.

### `serve`

```
pub fn serve(listener: Listener, handler: fn(Request) -> Response, options: ServeOptions)
```
