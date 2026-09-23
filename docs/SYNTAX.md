# Ziman — v0 Syntax Reference

File extension: `.zm`.

## Lexical rules

- Comments: `// to end of line`. No block comments in v0.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Casing is **enforced, not just
  conventional**: a `val`/`var`/parameter/function name must not start
  with an uppercase letter, and a `struct`/`enum`/enum-variant name must.
  This isn't only style — it's what makes struct-literal and enum-access
  parsing unambiguous everywhere, including inside an `if`/`while`
  condition; see "Structs" and "Enums" below.
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
- **Every comma-separated list can span multiple lines** — a struct
  literal's fields, a `with` override list, a function call's arguments,
  an array literal's elements, and a function's declared parameter list
  all tolerate a line break after each item and after each comma, the
  same tolerance a `struct`/`enum` *declaration*'s fields/variants
  already had. This needs explicit parser support (not just the lexical
  rule above): the token right before a list's *last* item is often one
  that *can* end a statement (an int literal, an identifier, a closing
  `)`/`]`/`}`...), so the newline right after it is a real, significant
  token that has to be explicitly skipped before checking for the closing
  `)`/`]`/`}` — done in `parse_field_inits`/`parse_comma_separated`/
  `parse_params`. A trailing comma before the close (`Point { x: 1, y: 2,
  }`) is still a syntax error either way — this only tolerates the line
  break, not an extra comma.

## Grammar (informal EBNF)

```
program        := { use_decl | top_level_decl }   (* use may appear anywhere
                     at the top level, though conventionally listed first *)
top_level_decl := [ "pub" ] ( function_decl | native_fn_decl | struct_decl | enum_decl | global_val_decl )

global_val_decl := "val" IDENT [ ":" type ] "=" expr
                    (* a module-wide constant -- see "Modules" for
                       evaluation order and why there's no global "var" *)

use_decl       := "use" IDENT { "/" IDENT } [ "as" IDENT ]
function_decl  := "fn" IDENT "(" [ param_list ] ")" [ "->" type ] block
native_fn_decl := "native" "fn" IDENT "(" [ param_list ] ")" [ "->" type ]
                    (* no body at all -- see "Modules" *)
struct_decl    := "struct" TYPE_IDENT "{" NEWLINE* { field NEWLINE* } "}"
field          := IDENT ":" type                  (* same shape as param *)
enum_decl      := "enum" TYPE_IDENT "{" NEWLINE* { variant NEWLINE* } "}"
variant        := TYPE_IDENT [ "(" type ")" ]     (* zero or one payload;
                    see "Enums" for why never more than one *)
param_list     := param { "," param }
param          := IDENT ":" type [ "..." ]
                    (* the trailing "..." marks a variadic parameter, only
                       legal on the last param in the list -- see
                       "Functions" -- "Variadic parameters" below *)
type           := "int" | "float" | "bool" | "string" | "[" type "]"
                 | TYPE_IDENT | IDENT "." TYPE_IDENT   (* qualifier.Type *)
                 | "Result" "<" type "," type ">"      (* Result<Ok, Err> *)
                 | "Channel" "<" type ">"              (* Channel<T> *)
                    (* a bare TYPE_IDENT names a struct or an enum -- the
                       grammar can't tell which (both are just PascalCase
                       identifiers); it resolves to whichever one is
                       actually declared under that name. See ast.h's Type
                       comment for how the compiler resolves this. *)

block          := "{" NEWLINE* { statement NEWLINE* } "}"

statement      := val_decl | var_decl | assign_stmt
                 | if_stmt | while_stmt | for_stmt
                 | break_stmt | continue_stmt | spawn_stmt | return_stmt | expr_stmt

val_decl       := "val" IDENT [ ":" type ] "=" expr
var_decl       := "var" IDENT [ ":" type ] "=" expr
assign_stmt    := IDENT "=" expr
if_stmt        := "if" expr block [ "else" (if_stmt | block) ]
while_stmt     := "while" expr block
for_stmt       := "for" IDENT [ "," IDENT ] "in" expr [ ".." expr ] block
                    (* ".." is only legal here, directly after the first
                       expr -- not a general expression form. See "for
                       loops" below for what each binder count/".." combo
                       means. *)
break_stmt     := "break"      (* only legal inside a while/for body --
                                   see "For loops" below *)
continue_stmt  := "continue"   (* same restriction as break_stmt *)
spawn_stmt     := "spawn" IDENT [ "." IDENT ] "(" [ arg_list ] ")"
                    (* the named function is resolved statically, exactly
                       like an ordinary call's callee -- never a
                       first-class function value. See "Concurrency". *)
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
                 | IDENT | qualified_call | qualified_struct_lit | enum_access
                 | "(" expr ")" | array_lit | struct_lit | with_expr
array_lit      := "[" [ expr { "," expr } ] "]"
struct_lit     := TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
qualified_call        := IDENT "." IDENT "(" [ arg_list ] ")"
qualified_struct_lit  := IDENT "." TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
enum_access    := TYPE_IDENT "." IDENT [ "(" [ arg_list ] ")" ]
                    (* `Shape.Circle(5.0)`/`Shape.Point` (construction) or
                       `Shape.is_Circle(s)`/`Shape.unwrap_Circle(s)` (a
                       generated consumption call) -- same shape either
                       way, disambiguated by the type checker rather than
                       the grammar. See "Enums". *)
with_expr      := "with" postfix "{" [ field_init { "," field_init } ] "}"
field_init     := IDENT ":" expr

(* TYPE_IDENT is an IDENT that starts with an uppercase letter -- see the
   casing note under Lexical rules. A struct literal is recognized purely
   by that casing, in every position a primary expression can appear,
   including an `if`/`while` condition -- there is no restricted position
   the way Rust needs one for its own bare struct literals.

   qualified_call/qualified_struct_lit/enum_access all look identical to
   plain field access (IDENT "." IDENT) until you see what follows the
   second identifier and, for enum_access, the first one's casing -- the
   parser tries the qualified forms and falls back to ordinary `.field`
   access (part of `postfix` above) if none match. A module qualifier is
   always lowercase (enforced on `use ... as`), so an uppercase first
   identifier can only mean enum_access, never qualified_call/
   qualified_struct_lit -- see "Modules" for what a lowercase qualifier
   resolves to, and "Enums" for the uppercase case. *)
```

