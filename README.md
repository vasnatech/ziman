<p align="center"><img src="docs/logo/ziman-logo.svg" alt="Ziman logo" width="120"></p>

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
    val parsed = try json.parse(text)
    val item = try item_from_json(parsed)
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
Err(name: expected a string)
```

This program is `tests/lang/readme_example.zm`, run by the test suite, so it stays true. More examples are the other fixtures in `tests/lang/`.

## The language at a glance

- **Types:** `int`, `float`, `bool`, `string`, `byte`, arbitrary-precision
  `bigint` and `decimal`, arrays `[T]`, `struct`s, tagged-union `enum`s,
  `Option<T>`, `Result<T, E>`, `Map<K, V>`, `Set<T>`, `Channel<T>`, and function types
  `fn(A) -> R`. Values are immutable unless declared `var`.
- **Control flow:** `if`/`else`, `while`, `for x in arr` / `for i in 0..n`,
  `break`/`continue`, an exhaustive `switch` over enums and results,
  `defer` for cleanup.
- **Errors are values:** `Result`/`Option` with a prefix `try` to propagate,
  and a narrow bare-value auto-wrap so `return x` works in a `Result`
  function.
- **Generics:** explicit type arguments (`Box<int>`), with
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
| `fs`, `os`, `process`, `clock`, `random` | files, environment and this process, starting and signalling child processes, time source, entropy |
| `schedule` | periodic background work in a long-running process: an interval, a daily UTC time or a six-field cron pattern, each due job run on its own thread so one failure cannot stop the rest |
| `time` | `java.time`-shaped dates, times, durations, instants |
| `uuid`, `number`, `encoding`, `crypto` | UUIDs, number formatting/parsing, base 2/8/16/32/64 over bytes, SHA-256/HMAC/PBKDF2 |
| `json`, `ndjson`, `csv`, `toml`\*, `yaml`\* | JSON, and the other formats as converters to and from the same `Json` tree |
| `xml`\* | XML 1.0 as a document tree of its own, not a `Json` tree, because text and elements interleave and attributes are not children; a reader with size, depth and attribute limits and no DTD at all, a writer, and a child-step path finder |
| `markdown`\* | Markdown as a document tree: a reader over a declared subset, a writer that escapes for the position each piece sits in, and a strict mode that refuses what it would otherwise misread -- Markdown having no syntax errors of its own |
| `properties`, `log`, `metrics`, `regex` | configuration files, levelled logging, counters/gauges/histograms in the Prometheus text format, regular expressions |
| `net`, `http`, `server`, `client` | TCP and TLS, the types HTTP shares, an HTTP/1.1 server toolkit (with the peer address on each request, a per-key rate limiter and a per-client connection cap), an HTTP/HTTPS client |
| `url` | percent-encoding, shared by `http` and `db` |
| `locale`\* | a `Locale` from the operating system's own locale data, and the process-wide default that locale-aware functions use when given none; the invariant `root()` is what protocol and identifier code folds case with |
| `db`, `sql`\*, `sqlite`, `pg`\*, `mysql`\*, `memdb`\* | one database API over SQLite, PostgreSQL, MySQL and an in-memory SQL engine written in Ziman, with pooling |

\* Experimental: less real-world use than the rest of the library, so treat a gap or a
protocol-conformance bug as more likely here. See [docs/DESIGN.md](docs/DESIGN.md)
"Experimental modules".

Builtins available without `use`: `print`, `printf`, `format`, `str`, `len`,
`push`, `insert`, `set`, `remove`, `contains`, `hash`, `compare`, `abs`,
`min`, `max`, `ok`, `err`, `unwrap`, the `channel` operations, the string
functions (`substring`, `split`, `trim`, `index_of`, `replace`,
`equals_ignore_case`, ...), and the mutable types (`Mutable<T>`, one cell, plus the four
collections `MutableArray<T>`, `MutableMap<K, V>`, `MutableSet<T>` and `MutableString`). Long-form
notes on all of it are in
[docs/FEATURE-NOTES.md](docs/FEATURE-NOTES.md).

## Building and running

```sh
make
echo 'fn main() { print("hello, ziman") }' > hello.zm
./ziman hello.zm
```

**Platforms.** Ziman is developed and tested on Linux (x86-64, Fedora); that is the only platform the test
suite has been run on. It needs a C11 compiler, `make`, and the development files for SQLite, OpenSSL
(3.2 or later, for Argon2id), libxcrypt (bcrypt) and zlib (it links `-lsqlite3 -lssl -lcrypto -lcrypt -lz`). macOS and the BSDs use the same POSIX interfaces
and will probably build, but nobody has tried, so treat them as untested. Windows does not build natively
(threads, sockets, directories and signals use POSIX calls directly); use WSL or a Linux container. The
named time zones in `time` read the operating system's zone files (`/usr/share/zoneinfo`, or `$TZDIR`),
so they also need a system that has them. See docs/ROADMAP.md, "Platform support".

Options: `--check` loads and type-checks without running; `--json` reports each
error as one JSON object on stderr (`{"severity":"error","phase":"type","file":"a.zm",
"line":3,"col":18,"context":null,"message":"..."}`, with `--check` a clean
run prints `{"ok":true}`); a run reports **every** error it can find, not
just the first (up to 25): all syntax errors in a file, then type errors in every
function and statement, sorted by file and position, each named after its own file (never only the ones in a
file other than the one given on the command line) and, in text mode, shown with the source line and a caret
under the column, with a "N errors" line at the end; `--emit-derived` prints the code each
`#[derive]` generates; `--lint` reports style findings in your own code
instead of running it (today: a declaration nothing ever reads, a redundant `as` alias that binds the name
the import would bind anyway, a `spawn` with no `on_error` that is handed a Channel -- if it fails, whoever
waits on that channel waits for ever -- a `var` that is never reassigned, and `x = concat(x, ..)` /
`x = concat_arrays(x, ..)` / `x = set(x, i, ..)` in a loop, which copies all of `x` on every pass -- use a
a MutableString or MutableArray; `x = push(x, ..)` is deliberately *not* reported, since appending to an array
nothing else holds extends it in place -- `var` and
`val` differ by one letter, so it is the typo that silently makes something
mutable) as `file:line:col: warning: ...`, or JSON with `"severity":"warning"`,
and exits 1 if there are any; `--coverage` runs the program and then reports to stderr, per file, how many
lines of your own code executed and which ones did not (`file.zm: 3/5 lines (60%)` and a `not run:`
list), exiting 1 if any did not -- stderr so the program's own stdout is unchanged, the same rule
`--profile` and `--locals` follow, while the tools that do not run your program (`--api`, `--check`,
`--fmt`, `--lint`) report on stdout because their findings are the whole output -- an executable line is one some statement starts on, so a `}`, a
blank line and a block's opening brace are never counted against you, and bundled modules and
`#[derive]`-generated code are left out the same way `--lint` leaves them out; `ziman test
--coverage file.zm ...` does the same across a whole suite, merging files parsed once per test file
so a shared helper counts as covered if any test reached it. `--coverage-lcov=F` additionally writes
the measurement to `F` in lcov's `.info` format, which `genhtml`, Codecov and Coveralls read, so
coverage can gate a build rather than only be looked at -- it implies `--coverage`, works with
`ziman test` too, and *merges* into an existing `F` so several invocations accumulate into one number
(delete `F` to start fresh, since counts only grow). `--profile` runs the program and then reports to stderr which functions the time went to, hottest
first (`self%  samples  function`) -- CPU-time sampling at 1kHz off the frame stack the interpreter
already keeps for error traces, so it costs nothing measurable and needs no instrumentation. The
figure is *self* time, the function's own statements rather than its callees, which is the one that
answers "what do I make faster"; core-library and native functions appear too, so a program whose
time really goes into `mutable_map_set` is told so. Time spent collecting garbage gets its own row instead
of being blamed on whichever function happened to allocate, and the report goes to stderr so the
program's own stdout is unchanged; `--locals` adds each stack frame's variables and their values
under it when a runtime error prints a trace (`xs: [int] = [1, 2, 3]`), innermost block first --
the post-mortem half of a debugger, which arrives on the first failure with no setup and so works
in a CI run or a crash someone else reported. Opt-in because a value can be large and a variable
can hold something that should not reach a log; a name shadowed by an inner block is shown once, as
the binding that was live, a string is quoted so it cannot be read as a number, and a long value is
truncated. `ziman --api <module>` prints what a module offers -- its `pub` types and function
signatures, in the language's own notation, read from the parsed declarations rather than scanned
out of text, so it is what the compiler sees. `ziman --api` with no module lists the bundled ones,
and `ziman --api builtins` lists the functions that need no `use` at all -- the prelude's with
their signatures, then the ones the interpreter dispatches by name, which have no declaration to
print one from. It takes a bundled name (`--api fs`) or a path to a `.zm`
file, so it answers for your own modules too. It exists because of a measurement: in the 2026-10-09
generation benchmark, *name discovery* -- not syntax -- was the single largest consumer of a cold
model's effort, with runs brute-forcing eight candidate names before reaching `fs.write_text` and
six before `map_get`. `--fmt file.zm...` prints the source formatted
(`--fmt --write` rewrites the files, `--fmt --check` lists the ones that would
change and exits 1, `--fmt -` reads stdin, `--width=N` sets the wrap column,
default 120, 0 = never, and `--canonical` also settles brace placement -- a line-leading `{` joins
what it opens and `else` joins the `}` that closed its `if`, which plain `--fmt` deliberately leaves
alone since moving a line break is the one thing it promises not to do; nesting deeper than 200
brackets is refused rather than formatted, because every line is indented by its depth and the
output would be quadratic -- real code reaches about 10 -- and so is anything that would format to
more than 16 MB, which is the same cost measured on the axis the depth limit cannot see, about 180x
the largest file here; a refused file is printed back unchanged so filtering a buffer through
`--fmt -` never empties it); `--stack-mb=N`, `--thread-stack-mb=N` and `--threads=N` size the thread stacks
(recursion is limited by stack, see docs/SYNTAX.md "Stack depth"); `--version`; `--help`. A function's parameter list, or a call's argument list, that is too wide or already wrapped between its
arguments is laid out one to a line with the parentheses on lines of their own (`fn name(` / `    a: A,` /
`    b: B` / `) -> R {`, and likewise `call(` / `    x: 1,` / `    y: 2` / `)`); the body is indented one level from
`fn`. A struct or map literal (`Point { x: 1 }`, `Map<K, V> { "a": 1 }`) that is too wide or already wrapped between
entries is laid out the same way, one entry per line with the closing brace on its own line. A call whose only line
breaks are inside a lambda (`f(fn() {` ... `}, g)`) keeps that shape.

