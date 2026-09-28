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
- String literals: `"..."` with escapes `\n`, `\t`, `\r`, `\"`, `\\`. No
  interpolation in v0. A plain string cannot span lines.
- Text blocks: `"""` at the end of a line opens a multi-line string, and the next `"""` closes it.

  ```
  val query = """
      SELECT id, name
        FROM person
       WHERE name = "Ada"
      """
  ```

  The newline after the opening `"""` and the one before the closing `"""` are not part of the
  value, so `query` above is the three lines `SELECT id, name`, `  FROM person`,
  `WHERE name = "Ada"` with no trailing newline (add a blank line before the closing `"""` for
  one). The indentation shared by every non-blank line, and by the closing `"""` when it sits on a
  line of its own, is removed, so the block can be indented with the code around it; a smaller
  indent on the closing line strips less. Blank lines stay empty and trailing blanks on each
  line are dropped. The escapes are the same as in a plain string (`\"""` puts three quotes in
  the text); a `"` or `""` needs none. `ziman --fmt` leaves a text block exactly as written.
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
                       function value or closure. See "Concurrency". *)
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
identifier, never an arbitrary expression (a function *value* is called
through a local name -- see "Function values"; `make()(x)` isn't valid), no indexed/field
*assignment* (`arr[i] = x` and `p.x = 1` aren't valid — see "Arrays" and
"Structs" below), and no expression-level `if`.

## Types

- `int` — 64-bit signed integer. **It never wraps: `+`, `-`, `*`, unary
  `-`, `/` (`MIN / -1`) and `abs(MIN)` that overflow are a runtime error
  ("integer overflow in '+'"), consistent with `bigint as int` and `int as
  byte` clamping instead of wrapping silently. Use `bigint` when a value
  can outgrow 64 bits.** An integer literal must fit: `9223372036854775807` is the
  largest and `-9223372036854775808` the smallest (a literal that overflows is
  a compile error naming the `n` suffix for a bigint, not a silently
  saturated value). `float as int` truncates toward zero and saturates to
  `INT64_MAX`/`INT64_MIN` (NaN is 0), the same clamp-don't-wrap rule as
  `bigint as int`.
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
- `unit` — the type with exactly one value, also written `unit`. It is
  what "nothing to give back" looks like when a value is required: the
  success type of a `Result` that only says it worked
  (`Result<unit, DbError>`), a generic argument (`with_transaction<unit>`),
  a function declared `-> unit`. In a function that returns `unit` or
  `Result<unit, E>`, a bare `return` is `return unit` (and, being a bare
  value, `return unit` in a `Result<unit, E>` function is `Ok(unit)`, like
  any other auto-wrap); a `-> unit` function that runs off its end returns
  `unit`. A function declared with *no* return type still has no value,
  so `val x = f()` on one is an error: write `-> unit` when the caller
  may want one. `unit` is `==`-comparable, hashable and prints as `unit`;
  it is a word, not a reserved keyword: it is the type wherever a type is written, and the value wherever no variable, parameter or
  field of that name is in scope, so a JSON field called `unit` is fine.
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
  precision before the cast ever runs); it converts the float's exact
  value, so a float too large for `int` becomes the exact (huge) bigint,
  and NaN or infinity is a runtime error (`float as decimal` the same).
- **Composes with everything else already built**: a `bigint` field in a
  struct, a `bigint` array element, a generic type argument
  (`Box<bigint>`), and a `Map<bigint, V>` key (it satisfies both
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
    print(math.decimal_round(1.2345d, 2))  // 1.23 (needs `use math`)
}
```

- **A literal needs the `d` suffix, always** — `19.99d`, `20d` — never
  bare `19.99`/`20`, which always mean `float`/`int`.
- **All of `float`'s operators/builtins work**: `+ - * / %`, unary `-`,
  `< > <= >= == !=`, `str`, `abs`, `min`/`max`, `hash`, and
  `parse_decimal(s: string) -> Result<decimal, string>`.
  `math.decimal_round(d: decimal, scale: int) -> decimal` (in the `math` module, so `use math`)
  is `decimal`'s equivalent of `math.floor`/`ceil`/`round` — a separate name, not an
  overload of `round`, since Ziman has no argument-based overloading
  anywhere (every name maps to exactly one signature).
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
  (`Box<decimal>`), and a `Map<K, decimal>` value all work.

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
  `Map<byte, V>` key (it satisfies both `Eq` and `Hash`) all work.

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
- **`==`/`!=` on arrays is allowed whenever the element type itself
  supports `==`** (a scalar, or a struct/enum/Result/another array made of
  comparable parts, recursively — see `contains` below and "Equality" in
  the operators table) — compares length then elements pairwise. An array
  of `Channel`s, or of anything else that isn't itself comparable, is
  still a type error.

See "Builtins" below for `len`/`push`/`insert`/`set`/`remove`'s exact
signatures.

## Maps and sets

`Map<K, V>` and `Set<T>` are built-in types. Keys must support `==` and hashing (the
`Hash` rule: scalars, and an array/struct/enum/Result made of hashable parts, however deeply, including one that
refers back to itself (`Json`, or a recursive `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`); a type
parameter bounded by `Hash`; a value may be anything.

```
var stock = Map<string, int> { "apple": 4, "pear": 6 }    // the type arguments are always written
var seen = Set<int> {}                                    // empty
stock = set(stock, "fig", 9)             // add or replace; returns a new map
stock = remove(stock, "pear")            // no-op if absent
print(len(stock))                        // 2
print(contains(stock, "apple"))          // true (a set: contains(seen, 3))
print(map_get(stock, "apple"))           // Some(4); None if absent -- always an Option<V>
seen = insert(seen, 3)                   // a set adds with insert, not set
print(map_keys(stock))                   // {apple, fig}, a Set<string>
print(map_values(stock))                 // [4, 9]; set_items(s) is the array of a set
for name, qty in stock { ... }           // key and value; `for name in stock` binds keys only
for x in seen { ... }                    // a set has one binder
```

- **Helpers** (all return new collections, all infer their types from the arguments):
  - Maps: `map_get_or(m, k, default)`, `map_merge(a, b)` (`b` wins on shared keys),
    `map_filter(m, keep)` with `keep: fn(K, V) -> bool`, `map_remove_all(m, keys)` (an array),
    `map_from_arrays(keys, values)` (equal lengths, else a runtime error; a repeated key keeps
    the last value).
  - Sets: `set_union(a, b)`, `set_intersect(a, b)`, `set_difference(a, b)` (all in `a`'s order,
    `b`'s extras appended for union), `set_is_subset(a, b)`, `set_from_array(items)`,
    `set_insert_all(s, items)`, `set_filter(s, keep)` with `keep: fn(T) -> bool`.
  - Written with their type arguments (the result type of a lambda is not inferred, as in
    `collections.map<T, U>`): `map_map_values<K, V, R>(m, f)` applies `f: fn(V) -> R` to every value,
    keys and order unchanged; `set_map<T, U>(s, f)` is the set of `f(x)`, elements that map to the same
    value merging; `map_entries<K, V>(m)` is an array of `MapEntry<K, V> { key, value }` in the map's
    order, for sorting or slicing entries (`for k, v in m` needs no array to just walk one).
  - In `collections`: `frequencies<T>(items)` counts occurrences into a `Map<T, int>`, and
    `index_by<T, K>(items, key)` builds a `Map<K, T>` (the later element wins a shared key; `group_by` keeps
    them all).
- **Builders** for filling one up entry by entry: `MapBuilder<K, V>` (`mapb_new`, `mapb_set`,
  `mapb_get`, `mapb_remove`, `mapb_contains`, `mapb_len`, `mapb_to_map`) and `SetBuilder<T>`
  (`setb_new`, `setb_add`, `setb_remove`, `setb_contains`, `setb_len`, `setb_to_set`), the
  counterparts of `ArrayBuilder`. A builder is mutable and changes its own private trie in
  place, so building an n-entry map allocates O(n) once instead of copying a few nodes per
  entry; use one in a loop that adds many entries (a parser, `group_by`, `distinct`). The type
  arguments are written out (`mapb_set<string, int>(mb, "a", 1)`), as for any generic function.
  `mapb_to_map` / `setb_to_set` snapshot the builder into an ordinary immutable Map/Set (an O(n)
  copy); the builder stays usable and later changes do not touch the snapshot. One thread at a
  time, like the other builders.
- **Values, like arrays.** `set`/`insert`/`remove` return a new collection and leave
  their argument untouched. Internally a persistent hash trie, so an update copies only
  a few nodes: building an n-entry map is O(n log n), not O(n^2).
- **Insertion order.** `map_keys`, `map_values`, `set_items`, `for` and `print` all go
  in the order keys were first inserted; overwriting a key keeps its place, removing and
  re-adding sends it to the end.
- **`for` walks a snapshot**, so rebinding the collection inside the body neither
  changes what the loop visits nor is an error. `break`/`continue` work as usual.
- **Equality and hashing ignore order.** `==` on two maps or sets is true when they hold the
  same entries (a map's values must themselves support `==`), and equal ones hash equal, so
  a map or set can be a struct field, a map key or a set element.
- **`#[derive(Json)]`** writes a `Map<string, V>` or `Map<int, V>` as a JSON object (an int
  key becomes its decimal text) and a `Set<T>` as an array (duplicates collapse on decoding).
  Other key types are a derive error. `Row` and `Properties` do not support them.
- **Names.** `Map` and `Set` are reserved words (a user `struct Map` is a syntax error).
  `map_get`, `map_keys` (a `Set<K>`), `map_values` (a `[V]`) and `set_items` (a `[T]`) are builtins that take the
  collection's type from their argument, so no type arguments are written on them.

## For loops

Three forms, chosen by binder count and whether the right-hand side of
`in` is a range or an array:

```
for x in arr { ... }          // value only -- x: T, once per element
for i in 0..len(arr) { ... }  // index only -- i: int, counting a range
for i, x in arr { ... }       // index + value together
```

- **`for x in iter`** also walks an `Iter<T>`, a lazy source that produces one item
  at a time, such as `fs.lines(path)`. It has one binder (no index). Nothing is read ahead, so
  the source can be far larger than memory, and `break`, `return` or a `try` inside the loop
  closes it; one that is never finished is closed when it is garbage collected. Take the items by
  hand with `iter_next(it)` (an `Option`) and release early with `iter_close(it)`. An `Iter` is
  walked once: a second `for` over it carries on where the first stopped. `iter_of_array(items)`
  makes an `Iter` from an array, and `iter_from<T>(next, close)` makes one from your own code:
  `next` returns `Some(item)` or `None`, `close` runs once when the source is finished, left
  early or closed. A closure cannot change a variable it captured, so keep the position in what
  the closures call (a native handle, a Channel). Unlike the built-in sources it is not closed
  by the garbage collector (that would run your code from a finalizer), so finish or close it.

  ```
  val lines = fs.lines("app.log")?            // Iter<Result<string, string>>
  var errors = 0
  for line in lines {
      val text = line?                        // a read error is an Err item
      if contains(text, "ERROR") {
          errors = errors + 1
      }
  }
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
- **`for i in a { ... a[i] ... }` is a compile error.** In this form `i`
  is the *element*, not its position -- unlike Go's `for i := range a`,
  JavaScript's `for (i in a)` or Python's `range(len(a))` habit -- so
  indexing the same array with it silently computes the wrong thing. The
  compiler rejects that exact shape (a single-binder loop over an
  identifier, indexing that identifier with the binder) and names both
  fixes: `for i in 0..len(a)` for positions, `for i, item in a` for both.
  A range loop or an inner loop that reuses the name shadows the element
  binder, and indexing a *different* array is fine. Deliberately narrow:
  it doesn't catch `for i in a { print(i) }` when a position was meant,
  and it does reject the rare legitimate case of an array whose elements
  are positions into itself (`for x in next { ... next[x] ... }`) -- write
  that with an index loop.
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

## Function values

A function can be passed around: its type is `fn(A, B) -> R` (no `-> R` for a
function that returns nothing). Two things are values of that type: the name of
a top-level function, and a *lambda*, an anonymous function written in place.

```
fn double(n: int) -> int { return n * 2 }
fn is_even(n: int) -> bool { return n % 2 == 0 }

fn apply(f: fn(int) -> int, x: int) -> int {
    return f(x)                       // a call through a local of function type
}

