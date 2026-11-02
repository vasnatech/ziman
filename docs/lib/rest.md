# `rest`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A REST toolkit over `http` and `server`: an endpoint is a method, a URL pattern and a
handler, a table of them is compiled once, and `dispatch` answers a request from it.

**Why this is not part of `server`.** It was, until 2026-11-01, and `lib/http/server/` was 2,900
lines holding two different things: an HTTP server -- accept loop, request parsing, keep-alive,
response writing -- and a REST framework on top of it. Different concerns and different readers:
someone writing a raw handler wants the first, someone writing a resource API wants the second.
Two symptoms said so before anyone decided it. `pub fn routes` lived in the file about metrics
and `pub fn dispatch` in the file about CORS -- neither on purpose, both because that was the
file that happened to be open -- and `cors.zm` had to reach for `Routes`, `capture` and
`segments_of` to keep `dispatch`, which was the single place the transport half depended on the
REST half. Moving those two functions out left the dependency pointing one way and the split
obvious: `http` is the protocol, `server` is the transport, `rest` is the framework.

**What `rest` holds**: `Endpoint` and `Api` (`handler.zm`), the ten handler constructors that do
the protocol work a controller should not -- 415 for a body in the wrong media type, 406 when
`Accept` allows none of the encoders, 400 for a body or a path id that does not decode, all
before the handler runs -- URL patterns and the request bindings (`route.zm`), the compiled table
(`table.zm`), and matching plus per-request metrics (`dispatch.zm`). `derive_routes.zm` beside
them is the `#![derive(Routes)]` generator, a module of its own because a generator is loaded as
a program rather than imported as a library.

**What it deliberately does not hold**: anything that reads or writes a socket. A `Response` is
built with `server`'s builders and a request arrives already parsed, so this module never sees a
byte of the wire.

`use rest` -- or `use rest.Name` to bring one item in bare.

## `rest.zm`

### `Params`

```
pub struct Params {
    names: [string]
    values: [string]
}
```

The `{name}` captures of a matched route, by position.

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
    encoders: [codec.Encoder<O>],
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
    encoders: [codec.Encoder<O>],
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
    encoders: [codec.Encoder<O>],
    status: Option<int>,
    run: fn(C, U, Q) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A secured endpoint whose input is in the query string (`GET /things?type=x`): `query` reads what it wants from the request, for
which `query_param` is the tool, and turns it into a `Q` (an `Err` message is answered 400, so a bad value never reaches `run`);
`run(ctx, caller, input)` returns the answer or an error.

### `public_query_handler`

```
pub fn public_query_handler<C, Q, O, E>(
    query: fn(Request) -> Result<Q, string>,
    encoders: [codec.Encoder<O>],
    status: Option<int>,
    run: fn(C, Q) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

`secure_query_handler` without the caller: an endpoint whose only input is its query string and
which anyone may ask. **Its absence was an asymmetry, not a decision** -- `#[route]`'s parameter
bindings asked for it by name the first time a query endpoint was written without a
`#[principal]`, which is how it was noticed.

### `secure_body_handler`

```
pub fn secure_body_handler<C, U, I, O, E>(
    who: fn(C, Request) -> Result<U, Denied>,
    decoders: [codec.Decoder<I>],
    encoders: [codec.Encoder<O>],
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
    decoders: [codec.Decoder<I>],
    encoders: [codec.Encoder<O>],
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
    encoders: [codec.Encoder<O>],
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
    encoders: [codec.Encoder<O>],
    status: Option<int>,
    run: fn(C, P) -> Result<O, E>,
    api: Api<E>
) -> fn(C, Request, Params) -> Response
```

A public endpoint whose input is in the path: `run(ctx, input)`.

### `public_body_handler`

```
pub fn public_body_handler<C, I, O, E>(
    decoders: [codec.Decoder<I>],
    encoders: [codec.Encoder<O>],
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
    decoders: [codec.Decoder<I>],
    encoders: [codec.Encoder<O>],
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

### `required`

```
pub fn required(given: Option<string>, name: string) -> Result<string, string>
```

The value, or `"<name> is required"` -- the message `read_query`'s hand-written version answered,
which `src/test/other` holds the API to.

### `given_or`

```
pub fn given_or(given: Option<string>, fallback: string) -> string
```

The value or a default, for `@RequestParam(defaultValue = "30")`'s equivalent. No `Result`: a
default means absence is not a failure.

### `as_int`

```
pub fn as_int(text: string, name: string) -> Result<int, string>
```

`text` as an int, or `"<name> must be a whole number: <text>"`.

### `as_uuid`

```
pub fn as_uuid(text: string, what: string) -> Result<Uuid, string>
```

`text` as a `Uuid`. **The message is `uuid_param`'s** -- "invalid <what> id: <text>" -- because a
client that matches on it should not be able to tell a generated binding from the hand-written
reader it replaced.

### `as_bool`

```
pub fn as_bool(text: string, name: string) -> Result<bool, string>
```

`text` as a bool, in any of the spellings the web actually sends. **Wider than
`xml.parse_bool`'s four on purpose**: that one implements `xs:boolean`, where the lexical space
is defined and `on` is not in it. A query parameter has no such definition, an HTML checkbox
sends `on`, and a hand-written reader in the North Star already took `on`/`off`/`yes`/`no` --
so binding to this must not be the thing that silently narrows what a client may say.

Case-insensitive for the same reason. Anything else is an `Err`, which a 400 carries:
`?includeDismissed=maybe` is a request to fix, not a false.

### `as_float`

```
pub fn as_float(text: string, name: string) -> Result<float, string>
```

`text` as a float, for a measurement or a threshold a client sends as a query parameter.

### `uuid_param`

```
pub fn uuid_param(kind: string) -> fn(Params) -> Result<Uuid, string>
```

A `{id}` path parameter is just a string; this turns it into a `Uuid`, and a bad one is answered 400
before the endpoint runs. `kind` names what the id belongs to, for the message ("invalid order id").

### `named_uuid_param`

```
pub fn named_uuid_param(name: string, kind: string) -> fn(Params) -> Result<Uuid, string>
```

The same as `uuid_param`, but for a captured segment named something other than `{id}`: a pattern with
two captures (`{orderId}/items/{itemId}`) needs to say which it means.

## `table.zm`

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

## `dispatch.zm`

### `dispatch`

```
pub fn dispatch<C>(table: Routes<C>, ctx: C, req: Request, fault: fn(int, string) -> Response) -> Response
```

### `metrics_handler`

```
pub fn metrics_handler<C>(ctx: C, req: Request, params: Params) -> Response
```

`/metrics` as an endpoint: register it like any other and the scraper reads what `server.declare_metrics`
declared. **Here rather than beside `metrics_response`** (which stayed in `server`) because it is
an `Endpoint` handler -- it takes `Params` and conforms to the handler signature -- and `Params`
is this module's.
