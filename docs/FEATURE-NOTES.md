# Feature notes

Long-form notes on what Ziman has today and why, moved out of the README so
that the README can stay a short introduction. Nothing here is normative:
[SYNTAX.md](SYNTAX.md) is the language reference, [DESIGN.md](DESIGN.md) the
reasoning behind each decision, [CORELIB.md](CORELIB.md) the generated
signature reference, and [ROADMAP.md](ROADMAP.md) what is still open. Some
passages describe the state at the time they were written (module counts,
"new" types); when they disagree with those documents, those win.

## Core language

v0: lexer, parser, a real type checker (static types, no implicit
coercion, "does this function return on every path" analysis), and a
tree-walking interpreter. Supports functions, `val`/`var` bindings,
`if`/`else`, `while`, `for` (value/index/index+value forms over an
array, or a range for a counted loop — `for x in arr`, `for i in 0..n`,
`for i, x in arr`), `break`/`continue`, seven scalar types (`int`,
`float`, `bool`, `string`, plus arbitrary-precision `bigint` and
`decimal` — `bigint` has `int`'s full operator/builtin surface,
`decimal` has `float`'s, both modeled on Java's
`BigInteger`/`BigDecimal` — and `byte`, unsigned 0-255, deliberately
*not* a number to compute with: no arithmetic, just construction,
`==`/`!=`, `hash`, `str`/`print`/`format`, and a saturating cast
to/from `int`), value-semantics arrays (`[T]`, nestable),
plain nominal value-semantics `struct`s (named-fields-only
construction, `with` for non-mutating field updates, no
methods/inheritance, field-by-field `==`/`!=` when every field is
itself comparable), multi-file programs via `use path/to/mod`
(qualified cross-module calls by default, `pub` for exports,
project-relative resolution first, falling back to a bundled core
module if no project file exists), or `use path/to/mod.Name [as alias]`
to import one function/struct/enum/global val under a bare local name
instead — resolved and privacy-checked once at the `use` line, not a
wildcard import (there's no `use mod.*`, and no unqualified name whose
origin isn't spelled out somewhere in the file's own `use` list),
`[pub] val NAME[: Type] = <expr>` at module top level for a global
value — evaluated once, before `main` runs, never per-call the way a
same-shaped local `val` is; no global `var`, since a mutable global
would be exactly the shared, lock-free mutable state Ziman's actor-
style concurrency (below) already exists to avoid, and unlike a local
`val`, a global one can never be shadowed by a same-named local (a
compile-time error, not silent shadowing) — see docs/DESIGN.md's
"Global val" entry for why, `native fn` for calling into
C (the `fs` module proves it
end to end), `Result<T, E>` for recoverable errors (`fs.read_text`
returns one instead of crashing on a missing file, variant-then-payload
`==`/`!=` when both `T` and `E` are comparable), user-declared `enum`
tagged unions with enum-qualified construction and compiler-generated
`is_<Variant>`/`unwrap_<Variant>` accessors, same shape `Result` itself
uses, and real OS-thread concurrency via `spawn` and a thread-safe
`Channel<T>` (a single-writer/actor pattern — one thread owns a piece
of mutable state, everyone else reaches it by message — rather than
shared memory and locks).

## Generics, bounds and `collections`

