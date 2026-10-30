# Networking, HTTP and regular expressions

Part of the [v0 syntax reference](README.md).

## Networking

The `net` core module is blocking TCP; `[byte]` helpers are prelude builtins, plus binary
encodings in `number`.

`net.connect_unix(path)` and `net.listen_unix(path)` do the same over a Unix-domain socket, which is
a filesystem path rather than a host and port -- a local PostgreSQL or MySQL usually listens on one.
The `Socket` behaves identically afterwards (`read`, `write`, `lines`, `set_timeout`, `close`, even
`start_tls`). Two differences, both reported rather than hidden: the path is limited to 107 bytes by
the operating system, and binding a path that already exists is refused -- a socket left by a
process that has gone looks exactly like a live one from here, so it is not removed for you
(`fs.remove` it once you know which it is). `listener_port` answers -1 for such a listener.

```ziman
use net
fn serve(l: net.Listener) {
    val client = unwrap(net.accept(l))
    unwrap(net.write_text(client, concat("echo:", unwrap(net.read_text(client, 1024)))))
    net.close(client)
}
fn main() {
    val l = unwrap(net.listen("127.0.0.1", 0))        // port 0: the OS picks one
    spawn serve(l)
    val s = unwrap(net.connect("127.0.0.1", net.listener_port(l)))
    unwrap(net.write_text(s, "hi"))
    print(unwrap(net.read_text(s, 100)))               // echo:hi
    net.close(s)
}
```

- **Blocking, thread-per-connection** -- a blocked `accept`/`read` blocks only its own thread;
  `spawn` a handler per connection. No event loop, no async.
- **Handles.** `Socket`/`Listener` are `{ handle: int }` over a native table, like
  `datasource.Connection`. Closing is explicit (`net.close`/`net.close`) -- no destructors,
  so a `try` returning early between `connect` and `close` leaks the socket. Close from the
  owning thread.
- **Reading.** `read(s, max)` blocks for at least one byte, returns up to `max`. `read_exact(s,
  n)` returns exactly `n` or errors (length-prefixed protocols). `read_text`/`write_text` are
  for text (a NUL is reported, not truncated). `write` sends every byte before returning.
- **EOF is an error**: a clean peer close makes a read return `Err(Closed)`, never an empty
  array -- match `NetError.is_Closed` where end-of-stream is legitimate.
- **`NetError`** is `ConnectionRefused | HostNotFound | Timeout | Closed | Io`, each with a
  message; `net_error_message(e)` renders one.
- **Timeouts**: `net.set_timeout(s, millis)` applies to reads/writes (else a dead peer waits
  forever); no connect timeout beyond the OS default.
- **`to_bytes`/`bytes_to_string`** are prelude natives: `bytes_to_string`
  is a `Result` (`Err` on invalid UTF-8, naming the byte offset, or a 0 byte) -- the one real
  boundary where untrusted bytes become a `string`, so the one place UTF-8 is checked (see
  "Strings are bytes" in "Builtins"). **`to_bytes` was `string_to_bytes` until 2026-10-22**: the
  prefix named argument one's world, which the argument says, and a conversion is named for what it
  answers. Its twin keeps the longer name on purpose — `to_string` would be a third spelling beside
  `str(x)` for scalars and `snapshot(sb)` for a builder, and the two were never mirror images
  anyway, since every string has bytes and not every byte sequence is a string. Those two are
  named for what they answer; the operations
  *on* bytes need no `bytes_` name of their own, because `[byte]` is an array: `concat(parts...)`
  joins many byte arrays in one pass, `index_of(haystack, needle, from)` finds a delimiter in one
  (both bare since F213, where they were `bytes_concat` and `bytes_index_of`), and sub-ranges are
  `slice<byte>(b, start, end)` (zero-copy).
- **`number`**: big-endian `u16_to_bytes`/`u32_to_bytes`/`u16_from_bytes`/`u32_from_bytes` and
  float encodings network protocols are built from.