fn main() {
    print(apply(double, 21))          // 42
    val keep = is_even                // a val of type fn(int) -> bool
    val evens = collections.filter<int>([1, 2, 3, 4], keep)
}
```

- **A lambda is `fn(params) -> R { body }`**, with the parameter and return
  types written out (no `-> R` means it returns nothing):
  ```
  val base = 10
  val add = fn(x: int) -> int { return x + base }
  val doubled = collections.map<int, int>(numbers, fn(n: int) -> int { return n * 2 })
  ```
  It is an expression, usable anywhere one is. `return`, `try` and `defer` inside
  it belong to the lambda, and `break`/`continue` cannot leave it.
- **Shorter forms.** The final bare expression of a lambda with a known
  non-void return type is its result, so `return` is only for early exits:
  `fn(a: int, b: int) -> int { a + b }`. Where the context says what function type
  is wanted -- an argument (`collections.map<int, int>(xs, fn(n) { n * 2 })`), a
  `val`/`var` with a declared type, an assignment, a `return`, a struct field --
  parameter types and the `->` can be left out and are taken from it. With no such
  context, parameter types must be written, and an omitted `->` means "the type of
  the final expression" (a final call that returns nothing, or no final
  expression, makes the lambda return nothing). A lambda that uses `return` needs
  its `-> T` unless the context gives it. A wrong-arity lambda, or a body of the
  wrong type, is a compile error naming the function type expected.
- **What a lambda captures, and how.** Any *immutable* local of the enclosing
  function that its body uses -- a `val`, a parameter, a loop or `switch`
  binder, another function value -- is captured **by value**, copied when the
  lambda is evaluated. A lambda cannot capture a `var` (compile error: copy it
  into a `val` first), so a lambda and its enclosing function never share
  mutable state and a function value is still safe to send to another thread.
  Global `val`s are not captured, just used. Lambdas nest, and capture through
  every level. A name declared inside a lambda (a parameter, a `val`) is its own
  and takes precedence over an outer variable of the same name; in a module
  (not the entry file) it may not share a global `val`'s name, the same rule as
  everywhere else.
- **A reference to a named top-level function** is the other kind of value:
  `apply(double, 21)`. It carries nothing.
- **What can be a value:** any top-level function (own module, `use mod.name`,
  or `mod.name`; a `native fn` too) that has no variadic parameter. A **generic**
  function is a value once its type arguments are written out, which fixes its
  types: `identity<int>` is an `fn(int) -> int`, `collections.sort<string>` an
  `fn([string]) -> [string]`, checked against the function's bounds like a call
  (`collections.sort<bool>` is an error). Without type arguments it is an error
  saying so. A *native* generic function (`slice`, `ab_new`, ...) can't be a
  value -- wrap it in a lambda. The builtins (`len`, `print`, ...) aren't
  functions, so they can't either.
- **Calling:** `f(x)` where `f` is a parameter or a `val`/`var` of function
  type. Arguments are checked against the parameter types, and the same
  bare-value auto-wrap into `Option`/`Result` applies. Named arguments and
  omitted `Option` parameters don't apply through a value. A **local always wins
  over a top-level function of the same name**. You can only call a *name*:
  `make()(x)` and `s.handler(x)` are not valid -- assign to a `val` first.
- **Where they can go:** parameters, returns, `val`s, struct fields, arrays,
  `Option`/`Result`, and channels (`Channel<fn(int) -> int>`), so a function
  value can be sent to another thread. `spawn f(x)` still names the function
  statically.
- **What they can't do:** be compared (`==`), hashed, ordered or serialised --
  a derive (`Json`, `Row`, ...) on a struct with a function field is an error.
  `print` shows `<function name>`.
- **In the standard library:** `collections.map`, `filter`, `flat_map`, `fold`, `count`, `any`,
  `all`, `find`, `first`, `last`, `min_by`, `max_by`, `distinct`, `group_by`, `for_each` and `sort_with`, with the type arguments written out at the
  call (`collections.map<int, string>(numbers, label)`). Pass a lambda when the
  function needs something from the enclosing scope --
  `collections.find<Route>(table, fn(r: Route) -> bool { return matches(req, r) })`
  is how the sample backend's route table finds the handler for a request.

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
  struct nested inside another struct, an array field, or a field
  containing a `Result<T, E>` or an `enum`, is fine as long as that
  field's own type is itself comparable this same way (see "Arrays"
  above); a field of anything that isn't comparable at all (a `Channel`,
  for instance) makes the whole struct a type error to compare, naming
  the struct. A struct that refers back to its own type, directly or
  through an array (`enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`), is
  assumed comparable rather than checked forever — see docs/DESIGN.md's
  "Recursive Eq/Hash" entry.
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
  a deliberate, narrow exception to "no implicit coercion" (closer to a
  C++ converting constructor than to anything in Rust, which makes you
  write `Some(x)` or `.into()`): wrapping a single, statically-known type
  is not lossy, unlike numeric coercion. The one place it *would* be
  ambiguous is `Option<Option<T>>`, where a bare `Option<T>` would turn a
  `None` into `Some(None)` -- that is a compile error; write
  `Option<Option<T>>.Some(...)` explicitly.
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
- **When `T == E`, a bare value is a compile error** — the sugar can't
  tell which one was meant in the degenerate same-type case (`return "bad
  input"` from a `Result<string, string>` function used to become `Ok`
  silently), so it refuses to guess: write `ok(value)` or `err(value)`
  (as a `return`, a typed `val`, a struct field or a `with` override; a
  bare `ok(...)`/`err(...)` still isn't a legal call argument, so assign
  it to a typed `val` first).
- **`Result<T, E>` parameters can never be omitted, in any call** —
  unlike `Option<T>`, there's no natural "absent" state an outcome type
  could fall back to, so this sugar is only ever about *what a supplied
  value looks like*, never about leaving one out. A `Result<T, E>`
  parameter also can't have a declared default (`= expr`) for the same
  reason — defaults exist to fill in for an omission, and this
  parameter is never omittable to begin with.

### `try` — Option\<T\>/Result\<T, E\> unwrap-or-propagate

```
fn parse_two_numbers(a: string, b: string) -> Result<int, string> {
    val x = try parse_int(a)
    val y = try parse_int(b)
    return x + y
}

fn first_positive_doubled(items: [int]) -> Option<int> {
    val idx = try first_positive_index(items)
    return items[idx] * 2
}

fn plus_one(s: string) -> Result<int, string> {
    return ok(try parse_int(s) + 1)
}
```

- **`try expr`, where `expr: Result<T, E>` or `expr: Option<T>`, unwraps the
  success value and keeps going, or immediately returns the failure from the
  *enclosing function*** — no boilerplate `if is_err(r) { return err_of(r) }`.
  `Ok(v)`/`Some(v)` → the whole `try expr` evaluates to `v`; `Err(e)` →
  immediately `return e` (auto-wrapped to `Err(e)` by the enclosing function's
  own bare-value auto-wrap); `None` → immediately returns a fresh `None`, of the
  *enclosing function's* own `Option<T2>` type. There is no postfix `?`: the
  early exit is written in front, where it is hard to miss when reading.
- **`try` covers the call or access chain after it**, as `-` and `!` do:
  `ok(try parse_int(s) + 1)` is `ok((try parse_int(s)) + 1)`, and
  `try parse(str(try parse(s))) * 2` nests.
- **It may sit anywhere an expression that runs exactly once, unconditionally,
  can:** a `val`/`var` initializer, an assignment, a `return` value, an `if`
  condition, a `switch` subject, a `for` range or iterable, an operand or an
  argument. The interpreter only knows the statement-level form (the whole
  initializer of a `val`/`var`, or a whole statement); the parser rewrites any
  other `try` into `val $t1 = try expr` statements ahead of the statement, and
  also moves whatever the statement evaluates *before* the `try` (a call, an
  index that may fail) into temporaries, so effects happen in the order
  written: `noisy("a") + try f() + noisy("b")` runs `noisy("a")`, then `f()`, and
  returns before `noisy("b")` if `f()` fails.
- **Where `try` is refused:** an `else if` or `while` condition (evaluated
  conditionally or repeatedly), the right side of `&&` and `||`, a `defer` or
  `spawn` argument, and an `if`/`switch` used as a value (its arms may not
  leave). Put it in its own `val` first.
- **`Result<T, E>`'s `try` requires the enclosing function to return
  `Result<T2, E>` with the *same* `E`, unless the function's error type declares
  a conversion.** Propagating `Err(e)` means handing the caller a real value, so
  the error type has to match. An enum may mark one variant `#[from]` (it must
  have a payload): a `try` on a `Result<_, P>`, in a function returning
  `Result<_, Enum>` where a `#[from]` variant of `Enum` has payload type `P`,
  wraps the error in that variant:

  ```
  enum ServiceError {
      #[from] Storage(DbError)
      #[from] Validation(ValidationError)
      Internal(string)
  }

  fn get(ctx: Ctx, id: Uuid) -> Result<Response, ServiceError> {
      val row = try repository.find_by_id(ctx.conn, id)   // Err(db) returns Err(Storage(db))
      return mapper.to_response(row)
  }
  ```

  At most one variant of an enum may be `#[from]` for a given payload type, so the
  conversion never depends on the order of the variants; `#[from]` is not
  supported on a generic enum. The conversion applies only to `try`, and only one
  step (the result is not converted again further out). `Option<T>`'s `try` only
  requires the enclosing function to return `Option<T2>` for *any* `T2` — `None`
  carries nothing to propagate.
- **`Some(x)` and `None` may be written bare** (not `Option<T>.Some(x)`/
  `Option<T>.None`) wherever an `Option<T>` is already expected — the same
  four positions `ok`/`err`/`channel_new` get: a `val`/`var`'s declared
  type, a struct literal field, a `with` override, or a function's
  declared return type. `return None` and `notes: Some(text)` work without
  naming `Option<string>`. Outside those positions (a bare `print(Some(5))`,
  a function-call argument, or anywhere else with no expected type already
  in hand) the fully qualified `Option<T>.Some(x)`/`Option<T>.None` is
  still required, with the same reasoning `ok`/`err` have: there is
  nothing here to infer the type from. This is separate from — and,
  unlike — the existing bare-value auto-wrap (`Option<T>` accepts a bare
  `T`, wrapped in `Some` automatically): auto-wrap never sees something
  already shaped like `Some(...)`/`None`, which is exactly what this
  covers instead.
- **A bare `try expr` statement (no `val`/`var`)** still fully checks and
  propagates, discarding the unwrapped value on success — useful for a fallible
  step whose success value isn't needed, only whether it succeeded.

### Must-use `Result`

```
fn risky(n: int) -> Result<int, string> {
    if n < 0 { return err("negative") }
    return ok(n)
}

fn main() {
    risky(-1)          // type error: this Result<int, string> is discarded
    val _ = risky(-1)  // fine: discarded on purpose
    try risky(-1)       // fine: propagates a real Err
}
```

- **A bare expression statement whose type is `Result<T, E>` is a
  compile-time error.** Nothing else marks the difference between "this
  call's outcome doesn't matter" and "the `Err` case was forgotten," so a
  real error — a failed write, a bad row, a closed connection — used to
  vanish exactly like a success would. `defer` is the one exception (see
  its own entry above): a deferred call has nowhere left to bind or
  propagate to by the time it runs, and this was already deliberate there.
- **`val _ = expr` says the discard is on purpose.** `_` is not an
  ordinary variable name: it is never actually bound, so it never
  conflicts with an earlier `_` in the same scope (`val _ = a(); val _ =
  b()` is fine, unlike any other name), and reading `_` back afterward is
  the same "use of undefined variable" any other never-declared name
  gets. This is the one place in the language `_` is special — a `val`,
  `var`, function parameter, loop binder, or `switch`/`if` payload binder
  named `_` all behave the same way, so `for _ in 0..n { ... }` and `if
  Some(_) = o { ... }` (already legal before this) work for the same
  reason a discarded `Result` now does.
- **`try expr`** also satisfies this, propagating instead of discarding.

### `if Variant(name) = expr` — test and bind in one step

```
fn main() {
    val a: Option<int> = Option<int>.Some(4)
    val b: Option<int> = Option<int>.None

    if Some(x) = a {
        print(x)                        // 4
    }
    if Some(x) = b {
        print("never")
    } else {
        print("b is empty")
    }
    // several parts, joined by &&: each runs only if the ones before it held,
    // and a name bound by one is visible in the parts after it and in the block
    if Some(x) = a && Some(y) = a && x + y > 5 {
        print(x + y)                    // 8
    }
    if Ok(v) = parse_int("12") {
        print(v)
    }
}
```

- **The variant is named bare and looked up from the type of the right-hand side**, exactly as a `switch` arm's
  is, so it works for `Option`, `Result` (`Ok`/`Err`) and every enum you declare: `if Circle(r) = shape`,
  `if Storage(d) = failure`. A variant with no payload is written without parentheses (`if None = o`), and
  `Variant = expr` on one that has a payload only tests it. `_` ignores the payload: `if Some(_) = o`.
- **The condition is a chain of `&&`-joined parts, each a boolean or a pattern.** `||` cannot join a pattern
  (wrap a boolean in parentheses if you need one). The bound names live in the block and in the parts after
  theirs, not in an `else`.
- **`else` and `else if` work as usual.** An `else` runs when any part failed.
- **It is a statement only**: `val n = if Some(x) = o { ... } else { ... }` is a syntax error; use `switch` for
  a value.
- **It is sugar**: the parser rewrites it to nested `if`s over the enum's `is_V`/`unwrap_V` helpers, so `--fmt`
  keeps what you wrote and nothing else in the toolchain knows the form exists. One consequence: a chain of
  several parts *with an* `else` is not seen by the check that every path returns a value (a single pattern with an
  `else`, or any chain without one, is), so such a function may need a final `return`.

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
  system to express "T must support X." The three exceptions are hardcoded
  bounds, `Eq` (meaning "supports `==`"), `Hash` (meaning "supports
  `hash`" -- a structural hash of any `==`-comparable type; see "Builtins"
  below) and `Ord` (meaning "supports `compare`",
  see "Ordering" below), combinable with `+`
  (`struct Map<K: Eq + Hash, V> { ... }`). Enforced at every
  instantiation (`Map<Channel<int>, string>` is a compile-time error,
  since `Channel` supports neither), not a general trait system, and
  each usable inside the bounded declaration's own body (see "Generic
  functions" below). A bound name other than `Eq`/`Hash`/`Ord` is
  a compile-time error. `Eq` works the same way `==` already does
  everywhere else (recursively, for a struct/enum whose fields/payloads
  are themselves comparable), and `Hash` follows the identical rule (see
  "Maps and sets" above) — a struct or enum made of hashable parts is
  itself hashable, so a `Map`/`Set` key is not restricted to scalars.
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
- **`Option` has bare shorthands**: `is_Some(a)`, `is_None(a)` and
  `unwrap_Some(a)` are `Option.is_Some(a)` and so on, and `unwrap(a)` on an
  `Option` is `unwrap_Some(a)` (`unwrap` on a `Result` is unchanged). They are
  rewritten to the qualified calls when the program is checked, so both forms
  behave the same; other enums have only the qualified helpers. The names are
  reserved, like `is_ok`.
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

- **A call site may leave the type arguments out; they are inferred from
  the arguments' own checked types when they can be.** `identity(5)` works
  without `identity<int>(5)`, and so does a named call
  (`pick(a: 1, b: 2)`). This only ever changes an ordinary call — a
  struct's construction, an enum's construction, `ok`/`err`/
  `channel_new()`, and `spawn` of a generic function are unchanged, still
  always explicit (see above and "`try`" and "Concurrency"). Only a fixed
  parameter whose *declared* type
  actually mentions one of the function's own type parameters
  contributes: `fn wrap<T>(items: [T], label: string) -> Option<T>`
  infers `T` from `items`, and `label`'s concrete `string` type
  contributes nothing (and needs nothing — it is checked normally either
  way). A generic function whose type parameters only ever appear in its
  *return* type (`fn make<T>() -> [T]`) has nothing to infer from and
  still requires `make<int>()`, exactly as before; the error is now more
  specific about *which* type argument is missing.
  An argument that needs its own expected type first (an empty array
  literal, `ok(...)`/`err(...)`/`channel_new()`, an unannotated lambda, an
  `if`/`switch` used as a value) contributes nothing to inference either
  — there is no expected type yet to give it one — and is checked
  normally, against the now-fully-known parameter type, once inference is
  done. Two arguments that would bind the same type parameter to two
  different types is a compile-time error naming both. Explicit type
  arguments (`identity<int>(5)`) still work exactly as before, and are
  used as given without any inference.
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
  To consume a variant, either test-then-unwrap with these accessors or use
  the exhaustive `switch` statement (see "`switch`"), which the compiler
  checks covers every variant. There is no `match` *expression* (`switch`
  is a statement; ROADMAP R21 tracks expression forms).
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

