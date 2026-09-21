# Ziman — v0 Syntax Reference

File extension: `.zm`.

## Lexical rules

- Comments: `// to end of line`. No block comments in v0.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Casing is **enforced, not just
  conventional**: a `val`/`var`/parameter/function name must not start
  with an uppercase letter, and a `struct` name must. This isn't only
  style — it's what makes struct-literal parsing unambiguous everywhere,
  including inside an `if`/`while` condition; see "Structs" below.
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
program        := { use_decl | top_level_decl }   (* use may appear anywhere
                     at the top level, though conventionally listed first *)
top_level_decl := [ "pub" ] ( function_decl | native_fn_decl | struct_decl )

use_decl       := "use" IDENT { "/" IDENT } [ "as" IDENT ]
function_decl  := "fn" IDENT "(" [ param_list ] ")" [ "->" type ] block
native_fn_decl := "native" "fn" IDENT "(" [ param_list ] ")" [ "->" type ]
                    (* no body at all -- see "Modules" *)
struct_decl    := "struct" TYPE_IDENT "{" NEWLINE* { field NEWLINE* } "}"
field          := IDENT ":" type                  (* same shape as param *)
param_list     := param { "," param }
param          := IDENT ":" type
type           := "int" | "float" | "bool" | "string" | "[" type "]"
                 | TYPE_IDENT | IDENT "." TYPE_IDENT   (* qualifier.Type *)
                 | "Result" "<" type "," type ">"      (* Result<Ok, Err> *)

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
postfix        := primary { ("[" expr "]") | ("." IDENT) }   (* indexing and
                    field access, freely mixed and chained: matrix[i][j],
                    shapes[i].corner.x *)
unary          := ( "!" | "-" ) unary | postfix
call           := primary { "(" [ arg_list ] ")" }
arg_list       := expr { "," expr }
primary        := INT_LIT | FLOAT_LIT | STRING_LIT | "true" | "false"
                 | IDENT | qualified_call | qualified_struct_lit
                 | "(" expr ")" | array_lit | struct_lit | with_expr
array_lit      := "[" [ expr { "," expr } ] "]"
struct_lit     := TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
qualified_call        := IDENT "." IDENT "(" [ arg_list ] ")"
qualified_struct_lit  := IDENT "." TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
with_expr      := "with" postfix "{" [ field_init { "," field_init } ] "}"
field_init     := IDENT ":" expr

(* TYPE_IDENT is an IDENT that starts with an uppercase letter -- see the
   casing note under Lexical rules. A struct literal is recognized purely
   by that casing, in every position a primary expression can appear,
   including an `if`/`while` condition -- there is no restricted position
   the way Rust needs one for its own bare struct literals.

   qualified_call/qualified_struct_lit look identical to plain field access
   (IDENT "." IDENT) until you see what follows the second identifier --
   the parser tries the qualified form and falls back to ordinary
   `.field` access (part of `postfix` above) if it doesn't match. See
   "Modules" for what a qualifier actually resolves to. *)
```

Notes on things this deliberately leaves out of v0 (tracked in ROADMAP.md):
function calls as a primary only chain off a plain or single-qualified
identifier, never an arbitrary expression (no first-class function values
yet, so there's nothing to call *except* a name), no indexed/field
*assignment* (`arr[i] = x` and `p.x = 1` aren't valid — see "Arrays" and
"Structs" below), no `for` loop (only `while`), and no expression-level
`if`.

## Types

- `int` — 64-bit signed integer.
- `float` — double-precision IEEE 754.
- `bool` — `true`/`false`.
- `string` — immutable text.
- `[T]` — a growable, homogeneous, indexed array of `T`; nests freely
  (`[[int]]` is an array of arrays of `int`). Value semantics, like every
  other type in Ziman: assigning or passing an array always yields an
  independent value, never a shared reference — see "Arrays" below.
- A `struct` name (e.g. `Point`) — a named, nominal aggregate of fields.
  Two structs are only the same type if they have the same declared name,
  even if their fields are identical (`Point{x,y}` and `Vector{x,y}` are
  different, incompatible types). Value semantics, same as arrays. See
  "Structs" below.
- `Result<T, E>` — a value that's either a success carrying `T` or a
  failure carrying `E`. A second hardcoded parametric type, like `[T]` —
  not general user-definable generics — so `T` and `E` can be any Ziman
  type (including another `Result`, an array, or a struct), but you can't
  declare your own `Result`-shaped type. `<>`, not `[]`: `[]` stays
  scoped to sequences (the array type, the array literal, and indexing
  all already share it); `<>` is the near-universal convention for
  "parameterize a builtin type" and never collides with the comparison
  operators, since a type only ever appears after `:`/`->`/`as`, never in
  expression grammar. See "Result" below.
- No implicit conversion between any scalar type. `x as float` / `x as
  int` are the only casts, and only between `int` and `float` (never
  arrays, structs, or Results).

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

## Structs

```
struct Point {
    x: int
    y: int
}