- **Lines**: `net.lines(socket)`/`lines_with(socket, max_line_bytes)` is a lazy
  `Iter<Result<string, NetError>>`, ended when the peer closes; each item has no `\n`/`\r\n`
  (a final line with none still counts), or an `Err` for a NUL byte, a line over 1 MiB (ends the
  iteration), or a read error. Reads ahead in blocks (take all lines from one iterator); doesn't
  close the socket.
- **TLS (client)**: `net.connect_tls(host, port)` is `connect` + `start_tls(sock, host)`,
  upgrading in place -- the same `read`/`write`/`set_timeout`/`close` work after. Verified by
  default (system CA store, hostname, TLS 1.2 minimum, SNI sent). `_with` forms take a
  `TlsVerify` (`Full`/`Chain`/`Off`) and an optional trusted-CA PEM. A handshake/verification
  failure is `NetError.Tls(message)` (OpenSSL's reason); `connect_tls` closes its socket on
  failure. A timeout still surfaces as `Timeout`, a peer close as `Closed`, over TLS.
- **Server-side TLS**: `listen_tls(host, port, cert_file, key_file)`. Not yet: client
  certificates, ALPN, Unix-domain sockets, UDP, a connect timeout beyond the OS default.

## HTTP

The `http` core module is an HTTP/1.1 **server toolkit** on `net`: parse a request off a
socket, route it, write a response -- plus a ready-made server loop.

```ziman ignore: `{ ... }` stands in for the handler's body
fn route(conn: datasource.Connection, req: http.Request) -> http.Response { ... }

val options = server.ServeOptions { idle_timeout_ms: 30000, header_timeout_ms: 5000, max_connections: 64, log_requests: true }
os.install_shutdown_handler()
server.serve(listener, fn(req) { route(conn, req) }, options)   // returns after SIGINT/SIGTERM
```

`server.serve(listener, handler: fn(Request) -> Response, options)` runs the accept loop and
the per-connection request loop, one thread per connection, at most `max_connections` at a
time (a channel as a counting semaphore; a client past the limit waits in the kernel's accept
queue). The handler is a function value (a lambda's captures are immutable, so safe to share
across connection threads). `serve` returns once a shutdown is requested
(`os.install_shutdown_handler()`) and every in-flight connection finishes; it neither closes
the listener nor installs the handler. Write your own accept/connection loop from the pieces
below (`open_session`, `read_request`, `write_response`) when `serve` doesn't fit.

- **Reading.** `read_request(session)` returns an `Exchange` (`Request` plus updated
  `Session`) or an `HttpError`. `Request` has `method`, `target`, `path`, `query`, `version`,
  `headers`, `body`; `http.header(req, "Name")` ignores case. `Err(Closed)` is a normal
  keep-alive end. Keep-alive (including pipelined requests) and `Expect: 100-continue` both
  work; `Content-Length` and `Transfer-Encoding: chunked` bodies are supported (any other
  transfer encoding is 501); `HEAD` gets headers with `Content-Length` and no body.
- **Bounded.** Headers total 16 KiB, a body 1 MiB (else `HeadersTooLarge`/`BodyTooLarge`, 431/
  413). `net.set_timeout` on the socket turns a stalled read into `Timeout` (408) -- but a
  per-read timeout alone doesn't stop a slowloris client trickling one byte at a time, so
  `server.open_session_with_timeouts(sock, idle_ms, head_ms)` bounds the *whole* request (from
  its first byte) to `idle_ms`, with a shorter `head_ms` for how long a new connection may take
  to send its first byte and head -- an honest client needs milliseconds, so a client that
  drips or says nothing frees its slot in seconds, not `idle_timeout_ms`, and can't starve
  `max_connections`. `Request`/`Response.body` is `[byte]` (may hold a NUL); the request line
  and headers are read as text, so a NUL there is a 400 (a Ziman `string` can't hold one). A
  flood from many addresses is for a firewall/proxy, not this layer.
- **No banner, one hardening header.** Every response carries `Date` and
  `X-Content-Type-Options: nosniff`, never `Server`; a handler setting either itself is not
  overridden. `Strict-Transport-Security` is the application's to add (it, not the library,
  knows whether TLS ends at the process or a proxy in front).
- **Strict about what a proxy might read differently** (request smuggling): 400 on differing
  `Content-Length` headers (identical repeats are fine), a non-digit `Content-Length` (`+5`,
  `5, 5`, `0x5`), two `Transfer-Encoding` or `Host` headers, no `Host` on HTTP/1.1, a
  non-token header name or method, or a control character (other than tab) in a header value
  or request line (a bare CR/LF would let one header line pass for two).
- **Response headers are checked on the way out.** `write_response` refuses a header whose
  name isn't a token or whose value has a control character (CRLF injection from unchecked
  client text) -- sends 500 in its place, closes the connection, logs to stderr, never the
  offending text. `with_header` itself never fails; the check is on the way out, where every
  response passes, since a `Response` can also be built as a plain struct literal.