`struct`, `enum`, and top-level `fn` can all take type parameters
(`struct Box<T> { value: T }`), with explicit type arguments required
at every use site — `Box<int>{ value: 5 }`, `identity<string>(x)` — and
no inference, since `Value` is runtime-type-erased and nothing is ever
monomorphized. A type parameter can be bounded with `Eq`, `Hash` and/or `Ord`
(`<K: Eq + Hash>`) to use `==`/`hash()`/`compare()` on it inside a generic
body (`hash` is structural, so a `Uuid`, an enum or a struct of hashable
fields works as a `Map` key) (`collections.sort<T: Ord>` is a stable merge sort, and
`#[derive(Ord)]` writes a struct's or enum's order); these are the only
three bounds Ziman has — there's no general
trait/interface system (see docs/DESIGN.md's Generics entry for why,
and what's deliberately left undecided). A small `collections` module
built on this (`lib/core/collections.zm`) provides sorting, searching and the
higher-order array helpers. (`Map<K, V>` and `Set<T>` were `HashMap`/`HashSet`
there, bucket arrays with O(bucket_count) writes; they are now built-in types on
a persistent hash trie -- see docs/SYNTAX.md "Maps and sets".)
(`Option<T>` used to live here too — see the prelude paragraph below for
where it moved and why. `Box<T>` also used to live here — removed
outright, not moved, since nothing ever actually used it beyond being
the first generic struct ever written; `struct Box<T> { value: T }`
above is illustrative syntax anyone can still write, just not a
`collections` export anymore.)

## Bundled core modules: `random`, `uuid`, `clock`, `time`

It's also one of the *bundled core modules* (22 now) (alongside `fs`, mentioned
above, plus `random`/`uuid`/`clock`/`time`/`json`/`number`/`sql`/`db`/`net`/`crypto`/`pg`/`http` below): each one's source is
embedded into the `ziman` binary at build time, so `use collections`/
`use fs` work from any project with no local copy needed — a
project-relative file of the same name still wins if one exists, but
this is the fallback that means one usually doesn't have to. `random`
is one native function, `random_bytes(n: int) -> [byte]`, reading real
OS entropy (`/dev/urandom`). `uuid` is pure Ziman on top of it: a
`Uuid` struct, `uuid_new()` for an RFC 4122 v4 UUID, `uuid_to_string`
for the canonical dashed-hex form and `uuid_from_string` for its
inverse (accepting either case, the form any real caller getting a
UUID back as a string — a JSON field, a URL path segment — actually
has to parse), version/variant bits set with plain
integer arithmetic rather than bitwise operators (Ziman has none).
`Uuid` satisfies `Eq`, `Hash` (a structural hash, like `==`) and `Ord`
(`uuid_compare`), so it works as a struct field, a `HashMap` key and in
`collections.sort`. `clock` is one more native function, `now_raw() ->
[int]`, reading the system clock — Ziman's other source of genuine
non-determinism. `time` is pure Ziman on top of it: seven `java.time`-
shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`,
`Instant`, `ZoneOffset`, `ZoneId`; `ZoneId` reads the system's IANA
zone files, see the `zone` module), `Instant`/`Duration` as `{seconds: int, nanos: int}` rather than
a packed number (matching both C's `struct timespec` and `java.time`'s
own shape), and calendar math via Howard Hinnant's public-domain
days-from-civil algorithm. Every type has a fixed ISO-8601 `_to_string`/
`_parse` pair, and the three calendar-shaped types plus `Instant` also
get a general `_format`/`_parse_pattern` pair taking a `DateTimeFormat`
— a pattern (`yyyy`/`MM`/`dd`/`HH`/`mm`/`ss`/`SSSSSSSSS`, fixed-width
only, no `java.time.DateTimeFormatter`-style "letter count changes
meaning") *compiled once* by `datetime_format_of`, not re-scanned on
every call; the ISO pair is implemented on top of the general one via
`pub val iso_*_format` presets, each compiled exactly once at program
start and reused for the process's whole lifetime. None of the nine new scalar/struct types
across `byte`/`uuid`/`time` (`byte`, `Uuid`, and the `time` module's
seven) got `<`/`>` — every one instead gets an explicit `_compare(a, b)
-> int` (written when Ziman had no Ord bound; that function is now what
makes each of them `Ord`, so `compare` and `collections.sort` work on them).

## `json` and `#[derive(Json)]`

