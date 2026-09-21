# Ziman — v0 Syntax Reference

File extension: `.zm`.

## Lexical rules

- Comments: `// to end of line`. No block comments in v0.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Convention: `snake_case` for values
  and functions, `PascalCase` for types (once user-defined types exist).
- Integer literals: `[0-9]+` (decimal only in v0).
- Float literals: `[0-9]+\.[0-9]+` (a digit is required on both sides of
  the dot — `.5` and `5.` are not valid literals).
- String literals: `"..."` with escapes `\n`, `\t`, `\"`, `\\`. No
  multi-line strings, no interpolation in v0.
- Booleans: `true`, `false`.
- Statement termination: a newline ends a statement, **unless** the token
  before it can't end a statement (e.g. it's a binary operator, `,`, `(`,
  `=`, or a keyword expecting more). This is a lexical rule, checked
  against the previous token only — never indentation. See `src/lexer.c`'s
  `can_end_statement` for the exact token set. In practice: write code the
  way you'd write Go, and it works.

## Grammar (informal EBNF)

```
program        := { function_decl }

function_decl  := "fn" IDENT "(" [ param_list ] ")" [ "->" type ] block
param_list     := param { "," param }
param          := IDENT ":" type
type           := "int" | "float" | "bool" | "string" | "[" type "]"

block          := "{" NEWLINE* { statement NEWLINE* } "}"

statement      := val_decl | var_decl | assign_stmt
                 | if_stmt | while_stmt | return_stmt | expr_stmt

val_decl       := "val" IDENT [ ":" type ] "=" expr
var_decl       := "var" IDENT [ ":" type ] "=" expr
assign_stmt    := IDENT "=" expr
if_stmt        := "if" expr block [ "else" (if_stmt | block) ]
while_stmt     := "while" expr block
return_stmt    := "return" [ expr ]
expr_stmt      := expr

expr           := logic_or
logic_or       := logic_and { "||" logic_and }
logic_and      := equality { "&&" equality }
equality       := comparison { ("==" | "!=") comparison }
comparison     := term { ("<" | ">" | "<=" | ">=") term }
term           := factor { ("+" | "-") factor }
factor         := cast { ("*" | "/" | "%") cast }
cast           := postfix [ "as" type ]
postfix        := primary { "[" expr "]" }        (* indexing, chains: matrix[i][j] *)
unary          := ( "!" | "-" ) unary | postfix
call           := primary { "(" [ arg_list ] ")" }
arg_list       := expr { "," expr }
primary        := INT_LIT | FLOAT_LIT | STRING_LIT | "true" | "false"
                 | IDENT | "(" expr ")" | array_lit
array_lit      := "[" [ expr { "," expr } ] "]"
```

Notes on things this deliberately leaves out of v0 (tracked in ROADMAP.md):
function calls as a primary only chain off an identifier (no first-class
function values yet, no method-call `.` syntax), no indexed *assignment*
(`arr[i] = x` isn't valid — see the array mutation section below), no
`for` loop (only `while`), and no expression-level `if`.

## Types

- `int` — 64-bit signed integer.
- `float` — double-precision IEEE 754.
- `bool` — `true`/`false`.
- `string` — immutable text.
- `[T]` — a growable, homogeneous, indexed array of `T`; nests freely
  (`[[int]]` is an array of arrays of `int`). Value semantics, like every
  other type in Ziman: assigning or passing an array always yields an
  independent value, never a shared reference — see "Arrays" below.
- No implicit conversion between any scalar type. `x as float` / `x as
  int` are the only casts, and only between `int` and `float` (never
  arrays).

## Arrays

```
var nums: [int] = []
var i = 0
while i < 5 {
    nums = push(nums, i * i)
    i = i + 1
}
print(nums[0])           // 0
print(len(nums))          // 5
nums = insert(nums, 0, -1)
nums = set(nums, 1, 999)
print(nums)                // [-1, 999, 1, 4, 9, 16]
```

- **There is no indexed assignment.** `arr[i] = x` is not valid syntax.
  Every array mutation follows one pattern — `arr = builtin(arr, ...)` —
  the same shape you already use to update a scalar `var`. `push`,
  `insert`, and `set` are ordinary pure functions: none of them mutate
  their `arr` argument, they return a new array. This holds regardless of
  whether the source binding is `val` or `var` — a value has no notion of
  mutability at all in Ziman; only a *binding* does (see docs/DESIGN.md).
- **An empty array literal (`[]`) has no elements to infer a type from.**
  It's only legal as the initializer of a `val`/`var` with an explicit
  type annotation (`val x: [int] = []`); anywhere else (a function
  argument, a non-empty context) it's a type error. A non-empty literal
  infers its element type from its first element and requires every
  other element to match exactly — `[1, "two"]` is a type error.
- **Indexing (`arr[i]`) and `set`/`insert`'s index argument are bounds-checked
  at runtime**, not compile time (the index is usually a computed value) —
  out of range is a runtime error, reported the same way as division by
  zero. `insert`'s valid range is `[0, len(arr)]` inclusive (`len` itself
  means "append"); `set`'s and indexing's valid range is `[0, len(arr))`.