- **Responses:** `server.response(status, body)`, `json_response`, `text_response`,
  `bad_request`, `not_found`, `method_not_allowed`, `with_header`. `write_response(sock, req,
  resp)` adds `Content-Length`/`Connection` and returns whether the connection stays open
  (HTTP/1.1 keeps alive unless either side said `close`; HTTP/1.0 closes unless asked to keep).
- **Methods:** `Request.method` is an `http.HttpMethod` -- `Get`/`Head`/`Post`/`Put`/`Patch`/
  `Delete`/`Options`, or `Other(name)` for anything else (case-sensitive: `get` is
  `Other("get")`). `http.method_parse`/`method_name` convert. `client` takes the same type.
- **Routing.** `server.route(req, method, pattern)` is `Some(params)` on a match, else `None`;
  `{name}` captures one non-empty, percent-decoded segment, a trailing slash is ignored, and
  `server.param(params, "name")` reads it (a name the pattern doesn't have is a fatal error --
  a bug in the route function, not a value to check). `not_found_or_method_not_allowed` is 405
  with `Allow` if the path matches a different method, else 404. `server.query_param(req, "q")`
  reads the query string (percent-decoded, `+` is a space). A `{id}` capture is always a
  *string*; converting it and answering 400 on a bad one stays in the application.
- **Typed endpoints.** `server.Endpoint<C> { method, pattern, handler }`, built by one of nine
  builders, chosen by where the input comes from -- `handler` (none), `path_handler` (`path:`
  turns captured `Params` into a `P`, an `Err` is 400), `body_handler` (`decoders:`),
  `path_body_handler` (both, for `PUT /things/{id}`), `*query_handler` (the query string,
  `query_param` the tool) -- crossed with who may call, `public_` or `secure_`. Every builder
  takes `encoders:`, `run:`, `api:`, and optional `status:` (default 200). `api:` is one
  `server.Api<E> { failure, fault }` per application: `failure(e)` renders an `Err` of the
  endpoint's own error type `E`, `fault(status, message)` renders what the library itself
  rejects. A `secure_` builder also takes `who: fn(C, Request) -> Result<U, Denied>`
  identifying the caller, passed to `run` as its second argument (`public_`'s `run` has no
  caller parameter); `Denied` is `Unauthenticated`/`Forbidden`/`Failed` (401/403/500),
  answered before the body is read, the path decoded, or a format negotiated. `run` returns
  `Result<O, E>` -- an ordinary function, no request-awareness needed. `encoders: []` answers
  with an empty body and no `Content-Type` (what a 204 needs). A `Decoder<I> { media, decode }`
  turns request bytes into an `I` (`Err` is 400); an `Encoder<O> { media, encode }` turns an
  answer into bytes -- built with `server.decoder`/`encoder` (bytes) or `text_decoder`/
  `text_encoder` (text; a body NUL is 400 there). An endpoint answers 415 with no decoder
  matching `Content-Type`, 406 with no encoder matching `Accept`. `Request`/`Response.body` is
  `[byte]` (`request_text`/`response_text` give text, or an `Err` on a NUL). `server.routes<C>`
  compiles a table once (a module-level `val`, not per request), splitting each pattern into
  segments and grouping by method/segment-count so a request only meets endpoints that could
  match; `server.dispatch<C>(table, ctx, req, fault)` calls the first match, 404, or 405 with
  `Allow`. Table order still decides `{param}` vs. a literal (`/items/{id}` before
  `/items/new`); `server.route`/`path_params` remain for a hand-written router.