`json` is a JSON parser/serializer, also
pure Ziman: `Json` is a genuinely recursive enum
(`JArray([Json])`/`JObject(Map<string, Json>)`) that works with no new language
feature, since the recursion goes through an array (a natural finite
base case, `[]`) rather than a bare self-reference — verified directly
before the module was written, given an old, since-resolved ROADMAP.md
note suggesting self-referential types couldn't be constructed at all.
`JNumber` holds a `float`, matching virtually every other JSON
implementation. `parse_json`/`json_to_string` are a real
recursive-descent parser/serializer; `json_object_get` (an `Option<Json>`
lookup) and `json_require_field`/`json_require_string_field` (the same
lookup, but treating a missing field as a `Result` error instead of
`None`, string-field extraction included as the single most common
required-field shape) are the convenience field lookups provided beyond
that — pulled together from two near-identical copies that grew
independently in `health-record-backend-ziman`'s own request/response
mapping layers before moving here. Turning arbitrary JSON into one
already-known struct (a specific request shape) is still left to
application code built on top of this module, not something it does
itself. `#[derive(Json)]` on a `struct`/`enum` closes that gap for the
common case: writing `#[derive(Json)]` above a declaration generates
`<name>_to_json`/`<name>_from_json` for it, with a priority-ordered
per-field conversion rule (a field's own `_to_json`/`_from_json`, then
its `_to_string`/`_from_string` pair, then a plain scalar, then
`Option<U>`/`[U]` recursion), lenient deserialization, and an
externally-tagged shape for enums (`{"type": "<Variant>", "value":
...}`) — the first consumer of a general, reusable `#[...]` attribute
mechanism, built by generating the two functions as ordinary Ziman
source text and feeding it back through the same lexer/parser, rather
than hand-constructing AST nodes.

## Networking: `net`, `defer`, `http`, TLS

`net` is blocking TCP over POSIX sockets (`connect`/`listen`/`accept`/`read`/
`write`, timeouts, a clean peer close reported as `Err(Closed)` rather than an
empty read) — blocking on purpose, since `spawn` gives every connection its own
OS thread — and the `[byte]` helpers that go with it are prelude builtins (string
conversion) and `number` (big-endian integers).

`defer f(args)` runs a call when the enclosing block exits by any path (`return`, a propagating `?`, `break`, `continue`, falling off the end), last-in first-out, with arguments evaluated when the `defer` statement runs — how a socket or transaction is released without destructors (`defer net.close(sock)`, `defer db.rollback(conn)`). Block-scoped, a single call only, its result discarded, and skipped by a fatal runtime error.

