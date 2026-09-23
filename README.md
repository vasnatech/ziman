# Ziman

Ziman ("language" in Kurdish) is a small, statically-typed language whose
syntax and semantics are designed to be cheap for an LLM to generate
correctly and cheap for a human to read without running it. See
[docs/DESIGN.md](docs/DESIGN.md) for the principles behind that and the
concrete decisions they led to, [docs/SYNTAX.md](docs/SYNTAX.md) for
the full v0 grammar and semantic rules, and
[docs/CORELIB.md](docs/CORELIB.md) for a generated signature-only
reference of every bundled core module (`collections`, `io`, `random`,
`uuid`, `clock`, `time`, `json`, `number`, `sql`, `db`, `bytes`, `net`, `crypto`, `pg`) — every `pub struct`/`pub fn` in one
place, kept in sync with the source by `make corelib-reference` rather
than hand-copied.

The reference interpreter (lexer → parser → type checker → tree-walking
evaluator) is written in C, but C is only the implementation's host
language — Ziman itself is not modeled on C. It's a from-scratch design.

## Status

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
C (a real, minimal `io` module — `read_file`/`write_file` — proves it
end to end), `Result<T, E>` for recoverable errors (`io.read_file`
returns one instead of crashing on a missing file, variant-then-payload
`==`/`!=` when both `T` and `E` are comparable), user-declared `enum`
tagged unions with enum-qualified construction and compiler-generated
`is_<Variant>`/`unwrap_<Variant>` accessors, same shape `Result` itself
uses, and real OS-thread concurrency via `spawn` and a thread-safe
`Channel<T>` (a single-writer/actor pattern — one thread owns a piece
of mutable state, everyone else reaches it by message — rather than
shared memory and locks).

`struct`, `enum`, and top-level `fn` can all take type parameters
(`struct Box<T> { value: T }`), with explicit type arguments required
at every use site — `Box<int>{ value: 5 }`, `identity<string>(x)` — and
no inference, since `Value` is runtime-type-erased and nothing is ever
monomorphized. A type parameter can be bounded with `Eq` and/or `Hash`
(`<K: Eq + Hash>`) to use `==`/`hash()` on it inside a generic body;
these are the only two bounds Ziman has — there's no general
trait/interface system (see docs/DESIGN.md's Generics entry for why,
and what's deliberately left undecided). A small `collections` module
built on this (`corelib/collections.zm`) provides
`HashMap<K: Eq + Hash, V>` and `HashSet<T: Eq + Hash>` (bucket-array
based with separate chaining, doubled and rehashed once the load factor
crosses 0.75 so `get`/`has`/`contains` stay O(1) average regardless of
size — `set`/`add`/`remove` stay O(bucket_count) either way, since
Ziman's arrays are immutable value types and every mutation copies the
whole outer bucket array; see docs/DESIGN.md's "Collections module"
entry on growth for why that's a real constant-factor win, not an
asymptotic one). `hashmap_size`/`hashmap_keys`/`hashmap_values` and
`hashset_size`/`hashset_items` enumerate one without reaching into its
private `buckets` field.
(`Option<T>` used to live here too — see the prelude paragraph below for
where it moved and why. `Box<T>` also used to live here — removed
outright, not moved, since nothing ever actually used it beyond being
the first generic struct ever written; `struct Box<T> { value: T }`
above is illustrative syntax anyone can still write, just not a
`collections` export anymore.)
It's also one of fourteen *bundled core modules* (alongside `io`, mentioned
above, plus `random`/`uuid`/`clock`/`time`/`json`/`number`/`sql`/`db`/`bytes`/`net`/`crypto`/`pg` below): each one's source is
embedded into the `ziman` binary at build time, so `use collections`/
`use io` work from any project with no local copy needed — a
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
`Uuid` satisfies `Eq` but not `Hash` (no derived hashing for a struct),
so it's usable as a struct field compared with `==`, not yet as a
`HashMap` key. `clock` is one more native function, `now_raw() ->
[int]`, reading the system clock — Ziman's other source of genuine
non-determinism. `time` is pure Ziman on top of it: seven `java.time`-
shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`,
`Instant`, `ZoneOffset`, `ZoneId`), `ZoneOffset` only (no IANA-style
named zones with DST — a data-maintenance problem, not a code-volume
one), `Instant`/`Duration` as `{seconds: int, nanos: int}` rather than
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
-> int`, since Ziman still has no Ord/Cmp bound and none of them were
the forcing case to add one. `json` is a JSON parser/serializer, also
pure Ziman: `Json` is a genuinely recursive enum
(`JArray([Json])`/`JObject([JField])`) that works with no new language
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
than hand-constructing AST nodes. `net` is blocking TCP over POSIX sockets (`connect`/`listen`/`accept`/`read`/
`write`, timeouts, a clean peer close reported as `Err(Closed)` rather than an
empty read) — blocking on purpose, since `spawn` gives every connection its own
OS thread — and `bytes` is the `[byte]` helpers that go with it (string
conversion, big-endian integers). `crypto` is just enough cryptography for SCRAM-SHA-256 (SHA-256, HMAC, PBKDF2, base64 — native, self-contained,
checked against the RFC vectors; not a general-purpose library), and `pg` is a PostgreSQL client written in Ziman on top of
`net`/`bytes`/`crypto`: trust, cleartext and SCRAM-SHA-256 authentication (the server's signature is verified too),
the simple and extended query protocols, tested against a real Postgres server by `make test-pg`.
`db.open("postgres://user:password@host:port/database")` selects it, so the same repository SQL and
`#[derive(Row)]` code run on SQLite and Postgres unchanged; TLS is refused rather than silently skipped. `sql` and `db` are a JDBC-shaped database layer: `db.open("sqlite::memory:")`
returns a `Connection`, `db.execute`/`db.query` take SQL text with `$1, $2`
bound parameters (never spliced in) as `[SqlValue]`, and rows come back as
`sql.Row`; `#[derive(Row)]` generates the row-to-struct mapping, and
`Uuid`/`time` types map through `Text` columns. SQLite (system
`libsqlite3`, linked with `-lsqlite3`) and Postgres are the two backends, chosen by URL
scheme behind the same API. `number` is shared formatting/parsing across all five numeric
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
lives in an auto-imported "prelude" module (`corelib/prelude.zm`), each
an ordinary `native fn` using the same mechanism the `io` module uses to
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
pieces: arr)`). Any call — to a
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
`sb_append`/`sb_to_string`/`sb_len`), so appending never copies
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
module (`collections`, `io`, ...), `corelib/prelude.zm` is never
reachable through `use` at all — everything it declares is already
unqualified everywhere, so
there'd be nothing to gain and a redundant path to create. See
[docs/ROADMAP.md](docs/ROADMAP.md) for what's deliberately not here yet.

## Example

```
fn fib(n: int) -> int {
    if n < 2 {
        return n
    } else {
        return fib(n - 1) + fib(n - 2)
    }
}

fn main() {
    var i = 0
    while i < 10 {
        print(fib(i))
        i = i + 1
    }
}
```

More in [examples/](examples/).

## Building and running

```sh
make
./ziman examples/hello.zm
```

## Testing

```sh
make test
```

Runs every fixture in `tests/fixtures/` — see `tests/run_tests.sh`.

## Editor support

[editors/vscode/](editors/vscode/) — a VS Code syntax-highlighting
extension (`.zm` files). Not published to the Marketplace; see its
README for how to load it locally.