fn distance_squared(a: Point, b: Point) -> int {
    val dx = a.x - b.x
    val dy = a.y - b.y
    return dx * dx + dy * dy
}

fn main() {
    val p1 = Point { x: 0, y: 0 }
    val p2 = Point { x: 3, y: 4 }
    print(distance_squared(p1, p2))    // 25

    val p3 = with p2 { x: 99 }
    print(p2)                           // Point { x: 3, y: 4 } -- unchanged
    print(p3)                           // Point { x: 99, y: 4 }
}
```

- **Declaration fields are typed exactly like function parameters**
  (`name: type`), one per line inside `{}` — no comma separator, same
  newline-ends-a-declaration rule as everywhere else, so a `struct` body
  reads like a function body.
- **Construction requires every field, named, in any order**:
  `Point { x: 0, y: 0 }`. There is no positional construction
  (`Point(0, 0)`) — with two same-typed fields, a positional call is
  exactly the "swapped x and y" bug class, silently. A missing field, an
  unknown field, or a field given twice are all type errors.
- **No field assignment (`p.x = 1`) and no methods.** Structs are plain
  data, value-typed like arrays — a function taking a `Point` can never
  mutate the caller's copy. To change a field, use `with`, which produces
  a *new* struct with the named fields replaced and everything else
  copied from the base: `val p2 = with p { x: 99 }`. Unlike construction,
  `with` never requires every field — anything left unmentioned is kept
  from the base, since there's a base to fall back on.
- **Functions operate on structs, not methods on them**
  (`distance_squared(a, b)`, not `a.distance_squared(b)`) — no `impl`
  blocks, no inheritance. See docs/DESIGN.md on why (the classes-vs-traits
  discussion this project's history has).
- **`==`/`!=` on structs is a type error in v0**, same as arrays — not
  implemented, tracked in ROADMAP.md.
- Field types can reference another struct (`struct Line { start: Point
  end: Point }`) and structs can be declared in any order — a field type
  can name a struct declared later in the file. Referencing a struct name
  that's never declared anywhere is a compile-time error at the field (or
  parameter/return type) that names it.

## Modules

One file is one module, identified by its filesystem path — no separate
`module Foo` declaration to keep in sync with the filename.

```
// models/user.zm
pub struct User {
    id: int
    name: string
}

pub fn greet(u: User) -> string {
    return format("hello, {}", u.name)
}

fn validate_name(name: string) -> bool {   // no `pub` -- private to this module
    return len(name) > 0
}
```

```
// main.zm
use models/user

fn main() {
    val u = user.User { id: 1, name: "Ada" }
    print(user.greet(u))
}
```

- **`use path/to/mod [as alias]`** — the path is slash-separated and maps
  directly onto the filesystem (`models/user` → `models/user.zm`),
  resolved relative to the entry file's own directory. Slash, not dot: a
  module path never shares a token with `.field` access, which already
  means something else. The name a module is referenced by (`user` above)
  is the path's last segment by default, or the explicit `as` alias if
  one is given — either way it's casing-enforced lowercase, like every
  other value-like name.
- **Every cross-module reference is qualified — `user.greet(u)`,
  `user.User { ... }`, `field: user.User` — never a bare name that
  happens to resolve to some import.** There is no unqualified/wildcard
  import form. A qualified call or struct literal looks identical to
  plain field access (`p.x`) until the parser sees what follows the
  second identifier; see the grammar note above.
- **`pub` marks a declaration for export; the default is private to the
  module.** A private function/struct can be called/constructed freely
  from within its own module, but referencing it from another module is
  a compile-time error naming both the declaration and its module
  (`function 'x' in module 'y' is private`). A consequence: struct and
  function names only need to be unique *within* their own module, not
  across the whole program — two modules can each declare their own
  `Point`, even both private, without colliding.
- **No separate manifest file.** The app's module dependency graph is
  just its entry file's `use` statements, followed transitively — each
  module is loaded and parsed at most once (a diamond dependency shares
  one instance), and a circular `use` is a compile-time error instead of
  infinite recursion.
- **The entry file (the one passed to `ziman`) is never itself a `use`
  target** — nothing imports an app's own entry point — so its own
  top-level names stay bare/unqualified exactly as in a single-file
  program. A program with no `use` statements behaves identically to
  before modules existed.
- **`native fn name(params) -> type`** declares a function with no body
  at all — its implementation is a C function registered under its exact
  fully-qualified name, looked up at call time. The type checker treats
  its signature like any other function's; calling one with nothing
  registered for it is a runtime error ("no registered implementation").
  A minimal real module ships today — `io`, with `read_file(path:
  string) -> Result<string, string>` and `write_file(path: string,
  contents: string)` — a small, honest proof of the mechanism, not a
  general file API (no append mode, no directory listing yet).
  `read_file` reports a missing/unreadable file as a `Result` (see
  "Result" below) rather than crashing the whole program, since that's
  an expected, recoverable condition; `write_file` is still fatal on
  failure for now (disk full, permissions), not yet migrated. `io.zm`
  isn't found by any bundled-library search path yet, so a project that
  wants it needs its own copy alongside the entry file for now.

## Result

```
fn safe_div(a: int, b: int) -> Result<int, string> {
    if b == 0 {
        return err("division by zero")
    }
    return ok(a / b)
}