### `switch` — exhaustive consumption of an enum or a Result

```
enum Shape { Circle(float) Square(float) Empty }

fn area(s: Shape) -> float {
    switch s {
        Circle(r) { return 3.14 * r * r }
        Square(w) { return w * w }
        Empty { return 0.0 }
    }
}
```

- **One arm per variant, none omitted, none repeated -- checked at compile
  time.** Adding a variant to an enum makes every `switch` over it fail to
  compile ("this 'switch' over Shape is not exhaustive: missing Triangle"),
  instead of compiling cleanly and crashing when the new variant arrives.
  There is deliberately **no default arm**: it would bring back the silent
  case this exists to remove.
- **Arms name variants bare** (`Circle`, not `Shape.Circle`) -- the
  scrutinee's type says which enum -- and bind a payload with `Circle(r)`.
  A variant that carries a value needs the binder; one that doesn't must
  not have one. The binder is an immutable `val` scoped to the arm's block.
- **Works on any enum (generic ones, ones from other modules, `Option<T>`
  with arms `Some(x)`/`None`) and on `Result<T, E>` (arms `Ok(v)`/`Err(e)`).**
  It is a statement, not an expression: each arm is a block, and a
  function that returns from every arm satisfies the "returns on every
  path" rule.
- The scrutinee is evaluated once. `switch` is checked and then rewritten
  into the `is_X`/`unwrap_X` calls you could have written by hand, so
  everything else about enums is unchanged; `if Shape.is_Circle(s)` remains
  fine for a single-variant test.
- Not (yet) supported: nested patterns, guards, matching several variants
  in one arm, or `switch` as an expression.

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
  A folder may hold its own module under its own name: `use controller`
  with no `controller.zm` but a `controller/controller.zm` is that module
  (the way the bundled `http` is `lib/http/http.zm`), and
  `use controller.ApiError` imports from it; the shorter file wins when both
  exist. Slash, not dot: a module path never shares a token with `.field`
  access, which already means something else. The name a module is
  referenced by (`user` above) is the path's last segment by default, or
  the explicit `as` alias if one is given — either way it's
  casing-enforced lowercase, like every other value-like name.
- **`use path/to/mod.Name [as alias]`** imports one specific
  function/struct/enum instead of the whole module, binding it to a bare
  local name — `use uuid.Uuid` then just `Uuid`,
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
  The filesystem module `fs` is the standard example — `fs.read_text(path:
  string) -> Result<string, string>`, `fs.write_text`, `fs.list_dir`, and
  so on (see "Files" below). A missing or unreadable file is a `Result`
  (see "Result" below) rather than a crash, since that's an expected,
  recoverable condition. `fs.zm` is one of the bundled core modules
  below, so a project needs no local copy of it.
- **A bundled *core module* is ordinary `.zm` source embedded in the
  `ziman` binary at build time** (see docs/DESIGN.md's "Bundled core
  modules"), resolved by `use` when no project-relative file by that
  name exists. There are 27 (docs/CORELIB.md lists every `pub` signature;
  README.md has a table): `collections` (sorting, searching and `map`/`filter`/`fold` -- see "Generics" and "Ordering"), `fs`,
  `os`, `clock` (`now_raw() -> [int]`, the one native function reading the
  system clock) and `random` (`random_bytes(n)`, the one native function
  reading OS entropy), `uuid` (`Uuid`, RFC 4122 v4, pure Ziman on top of
  `random`), `time` (see "Date and time"), `number` (see "Number formatting
  and parsing", which also holds the binary integer/float encodings for `[byte]`), `crypto`, `json` (a parser/serializer whose `Json`
  is a recursive enum, `JArray([Json])`/`JObject(Map<string, Json>)`, plus
  what `#[derive(Json)]` needs), `properties` and `log`, `net`, the HTTP modules
  `http` (the types they share), `server` and `client`, and the database layer `db`, `sql`, `sqlite`, `pg` and `mysql`. Any project can `use` one of these with no local file of its
  own. Each is still an ordinary, qualified module in every other respect --
  `use fs`, always `fs.read_text`, never a bare unqualified name the way the
  prelude's declarations are (though a per-name import, above, can still
  bind one bare on purpose). The prelude (`lib/prelude.zm`) is a deliberate
  exception to "every module needs `use`": `Option<T>` (and its helper
  `option_unwrap_or`), the builders, and a set of native functions
  (`parse_int`, `concat`, `split`, ...) live there, merged bare into every
  program the same way the entry file's own top-level names are, and never
  reachable through `use` (see docs/DESIGN.md's "Prelude" entry). `Uuid`
  satisfies `Eq`, `Hash` and `Ord`, so it works as a struct field, a
  `Map` key and in `collections.sort`. `Json` satisfies `Eq` and `Hash`
  (its own `JArray([Json])`/`JObject(Map<string, Json>)` refer back to
  `Json` itself, resolved the same way any other recursive type is — see
  docs/DESIGN.md's "Recursive Eq/Hash" entry) but not `Ord`: there is no
  `json_compare`, so `compare(a, b)`/`collections.sort` on `Json` values
  is still a type error.

### Naming conventions for module functions

Two usage modes exist (`use net` then `net.connect(...)`, or a per-name import
`use net.net_error_message` then a bare `net_error_message(...)`), and each has
a naming rule. The bundled modules follow it; new ones should too.

1. **Called qualified: a short verb, no module prefix.** `net.connect`,
   `fs.read_text`, `server.route`, `datasource.query`, `log.info`, `client.get`.
   The qualifier already says where it comes from, so `net.net_connect` would
   stutter. Don't import such names bare: `use server.response` makes a bare
   `response` that collides with an application module or variable of that name.
2. **Imported bare, or found by name: carry the noun.** A function another
   module imports by name, or one the compiler locates by a naming convention,
   keeps a self-describing name that is unique across modules:
   `byte_from_hex`, `string_to_bytes`, `net_error_message`, `sql_lift`,
   `method_name`, and everything `#[derive]` dispatches on
   (`<snake_type>_to_json`, `<snake_type>_compare`, `<snake_type>_to_sql`).
3. **A module that defines types names their operations after the type.**
   `uuid_new`, `localdate_add_days`,
   `duration_negate`: the type is the noun, so rule 2 applies and the same
   name is safe bare or qualified. The generic combinators on built-in types
   (`collections.map`, `filter`, `sort`, `find`) are rule 1.
4. **Conversions are `<from>_to_<to>` / `<to>_from_<from>`**, parsing is
   `parse`/`<type>_parse`, and a fallible call returns a `Result` rather than
   using a `try_` prefix.

The one thing to avoid is a short generic verb that is also meant for bare
import; that is the collision the two modes are otherwise designed around.

### `if` and `switch` as values

An `if` or `switch` can produce a value, which removes the "declare a `var`,
then assign it in every branch" pattern:

```
val label = if n > 0 { "positive" } else if n < 0 { "negative" } else { "zero" }

val status = switch e {
    NotFound(m) { 404 }
    Constraint(m) { 409 }
    Connection(m) { 500 }
    Sql(m) { 500 }
}

val n = switch parse_int(text) {
    Ok(v) { v }
    Err(msg) { return err(msg) }      // an arm may leave instead of producing a value
}
```

- **Where:** as the entire right-hand side of a `val`/`var`, an assignment
  (`x = if ...`) or `return` -- in those an arm may `return`, `break`, `continue`
  or use `try` to leave. Anywhere else an expression can appear -- a call
  argument (`print(if c { 1 } else { 2 })`), an operand (`1 + switch e { ... }`),
  an array element, a struct field -- it is also allowed, but **no arm may leave**
  (no `return`/`break`/`continue`/`try`; a `break` inside a loop the arm itself
  contains is fine): the interpreter can leave a statement, not the middle of an
  expression, so it says so and points you at `val x = if ...` where an arm may
  leave. As an operand it must be parenthesised when anything follows it on the
  line: `(if a { 1 } else { 2 }) + 3`; without the parentheses `... + 3` would
  read as part of the last arm's value, so it is a syntax error. At the start of a
  statement, `if`/`switch` are still statements.
- **The value of an arm** is its last expression: a block's statements run,
  and its final bare expression is the value. That final expression may be
  `foo()?`. A trailing `if`/`switch` is itself the value (so `else if`
  chains and nested choices work).
- **An arm may leave** (`return`, `break`, `continue`) instead of ending in a
  value; at least one arm must produce a value, and every path must do one or
  the other, or it is a compile error.
- **`if` needs an `else`;** a `switch` is exhaustive as always.
- **All arms have the same type.** With a declared type
  (`val x: Option<int> = if ...`), an assigned variable, or a `return`, each
  arm is checked against that type exactly like an initializer, so a bare
  value auto-wraps to `Some`/`Ok` and `ok(...)`/`err(...)` resolve; the same goes
  for a call argument, whose parameter type is the expected type. With no
  expected type the first arm decides, and there is no auto-wrap between arms.
- `switch` payload names (`Circle(r)`) scope over their own arm only.

## Date and time

The `time` core module (`use time.Name`) provides seven types, modeled
on `java.time` but scoped down: `ZoneId` reads the system's IANA
zone files (see "Named time zones" below), and every type is `{ ...: int }`-shaped
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
  `random`), `localdatetime_now(offset)`, `localdate_now(offset)` and `localtime_now(offset)` (the current
  date and time on a clock in `offset`, e.g. `localdate_now(zoneoffset_utc())`),
  `instant_of(seconds, nanos)` (normalizing, like
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
- **`ZoneId`** — an IANA time zone (`"UTC"`, `"Europe/Paris"`,
  `"America/New_York"`) with its rules. `zoneid_of(name)` reads them from
  the operating system's zone files (the `zone` module: `$TZDIR`, else
  `/usr/share/zoneinfo`) and is an Err for an unknown name or a system with
  no zone database; `zoneid_utc()` and `"UTC"` never need the files.
  `zoneid_offset_at(z, instant)` is the offset in force then (`-05:00` in
  New York in January, `-04:00` in July), with `zoneid_is_dst_at` and
  `zoneid_abbreviation_at` (`EST`/`EDT`). `instant_to_localdatetime_in(i, z)`
  and `localdatetime_to_instant_in(dt, z)` convert with daylight saving
  applied; a wall-clock time that happens twice (clocks going back) is the
  earlier instant, and one that never happens (clocks going forward) is moved
  forward by the length of the gap, as `java.time` does. Past the last
  transition listed in a zone file the rule string at its end (`EST5EDT,M3.2.0,M11.1.0`)
  decides, so dates in 2100 are right. `zoneid_to_string`/`zoneid_from_string`
  are the name, so JSON and SQL carry `"Europe/Paris"`. Two `ZoneId`s of the same
  zone compare equal.
- **No `<`/`>` operators on any of these seven types.** Ordering goes
  through `compare` and the `Ord` bound instead (see "Ordering"): every type
  has a `_compare(a, b) -> int` function (negative/zero/positive, the same
  convention as C's `strcmp`/`qsort` comparator), which is what makes it
  `Ord`, so these types work in `collections.sort` and as `Ord`-bounded
  type arguments.
  `==`/`!=` come free from structural equality on plain `int`/struct
  fields — no special-casing needed.
- **Calendar year is assumed non-negative throughout** (no BCE
  support) — undocumented territory nothing here needs.

`clock.sleep_millis(ms)` (`use clock`) blocks the calling thread -- only
that thread -- for at least `ms` milliseconds; a non-positive value
returns at once. It exists for retry backoff and for tests that need
overlapping work.

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

- **`Json`** is a recursive `enum`: `JNull`, `JBool(bool)`, `JInt(int)`,
  `JNumber(float)`, `JString(string)`, `JArray([Json])`,
  `JObject(Map<string, Json>)`.
- **Objects are Maps.** A field lookup is near-O(1); iteration (and so
  `json_to_string`) keeps the order keys first appeared in; a repeated key keeps its
  first position with the last value (`parse_json_strict` makes it an error instead). `parse_json_bytes` and `parse_json_bytes_strict` are the same over a `[byte]`
  (a file read with `fs.read_bytes`, a body read off a socket), with no string made first.
  Build one with `Json.JObject(Map<string, Json> { "k": v })`.
- **Numbers.** An integer literal that fits in 64 bits parses to `JInt` and is exact
  (a large id no longer loses precision); a fraction, an exponent or an integer too big
  for `int` parses to `JNumber(float)`. `int_from_json` accepts a `JInt` or a whole-valued
  `JNumber` in range; `float_from_json` accepts either.
- **`parse_json(s: string) -> Result<Json, string>`** — a native parser (about 40x faster than the
  Ziman one it replaced); errors give the byte position, and nesting past 512 levels is an error.
  All JSON string escapes are read (`\" \\ \/ \b \f \n \r \t` and `\uXXXX`, surrogate
  pairs included, decoded to UTF-8); `\u0000` is an error (a string cannot hold a 0 byte).
- **`json_to_string(j: Json) -> string`** — native compact serialization; a float is written with the
  fewest digits that read back exactly (`0.1`, not `0.100000`), NaN and infinity as `null`; control characters
  written as `\b \f \u00XX`. **`json_to_pretty_string(j, indent)`** indents.
- **Lookups:** `json_object_get(j, key) -> Option<Json>`; `json_get_path(j, ["items", "0",
  "name"]) -> Option<Json>` (an array segment is a decimal index); `json_require_field`,
  `json_require_string_field`, `json_require_int_field`, `json_require_float_field`,
  `json_require_bool_field` (all `Result`, with a message naming the field).
- **Map helpers:** `json_to_map<V>(j, decode)` / `json_from_map<V>(m, encode)` convert
  an object to and from a `Map<string, V>` (`json_to_map<int>(j, int_from_json)`);
  `json_to_string_map(j)` is the `Map<string, string>` case.
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

## Media types (`mime`)

`mime` names a kind of content: `mime.parse("text/html; charset=utf-8")` reads a media type (a `MediaType`
with `type`, `subtype` and `params`, all names lower-cased, an Err for a malformed one), `mime.to_string`
writes it back (quoting a parameter value that is not a token), `mime.essence` is `type/subtype` alone,
`mime.charset` and `mime.param` read parameters, and `mime.with_param` sets one. `mime.matches(pattern,
candidate)` compares with wildcards and ignores parameters (`*/*`, `text/*`); `mime.is_json` is true for
`application/json` and any `+json` type. `mime.of_extension("csv")` and `mime.of_path(path)` give the type of a
file, or None for an extension it does not know. Well-known types are functions (`mime.application_json()`,
`mime.text_csv()`, ...). The module has no HTTP in it: the `Accept` header and its weights are `http`'s.

## Serialization formats (`ndjson`, `csv`, `toml`, `yaml`)

Each format is a pure-Ziman module that converts text to and from a `Json` tree, so
`#[derive(Json)]` types decode from (and encode to) every format for free:

```
use toml
use json.Json

#[derive(Json)]
struct Server { host: string  port: int  tags: [string]  limits: Map<string, int> }

fn main() {
    val tree = unwrap(toml.parse_toml("host = \"a\"\nport = 80\ntags = [\"x\"]\n[limits]\nrps = 10\n"))
    val server = unwrap(server_from_json(tree))          // the same call for yaml.parse_yaml, csv rows, ...
    print(unwrap(toml.toml_to_string(server_to_json(server))))
}
```

| Module | Reads | Writes |
| --- | --- | --- |
| `ndjson` | `parse_ndjson(text) -> Result<[Json], string>` (one value per line, blank lines skipped); `records(path)` reads a file lazily as an `Iter` of `Result<Json, string>` for a `for` loop, one record in memory at a time | `ndjson_to_string([Json])` |
| `csv` | `parse_csv` / `parse_csv_typed` -> a `Json` array of objects keyed by the header row; `parse_csv_rows` -> `[[string]]`; `_with(text, delimiter)` variants for TSV etc.; `records(path)` (and `records_typed`, `_with`) reads a file lazily as an `Iter` of `Result<Json, string>` for a `for` loop, a record at a time, quoted newlines included | `csv_to_string(Json)`, `csv_rows_to_string([[string]])` |
| `toml` | `parse_toml(text) -> Result<Json, string>` | `toml_to_string(Json) -> Result<string, string>` |
| `yaml` | `parse_yaml(text) -> Result<Json, string>` | `yaml_to_string(Json) -> string` |

- **Errors** are `Err(String)` with a `line N:` prefix (CSV: `row N`).
- **CSV:** RFC 4180 quoting (a native scanner splits the cells); LF, CRLF or lone-CR line endings; blank
  lines skipped; a leading UTF-8 byte order mark ignored. `parse_csv` keeps every cell a string;
  `parse_csv_typed` reads an *unquoted* cell as a number (`42`, `-1.5`; not `007`), a boolean or null (empty),
  and a quoted cell always stays a string. `parse_csv_columns` / `_typed` take the column names for a file with
  no header row. The `_with(text, delimiter)` variants take a one-character delimiter (`"\t"` for TSV); an
  invalid delimiter is an `Err`. Errors name the *line* a row started on, including a quote left open (and where
  it opened). The writer takes the union of the rows' keys as columns (first-seen order), flattens an object
  value one level into `a.b` columns, rejects an array or deeper nesting, and quotes a string that would
  otherwise read back typed -- so `parse_csv_typed(csv_to_string(x))` round-trips. Lines end in `\n`.
- **TOML** 1.0: tables, arrays of tables, dotted keys, inline tables, all string forms and
  escapes, hex/octal/binary/`_` integers, floats, booleans, comments. Dates and times stay
  `JString` text; `inf`/`nan` are rejected; a duplicate key or table is an error. The writer
  omits `JNull` fields (TOML has no null) and writes scalars, then sub-tables, then `[[arrays]]`.
- **YAML** is a strict subset of 1.2: block maps and sequences, flow `[..]`/`{..}`, plain and
  quoted scalars, `|` and `>` block scalars, comments, a leading `---`; core-schema typing
  (`null`/`~`, `true`/`false`, numbers; `yes`/`no` are strings; `007` stays a string). Anchors,
  aliases, tags, the `<<` merge key, `? ` keys, multiple documents and tab indentation are errors,
  not misreadings. The writer quotes exactly the strings that would re-read as another type.
- These parsers are ordinary Ziman on the tree-walking interpreter: fine for configuration and
  export-sized data, slow for megabytes.

## Ordering (`Ord`, `compare`, sorting)

```
use collections

#[derive(Ord)]
struct Person { last: string  first: string }

val sorted = collections.sort<Person>(people)               // by last, then first
val by_age = unwrap(collections.sort_by_keys<Patient, int>(patients, ages))
```

- **`<`, `>`, `<=`, `>=` work on `int`, `float`, `bigint`, `decimal`, `byte`
  and `string`** (a string compares byte by byte, then by length). They stay
  non-overloadable: a struct or enum is compared with `compare(a, b)`.
- **`compare<T: Ord>(a, b) -> int`**: negative if `a` is smaller, zero if
  they are equal, positive if `a` is bigger -- **only the sign is meaningful,
  not the magnitude**, so a hand-written `_compare` may return any `int` (for
  example `a.score - b.score`) and the builtin's own `-1`/`0`/`1` is not
  something to rely on. Everything here (`sort`, `is_sorted`,
  `binary_search`, the derived functions, `compare` on a nested type) looks
  only at whether the result is `< 0`, `0` or `> 0`. (A subtraction can
  overflow, which is an error like any other `int` overflow.) `Ord` is satisfied by
  those scalars, by a struct or enum with a function
  `<snake_case_type>_compare(a: T, b: T) -> int` (`Person` looks for
  `person_compare`; a type from another module looks in its own module; the
  spelling without inserted underscores, `localdate_compare`, is also
  accepted, which is what the bundled `time` types use), and by a type
  parameter bounded with `Ord`. It also holds, recursively, for **arrays**
  (lexicographic, a shorter prefix first), **`Option<T>`** (`None` before every
  `Some`, then by payload) and **`Result<T, E>`** (every `Ok` before every
  `Err`, then by payload) whose element/payload types are `Ord` -- so
  `[Option<Result<int, string>>]` sorts. `bool` and channels have no order. A
  `float` order is total: NaN sorts after every other value and equals itself.
- **`#[derive(Ord)]`** writes `<type>_compare`: a struct compares its fields
  in declaration order (the first difference decides); an enum compares by
  variant position, then by payload when both are the same variant. Every
  field and payload must itself be `Ord`. A struct with no fields can't derive
  it. A generic type can: `#[derive(Ord)] struct Pair<A, B>` writes
  `pair_compare<A: Ord, B: Ord>`, and `Pair<int, string>` is `Ord` exactly when
  both arguments are (so `Pair<int, bool>` is rejected).
- **`collections.sort<T: Ord>(items) -> [T]`** is a stable merge sort (equal
  items keep their order) returning a new array. To sort by anything other
  than the type's own order, compute a key per item and use
  **`collections.sort_by_keys<T, K: Ord>(items, keys) -> Result<[T], string>`**
  (an `Err` if the lengths differ), or pass a comparator *function value* to
  **`sort_with<T>(items, cmp: fn(T, T) -> int)`** (also stable) for an order
  that is not a single key. Descending: **`sort_desc`** and
  **`sort_by_keys_desc`** (also `sort_indices_desc`) -- stable too, so equal
  keys keep their input order; that is not the same as reversing the ascending
  result, which would flip ties. Sorting a packed `[int]`/`[float]`/`[byte]` or an
  array of strings runs natively (about 10x faster); anything else uses a merge
  sort calling `compare`. Also `sort_indices`, `is_sorted`, `reverse`, and for
  sorted arrays `lower_bound` / `upper_bound` (insertion points) and
  `binary_search` (first match, `Option<int>`). `Uuid`, `LocalDate`, `LocalTime`, `LocalDateTime`,
  `Instant`, `Duration` and `ZoneOffset` are all `Ord`.

## Attributes and derive

`#[...]`, written directly above a `struct`/`enum` declaration, attaches
compile-time metadata to it. Two are interpreted: `derive` (below) and
`thread_bound`; other names parse (so the grammar doesn't need to change
later) but have no effect yet.

`#[thread_bound]` on a struct marks a value that belongs to the thread that
made it, as `StringBuilder`, `ArrayBuilder`, `MapBuilder`, `SetBuilder` and `Iter`
are (mutable handles that are not safe to share). A type that contains one
-- an array of them, a struct with one in a field, `Option<StringBuilder>` --
is refused where it would be shared: as a `Channel` element, as a `spawn`
argument, and as the type of a module-level `val`. Everything else that could
carry one to another thread (a closure that captured it, say) is caught when
the builder is used: each builder remembers its thread and a use from any
other is a runtime error. Hand the finished value over (`sb_to_string`,
`ab_to_array`, `mapb_to_map`, or the items of an iterator as an array), not the builder
or the iterator. The attribute is for the
prelude's builders and any future one of the kind; on a struct written in
Ziman it gives the compile-time refusals only (there is no native to check the
thread).

```
use json.Json
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
  (`int`/`float`/`bool`/`string`), or a `decimal` (a JSON number: written as
  the nearest float, read as the number's shortest text, so `0.1` is `0.1`; `#[derive(Row)]`
  stores it as TEXT, keeping its scale); (4) `Option<U>`/`[U]`, recursing into
  `U`. A field whose type is none of these (`bigint`, `byte`,
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
- **A derive imports what it generates code against.** `#[derive(Json)]`
  brings in `json.Json`, `json_object_get`, `json_require_field`
  and `json_require_string_field`; `#[derive(Row)]` brings in `db.SqlValue`,
  `DbError`, `Row`, `row_get` and the `sql_as_*` readers;
  `#[derive(Properties)]` imports the `properties` module. A name is added
  only if the module doesn't already bind it, so an explicit `use` (or a
  declaration of your own with that name) is left alone. What a derive can't
  supply is a field type's own conversion, with one exception: for `Uuid` and
  the `time` types (`Instant`, `LocalDate`, `LocalTime`, `LocalDateTime`,
  `Duration`, `ZoneOffset`, `ZoneId`) the derive also imports their
  `_to_json`/`_from_json` (or `_to_sql`/`_from_sql`) from `uuid` and `time`,
  so a struct with such a field derives with no extra `use`. For any other
  field type the conversion still has to exist and be imported, and a missing
  one says so ("give that type its own #[derive(Json)] or write it by hand"). Names the derive imported are
  visible to the rest of the module, so your own code may use them too.

## Testing

`#[test]` on a function marks it for `ziman test file.zm [file2.zm ...]` to run -- a separate CLI
mode from ordinary `ziman file.zm`, listed with `ziman --help`. A `#[test]` function takes no
parameters and returns either nothing or `Result<unit, E>` for any `E` (so a test body can freely
`try` a fallible call); anything else is a type error at the declaration. The file it's in needs
no `main` function, the same relaxation the language server already gets for a file with no entry
point.

`ziman test` loads and type-checks each file given, finds every `#[test]` function declared
directly in it, and runs each inside its own per-thread runtime-error boundary -- the same
mechanism `spawn` already uses to keep one thread's failure from ending the process (see
"Concurrency" below) -- so one failing test doesn't stop the run. A runtime error (an assertion
failure included, see `test` below) or a `Result` that comes back `Err` both count as a failure,
its message shown; anything else is a pass. Each test prints `PASS <file>::<name>` or
`FAIL <file>::<name>  <message>`, then a `N passed, M failed` summary once every file is done;
the exit code is 0 only if every test in every file passed.