- **`==`/`!=` on arrays is a type error in v0** — not implemented, tracked
  in ROADMAP.md.

See "Builtins" below for `len`/`push`/`insert`/`set`'s exact signatures.

## Semantic rules enforced before a program runs

These are checked by the type checker (`src/typecheck.c`) — a program that
violates one of these never executes any code, it's rejected up front:

- Every name must be declared (`val`/`var`/parameter) before use.
- `val` bindings cannot be reassigned; only `var` bindings can.
- A binding's value can never change type across its lifetime.
- Binary/unary operators require their operand types to match exactly
  (see the table below) — mixing `int` and `float` requires an explicit
  `as` cast.
- A function call's argument count and each argument's type must match
  the callee's declared parameter list exactly.
- A non-`void` function must return a value on every statically reachable
  path (an `if` without an `else`, or a bare `while`, is never assumed to
  guarantee a return).
- `void` (the result of a function with no return type, i.e. `print(...)`)
  can't be used as a value anywhere (assigned, passed as an argument,
  used in an expression).

| Operator group        | Operators                  | Operand types           | Result  |
|------------------------|-----------------------------|--------------------------|---------|
| Arithmetic             | `+ - * / %`                 | `int,int` or `float,float`* | same type |
| Comparison             | `< > <= >=`                 | `int,int` or `float,float` | `bool`  |
| Equality               | `== !=`                     | any two of the same *scalar* type (never arrays) | `bool`  |
| Logical                | `&& \|\|`                   | `bool, bool`             | `bool`  |
| Unary negate           | `-`                          | `int` or `float`         | same type |
| Unary not              | `!`                          | `bool`                   | `bool`  |

\* `%` is only defined for `int, int` in v0 (no float modulo).

Errors that can only be detected while running (division/modulo by zero)
are runtime errors, reported the same way as compile-time errors (see
`src/error.c`): `ziman: <phase> error at <line>:<col>: <message>`, then
exit code 1.

## Builtins

Builtin names are reserved — redefining any of them as a user function is
a type error.

- `print(x)` — takes exactly one argument of any non-void type, writes its
  display form followed by a newline to stdout, returns nothing.
- `concat(a: string, b: string) -> string` — joins two strings. Both
  arguments must already be `string`; there's no auto-stringify (that
  would be the same implicit-coercion problem `+` avoids, just moved to a
  function). Use `str` to convert first.
- `str(x) -> string` — converts an `int`, `float`, or `bool` to its
  display string. Rejects an argument that's already a `string` (there
  should be exactly one way to get a string from a value, not two).
- `format(fmt, args...) -> string` — the only variable-arity call in
  Ziman. `fmt` **must be a string literal** (not a variable or any other
  computed value) containing zero or more `{}` placeholders; the number
  of placeholders must exactly match the number of remaining arguments —
  both are checked at compile time, because the literal's content is
  visible right there at the call site. Each argument can be any
  non-void scalar type and is displayed the same way `print`/`str`
  display it — placeholders are untyped (no `%d`/`%s`-style verbs),
  since the argument's own declared type is already known and repeating
  it in the format string would just be a second place for it to drift
  out of sync. Literal `{`/`}` characters can't appear unescaped in a
  format string in v0 (no escape syntax yet — tracked in ROADMAP.md).
- `len(arr) -> int` — the element count of any `[T]`.
- `push(arr: [T], x: T) -> [T]` — a new array with `x` appended.
- `insert(arr: [T], i: int, x: T) -> [T]` — a new array with `x` inserted
  at index `i`; `i` must be in `[0, len(arr)]`, a runtime error otherwise.
- `set(arr: [T], i: int, x: T) -> [T]` — a new array with the element at
  `i` replaced by `x`; `i` must be in `[0, len(arr))`, a runtime error
  otherwise.

None of `push`/`insert`/`set` mutate their `arr` argument — see "Arrays"
above for why, and why that's true regardless of `val`/`var`.

`+` itself stays numeric-only in v0 — see docs/DESIGN.md principle #2 on
why string concatenation didn't just overload it.
