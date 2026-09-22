# Ziman

Ziman ("language" in Kurdish) is a small, statically-typed language whose
syntax and semantics are designed to be cheap for an LLM to generate
correctly and cheap for a human to read without running it. See
[docs/DESIGN.md](docs/DESIGN.md) for the principles behind that and the
concrete decisions they led to, and [docs/SYNTAX.md](docs/SYNTAX.md) for
the full v0 grammar and semantic rules.

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
to import one function/struct/enum under a bare local name instead —
resolved and privacy-checked once at the `use` line, not a wildcard
import (there's no `use mod.*`, and no unqualified name whose origin
isn't spelled out somewhere in the file's own `use` list), `native fn` for calling into
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
built on this (`corelib/collections.zm`) provides `Box<T>`, `Option<T>`,
`HashMap<K: Eq + Hash, V>`, and `HashSet<T: Eq + Hash>` (bucket-array
based, O(1) average-case lookup rather than a linear `Eq`-only scan).
It's also one of two *bundled core modules* (alongside `io`, mentioned
above): each one's source is embedded into the `ziman` binary at build
time, so `use collections`/`use io` work from any project with no local
copy needed — a project-relative file of the same name still wins if
one exists, but this is the fallback that means one usually doesn't
have to.

Builtins ship two ways. A small core set — `print`, `str`, `format`,
`len`, `push`, `insert`, `set`, `remove`, `contains`, `abs`, `min`,
`max`, `hash`, `channel_new`, `send`, `receive`, plus `==`/`!=` on
scalar-element arrays — is handled directly by the type checker and
interpreter, because each of those names needs to work unqualified
across several types at once (no argument-based overloading exists
anywhere else in the language). Everything else that has exactly one
fixed signature — `parse_int`, `parse_float`, `parse_bigint`,
`parse_decimal`, `floor`, `ceil`, `round`, `decimal_round`, `concat`,
`substring`, `split`, `trim`, `to_upper`, `to_lower` — instead lives in
an auto-imported native "prelude" module (`src/prelude.c`): available
unqualified with no `use`, but otherwise an ordinary `native fn`,
using the same mechanism the `io` module uses to call into C. See
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