- **Not yet:** HTTP/2, multipart, compression, trailers.

## Transactions (`datasource.with_transaction`)

`datasource.with_transaction<R>(conn, work: fn(Connection) -> Result<R, DbError>) ->
Result<R, DbError>` runs `work` in a transaction on one exclusive connection: borrows one from
a pool, begins, calls `work(tx)`, commits on `Ok` or rolls back on `Err`/a failed commit, and
hands the connection back on every path out, returning `work`'s result unchanged. `work` (a
function value, usually a lambda) must not call `begin`/`commit` itself.

```ziman ignore: `...` stands in for the statement's arguments
fn transfer(tx: datasource.Connection, t: Transfer) -> Result<int, DbError> {
    try datasource.execute(tx, "UPDATE acct SET balance = balance - $1 WHERE id = $2", ...)
    try datasource.execute(tx, "UPDATE acct SET balance = balance + $1 WHERE id = $2", ...)
    return ok(t.amount)
}
val moved = try datasource.with_transaction<int>(conn, fn(tx) -> Result<int, DbError> { return transfer(tx, t) })
```

## Regular expressions (`regex`)

```ziman fragment
use regex

val re = try regex.compile("(\\w+)@(\\w+)\\.com")           // Result<Regex, string>
val m = regex.find(re, "mail bob@site.com now")             // Option<Match>
regex.replace_all(re, "bob@site.com", "$2: $1")             // "site: bob"
```

`compile(pattern)` is `Err` with a message naming the problem's position; a `Regex` is
immutable (compile once, share across threads). `is_match`, `find`, `find_from`, `find_all`,
`group`, `group_named`, `replace`, `replace_all`, `split`, `escape` do the rest. A `Match` has
`start`/`end` (character indices, so a match composes with `substring` and `len`; they were byte
offsets until 2026-10-23), `text`, `groups` (`[Option<string>]`, 1..n). Replacements expand
`$0`, `$1`-`$9`, `${12}`, `$$`.

**Perl-flavoured, deliberately without backreferences or lookaround** -- matching runs in time
proportional to text length times pattern size, so a pattern from a request can't hang a
program (`(a+)+$` against many `a`s answers immediately). Supported: literals, `.`, classes
`[a-z]`/`[^...]` (with `\d \w \s` inside), `\d \D \w \W \s \S` (ASCII), `\b \B`, `^ $`, groups
`( )`/`(?: )`/named `(?P<year>...)`/`(?<year>...)`, `|`, `* + ? {n} {n,} {n,m}` (greedy, or lazy
with `?`). POSIX classes (`[[:alpha:]]` etc., ASCII, `[:^alpha:]` negates) and Unicode
properties (`\p{L}`/`\p{Lu}`/`\p{Ll}`/`\p{N}`/`\p{Nd}`/`\p{P}`/`\p{S}`/`\p{Sc}`/`\p{Z}`/`\p{M}`
and the rest of the general categories, plus main-block scripts `Latin Greek Cyrillic Armenian
Hebrew Arabic Devanagari Thai Hiragana Katakana Hangul Han`; `\P{L}`/`\p{^L}` negate, `\pL` is
`\p{L}`). A named group reads with `group_named(m, "year")`; a replacement uses `${year}` or
`$year` (`${year}_x` when text follows). Flags first: `(?i)` ASCII case-insensitive, `(?s)` `.`
matches newline, `(?m)` `^`/`$` match at line breaks. Text is UTF-8, `.`/classes match a whole
character, but `\d \w \s`/`(?i)` stay ASCII (use `\p{L}`/`\p{Nd}` for Unicode). `$` matches only
the very end unless `(?m)` (`^...$` for the whole text). A repetition count caps at 1000, a
compiled pattern at 20,000 instructions, and a pattern whose matching would need more than 64 MB of
capture buffers is refused when it compiles -- that last one is a cap on *capture groups* in
practice (about 1,170 of them), and the error says so and suggests `(?:...)` for a group whose text
you do not need. A match is also bounded in *work*, which that budget does not bound: a pattern that
is cheap to compile can still be expensive per input byte, so a search that would do more work than
its subject's length allows stops with a runtime error naming the subject's size, and a `find_all`
is budgeted as one scan rather than per match. Real patterns use a ten-thousandth of what is
allowed; what hits this is hundreds of capture groups over a short subject. `find_all` steps one
character past an empty match; `split` ignores empty matches.

