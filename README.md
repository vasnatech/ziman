# Ziman

Ziman ("language" in Kurdish) is a small, statically-typed language whose
syntax and semantics are designed to be cheap for an LLM to generate
correctly and cheap for a human to read without running it: no implicit
coercion, no exceptions, no operator overloading, one obvious way to spell
most things, and errors that say what to write instead.

The reference interpreter (lexer, parser, type checker, tree-walking
evaluator) is written in C, but C is only its host language; Ziman itself is
a from-scratch design. It is v0: complete enough to run a real backend (see
`health-record-backend-ziman`), not yet fast.

## A taste

```
use json

#[derive(Json)]
struct Item {
    name: string
    qty: int
}

fn total(items: [Item]) -> int {
    var sum = 0
    for item in items {
        sum = sum + item.qty
    }
    return sum
}

fn summarize(text: string) -> Result<string, string> {
    val parsed = json.parse_json(text)?
    val item = item_from_json(parsed)?
    return ok(format("{} x{}", item.name, item.qty))
}

fn main() {
    val items = [Item { name: "pen", qty: 3 }, Item { name: "ink", qty: 4 }]
    printf("total units: {}\n", total(items))
    print(summarize("{\"name\":\"cup\",\"qty\":2}"))
    print(summarize("{\"name\":1}"))
}
```

which prints

```
total units: 7
Ok(cup x2)
Err(expected a string)
```

This program is `tests/fixtures/readme_example.zm`, run by the test suite, so it stays true. More examples are the other fixtures in `tests/fixtures/`.

## The language at a glance

- **Types:** `int`, `float`, `bool`, `string`, `byte`, arbitrary-precision
  `bigint` and `decimal`, arrays `[T]`, `struct`s, tagged-union `enum`s,
  `Option<T>`, `Result<T, E>`, `Channel<T>`, and function types
  `fn(A) -> R`. Values are immutable unless declared `var`.
- **Control flow:** `if`/`else`, `while`, `for x in arr` / `for i in 0..n`,
  `break`/`continue`, an exhaustive `switch` over enums and results,
  `defer` for cleanup.
- **Errors are values:** `Result`/`Option` with a postfix `?` to propagate,
  and a narrow bare-value auto-wrap so `return x` works in a `Result`
  function.
- **Generics:** explicit type arguments (`HashMap<string, int>`), with
  `Eq`, `Hash` and `Ord` bounds. No general traits.
- **Derives:** `#[derive(Json)]`, `#[derive(Row)]` (SQL rows),
  `#[derive(Ord)]`, `#[derive(Properties)]`, on generic types too where it
  makes sense.
- **Concurrency:** real OS threads with `spawn` and thread-safe channels, in
  a single-writer/actor style rather than shared memory.
- **Functions:** named arguments, variadic parameters, `Option` parameters
  that can be omitted, function values: references to named functions, and lambdas that capture
  immutable locals by value.
- **Modules:** `use path/to/mod` (qualified) or `use mod.Name [as alias]`;
  a small bundled standard library needs no installation.

The full grammar and rules are in [docs/SYNTAX.md](docs/SYNTAX.md); the
reasoning is in [docs/DESIGN.md](docs/DESIGN.md).

## Standard library

Bundled with the binary (`use name`); every `pub` signature is in
[docs/CORELIB.md](docs/CORELIB.md).

| Module | What it is |
| --- | --- |
| `collections` | `HashMap`, `HashSet`, sorting, `map`/`filter`/`fold` |
| `fs`, `os`, `clock`, `random` | files, environment and process, time source, entropy |
| `time` | `java.time`-shaped dates, times, durations, instants |
| `uuid`, `number`, `bytes`, `crypto` | UUIDs, number formatting/parsing, byte helpers, SHA-256/HMAC/PBKDF2/base64 |
| `json`, `properties`, `log` | JSON, configuration files, levelled logging |
| `net`, `http`, `httpclient` | TCP and TLS, an HTTP/1.1 server toolkit, an HTTP/HTTPS client |
| `sql`, `db`, `sqlite`, `pg`, `mysql`, `dburl` | one database API over SQLite, PostgreSQL and MySQL, with pooling |

Builtins available without `use`: `print`, `printf`, `format`, `str`, `len`,
`push`, `insert`, `set`, `remove`, `contains`, `hash`, `compare`, `abs`,
`min`, `max`, `ok`, `err`, `unwrap`, the `channel` operations, the string
functions (`substring`, `split`, `trim`, `index_of`, `replace`,
`equals_ignore_case`, ...), and the builders (`StringBuilder`,
`ArrayBuilder<T>`). Long-form notes on all of it are in
[docs/FEATURE-NOTES.md](docs/FEATURE-NOTES.md).

## Building and running

```sh
make
echo 'fn main() { print("hello, ziman") }' > hello.zm
./ziman hello.zm
```

Options: `--check` loads and type-checks without running; `--json` reports each
error as one JSON object on stderr (`{"severity":"error","phase":"type",
"line":3,"col":18,"context":null,"message":"..."}`, with `--check` a clean
run prints `{"ok":true}`); `--emit-derived` prints the code each
`#[derive]` generates; `--lint` reports style findings in your own code
instead of running it (today: a `var` that is never reassigned -- `var` and
`val` differ by one letter, so it is the typo that silently makes something
mutable) as `file:line:col: warning: ...`, or JSON with `"severity":"warning"`,
and exits 1 if there are any; `--version`; `--help`.

`make gc` builds `./ziman-gc`, the same interpreter with a tracing garbage
collector (Boehm, built from a pinned source tree by `tools/build_bdwgc.sh`
-- needs git and a C compiler), so a long-running program's memory is
reclaimed instead of growing without bound; `make test-gc` runs the suite
against it. The default `./ziman` never frees a heap value, which is fine for
scripts and tests and not for a server.

## Testing

```sh
make test
```

Runs every fixture in `tests/fixtures/` — see `tests/run_tests.sh`.

## Documentation

| | |
| --- | --- |
| [docs/SYNTAX.md](docs/SYNTAX.md) | the language reference: grammar and semantic rules |
| [docs/DESIGN.md](docs/DESIGN.md) | the principles and the reasoning behind each decision |
| [docs/CORELIB.md](docs/CORELIB.md) | every bundled module's `pub` signatures (generated by `make corelib-reference`) |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is done and what is deliberately not here yet |
| [docs/FEATURE-NOTES.md](docs/FEATURE-NOTES.md) | long-form notes on the implemented features |

## Editor support

[editors/vscode/](editors/vscode/) — a VS Code syntax-highlighting
extension (`.zm` files). Not published to the Marketplace; see its
README for how to load it locally.