The interpreter has a tracing garbage collector (the Boehm-Demers-Weiser
collector, vendored in `vendor/bdwgc` and built by `make` -- no network, no
extra tools), so a long-running program's memory is reclaimed: the sample
backend under a request load stays around 40 MB where it would otherwise grow
past a gigabyte. Sockets, SQLite connections and TLS sessions are still closed by
hand. `make nogc` builds `./ziman-nogc` without the collector (about 1.3-2.3x
faster on allocation-heavy code, and it never frees anything); `make test-nogc`
runs the suite against it. `tests/bench/server_load.sh` measures memory and
request latency on the sample backend. The collector's `GC_MARKERS` (default 2
here) and its other environment variables apply.

## Repository layout

Two languages live here, and the tree says which is which. `src/` is C and nothing else --
the interpreter, 61 `.c` files. `lib/` is Ziman and nothing else -- the standard library, compiled
*into* the binary as generated headers, which is why there is no runtime to install and no path to
set. Build output goes to `build/`, never beside a source.

| | |
| --- | --- |
| `src/` | the interpreter, in C |
| `lib/` | the standard library, in Ziman (`core/`, `db/`, `http/`, `time/`, `prelude/`) |
| `tests/c/` | standalone C unit tests of the C internals |
| `tests/lang/` | the language itself: Ziman programs and their expected output |
| `tests/stdlib/` | `#[test]` units for the bundled modules |
| `tests/integration/` | suites needing a database, OpenSSL or a signal (`pg/`, `mysql/`, `tls/`, `shutdown/`, `lsp/`) |
| `tests/bench/` | benchmarks |
| `tools/` | the generators and the source-level checks, in Python and shell |
| `make/` | `c.mk` builds and checks the C, `ziman.mk` runs Ziman, `docs.mk` generates and checks the docs |