**Parametrized tests: `#[test(cases: NAME)]`.** A function taking exactly one parameter can be
run once per element of an array instead of once with no arguments:

```
struct AddCase { a: int, b: int, expected: int }

val add_cases = [AddCase { a: 1, b: 1, expected: 2 }, AddCase { a: 2, b: 2, expected: 4 }]

#[test(cases: add_cases)]
fn test_add(c: AddCase) {
    test.assert_eq(c.expected, c.a + c.b)
}
```

`NAME` is looked up in the same file only (no `mod.NAME` cross-module form, the same restriction
a global val's own bare reference already has) as either a val or a zero-parameter function, both
of type `[T]` where `T` is the `#[test]` function's own parameter type -- checked at compile time,
same as everything else here. A val's array is used as already evaluated; a function is called
once, before any case runs, to build it (also inside its own boundary, so a broken case-builder is
one reported failure, `could not build cases from 'NAME': ...`, not a crash). Each case is reported
independently, `<file>::name[i]`, so one failing case doesn't hide whether the others passed --
unlike an ordinary `for` loop over the cases inside a plain `#[test]` function, where the first
failure ends the whole test via the same fatal-error mechanism `test.assert*` always uses.

**The `test` module.** `lib/core/test.zm`'s assertions all fail through `unwrap` on a manufactured
`Err` -- the same explicit, opt-in escape hatch to a fatal error every Ziman program already has
(see "Result" below) -- so no new interpreter primitive was needed for any of them:

- `fail(message: string)` -- fails unconditionally.
- `assert(cond: bool)` / `assert_false(cond: bool)`.
- `assert_eq<T: Eq>(expected: T, actual: T)` / `assert_ne<T: Eq>(unexpected: T, actual: T)` --
  structural equality (see "Equality"), the failure message showing both values.
- `assert_lt`/`assert_le`/`assert_gt`/`assert_ge`, each `<T: Ord>(actual: T, bound: T)`.
- `assert_some<T>(opt: Option<T>) -> T` / `assert_none<T>(opt: Option<T>)`.
- `assert_ok<T, E>(r: Result<T, E>) -> T` / `assert_err<T, E>(r: Result<T, E>) -> E`.

`assert_some`/`assert_ok` return the unwrapped value on success, so a test can chain straight into
using it. Failure messages use `format`'s `{}` placeholder, not `str` -- `str` is deliberately
narrower (int/float/bool/bigint/decimal/byte only, see "Builtins") and, being a fixed allowlist
checked before a generic function's own type parameters are known, rejects a plain `T` outright;
`format`'s placeholder displays any value the same way `print` does, so `assert_eq`'s only
requirement is the `T: Eq` bound `==`/`!=` already need, nothing extra for the message.

## Databases

Two modules to know: `db` (what the drivers share -- `SqlValue`, `Row`,
`DbError`, the `sql_as_*` readers and the URL parser) and `datasource` (what a program calls: connections,
`execute`, `query`, `rows`, pools, transactions). SQLite (`sqlite::memory:` or
`sqlite:<path>`) and PostgreSQL (`postgres://user:password@host:port/database`)
are the two backends, chosen by URL scheme behind the same calls.

```
use db
use db.SqlValue
use db.row_get
use db.sql_as_text

fn main() {
    val conn = unwrap(datasource.open("sqlite::memory:"))
    unwrap(datasource.execute(conn, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"))
    unwrap(datasource.execute(conn, "INSERT INTO t (id, name) VALUES ($1, $2)", SqlValue.Int(1), SqlValue.Text("Ada")))
    val rows = unwrap(datasource.query(conn, "SELECT name FROM t WHERE id = $1", SqlValue.Int(1)))
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
- **Postgres** is written in Ziman (module `pg`, on `net`, `number` and
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
  statement. **TLS** is negotiated the way Postgres does it (an
  `SSLRequest`, then an in-place upgrade) and chosen by the URL's
  `sslmode`, with libpq's meanings: `disable` (the default here -- libpq
  defaults to `prefer`), `prefer` (TLS if offered, no verification),
  `require` (TLS or fail, but the certificate is **not** verified --
  encrypted yet open to a man in the middle, exactly as in libpq),
  `verify-ca` (also the certificate chain) and `verify-full` (also that it
  is valid for the host -- the one to use for a hosted database).
  `sslrootcert=<path>` names a PEM file of trusted CAs, else the system
  store is used. `allow` is unsupported, and a failed `prefer` handshake
  isn't retried over plaintext. **Not supported:** MD5 authentication,
  Kerberos/GSS, client certificates, IPv6 literals in the URL, COPY,
  LISTEN/NOTIFY, prepared-statement reuse, and SASLprep of a non-ASCII
  password.
- **`datasource.rows(conn, sql, params)`** is `query` as a lazy `Iter<Result<Row, DbError>>`
  for a `for` loop: on SQLite the rows come from a cursor, so a million-row result
  uses memory for one row (72 MB against 1.7 GB in a test) and is closed when it is used up, when the loop
  is left early, or with `iter_close`. On a pool the connection is borrowed for the whole
  iteration and returned the same way (finish or close it: a pooled iterator is not closed by
  the garbage collector). Postgres and MySQL stream too, 500 rows at a time: Postgres reads
  a portal with a row limit, and until the cursor is done, closed or dropped that connection carries
  no other statement (one sent meanwhile is an Err); MySQL uses a server-side cursor (a prepared statement
  opened read-only and read with `COM_STMT_FETCH`), where other statements may run between fetches but a
  connection holds one cursor at a time. A cursor dropped unfinished is ended when it is garbage
  collected. A 300,000-row result used 51 MB against 609 MB on Postgres and 43 MB against 706 MB on MySQL.
- **`datasource.query`** returns `[Row]`; `datasource.query_one` returns `Option<Row>`;
  `datasource.execute` returns the row count of the last statement, and takes a
  `;`-separated script when there are no parameters (for DDL).
- **Connection pools.** `datasource.open_pool(url, size)` opens `size` connections
  up front (failing fast, and closing any it opened, if one can't connect)
  and returns a pool-backed **`Connection` -- the same type `open`
  returns**, so repository code written against a `Connection` works
  unchanged. Statements run on it (`execute`/`query`/`query_one`) each
  borrow a connection for that one statement and give it back. A unit of
  work spanning several statements -- a transaction above all -- borrows
  one *exclusively*:

  ```
  val tx = datasource.acquire(conn)?
  defer datasource.release(conn, tx)      // returned on every path out
  datasource.begin(tx)?
  defer datasource.rollback(tx)           // runs before the release
  ...
  datasource.commit(tx)?
  ```

  The pool is a channel of idle connections, so a borrow blocks while
  every connection is busy (natural backpressure). `tx` is an ordinary
  non-pool `Connection`, so a repository called with it uses it directly
  and never borrows again -- borrowing while holding a connection could
  otherwise deadlock a small pool. `begin`/`commit`/`rollback` on the pool
  itself are **refused** (each statement would borrow a different
  connection and silently do the wrong thing). On a plain `Connection`,
  `acquire` returns it and `release` does nothing, so the same code runs
  against either. `datasource.close(pool)` waits for every connection to be
  released, then closes them all -- a borrow that was never released makes
  it wait forever, which is what `defer datasource.release` prevents.
  `datasource.acquire_timeout(pool, ms)` gives up with a `DbError.Connection`
  instead of waiting for ever. **A connection that dies is replaced:** a
  statement on the pool that fails with `DbError.Connection` closes that
  connection and opens a fresh one from the same URL (the statement itself
  is not re-sent, since it may have run); after a failure on an acquired
  connection, `datasource.discard(pool, tx)` does the same instead of `release`, and
  `with_transaction` does it by itself. **Health check:** a connection idle for
  30 seconds or more is tested with `SELECT 1` when it is lent out, and replaced first if that fails with a
  connection error (`datasource.open_pool_with(url, size, check_after_seconds)` changes the time; 0 turns it off;
  SQLite connections are never tested). **Limits:** no min/max sizing or idle timeout; a released connection isn't reset (release one with a
  transaction open and it goes back dirty); and a pool over
  `sqlite::memory:` is a **single** connection whatever `size` says, since
  every in-memory connection is its own private database. SQLite
  connections wait up to five seconds for the writer lock instead of
  failing at once, which is what makes a pool over a file usable.
- **Transactions** are plain `datasource.begin`/`datasource.commit`/`datasource.rollback` on the
  connection. Every call takes the connection's own lock, so sharing one
  `Connection` across threads is safe *statement by statement*, but a
  transaction is not safe to share -- another thread's statements can
  interleave inside it. A `try` that returns early from inside a
  transaction leaves it open unless the caller uses `defer datasource.rollback(tx)`
  or `datasource.with_transaction` (see "Transactions").
- **`#[derive(Row)]`** on a struct generates `<name>_from_row(row: Row)
  -> Result<Name, DbError>` and `<name>_to_params(v: Name) ->
  [SqlValue]` (field declaration order, the order a hand-written
  `INSERT ... VALUES ($1, $2, ...)` lists its columns in). Column names
  are the field names as written (already `snake_case`, no transform).
  `int`/`float`/`bool`/`string` map directly; a struct/enum field
  dispatches to `<name>_to_sql`/`<name>_from_sql` (provided for `Uuid` and
  every `time` type); `Option<U>` is NULL-or-value. An enum, a generic
  struct, an array field, or `bigint`/`decimal`/`byte` is a compile-time
  error. The derive imports what the generated code calls (`db.SqlValue`,
  `db.DbError`, `db.Row`, `db.row_get`, the `sql_as_*` readers, and the
  `_to_sql`/`_from_sql` of `Uuid` and the `time` types); the conversion of a
  field type of your own has to be imported by the module that derives.

## Networking

The `net` core module is blocking TCP; the `[byte]` helpers that go
with it are prelude builtins, plus the binary encodings in `number`.

```
use net

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
  a native table, like `datasource.Connection`. Closing is explicit (`net.close`,
  `net.close_listener`); there are no destructors, so a `try` that returns
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
- **`string_to_bytes` / `bytes_to_string` / `bytes_concat`** are prelude builtins
  (no `use`): `bytes_to_string` returns a `Result` -- a string can't hold a 0
  byte -- and `bytes_concat(parts...)` joins any number of byte arrays in one
  pass. Sub-ranges are the prelude's zero-copy `slice<byte>(b, start, end)`.
- **`number`** (binary encodings): the big-endian `u16_to_bytes`/`u32_to_bytes`/`u16_from_bytes`/
  `u32_from_bytes` and the float encodings that network protocols are built from.
- **Lines.** `net.lines(socket)` (and `lines_with(socket, max_line_bytes)`) is an `Iter<Result<string,
  NetError>>` of the lines a peer sends, read lazily for a `for` loop and ended when the peer closes. Each
  item is a line without its `\n` or `\r\n` (a last line with no newline counts), or an Err for a line
  with a NUL byte, one longer than 1 MiB (which ends the iteration) or a read error such as `Timeout`. It
  reads ahead in blocks, so take all of a socket's lines from one iterator; it does not close the socket.
- **TLS (client side).** `net.connect_tls(host, port)` is `connect` plus
  `net.start_tls(sock, host)`, which upgrades a connected socket **in
  place** -- afterwards the same `read`/`read_exact`/`write`/`read_text`/
  `set_timeout`/`close` work over the encrypted connection, so code built
  on `Socket` (the `pg` driver, a future HTTP layer) doesn't care which it
  has. **Verification is on by default**: the certificate chain against
  the system CA store *and* that it is valid for the host name (or IP
  address) you connected to; TLS 1.2 minimum; SNI is sent. The `_with`
  forms (`start_tls_with`, `connect_tls_with`) take a `TlsVerify` --
  `Full`, `Chain` (skip the host-name check) or `Off` (encryption only,
  open to a man in the middle) -- and an optional PEM file of trusted CAs
  to use instead of the system store (a private CA, a self-signed test
  certificate). A handshake or verification failure is
  `NetError.Tls(message)` with OpenSSL's reason, and `connect_tls` closes
  its socket if the handshake fails. A receive timeout still surfaces as
  `Timeout`, and a peer's close as `Closed`, over TLS.
- **Not yet:** server-side TLS (`listen_tls`), client certificates, ALPN,
  Unix-domain sockets, UDP, and a packed byte-buffer type (`[byte]` is an
  array of interpreter values -- fine for protocol messages, wasteful for
  bulk data).

## HTTP

The `http` core module is an HTTP/1.1 **server toolkit** on `net`: parse a
request off a socket, route it, write a response -- and, on top of that, a
ready-made server loop:

```
fn route(conn: datasource.Connection, req: http.Request) -> http.Response { ... }

val options = server.ServeOptions { idle_timeout_ms: 30000, max_connections: 64, log_requests: true }
os.install_shutdown_handler()
server.serve(listener, fn(req) { route(conn, req) }, options)   // the lambda captured `conn`
                                            // returns after SIGINT/SIGTERM
```

`server.serve(listener, handler: fn(Request) -> Response, options)`
runs the accept loop and the per-connection request loop and calls `handler`
for each request. The handler is a function value -- a named function, or a
lambda capturing whatever it needs (a database connection, configuration); the
captured values are immutable copies, so they are safe to share between the
connection threads. One thread
per connection, at most `max_connections` at a time (a channel used as a
counting semaphore; a client past the limit waits in the kernel's accept
queue). It returns when a shutdown was requested
(`os.install_shutdown_handler()`; see "Configuration") and every connection
being served has finished, so the caller then closes its listener and pool.
`serve` neither closes the listener nor installs the handler. You can still
write your own accept loop and connection loop from the pieces below when you
need something `serve` doesn't do.

```
use net
use http

fn route(conn: datasource.Connection, req: http.Request) -> http.Response {
    val create = server.route(req, HttpMethod.Post, "/patients")
    if Option.is_Some(create) {
        return server.json_response(201, unwrap(http.request_text(req)))
    }
    val one = server.route(req, HttpMethod.Get, "/patients/{id}")
    if Option.is_Some(one) {
        val id = server.param(Option.unwrap_Some(one), "id")     // percent-decoded string
        return server.json_response(200, lookup(conn, id))
    }
    return server.not_found_or_method_not_allowed(req, ["POST /patients", "GET /patients/{id}"])
}

fn serve_connection(conn: datasource.Connection, sock: net.Socket) {
    defer net.close(sock)                       // closed on every way out
    unwrap(net.set_timeout(sock, 30000))        // an idle client can't hold the thread forever
    var session = server.open_session(sock)
    var open = true
    while open {
        val next = server.read_request(session)
        if !is_ok(next) {
            server.write_error(sock, unwrap_err(next))   // 400/408/413/431/501, or nothing if the peer is gone
            open = false
        } else {
            val ex = unwrap(next)
            session = ex.session                       // carries any bytes read past this request
            val keep = server.write_response(sock, ex.request, route(conn, ex.request))
            if !is_ok(keep) || !unwrap(keep) {
                open = false
            }
        }
    }
}

fn main() {
    val l = unwrap(net.listen("0.0.0.0", 8080))
    while true {
        spawn serve_connection(conn, unwrap(net.accept(l)))    // thread per connection
    }
}
```

- **Reading.** `read_request(session)` returns an `Exchange` (the
  `Request` plus the updated `Session`) or an `HttpError`. A `Request` has
  `method`, `target`, `path`, `query`, `version`, `headers` and `body`;
  `http.header(req, "Name")` ignores case. `Err(Closed)` is the normal end
  of a keep-alive connection. Keep-alive works across requests, including
  pipelined ones already in the buffer, and `Expect: 100-continue` is
  answered. A request line, headers and a `Content-Length` body are
  supported, and so is a `Transfer-Encoding: chunked` body (decoded; any other
  transfer encoding is answered 501), and `HEAD` gets headers with the
  `Content-Length` and no body.
- **Bounded.** Headers may total 16 KiB and a body 1 MiB
  (`HeadersTooLarge`/`BodyTooLarge`, answered 431/413), so a client can't
  make a handler buffer without limit. Set `net.set_timeout` on the
  socket; a stalled read is `Timeout` (408). Bodies are `string`s, so a
  request carrying a 0 byte gets a 400. A per-read timeout does not stop a
  client that sends a byte just inside it, again and again (slowloris), so
  `server.open_session_with_timeout(sock, ms)` also bounds the whole request --
  head and body, from its first byte -- to `ms` (`Timeout`, 408); `serve` opens
  its sessions this way with `idle_timeout_ms`, so that one number is both how
  long a connection may sit idle and how long a request may take to arrive.
- **Strict about what a proxy in front might read differently**, since a
  difference between the two is how requests are smuggled. A request is a 400
  if it has `Content-Length` headers that differ (identical repeats are fine),
  a `Content-Length` that is not plain digits (`+5`, `-1`, `5, 5`, `0x5`), two
  `Transfer-Encoding` headers or two `Host` headers, no `Host` at all on
  HTTP/1.1 (HTTP/1.0 needs none), a header name that is not a token
  (`http.is_token`), a header value or request line with a control character
  other than a tab (`http.is_field_value`) -- a bare CR or LF included, which
  would otherwise let one header line pass for two -- or a method that is not a token.
- **Response headers are checked on the way out.** `write_response` refuses a
  header whose name is not a token or whose value has a control character (an
  application putting text from a client into a header without checking it:
  CRLF injection). It sends a `500` in place of the response, closes the
  connection and writes a line to stderr, never the offending text. `with_header`
  itself does not fail -- a `Response` can also be built as a struct literal, so the
  check is where every response passes.
- **Responses.** `server.response(status, body)`, `json_response`,
  `text_response`, `bad_request`, `not_found`, `method_not_allowed`, and
  `with_header`. `write_response(sock, req, resp)` adds `Content-Length`
  and `Connection`, and returns whether the connection stays open
  (HTTP/1.1 keeps alive unless either side said `close`; HTTP/1.0 closes
  unless asked to keep).
- **Methods.** `Request.method` is an `http.HttpMethod` -- `Get`, `Head`,
  `Post`, `Put`, `Patch`, `Delete`, `Options`, or `Other(name)` for anything
  else (method names are case-sensitive, so `get` is `Other("get")`).
  `http.method_parse(text)` and `http.method_name(method)` convert; a route
  or a `switch` over the method is checked by the compiler. `client`
  takes the same type: `client.request(method: HttpMethod.Post, ...)`.
- **Routing.** `server.route(req, method, pattern)` is `Some(params)` when
  the method matches and the path matches the pattern, else `None`.
  `{name}` captures one non-empty, percent-decoded segment; a trailing
  slash is ignored; `server.param(params, "name")` reads it (asking for a
  name the pattern doesn't have is a fatal error -- it is a bug in your
  route function, not a value to check). `not_found_or_method_not_allowed`
  is 405 with an `Allow` header if the path matches some route under a
  different method, else 404. `server.query_param(req, "q")` reads the query
  string (percent-decoded, `+` is a space). A route in a `{id}` position is
  still a *string*: converting it (a `Uuid`, an `int`) and answering 400 on
  a bad one stays in the application.
- **Typed endpoints.** `server.Endpoint<C> { method, pattern, handler }` with a handler built by one of eight builders:
  by where the input comes from -- `handler` (none, `run(ctx)`), `path_handler` (the path: `path` turns the captured
  `Params` into a `P`, an `Err` message being a 400, and `run(ctx, input)` gets it), `body_handler` (a request body:
  `decoders:`, `run(ctx, input)`) or `path_body_handler` (both, for a `PUT /things/{id}`: `run(ctx, path_input,
  body_input)`) -- and by who may call, `public_` or `secure_`. (`secure_query_handler` is the one builder with the input in the query string: `query` reads the
  request, `query_param` being the tool, and an `Err` message is a 400; `run(ctx, caller, input)`.) Every builder also takes `encoders:`,
  `status:`, `run:`, `failure:` and `fault:`. A `secure_` builder takes `who:`, a `fn(C, Request) -> Result<U, Denied>`
  that identifies the caller from the request, and passes the `U` to `run` as its second argument
  (`fn(ctx, caller, input)`), so an endpoint that needs a caller cannot be written without saying how to find one; a
  `public_` builder has no `who` and its `run` has no caller parameter. `Denied` is `Unauthenticated(message)` (401),
  `Forbidden(message)` (403) or `Failed(message)` (500), and is answered before anything else happens: before the
  answer is negotiated, the body is read or the path decoded. `run` returns `Result<O, E>`: the answer or an error of the
  application's own type, so it can be an ordinary function that knows nothing about requests. An endpoint with no `encoders` (`encoders: []`) answers with an empty body and no `Content-Type` and does not negotiate,
  which is what a `204` needs. A `Decoder<I> { media, decode }` turns the request body's bytes into an `I` (an `Err` is a
  400) and an `Encoder<O> { media, encode }` turns an answer into bytes; the server knows no format, the application supplies
  the functions. Build them with `server.decoder<I>(media, fn([byte]) -> Result<I, string>)` and
  `server.encoder<O>(media, fn(O) -> [byte])` for bytes (JSON, binary media), or `server.text_decoder<I>(media,
  fn(string) -> Result<I, string>)` and `server.text_encoder<O>(media, fn(O) -> string)` when the format is text (a body
  with a NUL byte is a 400 there). An endpoint lists the decoders it accepts (the request's `Content-Type` picks one)
  and the encoders it can answer with (`Accept` picks one); the handler answers 415 when there is no decoder for the
  `Content-Type` and 406 when `Accept` allows no encoder. `http.Request.body` and `http.Response.body` are `[byte]`
  (`http.request_text` / `http.response_text` give the text, or an Err for a NUL byte). `failure(e)` builds the
  response for an `Err`, and `fault(status, message)` the one for what the library itself rejects.
  `server.routes<C>(endpoints)` compiles the table once -- build it as a module-level `val`, not per request -- and
  `server.dispatch<C>(table, ctx, req, fault)` calls the first endpoint that matches, or answers 404, or 405 with
  `Allow` naming the methods the path does have, in table order. Compiling splits each pattern into its segments once
  and groups the endpoints by method and number of path segments, so a request splits its path once and only meets the
  endpoints that could match (a 117-endpoint table went from 3.9 ms to 0.6 ms for an unknown path, and about 0.3 ms
  faster for a matching one). The matching rules are the same as ever: table order decides between a `{param}` and a
  literal (`/items/{id}` before `/items/new` answers for `new`), a trailing slash is ignored, a capture is
  percent-decoded and never empty. `server.route` and `server.path_params` still match one pattern against one
  request, for a hand-written router. See `tests/fixtures/http_endpoints.zm` and `http_dispatch_routes.zm`.
- **Not yet:** HTTP/2, multipart, compression, trailers.

## Transactions (`datasource.with_transaction`)

`datasource.with_transaction<R>(conn, work: fn(Connection) -> Result<R, DbError>) ->
Result<R, DbError>` runs `work` in a transaction on one exclusive connection:
it borrows one from a pool, begins, calls `work(tx)`, commits if `work`
returned `Ok`, and rolls back if it returned `Err` (or the commit failed),
handing the connection back on every way out. The result of `work` is returned
unchanged. `work` is a function value, usually a lambda capturing what it needs
(the data to write, say). `work` must not call `datasource.begin`/`datasource.commit` itself.

```
fn transfer(tx: datasource.Connection, t: Transfer) -> Result<int, DbError> {
    try datasource.execute(tx, "UPDATE acct SET balance = balance - $1 WHERE id = $2", ...)
    try datasource.execute(tx, "UPDATE acct SET balance = balance + $1 WHERE id = $2", ...)
    return ok(t.amount)
}

val t = Transfer { ... }
val moved = try datasource.with_transaction<int>(conn, fn(tx: datasource.Connection) -> Result<int, DbError> {
    return transfer(tx, t)
})
```

## Regular expressions (`regex`)

```
val re = try regex.compile("(\\w+)@(\\w+)\\.com")           // Result<Regex, string>
val m = regex.find(re, "mail bob@site.com now")             // Option<Match>
regex.replace_all(re, "bob@site.com", "$2: $1")             // "site: bob"
```

`compile(pattern)` returns `Err` with a message naming the position of the
problem. A `Regex` is immutable: compile once, share it between threads.
`is_match`, `find`, `find_from`, `find_all`, `group`, `group_named`, `replace`,
`replace_all`, `split` and `escape` do the rest. A `Match` has `start`/`end` (byte offsets, like
`substring`), `text` and `groups` (`[Option<string>]`, groups 1..n).
Replacements expand `$0`, `$1`..`$9`, `${12}` and `$$`.

The syntax is Perl-flavoured **without backreferences or lookaround**, on
purpose: matching then runs in time proportional to text length times pattern
size, so a pattern taken from a request cannot make a program hang (`(a+)+$`
against a long run of `a`s answers immediately). Supported: literals, `.`,
classes `[a-z]` `[^...]` (with `\d \w \s` inside), `\d \D \w \W \s \S` (ASCII),
`\b \B`, `^ $`, groups `( )`, `(?: )` and named `(?P<year>...)` /
`(?<year>...)`, `|`, and `* + ? {n} {n,} {n,m}` (greedy, or lazy with a trailing
`?`). Inside a bracket class (or as an atom) `[[:alpha:]]`-style POSIX classes
(`alpha digit alnum upper lower space blank punct xdigit word cntrl print graph`,
ASCII, `[:^alpha:]` negates) and Unicode properties: `\p{L}` letters, `\p{Lu}`
`\p{Ll}` case, `\p{N}` `\p{Nd}` numbers, `\p{P}` punctuation, `\p{S}`
`\p{Sc}` symbols, `\p{Z}` separators, `\p{M}` marks and the rest of the general
categories (Unicode 16, generated from the character database), plus the scripts
`Latin Greek Cyrillic Armenian Hebrew Arabic Devanagari Thai Hiragana Katakana
Hangul Han`, which are their main Unicode blocks rather than exact script data.
`\P{L}` and `\p{^L}` negate; `\pL` is `\p{L}`. Lookaround and backreferences
remain unsupported and say so. A named group is read with
`regex.group_named(m, "year")`, and a replacement may use `${year}` or `$year`
(`$year_x` reads the whole name, so use `${year}_x`). Flags go first: `(?i)` ignore ASCII case,
`(?s)` `.` matches newline, `(?m)` `^`/`$` match at line breaks. Patterns and
text are UTF-8 and `.`/classes match a whole character; `\d \w \s` and `(?i)`
stay ASCII-only, so write `\p{L}`/`\p{Nd}` for Unicode letters and digits. `$` matches only at the
very end (unless `(?m)`), so use `^...$` to require the whole text. A repetition
count is limited to 1000 and a compiled pattern to 20,000 instructions.
`find_all` steps one character past an empty match, and `split` ignores empty
matches.

## HTTP client (`client`)

For a response too large to hold, or one that never ends, `client.stream(method: ..., url: ...)`
returns the status and headers at once and the body as an `Iter` of `Result<[byte], ClientError>` blocks
(at most 8192 bytes) read as a `for` loop asks for them; a timeout or read error is a last Err item, and
leaving the loop early closes the connection. It follows no redirects, sends no cookies and does not decode
compression (it asks for `identity`).

`client` lets a program call another service. `client.get(url)`,
`client.post_json(url, body)` and the general
`client.request(method, url, headers, body, timeout_ms, max_redirects,
ca_file)` (everything after `url` is optional, so it is called with named
arguments) return `Result<http.Response, ClientError>`. **Any** HTTP status,
404 and 500 included, is `Ok`; `Err` means the exchange failed: `BadUrl`,
`Network`, `Timeout`, `BadResponse`, `TooLarge` or `TooManyRedirects`
(`error_message` renders one). `http://` and `https://` (certificate
chain and host name verified; `ca_file` names a private CA's PEM file). A
keep-alive connection is parked after a response and reused by the next request
to the same host (`reuse: false` turns that off, `close_idle()` drops them; a
dead parked connection is detected and the request re-sent once). It reads `Content-Length`,
chunked and until-close bodies, follows 301/302/303/307/308 up to
`max_redirects` (default 5, 0 = return the 3xx; 303, and a POST answered
301/302, is re-sent as a body-less GET), and caps a body at 8 MiB. The
body is `[byte]` both ways -- `request` takes `body: Option<[byte]>` and the `http.Response` it returns has a `[byte]`
body, so images, files and a body with a 0 byte pass through untouched; `http.response_text(response)` reads it as
text (an Err for a NUL byte), and `post_json`, `put_json` and `post_form` take their body as text.

Also built in: `gzip`/`deflate` response decoding (`Accept-Encoding` is sent
unless `compress: false`); proxies from the `proxy` argument (`""` = none) or
`HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY`, with a `CONNECT` tunnel for `https://`
targets (an `https://` proxy itself is not supported); Basic auth from
`http://user:pass@host/` (Authorization and Cookie are dropped when a redirect
moves to another host, port or scheme); cookies through
`request_with_jar(jar, method, url, ...)`, which returns the updated jar
(`cookie_jar()` makes an empty one; no jar means nothing is remembered);
`post_form(url, Map)` and `form_encode`; `response_header`. Not yet: HTTP/2.
`parse_url(url)` is public.

## Files (`fs`)

`fs.read_text` (a whole file as a string), `fs.write_text`, `append_text`, `exists`, `is_dir`,
`size`, `remove` (a file or an empty directory), `make_dir`, `rename` and
`list_dir` (sorted names). `fs.lines(path)` reads a file lazily, line by line, as an `Iter`
for a `for` loop (see "For loops"): memory for one line, whatever the file size.
`fs.chunks(path, size)` does the same for the bytes of a file in blocks (`Iter<Result<[byte], string>>`),
and `fs.walk(dir)` finds the files under a directory depth first, sorted, without listing them all
first (`Iter<Result<string, string>>`; directories are not listed, a symlink to one is not followed). Everything that can fail returns `Result<_,
string>` carrying the OS message. (There used to be a separate `io` module
with a fatal-on-failure `write_file`; it was folded into `fs`.)

`fs.resolve(name) -> Option<string>` finds `name` (a file or a directory) by searching
`ZIMAN_RESOURCE_PATH` -- a colon-separated list of directories, checked in order, the first one
that has `name` winning, the same idea as `PATH` or a Java classpath. With the environment
variable unset, `resolve` returns `Some(name)` unchanged, so code that never sets it is unaffected;
with it set and no directory having `name`, `None`. `properties.load` and `migrate.run` both
resolve their file/directory argument through this, so e.g. a Maven-style project can set
`ZIMAN_RESOURCE_PATH=src/main/resources` for its app and
`ZIMAN_RESOURCE_PATH=src/test/resources:src/main/resources` for its tests, and
`properties.load("application")` finds the right `application.properties` in either case with no
code change.

## Logging (`log`)

There is no global logger (no mutable globals): build a `Logger` with
`log.logger(level: Level.Info, file: ..., timestamps: ..., prefix: ...)` and
pass it. `log.debug/info/warn/error(logger, message)` write
`<timestamp> LEVEL prefix message` to stderr, or append to `file` (falling
back to stderr if the file can't be written); anything below the logger's
level is dropped. `with_prefix` makes a per-component logger,
`level_parse("warn")` reads a level from configuration.

## Cryptography

The `crypto` core module is hashes, message authentication, key derivation, password hashing and base64 on
OpenSSL's libcrypto (already linked for TLS) and, for bcrypt, libxcrypt; `[byte]` in and out.

- **Hashes:** `sha1` (broken -- legacy formats and MySQL's login only), `sha256`, `sha512`.
- **Keyed:** `hmac_sha256`, `hmac_sha512`, and `pbkdf2_hmac_sha256` / `pbkdf2_hmac_sha512(password,
  salt, iterations, length)` for turning a password into a key or a storable hash.
- **Passwords:** `password_hash(password)` gives a string to store (Argon2id, PHC format, a fresh random salt inside
  it), `password_verify(password, stored)` checks one of either scheme -- Argon2id, or bcrypt (`$2a$`/`$2b$`/`$2y$`,
  the hashes Spring Security's `BCryptPasswordEncoder` writes) -- and `password_needs_rehash(stored)` says whether to
  replace it with a fresh `password_hash` when the user next logs in (it is bcrypt, or weaker Argon2id). The parts are
  public too: `bcrypt_hash(password:, cost:)` / `bcrypt_verify`, `argon2id_hash(password:, memory_kib:, iterations:,
  parallelism:)` / `argon2id_verify`, and the raw `argon2id_raw`. Comparisons take the same time whatever the contents.
- **Bytes:** `bytes_xor` (Ziman has no bitwise operators) and `bytes_equal`, a comparison that takes the
  same time whatever the contents -- use it for MACs and tokens, not `==`.
- **base64:** `base64_encode` / `base64_decode` (standard, padded, strict) and `base64url_encode` /
  `base64url_decode` (URL-safe, no padding: JWTs).
- **Hex** text of byte arrays is in `number`: `bytes_to_hex` and `bytes_from_hex`.
- **Errors:** a wrong argument (mismatched lengths, an out-of-range iteration count) is a runtime error;
  malformed data (`base64_decode`, `bytes_from_hex`) is an `Err(string)`.
- **Not a general crypto library:** no ciphers and no public-key operations.

### JSON Web Tokens (`jwt`)

`jwt` signs and verifies tokens with a shared secret (HS256, HS384, HS512), on `crypto`, `json` and `clock`.

```
val key = unwrap(base64_decode(secret))                      // a long random key
val claims = jwt.with_claim(jwt.claims("ada@example.com", 3600), "role", Json.JString("admin"))
val token = unwrap(jwt.sign(claims, key))
switch jwt.verify(token, key) {
    Ok(c) { print(unwrap_Some(jwt.subject(c))) }
    Err(e) { print(jwt.jwt_error_message(e)) }
}
```

- **`claims(subject, expires_in_seconds)`** is an object with `sub`, `iat` and `exp`; `with_claim` adds more.
  `sign` chooses the algorithm from the key's size as the common libraries do (32 bytes HS256, 48 HS384, 64
  HS512); `sign_with(claims, key, "HS512")` picks one.
- **`verify(token, key)`** returns the claims or a `JwtError` (`Malformed`, `UnsupportedAlgorithm`, `BadSignature`,
  `Expired`, `NotYetValid`). It refuses `alg: none` and any algorithm that is not one of the three, compares the
  signature in constant time, and checks `exp` and `nbf`; `verify_leeway` forgives a clock difference, and
  `verify_at(token, key, now, leeway)` checks at a chosen time. It does not look at `iss`, `aud` or `sub`.
- Tokens from other libraries verify (checked against RFC 7515's example), and the reverse.

### One-time passwords (`otp`)

`otp` makes and checks the six-digit codes of an authenticator app (TOTP, RFC 6238: HMAC-SHA-1, 30-second steps), in pure Ziman on `crypto`.

```
val secret = otp.generate_secret()                                   // base32, to show the user once
val uri = otp.provisioning_uri("My App", "ada@example.com", secret)  // otpauth://totp/... for a QR code
val key = unwrap(otp.base32_decode(secret))
otp.totp_valid(key, typed_code, now_seconds, 1)                      // this step and one either side
```
`hotp(key, counter, digits)` and `totp(key, unix_seconds)` give the codes themselves; `base32_encode` / `base32_decode` are public too.
The check compares in constant time. The library's tests use the RFC 4226 and 6238 test vectors.

## Configuration (`properties`, `os`)

`os.getenv(name) -> Option<string>` reads an environment variable
(`Some("")` for set-but-empty, `None` for unset). The `properties` module
builds Spring-Boot-style configuration on it.

The rest of `os` is what a long-running program needs to end cleanly:
`os.exit(code)` flushes stdout and stderr and leaves with that status (how a
fatal setup error becomes exit 1 instead of a crash on an `unwrap`);
`os.write_stderr(text)` writes to stderr with no newline added;
`os.install_shutdown_handler()` makes SIGINT/SIGTERM set a flag instead of
killing the process, `os.shutdown_requested() -> bool` reads it, and
`net.accept` returns an error within about a quarter of a second of the
signal so an accept loop can notice, stop taking connections, let the
connections in flight finish and exit normally:

```
os.install_shutdown_handler()
while !os.shutdown_requested() {
    val client = net.accept(listener)
    if is_ok(client) { ... }
}
```

The signal can arrive on any thread, so nothing blocked in a call other than
`accept` notices it; a program that needs to stop other blocking work has to
arrange that itself (for example with a timeout on the socket).

```
use properties

#[derive(Properties)]
struct ServerConfig {
    port: int
    idle_timeout_ms: int
    banner: Option<string>
}

val props = try properties.load("application")          // application.properties (+ -<profile>)
val cfg = try server_config_from_properties(props, "server")
```

- **Format.** `key=value` lines, `#` comments only at line start, lowercase
  dotted kebab-case keys, trimmed values, no quoting/escapes/inline
  comments. A duplicate key, an uppercase key, or a line that is neither
  blank, comment nor `key=value` is an error naming `file:line`.
- **Loading.** `load("application")` reads `application.properties`
  (required) from the working directory, then
  `application-<profile>.properties` if a profile is active: env
  `PROFILE_ACTIVE`, else `profile.active` in the base file. A missing
  profile file is an error. `parse(text, source)` is the pure form. `load`
  resolves the base and profile file names through `fs.resolve` (below)
  first, so a project that sets `ZIMAN_RESOURCE_PATH` finds
  `application.properties` wherever that variable points, unchanged code;
  a project that never sets it sees no difference from before.
- **Precedence.** environment (`server.port` -> `SERVER_PORT`) > profile
  file > base file.
- **Placeholders.** (resolved once at load/parse, so a bad one fails startup) `${name}` / `${name:default}` resolve against
  properties, then the environment (exact name), then the default (which
  may contain `:`); unresolved or cyclic is an error.
- **Getters.** `get_string/int/float/bool` (required); an optional key is `lookup(props, key)` (an `Option<Entry>`) plus `entry_int/float/bool/string`; errors name the key and its source, e.g.
  `server.port: '80x' is not an integer (application.properties:3)`.
- **`check_unknown(props, known_keys)`** fails on keys not in the list,
  suggesting the closest known key; `profile.active` is always allowed.
- **`report(props, keys)`** returns the effective values with their
  sources for a startup log; secrets (keys containing `password`,
  `secret`, `token`, `key`, and credentials inside URLs) are masked.
- **`#[derive(Properties)]`** on a struct of `int`/`float`/`bool`/`string`
  (or `Option` of those) generates `<snake>_from_properties(props,
  prefix) -> Result<T, string>` and `<snake>_keys(prefix) -> [string]`.
  Field `idle_timeout_ms` binds to `<prefix>.idle-timeout-ms`; `Option<T>`
  fields are optional keys, all others required. The consumer needs only
  `use properties`.

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
  rounds** — rounding is `math.round`/`math.decimal_round`'s job, not a
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

## The `math` module

`sqrt`, `pow`, `exp`, `log` (natural), `sin`/`cos`/`tan` (radians), and the constants `pi`/`e`
(global vals — `use math.pi`/`use math.e` to bring them in bare, same as any other global val,
no `math.pi` dotted access; see "Modules" above). All `float` in, `float` out, one-line wrappers
over libm.

```
use math

fn main() {
    print(math.sqrt(2.0))          // 1.4142135623730951
    print(math.pow(2.0, 10.0))     // 1024.0
    print(math.sqrt(-1.0))         // nan
    print(math.is_nan(math.sqrt(-1.0)))  // true
}
```

- **A domain error is NaN**, not a `Result` — `sqrt` of a negative number, `log` of zero or a
  negative number, same as C, Java and JavaScript. Ziman's own `compare<T: Ord>` already gives
  NaN a total order (see "Ordering" above), so it's an ordinary float value to check with
  `math.is_nan(x)` (`x != x`, the IEEE-754 definition) rather than something that needs
  unwrapping. (This is unlike `1.0 / 0.0`, which is a runtime error in Ziman, not an infinity —
  `/` checks for a zero divisor on `float` the same as it does on `int`, so `math` never
  produces a value your own arithmetic couldn't already have produced some other way.)
- **`math.floor`/`math.ceil`/`math.round`** round a `float` to an `int` — down, up, nearest with
  ties away from zero (`math.round(2.5)` is 3, `math.round(-2.5)` is -3) — and take only a
  `float` (an `int` is already rounded). **`math.floor_float`/`ceil_float`/`round_float`** apply
  the same three rules but keep the result a `float`, for a magnitude too large for `int` (they
  are not limited to 2^63, and can hold NaN or infinity) or a value that needs to stay `float`
  for further arithmetic. These used to be bare prelude builtins (`floor(x)` with no `use`); they
  moved into `math` together with the `decimal_*` functions below, so `floor(x)` alone is now an
  undefined function — write `use math` and `math.floor(x)`.
- **No decimal equivalent for the irrational ones.** `sqrt`/`exp`/`log`/`sin`/`cos`/`tan` are
  irrational for almost every input, so there's no exact `decimal` answer to compute — casting
  through `float` (`math.sqrt(x as float) as decimal`, rounded to a scale of your choosing with
  `math.decimal_round`) is the practical route, the same tradeoff `decimal_to_double`/`as float` /
  `as decimal` already make explicit elsewhere. `math.decimal_round`, `math.decimal_floor`,
  `math.decimal_ceil` (rounding to a given number of decimal places: to nearest with halves away
  from zero, toward negative infinity, toward positive infinity) and `math.decimal_pow(base,
  exponent)` (a non-negative integer power) *are* exact, since a whole-number power and a
  rounding are not irrational the way the others are. A negative `exponent` is a runtime error
  naming the `numerator / math.decimal_pow(base, -exponent)` composition instead, since there is
  no scale to pick for that division on `decimal_pow`'s own (`1.0000d / math.decimal_pow(2d, 3)`
  is `0.1250`; a bare `1d`, scale 0, would divide down to `0`).

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

- **`Result<T, E>` is a builtin, not a declared enum.** It is manipulated
  through `ok`, `err`, `is_ok`, `unwrap`, `unwrap_err` (see "Builtins"
  below for exact signatures) or consumed exhaustively with `switch`
  (arms `Ok(v)` and `Err(e)`); you can't define your own `Result`-shaped
  type, but you can declare an `enum` (see "Enums"). `switch` is a statement, not an expression (ROADMAP R21).
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

## Defer

`defer f(args)` runs a call when the enclosing *block* exits, by any path:
falling off the end, `return`, a `try` that propagates an error, `break`, or
`continue`. It is how a resource is released without destructors.

```
fn talk(port: int) -> Result<int, string> {
    val sock = unwrap(net.connect("127.0.0.1", port))
    defer net.close(sock)              // closed on every path out of talk
    val reply = try parse_int(read_reply(sock))   // an early return still closes it
    return reply
}

fn transfer(conn: datasource.Connection) -> Result<int, DbError> {
    try datasource.begin(conn)
    defer datasource.rollback(conn)            // undone unless we reach the commit
    try datasource.execute(conn, "...")
    try datasource.commit(conn)
    return 0
}
```

- **Block-scoped.** A `defer` runs when its own block ends -- an `if` or
  loop body, not just the function. A `defer` inside a loop body therefore
  runs at the end of *every iteration* (unlike Go, where defers pile up
  until the function returns).
- **Last in, first out.** Several defers in one block run in reverse order
  of registration.
- **Arguments are evaluated when the `defer` statement runs**, not when it
  fires: `var n = 1; defer show(n); n = 2` shows `1`. Only the call itself
  is postponed.
- **The return value is computed first.** `return x * 2` evaluates `x * 2`,
  then runs the defers, then returns -- so a deferred call can't change what
  the function returns, and a `try` propagates the same way.
- **A single call only** -- not a block, and not `f()?`. Cleanup that needs
  logic goes in a helper function you defer a call to. (A block form could
  be added later without breaking anything; it would need a second rule for
  when variables are read, which is why it isn't here yet.) The hardcoded
  builtins that read their argument *expressions* rather than values --
  `format`, `ok`, `err`, `channel_new` -- can't be deferred.
- **The result is discarded, including a `Result`.** A deferred
  `net.close` or `datasource.rollback` has nowhere to report failure. Every
  other bare statement whose value is a `Result` is a compile-time error
  (see "Must-use `Result`" below); `defer` is the one exception, checked
  separately, since there is nowhere left to bind or propagate to by the
  time it runs. That is a real cost: an error from the cleanup call is
  invisible. It is deliberate, and it is what lets
  `defer datasource.rollback(conn)` be harmless after a successful commit.
- **A fatal runtime error skips defers.** `unwrap` on an `Err`, an
  out-of-bounds index and the like exit the process instead of unwinding,
  so nothing deferred runs. The operating system closes the sockets, and a
  database server rolls back a dropped connection's open transaction, but
  a program relying on a deferred call for anything else should know.
- **Threads:** a function run by `spawn` gets its defers like any other.

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

- **Builders and iterators stay in their thread.** `StringBuilder`, `ArrayBuilder`,
  `MapBuilder`, `SetBuilder` and `Iter` are mutable, so each belongs to the thread that
  made it: not a `Channel` element, not a `spawn` argument, not a module-level
  `val` (compile errors), and any other use from another thread is a runtime
  error. See "Attributes and derive" (`#[thread_bound]`).
- **`Channel<T>` is a queue, thread-safe by construction, holding values
  of one element type `T`** — unified, not split into separate
  sender/receiver types the way Rust's `mpsc` is: any `Channel<T>` value
  supports both `send`/`receive`, freely passed to whoever needs either
  capability. A channel is **unbounded** (`channel_new()`: `send` never
  blocks) or **bounded** (`channel_new(8)`: at most 8 values wait in it, and
  `send` blocks while it is full, so a fast producer is held to the pace of a
  slow consumer -- backpressure). `receive(chan) -> T` blocks until a value is
  available. `try_send(chan, value) -> bool` and `try_receive(chan) ->
  Option<T>` never block (`false`/`None` when full/empty), and
  `channel_len(chan) -> int` is how many values are waiting. A bounded `send`
  with nobody receiving waits forever; there is no deadlock detection.
- **Closing, timed receives, and waiting on two channels.** `channel_close(chan)` says no more
  values will ever be sent (closing twice is harmless). Values already queued are still
  delivered after a close; it only stops new ones. `send`/`try_send` on a closed channel is a
  runtime error (the producer broke its own promise), and a `send` blocked on a full bounded
  channel is woken by the close and fails the same way. Plain `receive` on a channel that is
  closed *and* empty is a runtime error too, instead of waiting forever -- a code path that
  expects to see a close uses `receive_option(chan) -> Option<T>`, which blocks like `receive`
  but returns `None` once the channel is closed and drained, so a consumer loop needs no
  sentinel value. `receive_timeout(chan, millis) -> Option<T>` gives up after `millis`
  milliseconds with `None`; a timeout and a closed-and-empty channel both give `None` (they are
  not told apart). `select2(a, b) -> Selected<A, B>` waits on two channels, possibly of
  different element types, and returns whichever has a value first: `Selected.First(x)` from
  `a`, `Selected.Second(y)` from `b` (test with `Selected.is_First(r)` and read with
  `Selected.unwrap_First(r)`; `Selected` is a bare prelude enum like `Option`). If both are
  ready `a` wins. It is built by polling both channels with a 1 ms pause between rounds, so it
  can add up to about a millisecond of latency, and it has `receive`'s no-deadlock-detection
  property: two channels that are never sent to wait forever. The names avoid a general
  `Either`, which programs commonly declare themselves and a bare prelude `Either` would then
  collide with.
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
  never a function value (`spawn f(args)` names the function, it does not take
  one from a variable); see docs/DESIGN.md. A generic function is spawned with
  its type arguments written out, `spawn worker<int>(args)`. **The spawned
  function must return nothing (`void`).** There's no thread-handle/join
  mechanism in v0, so a spawned function's result has no way to be
  observed except through a `Channel` it's explicitly given — `spawn` is
  a statement, not an expression, and requiring `void` makes that
  discipline a compile-time rule instead of a convention.
- **A runtime error in a spawned thread ends that thread, not the program.**
  The error is written to stderr (`ziman: runtime error at 5:12: index 3 out of
  bounds ...`, then a line naming the `spawn` that started the thread), the
  thread's pending `defer`red calls run (innermost first, so what it opened is
  closed and what it held is given back), and everything else goes on. An
  error in the main thread still ends the program with status 1. To do
  something else, add an `on_error` clause: `spawn worker(x) on_error
  handler`, where `handler` is a `fn(string)` (a function or a lambda)
  called in the failed thread with the error's text. The handler runs first,
  while the thread's resources are still open, then the deferred calls;
  an error in the handler is reported and does not stop the cleanup. `on_error
  os.fatal` makes a failure in that thread end the whole program, with status 1,
  as one in the main thread does. `server.serve` uses this for every
  connection: a handler that crashes is answered `500` with a generic body, its
  slot and socket are released, and the next request is served. What the jump
  out of the failed thread does not do: it does not run anything but the
  `defer`s (no other cleanup), and a lock a native was holding at the moment of
  the error stays held (none of the library's natives raise errors while
  holding one).
- **What a thread can ask about itself, and per-thread values (`thread`).** `use thread`:
  `thread.id()` (0 for the main thread, then 1, 2, ... in the order threads started),
  `thread.name()` (`main`, or `<function>-<id>` for a spawned thread, e.g. `worker-3`;
  `thread.set_name(s)` renames the calling thread), `thread.is_main()` and
  `thread.stack_remaining()` (bytes left before the "stack depth exceeded" error). There is
  still no join or thread handle. `thread.ThreadLocal<T>` is a slot with a separate `T` on
  every thread, like Java's `ThreadLocal`: make the key once, as a module-level `val`, giving the
  function that makes a thread's first value --
  `val hits = thread.local<int>(fn() -> int { return 0 })` -- then each thread reads and replaces its
  own with `thread.get(hits)` / `thread.set(hits, v)`. `thread.has(key)` says whether the thread has
  a value yet and `thread.reset(key)` forgets it, so the next `get` runs `init` again. A value is
  immutable, so editing a slot means `set` with the new value (`push` on an array is amortised
  O(1)); for a large mutable cache keep a `MapBuilder`/`ArrayBuilder` in the slot -- a builder must
  stay on the thread that made it, which a thread's own slot guarantees, and `ThreadLocal` (marked
  `#[thread_key]`, a struct attribute like `#[thread_bound]`) is the one type that may hold one
  at module level, since the key itself is only a number and a function. A thread's slots are
  dropped when it ends; `server.serve` runs one thread per connection, so a thread-local lives as
  long as a connection, not the whole run -- right for per-request context and scratch space, not
  for a cache that must outlive connections (an actor thread owning the cache, reached over a
  channel, is the tool for that).
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

Recursion is limited by the stack the thread has, not by a fixed number of calls. Every thread knows where its stack ends
and each call and each nested expression checks how much is left; a program that runs it out stops with the runtime error
`stack depth exceeded: this main thread's N MB stack is used up` (never a crash). A Ziman call costs between about 3.5 and 11 KB of
stack depending on the shape of the code, so an 8 MB stack holds about 1,000-2,000 calls and a 256 MB one about 25,000-70,000.

The sizes are chosen when the program starts, from the machine: an eighth of the memory the process may still use (the smaller of
what the machine has available and what its container allows) is the budget for stacks. The main thread (the one that runs `main`)
takes a quarter of it, between 8 and 512 MB; the rest is shared between the threads the program plans for (64 unless told
otherwise), 1 to 64 MB for each `spawn`ed thread. Stacks are reserved address space and only the pages a thread touches use memory,
so these are ceilings. The spawned-thread size is recomputed at most once a second, since memory changes and a thread's stack cannot
once it exists. Override any of it:

| Option | Environment variable | Meaning |
|---|---|---|
| `--stack-mb=N` | `ZIMAN_STACK_MB` | the main thread's stack, 1 to 768 MB |
| `--thread-stack-mb=N` | `ZIMAN_THREAD_STACK_MB` | the stack of each `spawn`ed thread, 1 to 768 MB |
| `--threads=N` | `ZIMAN_THREADS` | how many threads to plan the memory for (default 64; a server with `max_connections` of 500 says 500) |

A flag wins over its variable, which wins over the computed size. The limit is 768 MB because the bundled garbage collector aborts
with stacks near 1 GB. The parser, the type checker and the comparison, hashing and printing of deeply nested values are checked the
same way, so a pathologically nested source file or value is an error and not a crash.

## Builtins

Builtin names are reserved — redefining any of them as a user function is
a type error.

- `print(x)` — takes exactly one argument of any non-void type, writes its
  display form followed by a newline to stdout, returns nothing. A `float`'s display form is the
  shortest decimal text that reads back as the same value — never a fixed number of significant
  digits — and always has a `.` or an `e` in it even at a whole-number magnitude (`1.0`, not
  `1`, so it's never confusable with an `int`'s own text); plain notation for magnitudes from
  `1e-7` up to `1e21`, scientific notation (`1.5e+300`) outside that. `format`'s `{}` placeholder
  displays a `float` argument the same way.
- `concat(a: string, b: string) -> string` — joins two strings. Both
  arguments must already be `string`; there's no auto-stringify (that
  would be the same implicit-coercion problem `+` avoids, just moved to a
  function). Use `str` to convert first.
- `str(x) -> string` — converts an `int`, `float`, `bool`, `bigint`,
  `decimal`, or `byte` to its display string (a `float` the same shortest-round-trip way
  `print` does). Rejects an argument that's already a `string` (there should be exactly one way
  to get a string from a value, not two) — and, being a fixed allowlist of concrete types
  checked before a generic function's own type parameters are resolved, also rejects a plain
  type-parameter `T` even when every type it could be instantiated with would be fine.
  `format`'s `{}` placeholder has no such restriction (same display code, checked per call site
  instead of per declaration), so a generic function wanting to display a `T` uses that instead.
- `printf(fmt, args...)` — `format`, written straight to standard output
  with **no newline added** (put `\n` in the format if you want one) and
  flushed, so a prompt or a progress line appears immediately. Same rules as
  `format`: a string-literal `fmt`, `{}` placeholders (not C's `%d`/`%s` --
  see below for why), count checked at compile time. Returns nothing.
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
  len(s)`, a runtime error otherwise. **It does not copy:** a string is a
  pointer plus a length over immutable bytes, so `substring`, `trim`,
  `char_at` and the pieces `split` returns are views into the original --
  free to make, and `len` is O(1). (Nothing observable changes: strings are
  immutable and compare by content. What a slice does keep alive is its
  whole parent buffer, which matters once memory is reclaimed.)
- `split(s: string, sep: string) -> [string]` — splits `s` on every
  occurrence of `sep`. `sep` must not be `""` (runtime error) — an empty
  separator has no well-defined split points. Empty pieces (adjacent
  separators, or a leading/trailing separator) are kept, so `len(result)`
  is always `count(sep in s) + 1`.
- **`[byte]`, `[bool]`, `[int]` and `[float]` are stored packed** (one real
  byte / byte / 64-bit integer / double per element, not one boxed value of
  80 bytes), which is invisible to programs except for cost: a million-element
  `[int]` is 8 MB. Everything below (indexing, `for`, `push`, `slice`, `==`,
  `hash`, `compare`) behaves identically for them.
- `slice<T>(a: [T], start: int, end: int) -> [T]` — the elements from
  `start` up to (not including) `end` (`0 <= start <= end <= len(a)`, a runtime
  error otherwise). **It does not copy:** arrays are immutable, so the slice
  shares the original's element storage, like `substring` on a string; it is
  free to make and keeps the whole original alive. (`push`/`insert`/`set` on
  either one build a new array, so neither can change under the other.)
- `concat_arrays<T>(parts: [T]...) -> [T]` — one array from many, in a
  single allocation: `concat_arrays<int>(a, b)` or
  `concat_arrays<int>(parts: arrays)`. (The empty-`[]` literal can't be
  inferred as a variadic argument; pass a typed `val`.)
- `char_at(s: string, i: int) -> string` — the one-byte string at index
  `i` (`0 <= i < len(s)`, a runtime error otherwise); `char_at(s, i) ==
  "x"` replaces `substring(s, i, i + 1) == "x"`.
- `split_once(s: string, sep: string) -> Option<[string]>` —
  `Some([before, after])` split at the *first* `sep`, `None` if it is
  absent; for `key=value`-shaped text whose value may itself contain the
  separator (`split` would cut it up).
- `index_of(s: string, needle: string, from: int) -> int` — the byte index
  of the first `needle` at or after `from` (`0 <= from <= len(s)`, a
  runtime error otherwise), or `-1`; an empty `needle` matches at `from`.
- `last_index_of(s: string, needle: string) -> int` — the byte index of the
  last `needle` (`len(s)` for an empty one), or `-1`.
- `starts_with(s, prefix) -> bool` / `ends_with(s, suffix) -> bool`.
- `is_char_at(s: string, i: int, c: string) -> bool` — whether `c` appears
  in `s` starting at `i`; `false` (never an error) when `i` is out of range
  or `c` doesn't fit, so it replaces the guarded `p < len(s) &&
  char_at(s, p) == "x"`. `c` is normally one character, but any text works
  (a literal matched at a position).
- `replace(s, old, new) -> string` swaps every non-overlapping `old`
  (`old` must not be empty).
- `equals_ignore_case(a: string, b: string) -> bool` — equality with ASCII
  letters compared case-insensitively (non-ASCII bytes compare exactly);
  allocates nothing, unlike `to_lower(a) == to_lower(b)`.

  None of the search/compare functions above build intermediate strings,
  unlike the `substring(s, i, i + n) == x` idiom they replace.
- `trim(s: string) -> string` — `s` with leading/trailing whitespace
  removed.
- `is_empty(s: string) -> bool` — `s == ""`; `is_blank(s: string) -> bool` —
  `s` is empty or only whitespace (what `trim` removes).
- `to_upper(s: string) -> string` / `to_lower(s: string) -> string` —
  ASCII case conversion.
- `parse_int(s: string) -> Result<int, string>` / `parse_float(s: string)
  -> Result<float, string>` / `parse_bigint(s: string) -> Result<bigint,
  string>` / `parse_decimal(s: string) -> Result<decimal, string>` —
  parses all of `s` as a number; `Err` (with a message) if `s` has
  leading/trailing junk or doesn't parse at all.
- `contains(s: string, needle: string) -> bool` / `contains(arr: [T], x:
  T) -> bool` — substring test, or array membership test via `==`. The
  array form requires `T` to support `==` — same rule as array `==`/`!=`
  itself (see "Arrays" above).
- `compare(a: T, b: T) -> int` — negative, zero or positive (only the sign
  means anything); `T` is any type with a
  total order (see "Ordering" below). The way to order a struct or enum;
  `<`, `>`, `<=`, `>=` are for numbers, strings and bytes only.
- `hash(x: T) -> int` — a hash of `x`. **`T` is any type that supports `==`
  and has only hashable parts**: `int`, `float`, `bool`, `string`, `bigint`,
  `decimal`, `byte`; an array, `Option`, `Result`, struct or enum built
  from hashable types, however deeply, including one that refers back to
  itself (`Json`, a recursive `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`
  — see docs/DESIGN.md's "Recursive Eq/Hash" entry) — so `Uuid`, a
  `(patient, year)` struct, a `Rank` enum and `Json` itself all work; or a
  type parameter bounded by `Hash` (see "Generics" above). The hash is
  structural -- the same shape `==` compares
  -- so `a == b` implies `hash(a) == hash(b)` with no function to write; a
  channel, or anything `==` rejects, is not hashable. `-0.0` and `0.0` hash
  alike (they are equal); a `NaN` key is never found, since it does not equal
  itself. Not cryptographic and not stable across
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
- Rounding (`floor`/`ceil`/`round`, `decimal_round`/`decimal_floor`/`decimal_ceil`) and
  `decimal_pow` are not builtins any more: they live in the `math` module (see "The `math`
  module").
- `push(arr: [T], x: T) -> [T]` — a new array with `x` appended. `arr = push(arr, x)` in
  a loop is amortised O(1) per call: the result keeps up to twice its length in spare
  room, and the next `push` on the *newest* version of that array writes into it
  instead of copying (a `push` on an older version still copies, so no version ever
  changes under another). `insert`, `set`, `remove` and `concat_arrays` always copy.
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
- `channel_new() -> Channel<T>` / `channel_new(capacity: int) ->
  Channel<T>` — construct an unbounded / a bounded (capacity >= 1, a runtime
  error otherwise) `Channel`. Only legal where the missing type parameter is
  available from context — see "Concurrency" above.
- `send(c: Channel<T>, value: T)` — enqueue `value`; blocks while a bounded
  channel is full (an unbounded one never blocks).
- `receive(c: Channel<T>) -> T` — dequeue the next value, blocking until
  one is available.
- `try_send(c, value) -> bool`, `try_receive(c) -> Option<T>`,
  `channel_len(c) -> int` — the non-blocking forms and the queue length.

None of `push`/`insert`/`set` mutate their `arr` argument — see "Arrays"
above for why, and why that's true regardless of `val`/`var`.

`+` itself stays numeric-only in v0 — see docs/DESIGN.md principle #2 on
why string concatenation didn't just overload it.