`http` is an HTTP/1.1 server toolkit on `net` (request parsing with keep-alive and pipelining, bounded headers and bodies, response framing, and route matching with path parameters) — a toolkit rather than a framework (function values don't capture, so a handler can't reach the application's database connection unless it is passed in as an explicit context value); `server.serve(listener, ctx, handler, options)` runs the accept and connection loops around a handler function value, or an application can write its own loops and `route` function; strings now have a `\r` escape.

`net` also does client-side TLS on the system OpenSSL (`net.connect_tls`/`net.start_tls`, upgrading a socket in place so everything built on `Socket` works over it), with certificate and host-name verification on by default and an explicit `TlsVerify` (`Full`/`Chain`/`Off`) plus optional CA file when relaxing it.

`db.open_pool(url, size)` returns a pool-backed `Connection` (same type, so repository code is unchanged): statements borrow a connection for one call, and `db.acquire`/`db.release` (with `defer`) hand out an exclusive connection for a transaction — the pool is just a channel of idle connections. A dead connection is replaced after a connection-class failure; `db.acquire_timeout` bounds the wait.

## Databases: `crypto`, `pg`, `sql`/`db`

`crypto` is just enough cryptography for SCRAM-SHA-256 (SHA-256, HMAC, PBKDF2, base64 — native, self-contained,
checked against the RFC vectors; not a general-purpose library), and `pg` is a PostgreSQL client written in Ziman on top of
`net`/`number`/`crypto`: trust, cleartext and SCRAM-SHA-256 authentication (the server's signature is verified too),
the simple and extended query protocols, tested against a real Postgres server by `make test-pg`.
`db.open("postgres://user:password@host:port/database")` selects it, so the same repository SQL and
`#[derive(Row)]` code run on SQLite and Postgres unchanged; `sslmode` follows libpq (`disable`, `prefer`, `require`, `verify-ca`, `verify-full`, `sslrootcert`), verified against a real TLS-enabled server. `sql` and `db` are a JDBC-shaped database layer: `db.open("sqlite::memory:")`
returns a `Connection`, `db.execute`/`db.query` take SQL text with `$1, $2`
bound parameters (never spliced in) as `[SqlValue]`, and rows come back as
`sql.Row`; `#[derive(Row)]` generates the row-to-struct mapping, and
`Uuid`/`time` types map through `Text` columns. SQLite (system
`libsqlite3`, linked with `-lsqlite3`) and Postgres are the two backends, chosen by URL
scheme behind the same API.

## `number`

`number` is shared formatting/parsing across all five numeric
types (`int`, `float`, `bigint`, `decimal`, `byte`), also pure Ziman:
rather than five near-duplicate implementations (Ziman has no
argument-based overloading, so a single `format(x: T, ...)` across
unrelated types isn't otherwise expressible), one engine
(`format_number`/`parse_number`) operates on the *string* each type's
own `str()` already produces, with `format_int`/`format_float`/
`format_bigint`/`format_decimal`/`format_byte` as thin per-type
wrappers over it. `NumberFormat` (thousands separator, decimal
separator, optional forced decimal-place count) and a `"#,###.00"`-
style pattern string (`parse_pattern`) are two ways to describe the
same thing — a deliberately small subset of DecimalFormat/.NET-style
patterns, forcing fewer decimal places always truncates rather than
rounds. Hex conversion (`byte_to_hex`/`int_to_hex`/`bigint_to_hex` and
their `_from_hex` inverses) covers the three genuinely integral types:
`byte` trivially (always 2 unsigned digits), `int` as fixed-width
64-bit two's complement (matching C/Java's own convention, computed via
`bigint` arithmetic since a negative `int`'s unsigned equivalent
doesn't fit in a signed `int64`), `bigint` as sign-prefixed magnitude
(no fixed width to complement against, matching Java's own
`BigInteger.toString(16)`).

## Builtins and the prelude

Builtins ship two ways. A small core set — `print`, `str`, `format`,
`len`, `push`, `insert`, `set`, `remove`, `contains`, `abs`, `min`,
`max`, `hash`, `channel_new`, `send`, `receive`, plus `==`/`!=` on
scalar-element arrays — is handled directly by the type checker and
interpreter, because each of those names needs to work unqualified
across several types at once (no argument-based overloading exists
anywhere else in the language). Everything else that has exactly one
fixed signature — `parse_int`, `parse_float`, `parse_bigint`,
`parse_decimal`, `floor`, `ceil`, `round`, `decimal_round`, `concat`,
`substring`, `split`, `trim`, `to_upper`, `to_lower`, `join` — instead
lives in an auto-imported "prelude" module (`lib/prelude.zm`), each
an ordinary `native fn` using the same mechanism the `fs` module uses to
call into C. `join(prefix: Option<string>, suffix: Option<string>,
separator: Option<string>, pieces: string...) -> string` is also the
first use of a *variadic parameter* — `join(pieces: "a", "b")`/
`join(pieces: arr)` (an already-built `[string]`) both work for the
trailing pieces, and it exists specifically so that joining many
strings is a single native, O(N) buffer build instead of the O(N²) a
pure-Ziman loop of `concat` calls would be (see docs/DESIGN.md's
"Variadic parameters" entry). Its own leading three parameters are the
first real caller of `Option<T>` parameter omission (below): the common
case, `join(pieces: arr)`, leaves all three out, each falling back to
its own do-nothing default (no prefix, no suffix, plain concatenation
with nothing between pieces) — give any of them, bare or wrapped, to
build a delimited, bracketed, or otherwise decorated string in the same
single native pass (`join(prefix: "[", suffix: "]", separator: ", ",
pieces: arr)`).

## Named arguments, `Option<T>` omission, auto-wrap and `?`

Any call — to a
`native fn`, a plain `fn`, or a generic one — may also name its
arguments instead of listing them positionally
(`localtime_of(hour: 1, minute: 2, second: 3, nano: 4)`, in any order),
reusing struct-literal `name: value` syntax; a call must be entirely
positional or entirely named, and the label must match the parameter's
own declared name (see docs/DESIGN.md's "Named arguments" entry). A
named call may also leave out an `Option<T>`-typed parameter entirely
(never a required one, and never in a positional call) — it becomes
`None`, or whatever the declaration's own `= expr` default says instead
(`priority: Option<int> = 3`), evaluated once, before `main` runs, the
same timing a global val's own initializer gets. Wherever an
`Option<T>` value is expected — a supplied argument, a default
expression, a `val`/`var`'s own declared type, a struct literal field,
a `with` override, or a function's returned value — a bare `T` works
too, auto-wrapped to `Some(T)` — a deliberate, narrow exception to "no
implicit coercion," modeled on Rust's own `impl<T> From<T> for
Option<T>` (see docs/DESIGN.md's "Option\<T\> parameter omission and
defaults" entry). `Result<T, E>` gets the same bare-value auto-wrap, in
all of those same positions except a default (`Result<T, E>` can't have
one): a bare `T` becomes `Ok(T)`, a bare `E` becomes `Err(E)`, and when
`T == E` a bare value always becomes `Ok` (write `err(value)`
explicitly for that one case). Unlike `Option<T>`, a `Result<T, E>`
parameter can never be omitted — there's no natural "absent" state an
outcome type could fall back to — so this sugar is only ever about
what a supplied value looks like, never about leaving one out (see
docs/DESIGN.md's "Result\<T, E\> bare-value auto-wrap" entry). A
postfix `expr?` unwraps a `Result<T, E>`/`Option<T>` success value and
keeps going, or immediately returns the failure from the enclosing
function (`Ok`/`Some` → the value; `Err`/`None` → an early `return`) —
only legal as a `val`/`var`'s entire initializer or as its own bare
statement, never nested inside a larger expression, a deliberate v0
scope cut that avoids needing the interpreter to unwind out of an
arbitrary expression tree (see docs/DESIGN.md's "? operator" entry).

## `StringBuilder` and `ArrayBuilder<T>`

The prelude isn't native-only, though: `Option<T>` (plus
one helper, `option_unwrap_or`) lives there too, as ordinary Ziman
source — an enum, not a hardcoded primitive like `Result<T, E>` is —
merged bare into every program the same way the entry file's own
top-level names are. So does `StringBuilder` — a general fix for the
O(N²)-allocation, O(N)-leaked-garbage pattern of building a string via
repeated `concat` in a loop (`uuid_to_string` used to be exactly this,
and now isn't — see docs/DESIGN.md's "StringBuilder" entry). It's a
plain, one-field struct (`StringBuilder { handle: int }`), not a new
built-in type the way `Channel<T>` is: the `handle` is an opaque index
into a native-side table of real, growable buffers (`sb_new`/
`sb_append`/`sb_append_byte`/`sb_to_string`/`sb_len`), so appending never copies
everything accumulated so far the way `concat` or `push` in a loop
does. `sb_new(capacity: Option<int> = 20, grow_factor: Option<float> =
2.0)` leans on `Option<T>` parameter omission for its own ergonomics:
the common case, `sb_new()`, tunes neither. `ArrayBuilder<T>` is the
same fix for `push` in a loop (`hashmap_keys`/`hashmap_values`/
`hashset_items` all used to be exactly this, and now aren't) — `push`
copies the whole existing array on every call for the same reason
`concat` copies the whole string, so it's the identical
`ab_new`/`ab_append`/`ab_to_array`/`ab_len` shape, generic this time
(`struct ArrayBuilder<T> { handle: int }`). Needed one small, genuinely
missing capability first: `native fn` couldn't take type parameters at
all before this (a parser gap, not a deliberate restriction — the type
checker's own generic-instantiation logic never special-cased native
functions once the syntax was accepted). Unlike every other bundled
module (`collections`, `fs`, ...), `lib/prelude.zm` is never
reachable through `use` at all — everything it declares is already
unqualified everywhere, so
there'd be nothing to gain and a redundant path to create. See
[docs/ROADMAP.md](docs/ROADMAP.md) for what's deliberately not here yet.