The worked examples in `docs/lib/examples/` look like stray test code and are not: the manual
generator includes them in the page and the suite runs them, which is how the documentation stays
true.

## Testing

```sh
make check
```

Everything that needs nothing but this repo and a C toolchain: every fixture in
`tests/lang/` on both builds (`make test`, `make test-nogc` — see `tests/run_tests.sh`), the
standalone C unit tests (`make unit-tests`), the TLS suite when `openssl` is on the machine
(`make tls-if-available`, skipped with a note when it is not), the North Star application when a
checkout of it is beside this one (`make northstar-if-available` — both its entry points
type-check, its demo runs, and its own ~980-check behavioural suite passes over the real HTTP API),
formatting (`make fmt-check`), and the documentation guards (`make docs-check`, which checks that `docs/lib/` and `docs/CORELIB.md` still match the
sources they are generated from, that no doc calls a `pub` name "not yet" supported once it exists,
and that this file's own module table agrees with each module's `#![experimental]`).

The rest need something the repo can't supply, so they are separate: `make test-pg` and
`make test-mysql` want a container runtime and a database image, and `tests/bench/` wants `ab` and
a checkout of the sample backend.

`make preflight` is the occasion for all of the below at once -- the three sanitizers, memcheck,
the static analyzer, the North Star suite against *both* database engines, and the two database
suites. It probes each prerequisite and prints what it skipped and why, which matters because
several of these need something this machine may not have; `tools/preflight.sh --only STEP` re-runs
one after a failure. Expect 15-25 minutes. Nothing runs it automatically, because there is no CI in
this repository -- it is the documented thing to do before a release, and it exists because every
target it calls was previously a command with no occasion that ran it.