Notes on things this deliberately leaves out of v0 (tracked in ROADMAP.md):
function calls as a primary only chain off a plain or single-qualified
identifier, never an arbitrary expression (no first-class function values
yet, so there's nothing to call *except* a name), no indexed/field
*assignment* (`arr[i] = x` and `p.x = 1` aren't valid — see "Arrays" and
"Structs" below), and no expression-level `if`.

## Types

- `int` — 64-bit signed integer.
- `float` — double-precision IEEE 754.
- `bool` — `true`/`false`.
- `string` — immutable text.
- `bigint` — arbitrary-precision signed integer, limited only by memory.
  A literal is written with a trailing `n` (`12345678901234567890n`) —
  never bare digits, which always mean `int`. See "Numbers" below.
- `decimal` — arbitrary-precision exact decimal (an unscaled `bigint`
  plus a scale, exactly Java's `BigDecimal` shape). A literal is written
  with a trailing `d` (`19.99d`, `20d`) — never bare digits or a bare
  fractional literal, which always mean `int`/`float`. See "Numbers"
  below.
- `byte` — unsigned, 0-255. A literal is written with a trailing `b`
  (`200b`) — never bare digits, which always mean `int`. Unlike the
  other four scalar types above, not a number to compute with: no
  arithmetic, no `abs`/`min`/`max`, just construction, `==`/`!=`,
  `hash`, `str`/`print`/`format`, and a cast to/from `int`. See
  "Numbers" below.
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
- An `enum` name (e.g. `Shape`) — a named, nominal tagged union: a value
  is exactly one of the enum's declared variants, optionally carrying one
  payload value. Unlike `[T]`/`Result<T, E>`, this is a type *users*
  declare, not hardcoded into the compiler — but it's still concrete
  (monomorphic), not generic. See "Enums" below.
- `Channel<T>` — a thread-safe FIFO queue of `T`, and Ziman's concurrency
  primitive: `send`/`receive` on one, and `spawn` to start a function
  running on a new OS thread. A third hardcoded parametric type, same
  trick as `[T]`/`Result<T, E>` — but unlike those, hardcoding this one
  isn't a shortcut around generics: its real value (safe blocking
  send/receive across real threads) is inherently native, something no
  amount of user-definable generics would ever let you express in pure
  Ziman. See "Concurrency" below.
- No implicit conversion between any scalar type. `x as float` / `x as
  int` / `x as bigint` / `x as decimal` are the only casts among
  `int`/`float`/`bigint`/`decimal` (never arrays, structs, enums,
  Results, or Channels) — every cast between two of these four is lossy
  in at least one direction, see "Numbers" below for exactly how.
  `byte` only casts to/from `int` (`x as byte`/`x as int`), not the
  other three — `int as byte` saturates to `[0, 255]` rather than
  wrapping.

## Numbers

`bigint` and `decimal` are a third and fourth numeric type alongside
`int`/`float`, added once a real need for exact, unbounded-size
arithmetic showed up (`100!`; exact money/measurement math) that
`int`'s fixed 64 bits can't cover and `float` can't represent exactly.
Neither ever implicitly mixes with any other numeric type — same "no
implicit coercion" rule as everywhere else, `use 'as' to cast
explicitly`.

```
fn factorial(n: bigint, limit: int, i: int) -> bigint {
    if i > limit {
        return n
    }
    return factorial(n * (i as bigint), limit, i + 1)
}

fn main() {
    print(factorial(1n, 100, 1))
    // 93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000
}
```

- **A literal needs the `n` suffix, always** — `12345678901234567890n`,
  never bare `12345678901234567890` (which would silently overflow as an
  `int` today; `bigint` gives you an explicit way to say "I mean this
  exactly," not an implicit fallback). No suffix on a fractional literal
  (`19.99n` is a syntax error) — `bigint` has no fractional part, that's
  what `decimal` (below) is for.
- **All of `int`'s operators/builtins work**: `+ - * / %`, unary `-`,
  `< > <= >= == !=`, `str`, `abs`, `min`/`max`, `hash`, and
  `parse_bigint(s: string) -> Result<bigint, string>` (the `bigint`
  equivalent of `parse_int`). `/`/`%` truncate toward zero and take the
  dividend's sign for the remainder, exactly like `int` — not Python's
  floor division. Division/modulo by zero is a runtime error, same as
  `int`/`float`.
- **Casts are lossy in both directions.** `bigint as int`/`bigint as
  float` saturate to `INT64_MAX`/`INT64_MIN` (or the nearest `float`) if
  the value doesn't fit — never silently wrap around. `int as
  bigint`/`float as bigint` are exact for `int`, but `float as bigint`
  truncates the fractional part first (a `float` may already have lost
  precision before the cast ever runs).
- **Composes with everything else already built**: a `bigint` field in a
  struct, a `bigint` array element, a generic type argument
  (`Box<bigint>`), and a `HashMap<bigint, V>` key (it satisfies both
  `Eq` and `Hash`) all work.
- Internally, arbitrary precision means a heap-allocated value (unlike
  `int`/`float`, which live directly inline) — see docs/DESIGN.md's
  "Numbers" entry for why this doesn't grow every other `Value` in the
  interpreter.

`decimal` is built directly on `bigint`: an unscaled `bigint` magnitude
plus a scale (digits after the decimal point) — exactly Java's
`BigDecimal` shape.

```
fn main() {
    val price: decimal = 19.99d
    val tax_rate: decimal = 0.0825d
    val total = price + (price * tax_rate)
    print(total)                        // 21.639175 -- exact, no binary rounding drift

    print(10d / 3d)                     // 3 -- rounds half-up to the operands' own scale
    print(decimal_round(1.2345d, 2))    // 1.23
}
```

- **A literal needs the `d` suffix, always** — `19.99d`, `20d` — never
  bare `19.99`/`20`, which always mean `float`/`int`.
- **All of `float`'s operators/builtins work**: `+ - * / %`, unary `-`,
  `< > <= >= == !=`, `str`, `abs`, `min`/`max`, `hash`, and
  `parse_decimal(s: string) -> Result<decimal, string>`.
  `decimal_round(d: decimal, scale: int) -> decimal` is `decimal`'s
  equivalent of `floor`/`ceil`/`round` — a separate name, not an
  overload of `round`, since Ziman has no argument-based overloading
  anywhere (every builtin name maps to exactly one signature).
- **`+`/`-` align scales first and are always exact**: `1.5d + 0.25d` is
  `1.75d`, never rounded. **`*` needs no alignment and is always
  exact**: `1.5d * 0.25d` is `0.375d` (scale 3). **`/` doesn't always
  terminate**, so it rounds half-up-away-from-zero to
  `max(a.scale, b.scale)` — `10d / 3d` is `3` (not `3.33...`), and
  `-2d / 3d` is `-1` (rounds away from zero, not toward it). `%` is an
  *exact* remainder (`a - trunc(a / b, scale 0) * b`) — no rounding
  involved, unlike `/`. Division/modulo by zero is a runtime error,
  same as `int`/`float`/`bigint`.
- **`==` compares numeric value, ignoring trailing-zero scale
  differences**: `1.0d == 1.00d` is `true`, and `hash(1.0d) ==
  hash(1.00d)` too — deliberately diverging from Java's well-known
  `BigDecimal.equals()` gotcha, where scale participates in `equals()`
  but not `compareTo()`.
- **Casts are lossy going through `float` in either direction** (a
  `float` already can't represent most decimals exactly), and
  truncating going to `int`/`bigint` (the fractional part is dropped,
  not rounded — same as `float as int` truncating today). `int as
  decimal`/`bigint as decimal` are exact.
- **Composes with everything else already built**: a `decimal` field in
  a struct, a `decimal` array element, a generic type argument
  (`Box<decimal>`), and a `HashMap<K, decimal>` value all work.

`byte` is a fifth scalar type, but a deliberately different kind of one
from the four numeric types above: unsigned, 0-255, and **not a number
to compute with** — no arithmetic operators, no `abs`/`min`/`max`. It
exists to represent raw octets (a UUID's bytes, cryptographically random
bytes) where "what's the sum of these two bytes" is a meaningless
question but "are these the same byte" and "print this byte" aren't.

```
fn main() {
    val flag: byte = 200b
    val arr: [byte] = [200b, 55b, 0b, 255b]
    print(arr)              // [200, 55, 0, 255]
    print(300 as byte)      // 255 -- saturates, doesn't wrap
    print(-5 as byte)       // 0
}
```

- **A literal needs the `b` suffix, always** — `200b` — never bare
  `200`, which means `int`. No suffix on a fractional literal (`19.99b`
  is not a byte literal at all; the lexer only recognizes `b` after a
  whole-number-shaped literal, same as `n` for `bigint`). **Out-of-range
  is a compile-time error**: `300b` fails to parse ("byte literal 300b
  is out of range (must be 0-255)") rather than silently wrapping or
  truncating — unlike a cast (below), a literal's value is already
  fully known at parse time, so there's no ambiguity about what "out of
  range" should do.
- **Supports `== !=`, `str`, `hash`, and casting to/from `int` only** —
  no `+ - * / %`, no unary `-`, no `< > <= >=`, no `abs`/`min`/`max`.
  Ziman has no operator overloading, so a type either gets the full
  numeric operator set or none of it; `byte` deliberately gets none of
  it, since nothing that needs a `byte` (so far) needs arithmetic on
  one — see docs/DESIGN.md's "byte" entry for the full reasoning.
- **`int as byte` saturates to `[0, 255]` rather than wrapping** — `300
  as byte` is `255`, `-5 as byte` is `0` — same "clamp, don't silently
  wrap" precedent `bigint as int` already set. `byte as int` is always
  exact (every byte value fits in an `int`). No cast to/from
  `float`/`bigint`/`decimal` — nothing needs one, and adding one would
  just be unused code.
- **Composes with everything else already built**: a `byte` field in a
  struct, a `[byte]` array (`==`/`contains` work, since `byte` is
  scalar), a generic type argument (`Box<byte>`), and a
  `HashMap<byte, V>` key (it satisfies both `Eq` and `Hash`) all work.

## Arrays

```
var nums: [int] = []
for i in 0..5 {
    nums = push(nums, i * i)
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
  It's only legal where an expected array type is already known from
  context: a `val`/`var` with an explicit type annotation
  (`val x: [int] = []`), a struct literal field or `with` override
  (`Bag { items: [], label: "empty" }`), or a `return` value against the
  enclosing function's declared return type. Anywhere else (a function
  argument, a non-empty context) it's a type error. A non-empty literal
  infers its element type from its first element and requires every
  other element to match exactly — `[1, "two"]` is a type error.
- **Indexing (`arr[i]`) and `set`/`insert`'s index argument are bounds-checked
  at runtime**, not compile time (the index is usually a computed value) —
  out of range is a runtime error, reported the same way as division by
  zero. `insert`'s valid range is `[0, len(arr)]` inclusive (`len` itself
  means "append"); `set`'s and indexing's valid range is `[0, len(arr))`.
- **`==`/`!=` on arrays is allowed only when the element type is scalar**
  (`int`/`float`/`bool`/`string`) — compares length then elements
  pairwise. An array of structs, Results, or nested arrays is still a
  type error, same reasoning as scalar-only `contains` below: those
  element types don't have a well-defined equality of their own yet.

See "Builtins" below for `len`/`push`/`insert`/`set`/`remove`'s exact
signatures.

## For loops

Three forms, chosen by binder count and whether the right-hand side of
`in` is a range or an array:

```
for x in arr { ... }          // value only -- x: T, once per element
for i in 0..len(arr) { ... }  // index only -- i: int, counting a range
for i, x in arr { ... }       // index + value together
```

- **`a..b` is exclusive of `b`** (`[a, b)`), same convention as
  `substring`'s `start`/`end`. `a`/`b` must both be `int`. If `a >= b`
  the loop body runs zero times — not a runtime error, same as an empty
  array iterates zero times.
- **`a..b` is not a first-class value.** It's only legal directly after
  `in` in a `for` header — there's no `Range` type you can store in a
  variable, pass to a function, or print. This is deliberate, same
  "hardcode the minimum, not general machinery" instinct behind
  `Result<T, E>`: a real `Range` type is a bigger design (does it own
  its own equality? can it be empty as a first-class value? does `for`
  need to special-case it or does it become a third iterable kind
  alongside arrays?) that nothing has asked for yet.
- **The two-binder form only works over an array, never a range** — `for
  i, j in 0..5` is a syntax error naming the offending second binder.
  There's nothing to name separately from the count itself when
  iterating a range.
- **Both loop variables are freshly bound each iteration and are always
  immutable** (`val`-like, not `var`) — assigning to one is the same
  "cannot assign, declared with 'val'" error as any other `val`.
  Reassigning a loop variable would be immediately overwritten by the
  next iteration anyway, so allowing it would only ever be a mistake.
- **Scoped to the loop body only** — a loop variable doesn't exist
  before the loop or after it ends, same as any other block-scoped
  binding.
- **Naming convention, not enforced by the grammar:** name a
  range-loop's single `int` binder `i`/`j`/`k` (matches the near-universal
  counted-loop convention), and an array value-loop's binder with a
  domain name (`user`, `item`, ...). Since `for x in expr { ... }`'s
  single binder means "the element" or "the index" depending on
  `expr`'s type, not its own syntax, this convention is what lets a
  reader (human or AI) tell which case they're in at a glance instead of
  having to check `expr`'s declared type.
- **`break` exits the nearest enclosing loop immediately; `continue`
  skips straight to that loop's next iteration.** Both work in `while`
  and every `for` form. Using either outside a loop is a compile-time
  error, including from inside an `if` — the check follows lexical
  nesting through blocks and `if`/`else`, not just "am I directly inside
  a loop's `{ }`". It does *not* cross a function boundary: a `break`
  inside a function called from a loop is still an error in that
  function, on its own — it can never reach back out to a loop in some
  different function that happened to call it.
- **No labels** — `break`/`continue` always target the *nearest*
  enclosing loop; there's no way to reach an outer loop from inside a
  nested one. Small orthogonal core for now — revisit only if a real
  program needs multi-level early exit often enough to justify it.

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
- **`==`/`!=` on structs compares every field, in declaration order** —
  `a == b` is `a.f1 == b.f1 && a.f2 == b.f2 && ...` for every field `f`,
  short-circuiting on the first mismatch. Both sides must be the same
  declared struct type, same rule as any other `==`. This only works
  when *every* field's own type supports `==` — recursively, so a
  struct nested inside another struct, or containing a `Result<T, E>` or
  an `enum`, is fine as long as that field's own type is itself
  comparable this same way, but an array field of anything but a scalar
  (see "Arrays" above) makes the whole struct a type error to compare,
  naming the struct.
- Field types can reference another struct (`struct Line { start: Point
  end: Point }`) and structs can be declared in any order — a field type
  can name a struct declared later in the file. Referencing a struct name
  that's never declared anywhere is a compile-time error at the field (or
  parameter/return type) that names it.

## Functions

### Variadic parameters

```
// join(prefix: Option<string>, suffix: Option<string>,
// separator: Option<string>, pieces: string...) -> string is a real,
// always-available prelude function (see "Modules" above and the
// Option<T> parameter section below) -- this is a second, hand-written
// example to show the variadic-parameter shape on its own, without
// redeclaring it.
fn sum(nums: int...) -> int {
    var total = 0
    for n in nums {
        total = total + n
    }
    return total
}

fn main() {
    print(sum())              // 0
    print(sum(5))              // 5
    print(sum(1, 2, 3))        // 6
    val nums: [int] = [10, 20, 30]
    print(sum(nums))           // 60 -- an existing array works too
}
```

- **A trailing `...` on the last parameter's type marks it variadic** —
  a caller may then supply *zero or more* individually-listed arguments
  of that element type, instead of exactly one. Only the last parameter
  in a list may be variadic (a compile-time error otherwise, since a
  variadic parameter has to "absorb" everything after it — nothing could
  come after it unambiguously).
- **Inside the function itself, a variadic parameter is an ordinary
  array of its declared element type** — `nums: int...` behaves as
  `nums: [int]` inside `sum`'s own body; the declared type in the
  signature is the *element* type only so a reader sees at a glance what
  a caller writes at the call site, not what the parameter becomes
  internally.
- **An already-built array works as the sole trailing argument too** —
  `sum(nums)` where `nums: [int]` passes the whole array through
  directly, without needing to unpack it back into individual arguments
  first. Which of the two a given call means (a single element, or the
  whole array) is decided purely by that argument's own static type —
  `int` vs `[int]` can never be confused, so there's no ambiguity to
  resolve at runtime the way Java's overload resolution sometimes has to
  choose between a varargs call and an explicit array argument. This
  only works when there's *exactly one* trailing argument; two or more
  trailing arguments are always treated as individual elements.
- **No syntax for spreading an array into a call that also has other
  individually-listed trailing arguments** (`sum(1, 2, nums)` doesn't
  merge `nums` into the list) — the "whole array" reading only applies
  when it's the *only* argument in the variadic position.