### Trace context

`http` reads and writes W3C Trace Context's `traceparent`, so one request can be followed across
services. `traceparent_of(req)` answers the incoming trace, or a new one when the header is absent
or malformed -- a restart is what the spec requires of a receiver that cannot parse it, so this is
not an error path. `traceparent_header_for(p)` is the `Header` to put on an outgoing request: the
same `trace_id`, a new `span_id`, and the sampling decision inherited rather than retaken.
`traceparent_parse`/`_new`/`_child`/`_value` are the pieces underneath, and `p.trace_id` is what
goes to `log.with_request_id` so every line of a request carries it.

Four validation rules are easy to get wrong and are all enforced: an all-zero trace or span id is
invalid even though it is well-formed hex; version `ff` is reserved; an upper-case hex field is
rejected rather than lower-cased, because the header is compared as bytes elsewhere; and undefined
flag bits are ignored rather than refused. A *future* version parses when the rest of the value
does, since refusing one would break against a future sender for no gain.

## HTTP client (`client`)

`client.get(url)`, `client.post_json(url, body)`, and the general `client.request(method, url,
headers, body, timeout_ms, max_redirects, ca_file)` (everything after `url` optional, called
with named arguments) return `Result<http.Response, ClientError>`. **Any** HTTP status
(404, 500 included) is `Ok`; `Err` means the exchange itself failed: `BadUrl`/`Network`/
`Timeout`/`BadResponse`/`TooLarge`/`TooManyRedirects` (`error_message` renders one). `http://`
and `https://` (chain and hostname verified; `ca_file` for a private CA). A keep-alive
connection is parked and reused for the next request to the same host (`reuse: false` disables;
`close_idle()` drops parked ones; a dead one is detected and the request re-sent once). Reads
`Content-Length`, chunked, and until-close bodies; follows 301/302/303/307/308 up to
`max_redirects` (default 5, 0 returns the 3xx; 303 and a POST answered 301/302 re-send as a
body-less GET); caps a body at 8 MiB. Body is `[byte]` both ways (`request`'s `body:
Option<[byte]>`, the response's `[byte]` body -- untouched even with a 0 byte);
`http.response_text` reads text (`Err` on NUL); `post_json`/`put_json`/`post_form` take text.

For a response too large to hold or that never ends, `client.stream(method:, url:)` returns
status/headers at once and the body as an `Iter<Result<[byte], ClientError>>` of up-to-8192-byte
blocks, pulled by a `for` loop (a timeout/read error is a last `Err` item; leaving early closes
the connection) -- no redirects followed, no cookies sent, no compression decoded (asks `identity`).

Also built in: `gzip`/`deflate` response decoding (`Accept-Encoding` sent unless `compress:
false`); proxies from `proxy` (`""` = none) or `HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY`, `CONNECT`
tunnel for `https://` targets (an `https://` proxy itself unsupported); Basic auth from
`http://user:pass@host/` (Authorization/Cookie dropped on a cross-host/port/scheme redirect);
cookies via `request_with_jar(jar, method, url, ...)` (returns the updated jar; `cookie_jar()`
makes an empty one, no jar means nothing remembered); `post_form(url, Map)`/`form_encode`;
`response_header`; public `parse_url(url)`. Not yet: HTTP/2.
