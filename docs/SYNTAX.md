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
top_level_decl := [ "pub" ] ( function_decl | native_fn_decl | struct_decl | enum_decl )

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
param          := IDENT ":" type
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
  int` are the only casts, and only between `int` and `float` (never
  arrays, structs, enums, Results, or Channels).

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
- **Same-module only, for now.** An enum can be constructed and consumed
  anywhere in the file that declares it — including from a different
  function — but there's no cross-module (`use`-qualified) enum access
  yet, tracked in ROADMAP.md.
- **No generics.** `enum Shape { ... }` above is a concrete, monomorphic
  declaration — there's no `enum Option<T> { Some(T) None }` yet, since
  that needs real type parameters on top of this, a separate, larger
  design pass (see ROADMAP.md's "Generics" entry).

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
  -> Result<float, string>` — parses all of `s` as a number; `Err` (with
  a message) if `s` has leading/trailing junk or doesn't parse at all.
- `contains(s: string, needle: string) -> bool` / `contains(arr: [T], x:
  T) -> bool` — substring test, or array membership test via `==`. The
  array form requires `T` to be a scalar (`int`/`float`/`bool`/`string`)
  — same restriction as array `==`/`!=`, since only scalars have a
  well-defined equality in v0.
- `abs(x: T) -> T` — absolute value; `T` is `int` or `float`.
- `min(a: T, b: T) -> T` / `max(a: T, b: T) -> T` — `T` is `int` or
  `float`; both arguments must be the same `T`, no implicit `int`/`float`
  mixing (consistent with the language's general no-implicit-coercion
  rule).
- `floor(x: float) -> int` / `ceil(x: float) -> int` / `round(x: float)
  -> int` — round towards negative infinity / positive infinity /
  nearest (ties away from zero). All three take only `float` (an `int`
  is already rounded) and return `int`, since that's the type the result
  actually is.
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
