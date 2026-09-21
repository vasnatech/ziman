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
`if`/`else`, `while`, the four scalar types (`int`, `float`, `bool`,
`string`), value-semantics arrays (`[T]`, nestable), plain nominal
value-semantics `struct`s (named-fields-only construction, `with` for
non-mutating field updates, no methods/inheritance), multi-file programs
via `use path/to/mod` (always-qualified cross-module calls, `pub` for
exports, project-relative resolution only so far), and a handful of
builtins (`print`, `concat`, `str`, `format`, `len`, `push`, `insert`,
`set`). See [docs/ROADMAP.md](docs/ROADMAP.md) for what's deliberately
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