fn main() {
    val r = safe_div(10, 0)
    if is_ok(r) {
        print(unwrap(r))
    } else {
        print(concat("failed: ", unwrap_err(r)))
    }
}
```

- **No sum types, no `enum`, no `match`.** `Result<T, E>` is a single
  opaque builtin type, manipulated only through five functions —
  `ok`, `err`, `is_ok`, `unwrap`, `unwrap_err` (see "Builtins" below for
  exact signatures). There's nothing to pattern-match on and no way to
  define your own `Result`-shaped type.
- **`ok(x)`/`err(e)` can't fully infer their own type** — each only
  determines half of `Result<T, E>` (an `ok(x)` call tells you `T` from
  `x`, but nothing about what `E` should be, and vice versa for `err`).
  Legal in exactly two places, both of which supply the missing half: a
  `val`/`var` with an explicit `Result<T, E>` annotation, or as the
  value in a `return` inside a function whose declared return type is
  `Result<T, E>` (by far the more common shape — `return ok(x)`/
  `return err(e)` is `Result`'s primary use). Anywhere else — a bare
  `ok(5)` statement, passing `ok(5)` as an argument — is a compile error
  ("cannot infer the type of 'ok(...)' here"), the same restriction
  empty array literals (`[]`) already have and for the same reason:
  there's nothing in the call itself to infer the missing type from.
- **`unwrap`/`unwrap_err` are the explicit, opt-in escape hatch back to
  a fatal error.** `unwrap(r)` returns `r`'s success value, or raises a
  runtime error if `r` is an `Err` (mirror for `unwrap_err`). This is
  the same fatal behavior every runtime error already has — the point
  isn't to avoid ever crashing, it's that crashing on failure is now a
  *choice* the caller makes explicitly, instead of something a function
  like `io.read_file` used to force unconditionally.
- **`==`/`!=` on `Result` is a type error**, same as arrays and structs
  — not implemented, tracked in ROADMAP.md.
- **`T`/`E` can be any Ziman type**, including another `Result`, an
  array, or a struct — this is what lets application code layer a
  richer error type (a struct with a `code` and a `message`, say) on top
  of a simpler one a native function returns (`io`'s is plain `string`),
  converting at the boundary rather than forcing one universal error
  shape everywhere.
- Printing a `Result` shows `Ok(...)` or `Err(...)` (e.g. `Ok(5)`,
  `Err(division by zero)`), recursively displaying the payload the same
  way `print` already displays anything else.

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
- A `val`/`var`/parameter/function name must not start with an uppercase
  letter; a `struct` name must. Enforced, not just conventional — see
  "Structs" above.
- Every struct field type and every function parameter/return type must
  name a type that actually exists — an unresolved struct name is caught
  at the declaration that names it, before any function body is checked.
- Struct types are nominal, not structural — see "Types" above.
- `ok(...)`/`err(...)` are only legal where an expected `Result<T, E>`
  type is available (a `val`/`var` annotation or the enclosing
  function's return type) — see "Result" above.
- `Result` is a reserved name, like `int`/`bool`/etc. — you can't declare
  your own `struct Result { ... }`.

| Operator group        | Operators                  | Operand types           | Result  |
|------------------------|-----------------------------|--------------------------|---------|
| Arithmetic             | `+ - * / %`                 | `int,int` or `float,float`* | same type |
| Comparison             | `< > <= >=`                 | `int,int` or `float,float` | `bool`  |
| Equality               | `== !=`                     | any two of the same *scalar* type (never arrays, structs, or Results) | `bool`  |
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
- `ok(x: T) -> Result<T, E>` / `err(e: E) -> Result<T, E>` — construct a
  `Result`. Only legal where the missing type parameter is available
  from context — see "Result" above.
- `is_ok(r: Result<T, E>) -> bool`
- `unwrap(r: Result<T, E>) -> T` — the success value, or a runtime error
  if `r` is `Err`.
- `unwrap_err(r: Result<T, E>) -> E` — the error value, or a runtime
  error if `r` is `Ok`.

None of `push`/`insert`/`set` mutate their `arr` argument — see "Arrays"
above for why, and why that's true regardless of `val`/`var`.

`+` itself stays numeric-only in v0 — see docs/DESIGN.md principle #2 on
why string concatenation didn't just overload it.
