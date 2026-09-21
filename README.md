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
`for i, x in arr`), `break`/`continue`, the four scalar types (`int`, `float`, `bool`,
`string`), value-semantics arrays (`[T]`, nestable), plain nominal
value-semantics `struct`s (named-fields-only construction, `with` for
non-mutating field updates, no methods/inheritance, field-by-field
`==`/`!=` when every field is itself comparable), multi-file programs
via `use path/to/mod` (always-qualified cross-module calls, `pub` for
exports, project-relative resolution only so far), `native fn` for
calling into C (a real, minimal `io` module — `read_file`/`write_file`
— proves it end to end), `Result<T, E>` for recoverable errors
(`io.read_file` returns one instead of crashing on a missing file,
variant-then-payload `==`/`!=` when both `T` and `E` are comparable),
user-declared `enum` tagged unions (concrete/monomorphic only — see
docs/ROADMAP.md's Generics entry — with enum-qualified construction and
compiler-generated `is_<Variant>`/`unwrap_<Variant>` accessors, same
shape `Result` itself uses), and a full set of builtins (`print`, `concat`, `str`, `format`, `len`,
`push`, `insert`, `set`, `remove`, `substring`, `split`, `trim`,
`to_upper`, `to_lower`, `parse_int`, `parse_float`, `contains`, `abs`,
`min`, `max`, `floor`, `ceil`, `round`, plus `==`/`!=` on scalar-element
arrays). See [docs/ROADMAP.md](docs/ROADMAP.md) for what's deliberately
not here yet.

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