- Lexed as an ordinary `..` (the same token a `for x in a..b` range
  uses) immediately followed by a `.` — no dedicated token, and no
  ambiguity with a range, since a range's `..` never appears in a
  parameter list to begin with.

### Named arguments

```
fn localtime_of(hour: int, minute: int, second: int, nano: int) -> string {
    return format("{}:{}:{}.{}", hour, minute, second, nano)
}

fn main() {
    // Same call, two ways -- both reach localtime_of identically.
    print(localtime_of(1, 2, 3, 4))
    print(localtime_of(hour: 1, minute: 2, second: 3, nano: 4))
    // Named arguments may be given in any order; the label is what
    // resolves each one to its parameter, not position.
    print(localtime_of(nano: 4, second: 3, minute: 2, hour: 1))
}
```

- **A call's arguments are either all positional or all named, never a
  mix** — `f(1, y: 2)` is a syntax error. Named arguments reuse the
  exact `name: value` shape a struct literal's fields already have
  (`Point { x: 1, y: 2 }`), rather than a second syntax for the same
  idea.
- **A named argument's label must match the callee's declared parameter
  name exactly** — there's no separate external label the way some
  languages let a call site and a declaration use different names for
  the same parameter; whatever a function's signature calls a
  parameter is the only name a caller may use for it.
- **Named arguments may appear in any order** — the label alone decides
  which parameter each value fills, so `f(y: 2, x: 1)` and
  `f(x: 1, y: 2)` are the same call.
- **Every parameter must still be given exactly once** — a named call
  is not yet a way to omit an argument (see the `Option<T>` parameter
  section below, once written, for the one case where omission is
  legal); giving too few, too many, an unknown name, or the same name
  twice is a compile-time error.
- **Named arguments only apply to real functions** — the small set of
  core builtins the type checker and interpreter special-case directly
  by name (`print`, `len`, `push`, `hash`, and the rest listed in
  README.md's "Builtins ship two ways") each have exactly one fixed,
  unlabeled signature and don't accept a named call.
- **A variadic parameter's named value is still a single expression**
  — a named call has no way to list several individually-labeled
  elements under one name, so the value given for a variadic parameter
  in a named call is treated as the single-trailing-argument case
  variadic calls already have: either the element type or the whole
  array type, same as `sum(nums)` above.
- A named call is resolved to the callee's declared parameter order
  once, during type checking — by the time a program runs, there's no
  remaining difference between how a positional and a named call to the
  same function behave.

### Option\<T\> parameter omission and defaults

```
fn describe(name: string, nickname: Option<string>, priority: Option<int> = 3) -> string {
    if Option.is_Some(nickname) {
        return format("{} ({}), priority {}", name, Option.unwrap_Some(nickname), Option.unwrap_Some(priority))
    }
    return format("{}, priority {}", name, Option.unwrap_Some(priority))
}

fn main() {
    // Every argument given, exactly as any other function call works.
    print(describe("Ada", Option<string>.Some("Countess"), 5))

    // A named call may leave an Option<T> parameter out entirely.
    // nickname has no declared default, so it becomes None; priority
    // has one (`= 3`), so it becomes that instead.
    print(describe(name: "Alan"))

    // A bare value works too, wherever an Option<T> was expected --
    // auto-wrapped to Some(...) -- both for a supplied argument and for
    // a parameter's own default (`= 3` above, not `= Option<int>.Some(3)`).
    print(describe(name: "Hedy", nickname: "Hedley"))
}
```

- **Only an `Option<T>`-typed parameter may ever be omitted from a
  call, and only in a named one** — a positional call is exactly as
  strict as it always was (every parameter, in order, no exceptions);
  a required (non-`Option<T>`) parameter can never be left out of
  *any* call, named or positional.
- **An omitted parameter with no declared default becomes `None`** —
  the same value writing `Option<T>.None` there by hand would produce.
- **`= expr` after a parameter's type gives it a default other than
  `None`**, used whenever that parameter is omitted
  (`priority: Option<int> = 3`). Only legal on an `Option<T>`
  parameter — a required parameter has no "omitted" state a default
  could ever apply to. Don't write `= Option<T>.None` explicitly: that
  is already the default with no `=` at all.
- **The default expression is evaluated exactly once, before `main`
  runs** — the same timing a global val's own initializer gets, not
  freshly on every call that happens to omit it. Behaviorally
  identical either way, since Ziman's values are immutable (nothing
  about a once-computed default could ever go stale across calls), and
  strictly cheaper computed once. Like a global val's own initializer,
  it can't reference this function's other parameters — only whatever
  is already in scope at program start.
- **Wherever an `Option<T>` value is expected — a supplied argument, a
  default expression, a `val`/`var`'s own declared type, a struct
  literal field, a `with` override, or a function's returned value — a
  bare `T` works too**, auto-wrapped to `Some(T)`. An already-`Option<T>`
  value (or `None`) is also legal and passed through unchanged. This is
  a deliberate, narrow exception to "no implicit coercion," modeled on
  Rust's own `impl<T> From<T> for Option<T>`: wrapping a single,
  statically-known type is never lossy or ambiguous, unlike numeric
  coercion.
- **A generic function's `Option<T>` parameter can't have a default
  when `T` is that function's own, still-unbound type parameter** —
  a default is evaluated once, at program start, long before any call
  instantiates `T` with a concrete type, so there's no type left to
  evaluate it against. Giving `Option<int>`, or any other already-
  concrete `Option<...>`, its own default works exactly as shown above.

### Result\<T, E\> bare-value auto-wrap

```
fn require_positive(r: Result<int, string>) -> string {
    if is_ok(r) {
        return concat("got ", str(unwrap(r)))
    }
    return concat("rejected: ", unwrap_err(r))
}

fn as_result(ok_path: bool) -> Result<int, string> {
    if ok_path {
        return 1              // a bare T return value -- becomes Ok(1)
    } else {
        return "bad input"    // a bare E return value -- becomes Err("bad input")
    }
}

fn main() {
    // A bare value of the Ok type or the Err type both auto-wrap.
    print(require_positive(5))       // "got 5" -- Ok(5)
    print(require_positive("no!"))   // "rejected: no!" -- Err("no!")

    // An already-Result<T, E> value still works too.
    val explicit: Result<int, string> = err("nope")
    print(require_positive(explicit))

    print(is_ok(as_result(true)))    // true
    print(is_ok(as_result(false)))   // false
}
```

- **Wherever a `Result<T, E>` value is expected — a supplied function
  argument, a `val`/`var`'s own declared type, a struct literal field,
  a `with` override, or a function's returned value — a bare value of
  either the `T` (Ok) or `E` (Err) type auto-wraps** — a bare `T`
  becomes `Ok(T)`; a bare `E` becomes `Err(E)`. An already-`Result<T,
  E>` value passes through unchanged. Same deliberate, narrow exception
  to "no implicit coercion" as `Option<T>`'s own auto-wrap, and for the
  same reason: wrapping a statically-known type is never lossy or
  ambiguous.
- **When `T == E`, a bare value always becomes `Ok`** — there's no way
  for the sugar itself to tell which one was meant in the degenerate
  same-type case, so it doesn't guess; write `err(value)` explicitly
  for that one case where you need `Err`.
- **`Result<T, E>` parameters can never be omitted, in any call** —
  unlike `Option<T>`, there's no natural "absent" state an outcome type
  could fall back to, so this sugar is only ever about *what a supplied
  value looks like*, never about leaving one out. A `Result<T, E>`
  parameter also can't have a declared default (`= expr`) for the same
  reason — defaults exist to fill in for an omission, and this
  parameter is never omittable to begin with.

### `?` — Option\<T\>/Result\<T, E\> unwrap-or-propagate

```
fn parse_two_numbers(a: string, b: string) -> Result<int, string> {
    val x = parse_int(a)?
    val y = parse_int(b)?
    return x + y
}

fn first_positive_index(items: [int]) -> Option<int> {
    var i = 0
    while i < len(items) {
        if items[i] > 0 {
            return i
        }
        i = i + 1
    }
    return Option<int>.None
}

fn first_positive_doubled(items: [int]) -> Option<int> {
    val idx = first_positive_index(items)?
    return items[idx] * 2
}
```

