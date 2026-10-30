# Semantic rules enforced before a program runs

Part of the [v0 syntax reference](README.md).

## Semantic rules enforced before a program runs

Checked by the type checker -- a program violating one of these never executes any code:

- Every name must be declared before use. `val` bindings cannot be reassigned; only `var` can.
  A binding's value can never change type across its lifetime.
- Operators require their operand types to match exactly (see the table below); mixing `int`
  and `float` needs an explicit `as` cast.
- A call's argument count and each argument's type must match the callee's declared parameters
  exactly.
- A non-`void` function must return on every statically reachable path (an `if` with no `else`,
  or a bare `while`, is never assumed to guarantee one).
- `void` (a no-return-type function's result) can't be used as a value anywhere.
- A `val`/`var`/parameter/function name must not start uppercase; a `struct`/`enum`/variant name
  must -- enforced, not conventional (see "Structs"/"Enums").
- Every field/payload/parameter/return type must name a type that actually exists, caught at
  the declaration that names it.
- Struct/enum types are nominal, not structural: a struct name and an enum name can't collide,
  but two different enums may reuse the same variant name.
- `ok(...)`/`err(...)`/`channel_new()` are legal only where an expected `Result<T, E>`/
  `Channel<T>` type is available (see "Result"/"Concurrency").
- `Result` and `Channel` are reserved names, like `int`/`bool` -- no `struct Result`/`enum
  Channel` of your own.

| Operator group        | Operators                  | Operand types           | Result  |
|------------------------|-----------------------------|--------------------------|---------|
| Arithmetic             | `+ - * / %`                 | `int,int` or `float,float`* | same type |
| Comparison             | `< > <= >=`                 | `int,int` or `float,float` | `bool`  |
| Equality               | `== !=`                     | any two of the same type, provided that type is "comparable": a scalar always, `[T]` whenever `T` itself is (recursively) comparable, a struct only if every field is (recursively) comparable, `Result<T, E>` only if both `T` and `E` are (recursively) comparable, an `enum` only if every payload-carrying variant's payload type is (recursively) comparable, never a `Channel` — a type that refers back to itself while this is being worked out (`Json`, a recursive `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`) is assumed comparable rather than checked forever | `bool`  |
| Logical                | `&& \|\|`                   | `bool, bool`             | `bool`  |
| Unary negate           | `-`                          | `int` or `float`         | same type |
| Unary not              | `!`                          | `bool`                   | `bool`  |

\* `%` is defined for `int`, `bigint` and `decimal` (both operands the same type); on `float` it is a compile error, there being no float remainder.

Errors that can only be detected while running (division/modulo by zero)
are runtime errors, reported the same way as compile-time errors (see
`src/error.c`): `ziman: <phase> error at <line>:<col>: <message>`, then
exit code 1.

### Stack depth

Recursion is limited by the thread's stack, not a fixed call count -- each call/nested
expression checks how much is left, and running out stops with a runtime error (`stack depth
exceeded: this main thread's N MB stack is used up`), never a crash. A call costs ~3.5-11 KB of
stack, so an 8 MB stack holds ~1,000-2,000 calls, a 256 MB one ~25,000-70,000.

Sizes are chosen at startup from the machine: an eighth of the usable memory is the stack
budget, a quarter of that (8-512 MB) for the main thread, the rest shared among planned threads
(64 by default, 1-64 MB each). Stacks are reserved address space -- only touched pages use
memory. Override with:

| Option | Environment variable | Meaning |
|---|---|---|
| `--stack-mb=N` | `ZIMAN_STACK_MB` | the main thread's stack, 1 to 768 MB |
| `--thread-stack-mb=N` | `ZIMAN_THREAD_STACK_MB` | the stack of each `spawn`ed thread, 1 to 768 MB |
| `--threads=N` | `ZIMAN_THREADS` | how many threads to plan the memory for (default 64) |

A flag wins over its variable, which wins over the computed size; 768 MB is the ceiling because
the bundled collector aborts near 1 GB. The parser, type checker, and comparison/hashing/
printing of deeply nested values are all checked the same way -- a pathological input is an
error, not a crash.

### Other limits

A quick reference for numeric ceilings scattered through this document, each a runtime error
rather than undefined behavior or a crash:

| What | Limit | Where it's enforced |
|---|---|---|
| Recursion / call depth | machine- and config-dependent (see "Stack depth" above) | every call and nested expression |
| `int`/`bigint` arithmetic overflow | traps, never silently wraps or saturates (`+`/`-`/`*`/`/`, `math.abs(MIN)`) | the interpreter |
| An `int` literal past `int`'s range | a compile-time error, not silent saturation | the parser |
| `bigint`/exact-`decimal` digit count (`parse_exact`, literals) | 1,000 digits | `json`'s number parser |
| A parsed number's exponent size | 100,000 | `json`'s number parser |
| `parse` nesting depth | 512 levels | `json` |
| A regex repetition count | 1,000 | `regex`, when it compiles |
| A compiled regex | 20,000 instructions, and 64 MB of capture buffers (~1,170 capture groups) | `regex`, when it compiles |
| The work one regex match may do | 256M capture-vector copies plus 100,000 per subject byte (a real pattern uses 7-34 per byte) | `regex`, while matching |
| `--fmt` nesting depth | 200 brackets | the formatter, which prints the input back unchanged |
| `--fmt` output size | 16 MB (~180x the largest file here) | the formatter, as it appends |
| A document the language server analyses | 4 MB, stored but not analysed past it | `--lsp`, which says so in a diagnostic |
| HTTP request headers | 16 KiB total, fixed | `server` (`HeadersTooLarge`, 431) |
| HTTP request body | 1 MiB, fixed | `server` (`BodyTooLarge`, 413) |
| Server connections | whatever `ServeOptions.max_connections` says, no built-in default | `server`'s `serve` |

This list is illustrative, not exhaustive -- each module's own header comment and docs/CORELIB.md
document a module's own limits in full; see "JSON" and "HTTP" elsewhere in this file for the ones above.