The sanitizers are separate for a different reason -- they are slow, not unsupported: `make asan`,
`make ubsan`, `make tsan` (or `make sanitizers` for all three) rebuild and run the suite under
Clang's or GCC's instrumentation. Each builds the binary **twice**, with the collector and without,
and runs the suite against both: 218 lines across 17 files sit behind `#ifdef ZIMAN_GC`, so a
single-configuration run leaves the shipped build unexamined. `make valgrind` runs a narrow set
of fixtures and every C unit test under Memcheck with `tools/valgrind.supp` for the conservative
stack scanner's deliberate reads, and `make analyze` runs GCC's static analyzer. If a sanitizer
runtime isn't installed system-wide, pass its location in `LDFLAGS` -- `dnf download libasan`
and an `rpm2cpio` into a scratch directory is enough, and needs no root.

## Documentation

| | |
| --- | --- |
| [docs/CHEATSHEET.md](docs/CHEATSHEET.md) | the twenty things, on one page -- start here, and read it before writing a helper that already exists |
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | a guided first hour: one small program, built up feature by feature |
| [docs/SYNTAX.md](docs/SYNTAX.md) | the language reference: grammar and semantic rules |
| [docs/DESIGN.md](docs/DESIGN.md) | the principles and the reasoning behind each decision |
| [docs/lib/](docs/lib/README.md) | the library manual: one page per bundled module (generated from the source, with runnable examples) |
| [docs/CORELIB.md](docs/CORELIB.md) | every bundled module's `pub` signatures (generated by `make corelib-reference`) |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is done and what is deliberately not here yet |
| [docs/FEATURE-NOTES.md](docs/FEATURE-NOTES.md) | long-form notes on the implemented features |
| [docs/MEMORY.md](docs/MEMORY.md) | the memory-management/GC diagnosis and the thread-owned-heap design not yet built |

## Editor support

[editors/vscode/](editors/vscode/) — a VS Code extension (`.zm` files):
syntax highlighting plus a client for `ziman --lsp`, the built-in language
server (diagnostics, formatting, symbols, go to definition, hover, completion;
setup for other editors in its README). Not published to the Marketplace.