- **`expr?`, where `expr: Result<T, E>` or `expr: Option<T>`, unwraps
  the success value and keeps going, or immediately returns the
  failure from the *enclosing function* — no boilerplate `if is_err(r)
  { return err_of(r) }`/`if Option.is_None(o) { return None }` needed.**
  `Ok(v)`/`Some(v)` → the whole `expr?` evaluates to `v`; `Err(e)` →
  immediately `return e` (auto-wrapped to `Err(e)` by the enclosing
  function's own bare-value auto-wrap); `None` → immediately returns a
  fresh `None`, of the *enclosing function's* own `Option<T2>` type.
- **Only legal as the entire initializer of a `val`/`var` declaration,
  or as its own bare statement** — never nested inside a larger
  expression (`return str(foo()?)`, `bar(foo()?, baz()?)` are both
  syntax errors). This is a deliberate v0 scope cut: full nesting would
  need the interpreter to unwind out of an arbitrary expression tree,
  not just a statement; restricting `?` to these two positions covers
  the common case (unwrap once, keep using the value) with no new
  interpreter control-flow mechanism.
- **`Result<T, E>`'s `?` requires the enclosing function to return
  `Result<T2, E>` with the *same* `E`** — propagating `Err(e)` means
  handing the caller a real value, so the error type has to match
  exactly; there's no `From`-based conversion the way Rust's `?` has
  (Ziman has no trait/conversion system at all). `Option<T>`'s `?` only
  requires the enclosing function to return `Option<T2>` for *any*
  `T2` — `None` carries nothing to propagate, so the inner `T` and the
  enclosing `T2` never need any relationship (`first_positive_doubled`
  above works despite `first_positive_index` and it having unrelated
  return types in general — here they're both `Option<int>`, but a
  `Option<string>`-returning helper feeding an `Option<int>`-returning
  caller would work exactly the same way).
- **A bare `expr?` statement (no `val`/`var`) still fully checks and
  propagates**, discarding the unwrapped value on success — useful for
  a fallible step whose success value isn't needed, only whether it
  succeeded.

## Generics

**Structs, enums, and functions, cross-module included** — see
docs/ROADMAP.md's "Generics" entry for how this was staged. `Box<T>`
and `Pair<A, B>` below are plain illustrative examples, not corelib
exports — anyone can declare a `Box`-shaped struct exactly like this;
nothing needs to `use` anything to do it.

```
struct Box<T> {
    value: T
}

struct Pair<A, B> {
    first: A
    second: B
}

fn main() {
    val b = Box<int> { value: 5 }
    print(b.value)              // 5

    val p = Pair<int, string> { first: 1, second: "one" }
    print(p.second)             // one

    val nested = Box<[int]> { value: [1, 2, 3] }
    print(nested.value)         // [1, 2, 3]
}
```

- **Explicit type arguments everywhere — no inference.** A generic
  struct's construction must always spell out `Name<Arg, ...>`; there is
  no `Box { value: 5 }` that infers `T = int` from the field. The same
  "explicit over implicit" instinct as `val`/`var` requiring a declared
  type wherever it can't be trivially read off the right-hand side.
- **No general bounds or constraints on a type parameter** — `T` can be
  stored and passed around, nothing else; there's no trait/interface
  system to express "T must support X." The two exceptions are hardcoded
  bounds, `Eq` (meaning "supports `==`") and `Hash` (meaning "supports
  `hash`" — see "Builtins" below), combinable with `+`
  (`struct Map<K: Eq + Hash, V> { ... }`). Enforced at every
  instantiation (`Map<Channel<int>, string>` is a compile-time error,
  since `Channel` supports neither), not a general trait system, and
  each usable inside the bounded declaration's own body (see "Generic
  functions" below). A bound name other than `Eq`/`Hash` (`<T: Ord>`) is
  a compile-time error. `Eq` works the same way `==` already does
  everywhere else (recursively, for a struct/enum whose fields/payloads
  are themselves comparable), but `Hash` is scalar-only — no struct or
  enum has a derived/structural hash, so a `Map`/`Set` key is effectively
  restricted to `int`/`float`/`bool`/`string`/`bigint`/`decimal` for now.
- `Box<int>` and `Box<string>` are different types: a mismatched
  instantiation is a type error at construction, and `==`/`!=` between
  two different instantiations of the same generic struct is rejected
  the same way comparing two unrelated structs would be.
- `Box { value: 5 }` (a generic struct with no type arguments) and
  `Point<int> { x: 0, y: 0 }` (type arguments on a non-generic struct)
  are both compile-time errors, as is giving the wrong number of type
  arguments (`Pair<int> { ... }` when `Pair` declares two).
- A type parameter name that collides with a real struct or enum name
  (`struct Box<Point> { value: Point }` when `Point` is already declared)
  is rejected at the generic struct's own declaration — without this
  check, every reference to the real `Point` inside `Box`'s own fields
  would be silently shadowed by the type parameter instead.
- **Cross-module works too**: `mod.Box<int> { ... }`, and a qualified
  type annotation (`val x: mod.Box<int>`), reach a generic struct
  declared in another module the same way a plain `mod.Type { ... }`
  already did for a non-generic one.

**Generic enums** work the same way, with construction spelling out type
arguments and consumption never needing to. `Option<T> { Some(T) None }`
is the running example below -- it's not a toy for this section, it's
the actual prelude-provided type (see "Modules" below), already
available bare with no declaration or `use` needed:

```
fn main() {
    val a = Option<int>.Some(5)
    val b = Option<int>.None

    print(Option.is_Some(a))       // true
    print(Option.unwrap_Some(a))   // 5
    print(Option.is_None(b))       // true
}
```

- **Construction spells out type arguments, same as a generic struct**:
  `Option<int>.Some(5)`, never bare `Option.Some(5)` (a compile-time
  error, "requires explicit type arguments").
- **Consumption (`is_X`/`unwrap_X`) never takes type arguments** —
  `Option.is_Some(a)`, `Option.unwrap_Some(a)`, not
  `Option<int>.is_Some(a)` (also a compile-time error, "takes no type
  arguments"). The concrete type is already fully known from `a`'s own
  value, so restating it would be pure redundancy — unlike construction,
  where there's no existing value to read it from.
- Two different instantiations of the same generic enum
  (`Option<int>.Some(1)` vs `Option<string>.Some("x")`) are different
  types: `==`/`!=` between them is rejected the same way a generic
  struct's mismatched instantiations are.
- A bound (e.g. `enum Foo<T: Eq> { ... }` on a hypothetical bounded enum
  — the real `Option<T>` has none) works the same as on a struct,
  enforced at every construction site.

**Generic functions** work the same explicit-type-argument way:

```
fn identity<T>(x: T) -> T {
    return x
}

fn are_equal<T: Eq>(a: T, b: T) -> bool {
    return a == b
}

fn main() {
    print(identity<int>(5))            // 5
    print(are_equal<int>(3, 3))        // true
    print(are_equal<string>("a", "b")) // false
}
```

- **Call sites spell out type arguments too**: `identity<int>(5)`, never
  bare `identity(5)` when the declaration is generic.
- **A bound is usable inside the function's own body**, unlike a struct
  or enum's — a struct/enum has no body to use it in. `a == b` inside
  `are_equal<T: Eq>` type-checks specifically because `T` carries the
  `Eq` bound; the same expression inside an unbounded `fn f<T>(a: T, b:
  T) -> bool { return a == b }` is a compile-time error naming the
  missing bound.
- **A generic call is the one place `<` is genuinely ambiguous with a
  comparison** (`identity<int>(5)` vs. `x < y`), since a function's
  callee is always lowercase — unlike a struct/enum reference, an
  uppercase name is never involved. Resolved with a one-token lookahead:
  the parser peeks at what immediately follows `<` and only commits to
  parsing type arguments if it looks like the start of a type (a
  primitive keyword, an uppercase identifier, `[`, `Result`, or
  `Channel`).
- **Cross-module works too**: `mod.identity<int>(5)`.

## Enums

A user-declared tagged union — unlike `[T]`/`Result<T, E>`, which are
hardcoded into the compiler, this is the first parametric-free type
*users* can declare that isn't a plain product type like `struct`.

```
struct RectDims {
    width: float
    height: float
}

enum Shape {
    Circle(float)
    Rectangle(RectDims)
    Point
}

fn area(s: Shape) -> float {
    if Shape.is_Circle(s) {
        val r = Shape.unwrap_Circle(s)
        return 3.14159 * r * r
    }
    val d = Shape.unwrap_Rectangle(s)
    return d.width * d.height
}

fn main() {
    print(area(Shape.Circle(2.0)))                          // 12.5664
    print(area(Shape.Rectangle(RectDims{width:3.0,height:4.0})))  // 12
    print(Shape.Point)                                        // Point
}
```

- **A variant holds zero or one payload value, never an inline tuple or
  inline struct-shaped payload.** `Circle(float)` and `Rectangle(RectDims)`
  are the only two shapes a variant declaration can take: no payload at
  all (`Point`), or exactly one value of a named type in parens. Ziman has
  no tuple type, so anything needing more than one payload value declares
  a separate `struct` and wraps it (`Rectangle(RectDims)` above) — the
  same "one value per case" shape `Result<T, E>`'s `Ok`/`Err` already use.
  A payload-less variant (`Point`) is the degenerate case of this same
  mechanism, not a separate one — it's what a closed set of named
  constants (Java's `enum Color { RED, GREEN, BLUE }` shape) reduces to
  once nothing carries a payload.
- **Nominal and closed, exactly like `struct`.** Variants are fixed at
  declaration and never reopened elsewhere. Enum names live in the same
  "type name" namespace as struct names (a program can't declare both a
  `struct Shape` and an `enum Shape`), but variant names only need to be
  unique *within* their own enum — two different enums may freely reuse a
  variant name (`enum A { Some(int) }` and `enum B { Some(string) }` in
  the same file is fine), since access is always enum-qualified.
- **Construction is always qualified by the enum's own name** —
  `Shape.Circle(5.0)`, `Shape.Point` — never bare. Rust/Swift-style
  languages let `Option`/`Result`'s variants be written bare only because
  they're special-cased into every file's prelude; an ordinary user enum
  in those languages requires qualification too. Since two unrelated
  enums can easily share a variant name, bare construction would make
  that silently ambiguous (or force an implicit "first declaration wins"
  rule) — exactly the kind of implicit behavior Ziman avoids elsewhere.
  A unit variant is written bare (`Shape.Point`); writing it with empty
  parens (`Shape.Point()`) is a type error, so there's exactly one way to
  write it. A payload variant requires parens with exactly one argument.
- **Consuming a variant uses two functions the compiler generates per
  enum, per variant: `is_<Variant>` and `unwrap_<Variant>`** — same
  predicate-plus-unwrap shape `Result<T, E>`'s `is_ok`/`unwrap`/
  `unwrap_err` already use, just generalized to *n* variants instead of
  2 and namespaced under the enum's own name so two enums' generated
  functions never collide either. `is_<Variant>(x) -> bool` works for
  every variant; `unwrap_<Variant>(x) -> T` only exists for a
  payload-carrying variant and is a **runtime error if `x` currently
  holds a different variant** — the same fatal-by-default contract
  `unwrap` already has, and the same reasoning: crashing on a wrong
  unwrap is a caller's explicit choice, not something forced silently.
  The generated name embeds the variant's exact declared casing
  (`unwrap_Circle`, not `unwrap_circle`) — simpler and unambiguous, since
  no case-folding step is needed to look either up.
  **There is no `match`/pattern-matching yet** — this predicate-plus-unwrap
  style is the only way to consume a variant in v0; tracked as a
  follow-up design pass in ROADMAP.md.
- **`==`/`!=` on an enum compares variant, then payload** — two values of
  *different* variants are never equal regardless of payload (an `Ok`-
  shaped mismatch is never accidentally "equal" to a payload-less one);
  two values of the *same* variant are equal iff their payloads are.
  Legal only when every payload-carrying variant's payload type is
  itself (recursively) comparable — same rule as struct fields and
  `Result<T, E>`'s `Ok`/`Err` types.
- **Cross-module works too**: `mod.Shape.Circle(5.0)`,
  `mod.Shape.is_Circle(s)`, `mod.Shape.unwrap_Circle(s)`, the same as a
  bare `Shape.Circle(...)` inside the declaring file itself.
- **No generics in the plain shape shown above.** `enum Shape { ... }`
  is a concrete, monomorphic declaration — a generic one (`enum
  Option<T> { Some(T) None }`, the actual prelude-provided `Option<T>`)
  needs real type parameters on top of this; see "Generics" above.

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
  resolved relative to the entry file's own directory first. If no such
  project file exists, resolution falls back to a bundled *core module*
  (`use collections` needs no local file — see below) before giving up;
  a project's own file of the same name always wins if one exists.
  Slash, not dot: a module path never shares a token with `.field`
  access, which already means something else. The name a module is
  referenced by (`user` above) is the path's last segment by default, or
  the explicit `as` alias if one is given — either way it's
  casing-enforced lowercase, like every other value-like name.
- **`use path/to/mod.Name [as alias]`** imports one specific
  function/struct/enum instead of the whole module, binding it to a bare
  local name — `use collections.HashMap` then just `HashMap<int, string>`,
  no `collections.` prefix. One name per import, no `{A, B, C}` list
  form. The bare name is `Name` itself, or the `as` alias if one is
  given; either way its casing must match `Name`'s own (both uppercase
  or both lowercase) — checked at parse time, before the loader has even
  resolved what `Name` is, since Ziman's parser already relies on
  casing alone to know a struct literal from a function call (see the
  grammar note above), so this is the same invariant every other
  binder enforces, not a new rule. A name imported this way is
  otherwise still resolved and privacy-checked exactly like a dotted
  reference would be (`'x' in module 'y' is private` on a non-`pub`
  target) — just once, at the `use` line, instead of at every call site.
  It's a compile-time error for the same local name to be imported
  twice, or for an import to collide with something the importing
  module declares itself.
- **Every cross-module reference is qualified, or explicitly imported by
  name at the top of the file — never a bare name that silently happens
  to resolve to some import.** `user.greet(u)`, `user.User { ... }`,
  `field: user.User` stay dotted for anything reached through a
  whole-module `use`. There is no wildcard import form (`use models/user.*`
  doesn't exist) — a per-name import always names the one thing it
  binds, right there in the `use` list, so grepping a file's `use` lines
  still tells you everywhere a bare name in its body could have come
  from. A qualified call or struct literal looks identical to plain
  field access (`p.x`) until the parser sees what follows the second
  identifier; see the grammar note above.
- **`pub` marks a declaration for export; the default is private to the
  module.** A private function/struct can be called/constructed freely
  from within its own module, but referencing it from another module is
  a compile-time error naming both the declaration and its module
  (`function 'x' in module 'y' is private`). A consequence: struct and
  function names only need to be unique *within* their own module, not
  across the whole program — two modules can each declare their own
  `Point`, even both private, without colliding.
- **`[pub] val NAME[: Type] = <expr>` at module top level declares a
  global value** — a module-wide constant, evaluated once before `main`
  runs (in file order, so a later one may reference an earlier one),
  never per-call the way a same-shaped local `val` inside a function
  body is. No global `var`: Ziman's concurrency model is single-writer
  actors passing messages over a `Channel<T>` (see "Concurrency" below),
  and a mutable global would be exactly the shared, lock-free mutable
  state that model exists to avoid — see docs/DESIGN.md's "Global val"
  entry for the fuller reasoning. A bare reference to one (same-module,
  or reached through a per-name import, `use mod.NAME`) works from any
  function in that module with no qualifier — but, unlike an ordinary
  local `val`, a global one can never be shadowed: reusing its name as a
  local `val`/`var`/parameter/loop-binder anywhere in the module is a
  compile-time error, not silent shadowing. There is deliberately no
  `mod.NAME` *dotted* bare access the way a qualified function call or
  struct literal has — the established, already-recommended way to reach
  one specific name from another module is still a per-name import.

  ```
  val max_retries = 3
  pub val default_timeout_seconds: float = 30.0

  fn should_retry(attempt: int) -> bool {
      return attempt < max_retries
  }
  ```
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
- **`native fn name[<TypeParams>](params) -> type`** declares a function
  with no body at all — its implementation is a C function registered
  under its exact fully-qualified name, looked up at call time. The type
  checker treats its signature like any other function's — including,
  like a plain `fn`, optionally taking type parameters (explicit type
  arguments required at every call, no inference, same as everywhere
  else generics appear) — and calling one with nothing registered for it
  is a runtime error ("no registered implementation"). A generic native
  fn's own C implementation never needs to know which concrete type a
  given call instantiated it with to do its job, since `Value` is
  already runtime-type-erased regardless of the language's static types
  (see "Generics" below).
  A minimal real module ships today — `io`, with `read_file(path:
  string) -> Result<string, string>` and `write_file(path: string,
  contents: string)` — a small, honest proof of the mechanism, not a
  general file API (no append mode, no directory listing yet).
  `read_file` reports a missing/unreadable file as a `Result` (see
  "Result" below) rather than crashing the whole program, since that's
  an expected, recoverable condition; `write_file` is still fatal on
  failure for now (disk full, permissions). `io.zm` is one of the
  bundled core modules below, so a project needs no local copy of it.
- **A bundled *core module* is ordinary `.zm` source embedded in the
  `ziman` binary at build time** (see docs/DESIGN.md's "Bundled core
  modules"), resolved by `use` when no project-relative file by that
  name exists. Eight so far: `collections` (`HashMap<K, V>`,
  `HashSet<T>` — see "Generics" above), `io`
  (`read_file`/`write_file`, above), `random` (`random_bytes(n: int) ->
  [byte]`, the one native function reading real OS entropy — no other
  source of non-determinism exists to build a PRNG on top of), `uuid`
  (`Uuid`, `uuid_new() -> Uuid`, `uuid_to_string(u: Uuid) -> string` —
  RFC 4122 v4, written in pure Ziman on top of `random`, version/variant
  bits set with plain integer arithmetic since Ziman has no bitwise
  operators), `clock` (`now_raw() -> [int]`, the one native function
  reading the system clock — `[seconds, nanos]` from a single
  `clock_gettime` call), `time` (`LocalDate`, `LocalTime`,
  `LocalDateTime`, `Duration`, `Instant`, `ZoneOffset`, `ZoneId` — see
  "Date and time" below), and `json` (a JSON parser/serializer,
  `parse_json(s: string) -> Result<Json, string>` /
  `json_to_string(j: Json) -> string` / `json_object_get(j: Json, key:
  string) -> Option<Json>` — `Json` is a genuinely recursive enum,
  `JArray([Json])`/`JObject([JField])`, which works today with no new
  language feature since the recursion goes through an array, not a
  bare self-reference), and `number` (shared formatting/parsing across
  `int`/`float`/`bigint`/`decimal`/`byte` — see "Number formatting and
  parsing" below). Any project can `use` one of these with no
  local file of its own. Each is still an ordinary, qualified module in
  every other respect — `use io`, always `io.read_file`, never a bare
  unqualified name the way the prelude's declarations are (though a
  per-name import, above, can still bind one bare on purpose). The
  prelude (`corelib/prelude.zm`) is a deliberate exception to "every
  module needs `use`" for exactly this reason: `Option<T>` (and its one
  helper, `option_unwrap_or`) lives there, not in `collections`, and a
  handful of native functions (`parse_int`, `concat`, `split`, ...) live
  there too — merged bare into every program the same way the entry
  file's own top-level names are, and, unlike the eight modules above,
  never reachable through `use` at all (see docs/DESIGN.md's "Prelude"
  entry). `Uuid`
  satisfies `Eq` (structural `==` on its `[byte]` field) but not `Hash`
  (hashing has no derived form for a struct), so it works as a struct
  field compared with `==` but not yet as a `HashMap` key. `Json`
  satisfies neither — array `==`/`!=` requires a *scalar* element type
  (see "Arrays" above), and `[Json]`'s element (`Json` itself) isn't
  one. See docs/CORELIB.md for a generated, signature-only listing of
  every `pub struct`/`pub fn` these eight modules export — a quick
  lookup, not
  a replacement for the explanations here.

## Date and time

The `time` core module (`use time.Name`) provides seven types, modeled
on `java.time` but scoped down in two deliberate ways: `ZoneOffset`
only, no IANA-style `ZoneId` with historical/future DST rules (a
different kind of problem than the rest of the module — see
docs/DESIGN.md's "time" entry), and every type is `{ ...: int }`-shaped
(plus `Instant`/`Duration`'s `bigint`-free `{ seconds: int, nanos: int
}`), never a single packed number.

Every type has a fixed ISO-8601-shaped `_to_string`/`_from_string` pair (no
extra arguments, and each is the other's exact inverse). The three
calendar-shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`) and
`Instant` also get a general `_format(value, fmt: DateTimeFormat) ->
string` / `_parse_pattern(s, fmt: DateTimeFormat) -> Result<Value,
string>` pair — the ISO pair is implemented *in terms of* these, via
one of five `pub val iso_*_format` presets (`iso_date_format`,
`iso_time_format`/`iso_time_nanos_format`, `iso_datetime_format`/
`iso_datetime_nanos_format`), rather than as a second, independently-
maintained implementation.

**`DateTimeFormat`** is a *compiled* representation of a pattern, not a
wrapper around the pattern string: `datetime_format_of(pattern:
string) -> DateTimeFormat` scans the pattern exactly once into an
array of parts (each either a known field or a run of literal
characters), so a `DateTimeFormat` is meant to be built once — the
five `iso_*_format` vals are each compiled exactly once, at program
start (they're global vals — see "Modules" above) — and then reused
for as many `_format`/`_parse_pattern` calls as needed, never
re-scanning the original pattern text. The pattern language itself is
deliberately small and fixed-width-only — nothing like
`java.time.DateTimeFormatter`'s "letter count changes meaning" (`M` vs
`MM` vs `MMM` vs `MMMM` are four different things there; here there's
exactly one recognized run length per letter):

```
yyyy       4-digit zero-padded year
MM         2-digit zero-padded month
dd         2-digit zero-padded day
HH         2-digit zero-padded hour (0-23)
mm         2-digit zero-padded minute
ss         2-digit zero-padded second
SSSSSSSSS  9-digit zero-padded nanosecond-of-second
```

Anything else in a pattern is a literal, copied through character by
character (consecutive literal characters are coalesced into one part
at compile time, not kept one part per character) — no escaping
mechanism, since none of the separators any pattern here needs (`"-"`,
`":"`, `"."`, `"T"`) collide with a token letter. A different run
length of one of these letters (e.g. three `S`s) matches no token and
is folded into the surrounding literal text too, rather than erroring
— a documented scope cut, not a guarded-against trap. There's no
month/day name support and no locale-awareness at all. `Duration`/
`ZoneOffset`/`ZoneId` only get the fixed ISO `_to_string`/`_from_string` pair
— their shapes (`"PT...S"`, `"Z"`/`"+HH:MM"`, a bare zone name) aren't
a calendar field breakdown, so the pattern language doesn't fit them.

```
use time.LocalDate
use time.localdate_of
use time.localdate_add_months
use time.localdate_to_string

fn main() {
    val d = unwrap(localdate_of(2024, 1, 31))
    print(localdate_to_string(localdate_add_months(d, 1))) // 2024-02-29
}
```

- **`LocalDate { year: int, month: int, day: int }`** — a calendar
  date, no time, no zone. `localdate_of(year, month, day) ->
  Result<LocalDate, string>` validates `month` (1–12) and `day` against
  that month's actual length (leap years handled correctly — the
  400/100/4 rule). `localdate_add_days`/`_months`/`_years` (the month/
  year forms clamp the day to the target month's length, same as
  `java.time.LocalDate.plusMonths` — `2024-01-31` plus one month is
  `2024-02-29`, never `2024-03-02`), `localdate_diff_days`,
  `localdate_compare` (negative/zero/positive), `localdate_to_epoch_day`/
  `localdate_from_epoch_day` (days since 1970-01-01, negative before
  it), `localdate_is_leap_year`, `localdate_to_string` (`"YYYY-MM-DD"`)
  and its exact inverse `localdate_from_string(s) -> Result<LocalDate,
  string>`, plus `localdate_format(d, fmt)`/`localdate_parse_pattern(s,
  fmt)` for a compiled `DateTimeFormat` (see above).
  The epoch-day conversion is Howard Hinnant's public-domain
  days-from-civil algorithm (used in libc++'s `<chrono>`), correct for
  the whole proleptic Gregorian calendar, translated directly into
  Ziman since it was already written against truncating (toward zero)
  integer division — exactly what Ziman's `/`/`%` already do.
- **`LocalTime { hour: int, minute: int, second: int, nano: int }`** —
  a time of day, no date, no zone. `localtime_of(...) ->
  Result<LocalTime, string>` validates each field's range (`nano` is
  0–999999999). `localtime_add_hours`/`_minutes`/`_seconds`/`_nanos`
  wrap at 24h and silently discard day overflow (same as
  `java.time.LocalTime.plusHours` — use `LocalDateTime`'s add functions
  when day rollover needs to be tracked), `localtime_diff_nanos`,
  `localtime_compare`, `localtime_to_string` (`"HH:MM:SS"`, or
  `"HH:MM:SS.nnnnnnnnn"` when `nano != 0`) and its inverse
  `localtime_from_string` (auto-detects which of those two shapes it's
  looking at), plus `localtime_format`/`localtime_parse_pattern` for a
  compiled `DateTimeFormat`.
- **`LocalDateTime { date: LocalDate, time: LocalTime }`** — the two
  combined, still no zone. `localdatetime_of(date, time)`,
  `localdatetime_add_hours`/`_minutes`/`_seconds`/`_nanos` (these DO
  carry into the date on overflow, unlike `LocalTime`'s own — adding an
  hour to `23:30` on New Year's Eve correctly lands on next year's
  `Jan 1`), `localdatetime_add_days`/`_months`/`_years` (delegate to
  `LocalDate`'s, leaving the time untouched), `localdatetime_compare`,
  `localdatetime_diff(a, b) -> Duration` (elapsed wall-clock time
  treating both sides as the same fixed calendar — well-defined without
  a zone, not a real cross-zone elapsed-time computation),
  `localdatetime_to_string` (`"YYYY-MM-DDTHH:MM:SS"`) and its inverse
  `localdatetime_from_string`, plus `localdatetime_format`/
  `localdatetime_parse_pattern` for a compiled `DateTimeFormat`.
- **`Duration { seconds: int, nanos: int }`** — a length of time, not a
  point. `nanos` is always kept in `[0, 999999999]`; `seconds` alone
  carries the overall sign — so `-0.5` seconds is `{seconds: -1, nanos:
  500000000}`, not `{seconds: 0, nanos: -500000000}`. Every
  constructor/arithmetic function maintains this invariant, which is
  also exactly why plain `seconds` comparison is a correct total
  ordering (not just a convenient shortcut) — `nanos` only ever breaks
  a tie. `duration_of_seconds(seconds, nanos)` normalizes an
  out-of-range or negative `nanos` rather than rejecting it (there's no
  invalid input here, unlike `LocalDate`), `duration_zero()`,
  `duration_add`/`_subtract`/`_negate`, `duration_compare`,
  `duration_to_string` (`"PT{seconds}[.{nanos}]S"` — ISO-8601-*shaped*,
  not full ISO-8601 duration support; a negative duration prints its
  true magnitude with a leading `-`, e.g. `{seconds: -1, nanos:
  500000000}` — which *is* `-0.5`s — prints `"PT-0.5...S"`, not the
  misleading `"PT-1.5...S"` a naive field-by-field print would give,
  the same care `java.time.Duration.toString()` takes), and its exact
  inverse `duration_from_string` (has to invert that same magnitude transform
  for a negative-with-fraction duration, not just negate the parsed
  seconds — verified against `duration_to_string`'s own worked example
  before trusting it).
- **`Instant { seconds: int, nanos: int }`** — a point on the UTC
  timeline. Same `{seconds, nanos}` shape and invariant as `Duration`,
  not a single `bigint` nanoseconds-since-epoch count — see
  docs/DESIGN.md's "time" entry for why `bigint` was tried first and
  dropped. `instant_now() -> Instant` reads the real system clock (the
  `clock` core module's one native function, `clock_gettime` under the
  hood — Ziman's only source of genuine non-determinism besides
  `random`), `instant_of(seconds, nanos)` (normalizing, like
  `Duration`), `instant_add`/`_subtract` (with a `Duration`),
  `instant_diff(a, b) -> Duration`, `instant_compare`,
  `instant_to_localdatetime(i, offset: ZoneOffset) -> LocalDateTime` /
  `localdatetime_to_instant(dt, offset) -> Instant` (the only place a
  `ZoneOffset` actually does something — shifting between a UTC instant
  and wall-clock time in some zone), `instant_to_string` (UTC ISO-8601,
  `"YYYY-MM-DDTHH:MM:SS[.nnnnnnnnn]Z"`, via `instant_to_localdatetime`
  with `zoneoffset_utc()`) and its inverse `instant_from_string` (accepts
  either `"Z"` or a real `"+HH:MM"`/`"-HH:MM"` offset suffix, not just
  the `"Z"` `instant_to_string` always produces — a string from a real
  external API may carry a genuine offset), plus `instant_format(i,
  offset, fmt)`/`instant_parse_pattern(s, fmt)` for a compiled
  `DateTimeFormat` applied to the calendar part, with the offset always
  handled separately (formatted via `zoneoffset_to_string` and
  appended; parsed via `zoneoffset_from_string` from whatever follows the
  format's own precomputed `width`).
- **`ZoneOffset { total_seconds: int }`** — a fixed UTC offset
  (`+05:30`), not a named zone with rules. `zoneoffset_utc()`,
  `zoneoffset_of_seconds(total_seconds) -> Result<ZoneOffset, string>`
  (validated to `±18:00`, matching `java.time.ZoneOffset`'s own limit),
  `zoneoffset_of_hours_minutes(hours, minutes) -> Result<ZoneOffset,
  string>` (rejects mismatched signs — `(-5, 30)` is invalid, must be
  `(-5, -30)` or `(-5, 0)` — the same footgun
  `java.time.ZoneOffset.ofHoursMinutes` itself guards against),
  `zoneoffset_compare`, `zoneoffset_to_string` (`"Z"` for UTC,
  `"+HH:MM"`/`"-HH:MM"` otherwise) and its exact inverse
  `zoneoffset_from_string`.
- **`ZoneId { name: string }`** — a named zone, of which `"UTC"` is the
  *only* value that exists. `zoneid_utc()` is the only constructor (no
  `zoneid_of(name: string)` — there's nothing else valid to construct,
  and inventing that validation surface ahead of a real second zone
  would be speculative), `zoneid_to_offset(z) -> ZoneOffset` (always
  returns UTC's offset — the only case that can ever occur),
  `zoneid_to_string`/`zoneid_from_string` (the latter just validates its
  argument is exactly `"UTC"` — a formality given there's only one
  legal value, included anyway so every type here has the same pair).
- **No `<`/`>` on any of these seven types.** Ziman has no Ord/Cmp
  bound, and this wasn't the forcing case to build one. Every type
  instead gets a `_compare(a, b) -> int` function (negative/zero/
  positive, the same convention as C's `strcmp`/`qsort` comparator).
  `==`/`!=` come free from structural equality on plain `int`/struct
  fields — no special-casing needed.
- **Calendar year is assumed non-negative throughout** (no BCE
  support) — undocumented territory nothing here needs.

## JSON

The `json` core module (`use json.Name`) is a parser and serializer,
written in pure Ziman — no native code needed, since this is plain
string processing with nothing OS-level involved.

```
use json.parse_json
use json.json_to_string
use json.Json
use json.json_object_get

fn main() {
    val j = unwrap(parse_json("{\"name\": \"Ada\", \"age\": 210}"))
    print(json_to_string(j))                 // {"name":"Ada","age":210}
    val name = json_object_get(j, "name")     // Option<Json>, not Result -- unwrap_Some, not unwrap
    print(Json.unwrap_JString(Option.unwrap_Some(name)))  // Ada
}
```

- **`Json`** is a genuinely recursive `enum` — `JNull`, `JBool(bool)`,
  `JNumber(float)`, `JString(string)`, `JArray([Json])`,
  `JObject([JField])` where `JField { key: string, value: Json }` —
  and this works today with *no new language feature*, despite an old
  ROADMAP.md note that "a self-referential struct can't be
  constructed." That limitation is about a *direct* self-reference with
  no escape (`struct Node { next: Node }`, genuinely infinite); `Json`'s
  recursion goes through `[Json]` (an array), which has a natural
  finite base case (`[]`) — confirmed by direct testing before this
  module was written, not assumed.
- **`JNumber` holds a `float`**, matching how virtually every other
  JSON implementation represents JSON numbers (including JavaScript's
  own `Number`) — JSON's grammar technically allows arbitrary
  precision, but nothing needs exact `decimal`/`bigint` precision for a
  JSON number specifically (an ID needing exact precision, like a
  `Uuid`, is always transmitted as a JSON *string* in practice).
- **`parse_json(s: string) -> Result<Json, string>`** — a real
  recursive-descent parser (not string-searching for field names, which
  can't handle a field's own string value containing the same text, or
  nested braces correctly). String escape handling covers `\" \\ \/ \n
  \t`; `\r \b \f` and `\uXXXX` unicode escapes are rejected with a
  clear parse error rather than silently mishandled — Ziman's own
  string literal syntax has no way to write a literal `\r`/`\b`/`\f`
  character (see "Lexical rules" above), and `\uXXXX` needs a
  codepoint-to-UTF-8 encoder nothing here has built.
- **`json_to_string(j: Json) -> string`** — compact serialization (no
  pretty-printing/indentation). Round-trips exactly through the escapes
  `parse_json` supports.
- **`json_object_get(j: Json, key: string) -> Option<Json>`** — the one
  convenience lookup beyond parse/serialize, since every schema-specific
  parser built on top of this module needs it at least once per field.
  Not a general query language — deliberately just this one helper.
- **No `==`/`!=` on `Json`.** Array equality requires a *scalar*
  element type (see "Arrays" above); `[Json]`'s element (`Json` itself,
  an enum) isn't one. A real, known limitation, left alone since nothing
  currently needs to compare two `Json` trees for equality.
- **This is the general parsing engine, not a schema-specific request
  parser.** Turning arbitrary JSON into one particular already-known
  struct (e.g. a `CreatePatientRequest`) is expected to be built on top
  of this module in application code — `parse_json`, then
  `json_object_get` each expected field, then validate/convert each one
  — not something this module does itself.

## Attributes and derive

`#[...]`, written directly above a `struct`/`enum` declaration, attaches
compile-time metadata to it. The only attribute anything currently
interprets is `derive`; other names parse (so the grammar doesn't need
to change later) but have no effect yet.

```
use json.Json
use json.JField
use json.json_to_string
use json.json_require_field
use json.json_object_get

#[derive(Json)]
pub struct Point {
    x: int
    label: Option<string>
}

fn main() {
    val p = Point { x: 42, label: Option<string>.Some("hi") }
    print(json_to_string(point_to_json(p)))          // {"x":42,"label":"hi"}
    val back = unwrap(point_from_json(point_to_json(p)))
    print(back.x)                                     // 42
}
```

- **`#[derive(Json)]`** generates `<snake_case_type_name>_to_json(v:
  Type) -> Json` and `<snake_case_type_name>_from_json(j: Json) ->
  Result<Type, string>` — always both directions, even if a given type
  only ever needs one. The generated name uses a real multi-word-aware
  `PascalCase` -> `snake_case` conversion (`ValidationError` ->
  `validation_error`), not just lowercasing.
- **Per-field conversion, in priority order**: (1) the field's type has
  its own `<type>_to_json`/`<type>_from_json` (hand-written, or
  generated by that type's own `#[derive(Json)]`); (2) the field's type
  has a `<type>_to_string`/`<type>_from_string` pair (every domain type
  in `time`/`uuid` does — each gets a thin `_to_json`/`_from_json`
  wrapper over its own `_to_string`/`_from_string`, so this is really
  tier (1) one level of indirection out); (3) a plain scalar
  (`int`/`float`/`bool`/`string`); (4) `Option<U>`/`[U]`, recursing into
  `U`. A field whose type is none of these (`bigint`, `decimal`, `byte`,
  `Result<T, E>`, `Channel<T>`, or a generic struct/enum) is a
  compile-time error, not a silently wrong answer.
- **Field names map `snake_case` -> `camelCase`** for the JSON key
  (`date_of_birth` -> `"dateOfBirth"`), matching Jackson's default
  naming. Fixed, not configurable yet.
- **`Option<U>` fields tolerate absence**: a missing key *or* an
  explicit JSON `null` both decode to `None`. Every other field is
  required — a missing key is a decode error.
- **Deserialization is lenient**: an unrecognized extra key in the input
  JSON object is silently ignored.
- **An enum derives an externally tagged shape**: `{"type":
  "<Variant>"}` for a unit variant, `{"type": "<Variant>", "value":
  ...}` for a payload one (the variant's payload follows the same
  per-field priority order above). `Option<T>` is never tagged this way
  — it keeps its own `null`/unwrapped handling from the per-field rules
  even when it shows up as an enum variant's payload type.
- **The consuming module must `use` whatever the generated code calls**
  — `Json`, `JField`, `json_require_field`, `json_object_get`, and each
  field type's own `_to_json`/`_from_json` (or `_to_string`/
  `_from_string`) — exactly as hand-written code doing the same job
  would need to. Nothing is auto-imported.

## Databases

Two core modules: `sql` (the shared vocabulary -- `SqlValue`, `Row`,
`DbError`, and the `sql_as_*` readers) and `db` (the driver: connections,
`execute`, `query`, transactions). SQLite (`sqlite::memory:` or
`sqlite:<path>`) and PostgreSQL (`postgres://user:password@host:port/database`)
are the two backends, chosen by URL scheme behind the same calls.

```
use db
use sql.SqlValue
use sql.row_get
use sql.sql_as_text

fn main() {
    val conn = unwrap(db.open("sqlite::memory:"))
    unwrap(db.execute(conn, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)", []))
    unwrap(db.execute(conn, "INSERT INTO t (id, name) VALUES ($1, $2)", [SqlValue.Int(1), SqlValue.Text("Ada")]))
    val rows = unwrap(db.query(conn, "SELECT name FROM t WHERE id = $1", [SqlValue.Int(1)]))
    print(unwrap(sql_as_text(unwrap(row_get(rows[0], "name")))))   // Ada
}
```

- **Bound parameters only.** SQL text carries `$1, $2, ...` placeholders
  (Postgres-style; the SQLite driver rewrites them to its own `?N`,
  leaving string literals alone) and values go in the `[SqlValue]`
  argument -- nothing is ever spliced into the SQL string.
- **`SqlValue`** is `Null | Int(int) | Float(float) | Text(string) |
  Bool(bool) | Bytes([byte])`. Nothing dialect-specific crosses the
  driver: `Uuid`, `LocalDate`, `Instant`, `decimal` and `bigint` all
  travel as `Text` and are converted by the mapping layer. A `bool` column
  reads back as an integer under SQLite; `sql_as_bool` accepts either.
- **`DbError`** is `NotFound | Constraint | Connection | Sql`.
  `Constraint` is a unique/foreign-key/not-null/check violation (SQLite
  connections turn foreign keys on, as Postgres always has them).
  `NotFound` is never raised by the driver -- a repository whose lookup
  came back empty returns it, so one error type covers the whole
  data-access layer. `db_error_message(e)` renders any of them.
- **Postgres** is written in Ziman (module `pg`, on `net`, `bytes` and
  `crypto`) and speaks protocol 3.0: trust, cleartext-password and
  **SCRAM-SHA-256** authentication (Postgres's default), the simple query
  protocol for parameterless statements (so `execute` takes a `;`-separated
  DDL script) and the extended protocol for `$1, $2, ...` parameters.
  Values travel in text format and are interpreted by the result column's
  type: integers, floats, booleans and bytea become the matching
  `SqlValue`, and everything else (text, uuid, dates, timestamps, numeric,
  json, ...) arrives as `Text` for the mapping layer -- so the same
  repository SQL, schema and `#[derive(Row)]` code run on both backends.
  A `Bool` parameter is sent as `1`/`0`, which Postgres accepts for both
  `BOOLEAN` and `INTEGER` columns. Percent-escapes work in the URL's
  user, password and database. One connection is owned by one thread that
  serialises requests, so sharing it across threads is safe statement by
  statement. **Not supported:** TLS (a `sslmode=require` URL is *refused*,
  not downgraded), MD5 authentication, Kerberos/GSS, IPv6 literals in the
  URL, COPY, LISTEN/NOTIFY, prepared-statement reuse, and SASLprep of a
  non-ASCII password.
- **`db.query`** returns `[Row]`; `db.query_one` returns `Option<Row>`;
  `db.execute` returns the row count of the last statement, and takes a
  `;`-separated script when there are no parameters (for DDL).
- **Transactions** are plain `db.begin`/`db.commit`/`db.rollback` on the
  connection. Every call takes the connection's own lock, so sharing one
  `Connection` across threads is safe *statement by statement*, but a
  transaction is not safe to share -- another thread's statements can
  interleave inside it. A `?` that returns early from inside a
  transaction leaves it open (no destructors, no closures, so no
  `with_transaction(fn)`). Accepted for now; see docs/DESIGN.md.
- **`#[derive(Row)]`** on a struct generates `<name>_from_row(row: Row)
  -> Result<Name, DbError>` and `<name>_to_params(v: Name) ->
  [SqlValue]` (field declaration order, the order a hand-written
  `INSERT ... VALUES ($1, $2, ...)` lists its columns in). Column names
  are the field names as written (already `snake_case`, no transform).
  `int`/`float`/`bool`/`string` map directly; a struct/enum field
  dispatches to `<name>_to_sql`/`<name>_from_sql` (provided for `Uuid` and
  every `time` type); `Option<U>` is NULL-or-value. An enum, a generic
  struct, an array field, or `bigint`/`decimal`/`byte` is a compile-time
  error. The consuming module must `use` what the generated code calls
  (`sql.SqlValue`, `sql.DbError`, `sql.Row`, `sql.row_get`, the
  `sql_as_*` readers, and each domain type's `_to_sql`/`_from_sql`).

## Networking

The `net` core module is blocking TCP; `bytes` is the `[byte]` helpers
that go with it.

```
use net
use bytes

fn serve(l: net.Listener) {
    val client = unwrap(net.accept(l))
    val msg = unwrap(net.read_text(client, 1024))
    unwrap(net.write_text(client, concat("echo:", msg)))
    net.close(client)
}

fn main() {
    val l = unwrap(net.listen("127.0.0.1", 0))        // port 0: the OS picks one
    spawn serve(l)
    val s = unwrap(net.connect("127.0.0.1", net.listener_port(l)))
    unwrap(net.write_text(s, "hi"))
    print(unwrap(net.read_text(s, 100)))               // echo:hi
    net.close(s)
    net.close_listener(l)
}
```

- **Blocking, thread-per-connection.** A blocked `accept`/`read` blocks
  only its own thread; `spawn` a handler per connection. There is no
  event loop and no async.
- **Handles.** `Socket` and `Listener` are `{ handle: int }` structs over
  a native table, like `db.Connection`. Closing is explicit (`net.close`,
  `net.close_listener`); there are no destructors, so a `?` that returns
  early between `connect` and `close` leaks the socket. Close a socket
  from the thread that owns it.
- **Reading.** `read(s, max)` blocks until at least one byte is there and
  returns what has arrived, up to `max`. `read_exact(s, n)` returns
  exactly `n` bytes or an error -- the shape a length-prefixed protocol
  wants. `read_text`/`write_text` are for text protocols; a string can't
  carry a 0 byte, so `read_text` reports one instead of truncating. `write`
  sends every byte before returning.
- **EOF is an error.** A peer closing cleanly makes a read return
  `Err(Closed)`, never an empty array, so it can't be silently
  forgotten; a protocol where end-of-stream is legitimate matches
  `NetError.is_Closed` explicitly.
- **`NetError`** is `ConnectionRefused | HostNotFound | Timeout | Closed
  | Io`, each carrying a message; `net_error_message(e)` renders one.
- **Timeouts.** `net.set_timeout(s, millis)` applies to that socket's
  reads and writes; a call that waits longer fails with `Timeout`.
  Without one a read on a dead peer waits forever. There is no connect
  timeout beyond the OS default.
- **`bytes`**: `string_to_bytes`/`bytes_to_string` (the latter a `Result`
  -- a string can't hold a 0 byte), `bytes_concat(parts...)`,
  `bytes_slice(b, start, end)`, and the big-endian `u16_to_bytes`/
  `u32_to_bytes`/`u16_from_bytes`/`u32_from_bytes` network protocols are
  built from.
- **Not yet:** TLS, Unix-domain sockets, UDP, a packed byte-buffer type
  (`[byte]` is an array of interpreter values -- fine for protocol
  messages, wasteful for bulk data).

## Cryptography

The `crypto` core module is just enough to speak SCRAM-SHA-256:
`sha256`, `hmac_sha256`, `pbkdf2_hmac_sha256(password, salt, iterations,
length)`, `base64_encode`/`base64_decode`, `bytes_xor` (Ziman has no
bitwise operators) and `bytes_to_hex`. All native and self-contained (no
libcrypto), checked against the published test vectors. **It is not a
general-purpose crypto library**: not constant-time, no randomness (use
`random.random_bytes`), no ciphers or public-key operations -- it grows
only when a real consumer needs more.

## Number formatting and parsing

The `number` core module (`use number.Name`) is shared formatting and
parsing across all five numeric types (`int`, `float`, `bigint`,
`decimal`, `byte`), written in pure Ziman on top of each type's own
`str()` — no native code needed.

```
use number.parse_pattern
use number.format_int
use number.format_float

fn main() {
    val fmt = parse_pattern("#,###.00")
    print(format_int(1234567, fmt))    // 1,234,567.00
    print(format_float(1234.5, fmt))   // 1,234.50
}
```

- **One engine, not five.** Ziman has no argument-based overloading, so
  a single `format(x: T, ...)` across five unrelated types isn't
  expressible directly. Instead, `format_number(s: string, fmt:
  NumberFormat) -> string` and `parse_number(s: string, fmt:
  NumberFormat) -> string` operate on the *string* each type already
  produces via `str()`/expects via `parse_int` and friends, never on the
  numeric types themselves. `format_int`/`format_float`/
  `format_bigint`/`format_decimal`/`format_byte` are thin one-line
  wrappers over `format_number` for ergonomics at call sites.
- **`NumberFormat { thousands_separator: string, decimal_separator:
  string, decimal_places: Option<int> }`** describes how to format a
  number — an empty `thousands_separator` disables grouping,
  `decimal_places` of `None` leaves however many decimal digits the
  value already has. `number_format_plain() -> NumberFormat` is the
  no-grouping, no-forced-places default (`.` as the decimal separator).
- **`parse_pattern(pattern: string) -> NumberFormat`** builds a
  `NumberFormat` from a `"#,###.00"`-style pattern — the familiar shape
  from Java's `DecimalFormat`/.NET custom format strings/Excel, but a
  deliberately small subset of what those support: a comma anywhere in
  the integer part just turns standard group-by-3 grouping on (its
  exact position doesn't matter), and the digit count after the `.`
  forces that many decimal places. No `0`-vs-`#` distinction (both mean
  "a forced decimal place" here, never "trim trailing zeros"), no `%`
  (percentage), no `E` (scientific notation), no `;`-separated
  positive/negative subpatterns, no literal prefix/suffix text or
  escaping, no custom (e.g. Indian-style) grouping sizes.
- **Forcing fewer decimal places than a value has truncates, never
  rounds** — rounding is `round`/`decimal_round`'s job, not a
  formatter's; a formatter that also rounded would mean the same value
  could display differently depending on what last touched it.
- **A value already in scientific notation is passed through
  unchanged**, not reformatted. `float`'s own `str()` switches to
  scientific notation past 6 significant digits (`str(1234567.891)` is
  `"1.23457e+06"`), which grouping/decimal-place-forcing has no
  sensible meaning for — `format_number` checks for `"e"`/`"E"` first
  and returns the string as-is rather than mangling it (a real bug
  found by testing: naively splitting `"1.23457e+06"` on `.` and
  truncating its `"23457e+06"` half to 2 places produced the nonsense
  `"1.23"`).
- **Hex conversion covers the three genuinely integral types** —
  `byte_to_hex(b: byte) -> string`/`byte_from_hex(s: string) ->
  Result<byte, string>`, `int_to_hex(x: int) -> string`/`int_from_hex(s:
  string) -> Result<int, string>`, `bigint_to_hex(n: bigint) ->
  string`/`bigint_from_hex(s: string) -> Result<bigint, string>` — not
  `float`/`decimal`, which have no natural hex representation for a
  non-whole value. `byte` is trivial (always exactly 2 unsigned hex
  digits, no sign). `int` is fixed-width (64 bits), so it uses the same
  convention as C's `printf("%x", ...)`/Java's `Long.toHexString`: two's
  complement, so `-1` is `"ffffffffffffffff"`, not a sign-prefixed
  `"-1"` — computed via `bigint` arithmetic (`(x as bigint) +
  2^64`, with `2^64` built from 64 repeated `bigint` multiplications)
  since a negative `int`'s unsigned 64-bit equivalent doesn't fit in a
  signed `int64`. `bigint` has no fixed width at all, so two's
  complement isn't even well-defined for it — the only sensible
  convention is sign-prefixed magnitude (`"-ff"`), matching Java's own
  `BigInteger.toString(16)` for the same reason.
- **`parse_byte(s: string) -> Result<byte, string>`** — `byte` had no
  parser until this module; delegates to `parse_int` and range-checks
  the result (0–255).

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
  ("cannot infer the type of 'ok(...)' here"), the same kind of
  restriction empty array literals (`[]`) already have and for the same
  reason: there's nothing in the call itself to infer the missing type
  from. (Empty array literals get inferred in a couple more places than
  `ok`/`err` currently do — struct literal fields and `with` overrides,
  see "Arrays" above — since those didn't need a second type parameter
  disambiguated the way `Result<T, E>` does.)
- **`unwrap`/`unwrap_err` are the explicit, opt-in escape hatch back to
  a fatal error.** `unwrap(r)` returns `r`'s success value, or raises a
  runtime error if `r` is an `Err` (mirror for `unwrap_err`). This is
  the same fatal behavior every runtime error already has — the point
  isn't to avoid ever crashing, it's that crashing on failure is now a
  *choice* the caller makes explicitly, instead of something a function
  like `io.read_file` used to force unconditionally.
- **`==`/`!=` on `Result<T, E>` compares variant then payload** — an `Ok`
  and an `Err` are never equal to each other regardless of payload; two
  values of the same variant are equal iff their payloads are (`T` for
  `Ok`, `E` for `Err`). Legal only when *both* `T` and `E` are
  comparable — recursively, using the exact same rule structs and
  arrays use — since a `Result`'s variant isn't known statically, so
  both branches have to type-check as comparable for the type itself to
  be. A struct with a `Result<T, E>` field is comparable exactly when
  that `Result` is (see "Structs" above).
- **`T`/`E` can be any Ziman type**, including another `Result`, an
  array, or a struct — this is what lets application code layer a
  richer error type (a struct with a `code` and a `message`, say) on top
  of a simpler one a native function returns (`io`'s is plain `string`),
  converting at the boundary rather than forcing one universal error
  shape everywhere.
- Printing a `Result` shows `Ok(...)` or `Err(...)` (e.g. `Ok(5)`,
  `Err(division by zero)`), recursively displaying the payload the same
  way `print` already displays anything else.

## Concurrency

`Channel<T>` and `spawn` — a third hardcoded parametric type (after
`[T]`/`Result<T, E>`) plus one new statement, giving Ziman real OS-thread
concurrency built on a single-writer / actor pattern: one thread owns a
piece of mutable state and is the only thing that ever touches it; every
other thread reaches it only by sending a message and waiting for a
reply.

```
enum PatientMessage {
    Create(string)
    GetById(int)
}

fn run_patient_actor(inbox: Channel<PatientMessage>, replies: Channel<string>) {
    var next_id = 1
    while true {
        val msg = receive(inbox)
        if PatientMessage.is_Create(msg) {
            val name = PatientMessage.unwrap_Create(msg)
            send(replies, format("created #{}: {}", next_id, name))
            next_id = next_id + 1
        } else {
            send(replies, "lookup not shown here")
        }
    }
}

fn main() {
    val inbox: Channel<PatientMessage> = channel_new()
    val replies: Channel<string> = channel_new()
    spawn run_patient_actor(inbox, replies)

    send(inbox, PatientMessage.Create("Ada"))
    print(receive(replies))   // created #1: Ada
}
```

- **`Channel<T>` is a queue, thread-safe by construction, holding values
  of one element type `T`** — unified, not split into separate
  sender/receiver types the way Rust's `mpsc` is: any `Channel<T>` value
  supports both `send`/`receive`, freely passed to whoever needs either
  capability. `send(chan, value)` never blocks (the queue is unbounded in
  v0 — no backpressure yet, tracked in ROADMAP.md); `receive(chan) -> T`
  blocks until a value is available.
- **`channel_new()` can't infer its own type** (the call itself, zero
  arguments, carries no information about what it's a channel *of*) —
  same "expected type from context" mechanism `ok(...)`/`err(...)` and
  an empty array literal (`[]`) already use, legal in exactly the same
  four positions: a `val`/`var` with an explicit `Channel<T>` annotation,
  a struct literal field, a `with` override, or a function's declared
  return type.
- **Not comparable.** `==`/`!=` on a `Channel` is a type error — unlike
  arrays/structs/enums, a channel is a stateful, identity-based handle,
  not a data aggregate, so "are two channels equal" has no obvious
  structural meaning.
- **`spawn name(args)` starts `name` running on a new OS thread**, with
  `args` evaluated on the *calling* thread first (ordinary call
  semantics) and handed off to the new one. `name` must already be a
  declared function (`spawn mod.name(args)` for one from an imported
  module, subject to the same `pub` rule as an ordinary qualified call) —
  never a first-class function value, since Ziman doesn't have those; see
  docs/DESIGN.md for why that gap doesn't block this. **The spawned
  function must return nothing (`void`).** There's no thread-handle/join
  mechanism in v0, so a spawned function's result has no way to be
  observed except through a `Channel` it's explicitly given — `spawn` is
  a statement, not an expression, and requiring `void` makes that
  discipline a compile-time rule instead of a convention.
- **Sharing a `Value` across threads needs no locking of its own.**
  Nothing in the interpreter ever frees an `ArrayObj`/`StructObj`/
  `EnumObj`/`ResultObj` (see docs/DESIGN.md), and nothing mutates one
  after construction — so a value that crosses a channel is just a
  pointer copy: no use-after-free (nothing's ever freed) and no data
  race (nothing mutates it after it's built). Only the channel's own
  queue needs real synchronization.

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
  letter; a `struct`/`enum`/enum-variant name must. Enforced, not just
  conventional — see "Structs" and "Enums" above.
- Every struct field type, enum variant payload type, and function
  parameter/return type must name a type that actually exists — an
  unresolved struct or enum name is caught at the declaration that names
  it, before any function body is checked.
- Struct and enum types are nominal, not structural — see "Types" above.
  A struct name and an enum name can't collide (a program can't declare
  both `struct Shape` and `enum Shape`), but two different enums may
  freely reuse the same variant name.
- `ok(...)`/`err(...)`/`channel_new()` are only legal where an expected
  `Result<T, E>`/`Channel<T>` type is available (a `val`/`var`
  annotation, a struct literal field, a `with` override, or the
  enclosing function's return type) — see "Result"/"Concurrency" above.
- `Result` and `Channel` are reserved names, like `int`/`bool`/etc. — you
  can't declare your own `struct Result { ... }` or `enum Channel { ...
  }`.

| Operator group        | Operators                  | Operand types           | Result  |
|------------------------|-----------------------------|--------------------------|---------|
| Arithmetic             | `+ - * / %`                 | `int,int` or `float,float`* | same type |
| Comparison             | `< > <= >=`                 | `int,int` or `float,float` | `bool`  |
| Equality               | `== !=`                     | any two of the same type, provided that type is "comparable": a scalar always, `[T]` only for scalar `T`, a struct only if every field is (recursively) comparable, `Result<T, E>` only if both `T` and `E` are (recursively) comparable, an `enum` only if every payload-carrying variant's payload type is (recursively) comparable, never a `Channel` | `bool`  |
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
- `str(x) -> string` — converts an `int`, `float`, `bool`, `bigint`,
  `decimal`, or `byte` to its display string. Rejects an argument that's
  already a `string` (there should be exactly one way to get a string
  from a value, not two).
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
- `len(arr) -> int` / `len(s: string) -> int` — the element count of any
  `[T]`, or the byte length of a `string`.
- `substring(s: string, start: int, end: int) -> string` — the substring
  `[start, end)`. `start`/`end` must satisfy `0 <= start <= end <=
  len(s)`, a runtime error otherwise.
- `split(s: string, sep: string) -> [string]` — splits `s` on every
  occurrence of `sep`. `sep` must not be `""` (runtime error) — an empty
  separator has no well-defined split points. Empty pieces (adjacent
  separators, or a leading/trailing separator) are kept, so `len(result)`
  is always `count(sep in s) + 1`.
- `trim(s: string) -> string` — `s` with leading/trailing whitespace
  removed.
- `to_upper(s: string) -> string` / `to_lower(s: string) -> string` —
  ASCII case conversion.
- `parse_int(s: string) -> Result<int, string>` / `parse_float(s: string)
  -> Result<float, string>` / `parse_bigint(s: string) -> Result<bigint,
  string>` / `parse_decimal(s: string) -> Result<decimal, string>` —
  parses all of `s` as a number; `Err` (with a message) if `s` has
  leading/trailing junk or doesn't parse at all.
- `contains(s: string, needle: string) -> bool` / `contains(arr: [T], x:
  T) -> bool` — substring test, or array membership test via `==`. The
  array form requires `T` to be a scalar
  (`int`/`float`/`bool`/`string`/`bigint`/`decimal`/`byte`) — same
  restriction as array `==`/`!=`, since only scalars have a well-defined
  equality in v0.
- `hash(x: T) -> int` — a hash of `x`; `T` is `int`, `float`, `bool`,
  `string`, `bigint`, `decimal`, `byte`, or a type parameter bounded by
  `Hash` (see "Generics" above). Not cryptographic and not stable across
  a future language version, so never persist a hash — only use it
  within a single program run (e.g. `collections.HashMap<K, V>`'s bucket
  index).
- `abs(x: T) -> T` — absolute value; `T` is `int`, `float`, `bigint`, or
  `decimal`. Not `byte` — see "Numbers" above for why.
- `min(a: T, b: T) -> T` / `max(a: T, b: T) -> T` — `T` is `int`,
  `float`, `bigint`, or `decimal`; both arguments must be the same `T`,
  no implicit mixing between any of the four (consistent with the
  language's general no-implicit-coercion rule). Not `byte` — see
  "Numbers" above.
- `floor(x: float) -> int` / `ceil(x: float) -> int` / `round(x: float)
  -> int` — round towards negative infinity / positive infinity /
  nearest (ties away from zero). All three take only `float` (an `int`
  is already rounded) and return `int`, since that's the type the result
  actually is. `decimal_round(d: decimal, scale: int) -> decimal` is
  `decimal`'s equivalent — see "Numbers" above for why it's a separate
  name rather than an overload of `round`.
- `push(arr: [T], x: T) -> [T]` — a new array with `x` appended.
- `insert(arr: [T], i: int, x: T) -> [T]` — a new array with `x` inserted
  at index `i`; `i` must be in `[0, len(arr)]`, a runtime error otherwise.
- `set(arr: [T], i: int, x: T) -> [T]` — a new array with the element at
  `i` replaced by `x`; `i` must be in `[0, len(arr))`, a runtime error
  otherwise.
- `remove(arr: [T], i: int) -> [T]` — a new array with the element at `i`
  removed; `i` must be in `[0, len(arr))`, a runtime error otherwise.
  There's no "pop" — indexing already covers peeking at an element, so a
  separate function to do both at once would just be two ways to do the
  same thing.
- `ok(x: T) -> Result<T, E>` / `err(e: E) -> Result<T, E>` — construct a
  `Result`. Only legal where the missing type parameter is available
  from context — see "Result" above.
- `is_ok(r: Result<T, E>) -> bool`
- `unwrap(r: Result<T, E>) -> T` — the success value, or a runtime error
  if `r` is `Err`.
- `unwrap_err(r: Result<T, E>) -> E` — the error value, or a runtime
  error if `r` is `Ok`.
- `channel_new() -> Channel<T>` — construct a `Channel`. Only legal
  where the missing type parameter is available from context — see
  "Concurrency" above.
- `send(c: Channel<T>, value: T)` — enqueue `value`; never blocks (the
  queue is unbounded in v0).
- `receive(c: Channel<T>) -> T` — dequeue the next value, blocking until
  one is available.

None of `push`/`insert`/`set` mutate their `arr` argument — see "Arrays"
above for why, and why that's true regardless of `val`/`var`.

`+` itself stays numeric-only in v0 — see docs/DESIGN.md principle #2 on
why string concatenation didn't just overload it.
