# Ziman — v0 Syntax Reference

File extension: `.zm`.

## Lexical rules

- Comments: `// to end of line`. No block comments in v0.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Casing is **enforced, not just conventional**: a
  `val`/`var`/parameter/function name must not start uppercase, and a `struct`/`enum`/
  enum-variant name must -- this is what makes struct-literal and enum-access parsing
  unambiguous everywhere a primary expression can appear, including an `if`/`while` condition.
- Integer literals: `[0-9]+` (decimal only in v0).
- Float literals: `[0-9]+\.[0-9]+` (a digit is required on both sides of
  the dot — `.5` and `5.` are not valid literals), optionally with an exponent:
  `[0-9]+(\.[0-9]+)?[eE][+-]?[0-9]+` (`1e3`, `2.5E-2`, `1.5e-10`). An exponent
  makes the literal a `float` even without a point (`1e3` is `1000.0`), and it is
  only read when digits follow the `e` (`1e` is still `1` and a name). An
  exponent literal takes no `n`/`d`/`b` suffix (`1e3d` is a lexical error): write
  a decimal as `15000d` or `1500.5d`.
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
- Statement termination: a newline ends a statement unless the token before it can't end one
  (a binary operator, `,`, `(`, `=`, or a keyword expecting more), checked against the previous
  token only -- never indentation. In practice: write code the way you'd write Go.
- **Every comma-separated list can span multiple lines** -- a struct literal's fields, a `with`
  override list, a call's arguments, an array literal's elements, a declared parameter list all
  tolerate a line break after each item/comma, the same tolerance a `struct`/`enum`
  *declaration* already had. A trailing comma before the close (`Point { x: 1, y: 2, }`) is
  still a syntax error -- only the line break is tolerated, not an extra comma.

## Grammar (informal EBNF)

```ziman ignore: a grammar, not Ziman
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
param          := IDENT ":" [ "variadic" ] type
                    (* "variadic" marks a variadic parameter, only legal on
                       the last param in the list -- see "Functions" --
                       "Variadic parameters" below *)
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

(* TYPE_IDENT is an IDENT starting with an uppercase letter. A struct literal is recognized
   purely by that casing in every position a primary expression can appear, including an
   `if`/`while` condition -- no restricted position the way Rust needs for bare struct literals.

   qualified_call/qualified_struct_lit/enum_access look identical to plain field access
   (IDENT "." IDENT) until the parser sees what follows the second identifier and, for
   enum_access, the first one's casing -- falling back to ordinary `.field` access (postfix)
   if none match. A module qualifier is always lowercase (enforced on `use ... as`), so an
   uppercase first identifier can only mean enum_access. *)
```

Left out of v0 (tracked in ROADMAP.md): a call as a primary only chains off a plain or
single-qualified identifier, never an arbitrary expression (a function *value* is called
through a local name -- `make()(x)` isn't valid); no indexed/field *assignment*
(`arr[i] = x`/`p.x = 1` aren't valid). `if`/`switch` *are* usable as expressions in a
restricted set of positions -- see "`if` and `switch` as values" below.

## Types

- `int` — 64-bit signed integer. **Never wraps**: `+ - * /` (`MIN / -1`), unary `-`, `abs(MIN)`
  overflow are a runtime error, consistent with `bigint as int`/`int as byte` clamping instead
  of wrapping. A literal must fit `[-9223372036854775808, 9223372036854775807]` (overflow is a
  compile error naming the `n` bigint suffix, not silent saturation). `float as int` truncates
  toward zero and saturates to `INT64_MAX`/`INT64_MIN` (NaN is 0).
- `float` — double-precision IEEE 754. `bool` — `true`/`false`. `string` — immutable text.
- `bigint`/`decimal`/`byte` — see "Numbers" below for the full rules; in brief, arbitrary-
  precision integer (`n` suffix), arbitrary-precision exact decimal (`d` suffix), and an
  unsigned 0-255 non-arithmetic scalar (`b` suffix) — bare digits always mean `int`/`float`.
- `unit` — the type with exactly one value, also written `unit`: what "nothing to give back"
  looks like where a value is required (`Result<unit, DbError>`, a generic argument, `-> unit`).
  A bare `return` in a `-> unit`/`Result<unit, E>` function is `return unit` (auto-wrapping to
  `Ok(unit)` in the latter); a `-> unit` function running off its end returns `unit`. A function
  with *no* declared return type still has no value (`val x = f()` on one errors) -- declare
  `-> unit` when the caller wants one. `==`-comparable, hashable, prints as `unit`; a word, not
  a keyword (the type where a type is expected, an ordinary name otherwise, so a JSON field
  called `unit` is fine).
- `[T]` — a growable, homogeneous, indexed array, nesting freely (`[[int]]`). Value semantics
  like everything else in Ziman: assigning/passing always yields an independent value, never a
  shared reference. See "Arrays".
- A `struct` name — a named, nominal aggregate. Same name required for two structs to be the
  same type, even with identical fields (`Point{x,y}` and `Vector{x,y}` don't unify). Value
  semantics. See "Structs".
- `Result<T, E>` — a success carrying `T` or a failure carrying `E`; a hardcoded parametric
  type like `[T]` (not general user-definable generics), so `T`/`E` can be any Ziman type but
  you can't declare your own `Result`-shaped type. `<>` not `[]`, since `[]` stays scoped to
  sequences. See "Result".
- An `enum` name — a named, nominal tagged union, one declared variant optionally carrying one
  payload. Unlike `[T]`/`Result`, a type *users* declare, but still concrete (monomorphic), not
  generic on its own. See "Enums".
- `Channel<T>` — a thread-safe FIFO queue and Ziman's concurrency primitive (`send`/`receive`,
  `spawn`). A third hardcoded parametric type, but not a generics shortcut like the other two --
  its real value (safe blocking send/receive across real OS threads) is inherently native. See
  "Concurrency".
- **No implicit conversion between any scalar type.** `as` is the only cast among
  `int`/`float`/`bigint`/`decimal` (never arrays/structs/enums/`Result`/`Channel`) -- each
  lossy in at least one direction (see "Numbers"). `byte` only casts to/from `int`, saturating
  on the way in.

## Numbers

`bigint` (arbitrary-precision integer) and `decimal` (arbitrary-precision exact decimal —
an unscaled `bigint` magnitude plus a scale, exactly Java's `BigDecimal` shape) extend
`int`/`float` for exact, unbounded arithmetic. Neither ever implicitly mixes with any
other numeric type — cast explicitly with `as`, same rule as everywhere else.

```ziman
fn factorial(n: bigint, limit: int, i: int) -> bigint {
    if i > limit { return n }
    return factorial(n * (i as bigint), limit, i + 1)
}
fn main() { print(factorial(1n, 100, 1)) }   // a 158-digit number
```

**`bigint`**: literal needs `n` (`12345678901234567890n`; bare digits always mean `int`,
no suffix on a fractional literal). All of `int`'s operators/builtins work (`+ - * / %`,
unary `-`, `< > <= >= == !=`, `str`, `abs`, `min`/`max`, `hash`, `parse_bigint`); `/`/`%`
truncate toward zero like `int`, not floor division; division/modulo by zero is a runtime
error. Casts: `bigint as int`/`as float` saturate to the type's min/max rather than wrap;
`int as bigint` is exact; `float as bigint` truncates the fractional part then converts
the exact remaining value (NaN/infinity is a runtime error, same for `float as decimal`).
Heap-allocated internally, unlike `int`/`float`.

```ziman
use math

fn main() {
    val total = 19.99d + (19.99d * 0.0825d)
    print(total)             // 21.639175 -- exact, no binary rounding drift
    print(10d / 3d)          // 3 -- rounds half-up to the operands' own scale
    print(math.decimal_round(1.2345d, 2))  // 1.23 (`use math`)
}
```

**`decimal`**: literal needs `d` (`19.99d`, `20d`; bare digits always mean `float`/`int`).
All of `float`'s operators/builtins work, plus `parse_decimal` and
`math.decimal_round(d, scale)` (a separate name, not a `round` overload — Ziman has no
user-level function overloading). `+`/`-` align scale first and are always exact; `*`
needs no alignment and is exact; `/` doesn't always terminate, so it rounds
half-up-away-from-zero to `max(a.scale, b.scale)` (`10d / 3d` is `3`, `-2d / 3d` is `-1`);
`%` is an exact remainder, no rounding. Division/modulo by zero is a runtime error.
`==`/`hash` ignore trailing-zero scale differences (`1.0d == 1.00d`, unlike Java's
`BigDecimal.equals()`). Casts are lossy through `float` either way, and truncate (not
round) going to `int`/`bigint`; `int`/`bigint as decimal` are exact.

```ziman fragment
print(300 as byte)   // 255 -- saturates, doesn't wrap
print(-5 as byte)     // 0
```

**`byte`**: a fifth scalar, unsigned 0-255, but not a number to compute with — no
arithmetic, no `abs`, only `== !=`, `str`, `hash`, and `compare`/`sort`/`min`/`max` (it
satisfies `Ord`; ordering isn't arithmetic). Literal needs `b` (`200b`); out-of-range is a
compile-time error (`300b` fails to parse), not a silent wrap, since a literal's value is
already fully known at parse time. Casts only to/from `int`: `int as byte` saturates to
`[0, 255]`, `byte as int` is always exact; no cast to/from `float`/`bigint`/`decimal`.

All three extra scalar types work everywhere `int`/`float` already do: a struct field, an
array element (`[byte]` supports `==`/`contains`, since `byte` is scalar), a generic type
argument, and a `Map`/`Set` key where `Eq`/`Hash` apply.

## Arrays

```ziman fragment
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

- **No indexed assignment** -- `arr[i] = x` isn't valid syntax. Every mutation follows one
  pattern, `arr = builtin(arr, ...)`, the same shape as updating a scalar `var`. `push`/`insert`/
  `set` are pure, never mutating their `arr` argument, regardless of `val`/`var` (a value has no
  notion of mutability in Ziman, only a *binding* does).
- **An empty array literal (`[]`) has nothing to infer a type from** -- legal only where an
  expected array type is already known (an annotated `val`/`var`, a struct field/`with`
  override, a `return`, a call argument, an enum variant's payload); elsewhere it's a type
  error. A non-empty literal infers its element
  type from the first element, requiring every other to match exactly (`[1, "two"]` errors).
- **Indexing and `set`/`insert`'s index are bounds-checked at runtime** (a computed index can't
  be checked earlier) -- out of range is a runtime error, like division by zero. `insert`'s
  range is `[0, len(arr)]` inclusive (append included); `set`'s/indexing's is `[0, len(arr))`.
- **`==`/`!=` works whenever the element type itself supports `==`** (a scalar, or a
  struct/enum/`Result`/array of comparable parts, recursively) -- compares length then elements;
  an array of `Channel`s or anything else incomparable is a type error.
- **`push` in a loop costs what you'd expect; `insert`/`set`/`remove` don't.** `arr = push(arr,
  x)` is amortised O(1) (spare room kept, the next `push` on that exact array writes into it
  instead of copying); `insert`/`set`/`remove` always copy the whole array, so replacing every
  element one `set` at a time is O(n²) -- `MutableArray<T>` (`mutable_array_new`,
  `mutable_array_push`, `mutable_array_get`, `mutable_array_set`, `mutable_array_insert`,
  `mutable_array_remove`, `mutable_array_truncate`, `mutable_array_swap`, `mutable_array_reverse`,
  `mutable_array_filter`, `mutable_array_sort`/`_desc`/`_with`, plus the bare `len`, `contains`,
  `clear` and `snapshot`) is the right tool for building/editing a
  large array piece by piece. `insert` takes `0..len` inclusive, so inserting at the length appends;
  `remove` answers what it removed, which is why there is no `pop`. Both are O(n), where `push` is
  amortised O(1) -- a loop of `mutable_array_insert(xs, 0, v)` is quadratic, and that is why there
  is no `push_front`: the name would read as `push`'s twin and promise a cost it cannot give. Build
  tail-first and `mutable_array_reverse` once, which copies nothing. `mutable_array_filter` keeps
  what its predicate accepts, in place and **allocating nothing** -- the only filter in the language
  that doesn't; the three sorts land their result back in the same buffer but do make two copies
  getting there (the snapshot, and the sorted array), which the prelude says plainly rather than
  implying in-place means free. It
  reads back in place: `len(xs)`, `mutable_array_get` and `for x in xs` cost nothing, where
  `snapshot` copies the whole buffer every call (`--lint` flags that in a loop). See
  "Builtins" for exact signatures.

## Maps and sets

`Map<K, V>` and `Set<T>` are built-in types. Keys must support `==` and hashing (the
`Hash` rule: scalars, and an array/struct/enum/Result made of hashable parts, however deeply, including one that
refers back to itself (`Json`, or a recursive `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }`); a type
parameter bounded by `Hash`; a value may be anything.

```ziman ignore: the `for` lines end in `...` to show the binders, not a body
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

- **Helpers** (return new collections, infer types from arguments): maps --
  `map_get_or(m, k, default)`, `map_merge(a, b)` (`b` wins on shared keys), `map_filter(m, keep:
  fn(K, V) -> bool)`, `map_remove_all(m, keys)`, `map_from_arrays(keys, values)` (equal lengths,
  else runtime error; a repeated key keeps the last value); sets -- `set_union`/`set_intersect`/
  `set_difference(a, b)` (`a`'s order, `b`'s extras appended for union), `set_is_subset(a, b)`,
  `set_from_array(items)`, `set_insert_all(s, items)`, `set_filter(s, keep: fn(T) -> bool)`.
  A type argument a lambda's result would have to supply is inferred only when the lambda is fully
  annotated (every parameter typed and `->` written), which is the same information a named function
  gives; an unannotated lambda takes its types *from* the expected type, so it has none to contribute
  and the call needs them written: `map_map_values<K, V, R>(m, f)` applies `f: fn(V) -> R` to every value (keys/order unchanged);
  `set_map<T, U>(s, f)` merges elements mapping to the same value; `map_entries<K, V>(m)` is an
  array of `MapEntry<K, V> { key, value }` in map order. In `collections`: `frequencies<T>`
  counts into `Map<T, int>`, `index_by<T, K>(items, key)` builds `Map<K, T>` (later element wins;
  `group_by` keeps them all), `join(items: [string], sep)` puts `sep` *between* the pieces (the
  builtin `join` is a path join over strings, not this), and `sum(items: [int])` totals them --
  integers only, since `+` traps on overflow so the answer is exact or an error, where a float
  total depends on summation order and `fold` expresses whichever order you want.
- **The mutable collections**: `MutableMap<K, V>` (`mutable_map_new`/`_set`/`_get`/`_get_or`/
  `_remove`) and `MutableSet<T>` (`mutable_set_new`/`_add`/`_remove`), the `MutableArray`
  counterparts. Each mutates its own private trie in place (O(n) total instead of copying per
  entry) -- use one in a loop adding many entries. Type arguments are written out
  (`mutable_map_set<string, int>(m, "a", 1)`). **The four questions every collection answers have
  no per-type names**: `len(m)`, `contains(m, k)`, `snapshot(m)` and `clear(m)` work on all four
  mutable types, where there used to be `mapb_len`, `setb_contains`, `mapb_to_map` and the rest.
  `snapshot` copies into an immutable `Map`/`Set` (O(n), the handle stays usable, and later changes
  do not touch the copy). One thread at a time -- every `Mutable*` type belongs to the thread that
  made it.
- **Values, like arrays** -- `set`/`insert`/`remove` return a new collection, leaving the
  argument untouched. A persistent hash trie internally, so an update copies only a few nodes:
  building an n-entry map is O(n log n).
- **Insertion order**: `map_keys`/`map_values`/`set_items`/`for`/`print` all go in first-inserted
  order; overwriting a key keeps its place, removing and re-adding sends it to the end. `for`
  walks a snapshot (rebinding the collection mid-loop doesn't change what's visited or error).
- **Equality/hashing ignore order** -- `==` is true for the same entries (a map's values must
  themselves support `==`), and equal ones hash equal, so either can be a struct field, map key,
  or set element.
- **`#[derive(Json)]`** writes `Map<string, V>`/`Map<int, V>` as a JSON object (an int key
  becomes decimal text) and `Set<T>` as an array (duplicates collapse on decode); other key
  types are a derive error. `Row`/`Properties` don't support them.
- **`NaN` as a key**: a key is matched by an equality that is reflexive on `NaN`, which `==` is
  not. `nan == nan` is `false` (IEEE, as everywhere else in the language), but a `NaN` stored in a
  `Map`, `Set` or array is found again by `map_get`/`contains`, and storing it twice keeps one
  entry -- otherwise a key could never be retrieved, and a `Map<float, _>` would grow without bound
  while `map_get` returned `None`. `compare(nan, nan)` is `0` and agrees with key lookup. This is the
  same line Java draws between `==` and `Double.equals`. `-0.0` and `0.0` are one key and `==` also
  equates them, so nothing differs there.
- **Names**: `Map`/`Set` are reserved words. `map_get`, `map_keys` (`Set<K>`), `map_values`
  (`[V]`), `set_items` (`[T]`) take their collection's type from the argument -- no type
  arguments written.

## For loops

Three forms, chosen by binder count and whether the right-hand side of
`in` is a range or an array:

```ziman ignore: shapes of `for`, with `...` standing in for a body
for x in arr { ... }          // value only -- x: T, once per element
for i in 0..len(arr) { ... }  // index only -- i: int, counting a range
for i, x in arr { ... }       // index + value together
```

- **`for x in ab`** also walks a `MutableArray<T>`, in both shapes an array takes (`for x in ab`
  and `for i, x in ab`) -- which is how a builder is read without `snapshot`'s copy. It reads
  the live buffer: a body that appends sees its own additions, as it would in the array the
  builder is becoming, and one that calls `clear` ends the loop. Neither can read past the
  end, since the length is read again on every pass.
- **`for x in iter`** also walks an `Iter<T>` (a lazy source like `fs.lines(path)`), one binder,
  no index. Nothing is read ahead (the source can exceed memory); `break`/`return`/`try` inside
  the loop closes it, and one never finished is closed on garbage collection. Take items by hand
  with `iter_next(it) -> Option<T>`, release early with `iter_close(it)`. Walked once (a second
  `for` picks up where the first stopped). `iter_of_array(items)` makes one from an array;
  `iter_from<T>(next, close)` from your own code (`next` returns `Some`/`None`, `close` runs
  once when finished/left early/closed -- keep position in what a captured closure calls, since
  a closure can't mutate what it captured). Not GC-closed like built-in sources -- finish/close it.

  ```
  val lines = fs.lines("app.log")?            // Iter<Result<string, string>>
  var errors = 0
  for line in lines {
      val text = line?                        // a read error is an Err item
      if contains(text, "ERROR") { errors = errors + 1 }
  }
  ```

- **`a..b` is exclusive of `b`** (`[a, b)`, both `int`, same convention as `substring`); `a >= b`
  runs the body zero times, not an error.
- **`a..b` isn't a first-class value** -- legal only directly after `in` in a `for` header, no
  `Range` type to store/pass/print. Deliberate; see docs/DESIGN.md's "Rationale consolidated
  from SYNTAX.md" entry.
- **The two-binder form only works over an array, never a range** (`for i, j in 0..5` is a
  syntax error naming the second binder) -- nothing to name separately from the count.
- **Both loop variables are freshly bound each iteration and always immutable** (assigning to
  one is the usual "cannot assign, declared with 'val'" error) and **scoped to the loop body
  only**.
- **`for i in a { ... a[i] ... }` is a compile error.** `i` is the *element* here, not its
  position (unlike Go/JS/Python habits), so indexing with it silently computes the wrong thing.
  The compiler rejects that exact shape and names both fixes: `for i in 0..len(a)` for
  positions, `for i, item in a` for both. A reused name (a range loop, an inner loop) shadows
  the binder and indexing a *different* array is fine. Deliberately narrow -- doesn't catch
  `for i in a { print(i) }`, and rejects the rare case of an array whose elements are its own
  indices (`for x in next { ... next[x] ... }`, write with an index loop instead).
  **Naming convention** (not enforced): `i`/`j`/`k` for a range binder, a domain name (`user`,
  `item`) for a value binder -- since a single binder's meaning depends on `expr`'s type, not
  its own syntax.
- **`break` exits the nearest enclosing loop immediately; `continue` skips to its next
  iteration.** Both work in `while` and every `for` form; outside a loop (even inside an `if`,
  by lexical nesting) is a compile error. Neither crosses a function boundary -- a `break`
  inside a called function is an error there, on its own.
- **No labels** -- `break`/`continue` always target the nearest enclosing loop.

## Function values

A function can be passed around: its type is `fn(A, B) -> R` (no `-> R` for a
function that returns nothing). Two things are values of that type: the name of
a top-level function, and a *lambda*, an anonymous function written in place.

```ziman
use collections

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

- **A lambda is `fn(params) -> R { body }`** (no `-> R` returns nothing), an expression usable
  anywhere one is:
  ```
  val add = fn(x: int) -> int { return x + 1 }
  ```
  `return`/`try`/`defer` inside belong to the lambda; `break`/`continue` cannot leave it.
- **Shorter forms.** A lambda's final bare expression is its result when the return type is
  known non-void, so `return` is only for early exits (`fn(a: int, b: int) -> int { a + b }`).
  Where context says what function type is wanted (an argument, a declared-type `val`/`var`, an
  assignment, a `return`, a struct field), parameter types and `->` can be left out and taken
  from it (`fn(n) { n * 2 }`). With no such context, parameter types must be written, and an
  omitted `->` means "the final expression's type" (nothing returned if it doesn't return one).
  A lambda using `return` needs `-> T` unless context gives it. A wrong arity or wrong body type
  is a compile error naming the expected function type.
- **What a lambda captures.** Any *immutable* local the body uses (a `val`, a parameter, a loop/
  `switch` binder, another function value) is captured **by value**, copied at evaluation -- a
  `var` can't be captured (copy into a `val` first), so mutable state is never shared and a
  function value is always safe to send to another thread. Global `val`s are used, not
  captured. Lambdas nest and capture through every level; a name declared inside one shadows an
  outer variable of the same name.
- **`val NAME = fn(params) -> R { ... }` lets the body call itself by `NAME`** -- never `var`
  (ambiguous "self"), only with an explicit return type (an omitted one is inferred *from* the
  body, which self-reference would need to already know):
  ```
  val fact = fn(n: int) -> int { return if n <= 1 { 1 } else { n * fact(n - 1) } }
  ```
  The only recursion a lambda gets -- no mutual recursion between separate lambdas. An
  omitted-return-type or `var`-bound lambda gets no special treatment (self-call is an ordinary
  "undefined function" error). A parameter sharing `NAME` is a compile error naming both.
- **A reference to a named top-level function** is the other kind of value (`apply(double, 21)`),
  carrying nothing.
- **What can be a value**: any top-level function (own module, imported, or a `native fn`) with
  no variadic parameter. A **generic** function is a value once its type arguments are written
  (`identity<int>` is `fn(int) -> int`, checked against bounds like a call); without them it's
  an error. A *native* generic function can't be a value (wrap in a lambda); neither can a
  builtin (`len`, `print`).
- **Calling**: `f(x)` where `f` is a parameter or `val`/`var` of function type -- arguments
  checked against parameter types, with the same `Option`/`Result` auto-wrap (named arguments
  and omitted `Option` parameters don't apply). A local always wins over a top-level function of
  the same name. Only a *name* can be called: `make()(x)`/`s.handler(x)` aren't valid.
- **Where they can go**: parameters, returns, `val`s, struct fields, arrays, `Option`/`Result`,
  channels -- so a function value can cross a thread (`spawn f(x)` still names statically).
- **What they can't do**: be compared/hashed/ordered/serialized (a derive on a struct with a
  function field errors); `print` shows `<function name>`.
- **In the standard library**: `collections.map`/`filter`/`flat_map`/`fold`/`count`/`any`/`all`/
  `find`/`first`/`last`/`min_by`/`max_by`/`distinct`/`group_by`/`for_each`/`sort_with`, type
  arguments written at the call. Pass a lambda for anything needing the enclosing scope.

## Structs

```ziman
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

- **Fields are typed like function parameters** (`name: type`), one per line inside `{}`, no
  comma separator -- a `struct` body reads like a function body.
- **Construction requires every field, named, in any order**: `Point { x: 0, y: 0 }`. No
  positional construction (`Point(0, 0)`) -- with two same-typed fields that's a silent
  swapped-argument bug. A missing/unknown/repeated field is a type error.
- **No field assignment (`p.x = 1`) and no methods.** Value-typed like arrays -- a function
  taking a `Point` can never mutate the caller's copy. `with` produces a *new* struct with named
  fields replaced, everything else copied from the base (`val p2 = with p { x: 99 }`) -- unlike
  construction, unmentioned fields are kept, not required.
  **Functions operate on structs, not methods** (`distance_squared(a, b)`, not
  `a.distance_squared(b)`) -- no `impl` blocks, no inheritance (see docs/DESIGN.md's
  classes-vs-traits discussion). A field that *holds* a function value is the one exception to the
  shape, not to the rule: `h.run(21)` calls the function in `h`'s `run` field, which is the field's
  value doing the work rather than a method dispatched on `h`'s type. A field of any other type says
  so when called (`'h.name' is a string, which cannot be called`), and a qualifier that is neither an
  imported module nor a local with a callable field still reports the module error.
- **`==`/`!=` compares every field in declaration order**, short-circuiting on the first
  mismatch; both sides must be the same declared type. Works only when every field's type
  itself supports `==`, recursively (an incomparable field, like `Channel`, makes the whole
  struct a type error to compare); a struct referring back to its own type is assumed
  comparable rather than checked forever (see docs/DESIGN.md's "Recursive Eq/Hash" entry).
- Field types can reference another struct declared anywhere in the file (order doesn't
  matter); a never-declared struct name is a compile error at the field/parameter/return type
  that names it.

## Functions

### Variadic parameters

```ziman
fn sum(nums: variadic int) -> int {
    var total = 0
    for n in nums { total = total + n }
    return total
}
fn main() {
    print(sum())            // 0
    print(sum(1, 2, 3))     // 6
    val nums: [int] = [10, 20, 30]
    print(sum(nums))        // 60 -- an existing array works too
}
```

- **`variadic` before the last parameter's type marks it variadic** — a caller supplies zero
  or more individually-listed arguments of that element type instead of exactly one. Only the
  last parameter may be variadic (it has to absorb everything after it).
- **Inside the function it's an ordinary array** of the declared element type (`nums: variadic int`
  behaves as `nums: [int]`) — the signature shows the element type only, for the caller's sake.
- **An already-built array works as the sole trailing argument** (`sum(nums)`), decided purely
  by that argument's static type (`int` vs `[int]`, never ambiguous) — only when there's
  exactly one trailing argument; two or more are always individual elements, and there's no
  syntax to spread an array alongside other listed arguments (`sum(1, 2, nums)` doesn't merge).
- **A trailing `...` on the type was the spelling until 2026-10-12**, and both it and Go's
  `...int` prefix are now syntax errors that name the keyword form instead. `...` means nothing
  anywhere in Ziman — not at a declaration, and not at a call (`f(args...)`, which a variadic
  parameter never needed: it takes an array directly).

### Named arguments

```ziman
fn localtime_of(hour: int, minute: int, second: int, nano: int) -> string {
    return format("{}:{}:{}.{}", hour, minute, second, nano)
}
fn main() {
    print(localtime_of(1, 2, 3, 4))                              // positional
    print(localtime_of(hour: 1, minute: 2, second: 3, nano: 4))  // named, same call
    print(localtime_of(nano: 4, second: 3, minute: 2, hour: 1))  // any order
}
```

- **A call is all positional or all named, never mixed** (`f(1, y: 2)` is a syntax error) —
  named arguments reuse a struct literal's `name: value` shape.
- **A label must match the declared parameter name exactly** (no separate external label), and
  may appear in **any order** — the label alone resolves it.
- **Every parameter must still be given exactly once** (except an `Option<T>` one, next
  section) — too few, too many, an unknown name, or a repeated name is a compile error.
- **Only real functions accept named calls** — the small set of core builtins the compiler
  special-cases by name (`print`, `len`, `push`, `hash`, ...) have one fixed unlabeled signature.
- **A variadic parameter's named value is still one expression** — the single-trailing-argument
  rule above (element or whole array) applies the same way.
- Resolved to declared parameter order once, at type-check time — no runtime difference
  between a positional and a named call to the same function.

### Option\<T\> parameter omission and defaults

```ziman
fn describe(name: string, nickname: Option<string>, priority: Option<int> = 3) -> string {
    return format("{}, priority {}", name, Option.unwrap_Some(priority))
}
fn main() {
    print(describe("Ada", Option<string>.Some("Countess"), 5))  // every argument given
    print(describe(name: "Alan"))               // nickname -> None, priority -> 3 (its default)
    print(describe(name: "Hedy", nickname: "Hedley"))  // a bare value auto-wraps to Some
}
```

- **Only an `Option<T>`-typed parameter may be omitted, and only in a named call** — a
  positional call stays exactly as strict as ever; a required parameter can never be left out.
- **An omitted parameter with no declared default becomes `None`.**
- **`= expr` after the type gives a default other than `None`** (`priority: Option<int> = 3`),
  legal only on an `Option<T>` parameter. Don't write `= Option<T>.None` explicitly -- that's
  already the default.
- **The default is evaluated once, before `main` runs** (same timing as a global val), not
  fresh per omitted call -- harmless since values are immutable, and cheaper. Like a global
  val's initializer, it can't reference this function's other parameters.
- **A bare `T` works anywhere an `Option<T>` is expected** (an argument, a default, a `val`'s
  declared type, a struct field, a `with` override, a return value), auto-wrapped to `Some(T)`;
  an already-`Option<T>` value (or `None`) passes through unchanged. A deliberate, narrow
  exception to "no implicit coercion" (see docs/DESIGN.md's "Rationale consolidated from
  SYNTAX.md" entry). `Option<Option<T>>` is the one ambiguous case (would a bare `Option<T>`'s
  `None` become `Some(None)`?) -- a compile error there; write `Option<Option<T>>.Some(...)`.
- **A generic function's `Option<T>` parameter can't default when `T` is its own unbound type
  parameter** -- a default is evaluated before any call instantiates `T`. An already-concrete
  `Option<...>` (like `Option<int>` above) works fine.

### Result\<T, E\> bare-value auto-wrap

```ziman
fn as_result(ok_path: bool) -> Result<int, string> {
    if ok_path { return 1 }              // a bare T return value -- becomes Ok(1)
    return "bad input"                    // a bare E return value -- becomes Err("bad input")
}
fn main() {
    print(is_ok(as_result(true)))    // true
    print(is_ok(as_result(false)))   // false
}
```

- **Wherever a `Result<T, E>` is expected** (an argument, a `val`'s declared type, a struct
  field, a `with` override, a return value), **a bare `T` becomes `Ok(T)` and a bare `E`
  becomes `Err(E)`**; an already-`Result<T, E>` value passes through unchanged. Same
  deliberate, narrow exception as `Option<T>`'s auto-wrap.
- **When `T == E`, a bare value is a compile error** -- the sugar can't tell which was meant,
  so it refuses to guess: write `ok(value)`/`err(value)` explicitly (assign to a typed `val`
  first if it isn't already a legal call argument position).
- **`Result<T, E>` parameters can never be omitted, in any call, and can't have a default** --
  unlike `Option<T>` there's no natural "absent" state for an outcome type to fall back to.

### `try` — Option\<T\>/Result\<T, E\> unwrap-or-propagate

```ziman
fn plus_one(s: string) -> Result<int, string> {
    return ok(try parse_int(s) + 1)
}
```

- **`try expr`** (`expr: Result<T, E>` or `Option<T>`) unwraps the success value and keeps
  going, or immediately returns the failure from the *enclosing function* -- no `if is_err(r) {
  return err_of(r) }` boilerplate. `Ok(v)`/`Some(v)` → the whole expression is `v`; `Err(e)` →
  `return e` (auto-wrapped to `Err(e)`); `None` → returns a fresh `None` of the enclosing
  function's own `Option<T2>` type. No postfix `?` -- the early exit is written in front.
- **`try` covers the chain after it**, like `-`/`!`: `ok(try parse_int(s) + 1)` is
  `ok((try parse_int(s)) + 1)`, and it nests.
- **Legal anywhere an expression runs exactly once, unconditionally**: a `val`/`var`
  initializer, an assignment, a `return`, an `if` condition, a `switch` subject, a `for`
  range/iterable, an operand, an argument. The parser hoists any non-statement-level `try` into
  a `val $t1 = try expr` ahead of the statement, and hoists whatever evaluates *before* it too,
  so side effects still happen in the order written.
- **Refused** in an `else if`/`while` condition (evaluated conditionally/repeatedly), the right
  side of `&&`/`||`, a `defer`/`spawn` argument, or an `if`/`switch` used as a value (arms may
  not leave) -- put it in its own `val` first.
- **`Result<T, E>`'s `try` requires the enclosing function to return `Result<T2, E>` with the
  same `E`**, unless its error type declares a conversion: an enum may mark one payload-carrying
  variant `#[from]`, and a `try` on a `Result<_, P>` inside a function returning `Result<_,
  Enum>` wraps the error in the `#[from]` variant whose payload is `P`:
  ```
  enum ServiceError { #[from] Storage(DbError)  #[from] Validation(ValidationError)  Internal(string) }
  fn get(ctx: Ctx, id: Uuid) -> Result<Response, ServiceError> {
      val row = try repository.find_by_id(ctx.conn, id)   // Err(db) returns Err(Storage(db))
      return mapper.to_response(row)
  }
  ```
  At most one variant may be `#[from]` per payload type (not supported on a generic enum); the
  conversion applies only to `try`, only one step. `Option<T>`'s `try` requires only that the
  enclosing function return `Option<T2>` for *any* `T2` -- `None` carries nothing to propagate.
- **`Some(x)`/`None` may be written bare** wherever an `Option<T>` is already expected (the same
  five positions `ok`/`err`/`channel_new` get: a val/var with a declared type, a struct literal
  field, a `with` override, a `return`, and a call argument, whose parameter type is an expected
  type like any other) -- `return None`, `notes: Some(text)` and `f(Some(4))` work
  without naming `Option<string>`. Elsewhere the qualified `Option<T>.Some(x)`/`.None` is still
  required (nothing to infer from). Distinct from bare-value auto-wrap: that only sees a bare
  `T`, never something already shaped like `Some(...)`/`None`.
- **A bare `try expr` statement** still fully checks and propagates, discarding the unwrapped
  value on success.

### Must-use `Result`

```ziman fails
fn risky(n: int) -> Result<int, string> {
    if n < 0 { return err("negative") }
    return ok(n)
}
fn caller() -> Result<int, string> {
    risky(-1)              // type error: this Result<int, string> is discarded
    val _ = risky(-1)      // fine: discarded on purpose
    return try risky(-1)   // fine: propagates a real Err
}
```

- **A bare expression statement of type `Result<T, E>` is a compile error** -- nothing else
  marked "outcome doesn't matter" from "the `Err` case was forgotten." `defer` is the one
  exception (a deferred call has nowhere left to propagate to when it runs).
- **`val _ = expr` marks the discard deliberate.** `_` is never actually bound, so it never
  conflicts with an earlier `_` in scope, and reading it back is "use of undefined variable" --
  the one place `_` is special, the same way `for _ in 0..n`/`if Some(_) = o` already used it.
- **`try expr`** also satisfies this, propagating instead of discarding.

### `if Variant(name) = expr` — test and bind in one step

```ziman
fn main() {
    val a: Option<int> = Option<int>.Some(4)
    if Some(x) = a { print(x) }                     // 4
    if Some(x) = a && Some(y) = a && x + y > 5 { print(x + y) }  // 8 -- chained, later parts see earlier binds
    if Ok(v) = parse_int("12") { print(v) }
}
```

- **The variant is named bare, looked up from the right-hand side's type** (as a `switch` arm
  is) -- works for `Option`, `Result` (`Ok`/`Err`) and any declared enum. No-payload variants
  skip the parentheses (`if None = o`); `_` ignores a payload (`if Some(_) = o`).
- **The condition is `&&`-joined parts**, each a boolean or a pattern (`||` can't join a
  pattern -- parenthesize a boolean if needed). Bound names live in the block and later parts,
  not in `else`. `else`/`else if` work as usual, running when any part failed.
  It is a statement only (`val n = if Some(x) = o {...} else {...}` is a syntax error; use
  `switch` for a value).
- **Sugar**: the parser rewrites it to nested `if`s over `is_V`/`unwrap_V` (so `--fmt` keeps
  what you wrote). One consequence: a multi-part chain *with* an `else` isn't seen by the
  "every path returns" check (a single pattern with `else`, or any chain without one, is), so
  such a function may need a final `return`.

## Generics

Structs, enums and functions can all be generic, cross-module included (see docs/ROADMAP.md's
"Generics" entry for how this was staged). `Box<T>`/`Pair<A, B>` below are plain illustrative
structs, not corelib exports.

```ziman
struct Box<T> { value: T }
fn main() {
    val b = Box<int> { value: 5 }
    print(b.value)              // 5
    val nested = Box<[int]> { value: [1, 2, 3] }
    print(nested.value)         // [1, 2, 3]
}
```

- **Explicit type arguments everywhere for construction — no inference.** `Box<int> { value: 5
  }`, never `Box { value: 5 }` inferring `T = int` -- the same "explicit over implicit"
  instinct `val`/`var` already have.
- **No general bounds/constraints on a type parameter** -- `T` can be stored and passed around,
  nothing else; no trait/interface system for "T must support X." Three hardcoded exceptions,
  combinable with `+` (`struct Map<K: Eq + Hash, V> { ... }`): `Eq` ("supports `==`", the same
  recursive rule `==` already follows), `Hash` ("supports `hash`", the same rule as "Maps and
  sets"), `Ord` ("supports `compare`", see "Ordering"). Enforced at every instantiation
  (`Map<Channel<int>, string>` is a compile error), usable inside the bounded declaration's own
  body (functions only -- a struct/enum has no body). Any other bound name is a compile error.
- Two instantiations of the same generic struct (`Box<int>`/`Box<string>`) are different types:
  a mismatch is a type error at construction, and `==`/`!=` between them is rejected. Omitting
  type arguments entirely, giving them to a non-generic struct, or giving the wrong count, are
  all compile errors -- as is a type parameter name colliding with a real struct/enum name
  (it would silently shadow the real type inside the declaration's own fields).
- Cross-module works too: `mod.Box<int> { ... }`, `val x: mod.Box<int>`.

**Generic enums** work the same way -- construction spells out type arguments, consumption
never does. `Option<T> { Some(T) None }` below is the actual prelude-provided type, always
available bare.

```ziman
fn main() {
    val a = Option<int>.Some(5)
    print(Option.is_Some(a))       // true
    print(Option.unwrap_Some(a))   // 5
}
```

- **Construction spells out type arguments**: `Option<int>.Some(5)`, never bare `Option.Some(5)`.
- **Consumption (`is_X`/`unwrap_X`) never takes type arguments**: `Option.is_Some(a)`, not
  `Option<int>.is_Some(a)` -- the concrete type is already known from `a`'s own value.
- **`Option` has bare shorthands**: `is_Some(a)`/`is_None(a)`/`unwrap_Some(a)` for
  `Option.is_Some(a)` etc., and `unwrap(a)` on an `Option` is `unwrap_Some(a)`
  (`unwrap`-on-`Result` unchanged) -- rewritten to the qualified form at check time; other
  enums have only the qualified helpers. Reserved names, like `is_ok`.
- Two instantiations (`Option<int>.Some(1)` vs `Option<string>.Some("x")`) are different types,
  `==`/`!=` between them rejected. A bound (`enum Foo<T: Eq> { ... }`) works as on a struct,
  enforced at every construction site.

**Generic functions** work the same explicit-argument way, but a call site may omit them:

```ziman
fn identity<T>(x: T) -> T { return x }
fn are_equal<T: Eq>(a: T, b: T) -> bool { return a == b }
fn main() {
    print(identity(5))                 // 5 -- inferred, no <int> needed
    print(are_equal<string>("a", "b")) // false
}
```

- **Type arguments are inferred from the arguments' own checked types when possible.**
  `identity(5)` works without `identity<int>(5)` (a named call too). Only a *fixed* parameter
  whose declared type mentions one of the function's own type parameters contributes
  (`fn wrap<T>(items: [T], label: string) -> Option<T>` infers `T` from `items`, not `label`).
  A type parameter appearing only in the return type (`fn make<T>() -> [T]`) has nothing to
  infer from and still needs `make<int>()`. An argument needing its own expected type first (an
  empty array literal, `ok`/`err`/`channel_new()`, an unannotated lambda, an `if`/`switch`
  value) contributes nothing and is checked normally once inference finishes. Those same forms
  are what an enum variant's payload accepts, for the same reason: one declared payload type
  says what the value must be as precisely as a parameter does. Two arguments
  binding the same type parameter to different types is a compile error naming both. This
  never changes a struct/enum construction, `ok`/`err`/`channel_new()`, or `spawn` of a generic
  function -- those stay always-explicit. Explicit type arguments still work as given, no
  inference attempted.
- **A bound is usable inside the function's own body**: `a == b` in `are_equal<T: Eq>`
  type-checks because `T` carries `Eq`; the same expression in an unbounded `fn f<T>(a: T, b:
  T)` is a compile error naming the missing bound.
- **`<` is genuinely ambiguous with comparison only at a generic call** (`identity<int>(5)` vs.
  `x < y`, since a callee is always lowercase) -- resolved by a one-token lookahead: the parser
  only commits to type arguments if what follows `<` looks like the start of a type.
- Cross-module works too: `mod.identity<int>(5)`.

## Enums

A user-declared tagged union — the first parametric-free type *users* can declare that isn't
a plain product type like `struct`.

```ziman
struct RectDims {
    width: float
    height: float
}
enum Shape { Circle(float) Rectangle(RectDims) Point }

fn area(s: Shape) -> float {
    if Shape.is_Circle(s) { return 3.14159 * Shape.unwrap_Circle(s) * Shape.unwrap_Circle(s) }
    val d = Shape.unwrap_Rectangle(s)
    return d.width * d.height
}
```

- **A variant holds zero or one payload value, never a tuple or inline struct-shaped one.**
  No payload at all (`Point`) or exactly one named type in parens (`Rectangle(RectDims)`) --
  Ziman has no tuple type, so more than one payload value means declaring a separate `struct`
  and wrapping it, the same "one value per case" shape `Result<T, E>`'s `Ok`/`Err` use.
- **Nominal and closed, like `struct`.** Enum names share the type-name namespace with structs
  (no `struct Shape` and `enum Shape` both); variant names need only be unique *within* their
  own enum (two enums may both declare `Some`), since access is always enum-qualified.
- **Construction is always qualified**: `Shape.Circle(5.0)`, `Shape.Point`, never bare (see
  docs/DESIGN.md's "Rationale consolidated from SYNTAX.md" entry for why). A unit variant is
  written with no parens; `Shape.Point()` is a type error.
- **Consuming a variant uses two compiler-generated functions per variant: `is_<Variant>` and
  `unwrap_<Variant>`** — the same predicate-plus-unwrap shape `Result`'s `is_ok`/`unwrap_err`
  use, generalized to *n* variants and namespaced under the enum. `is_<Variant>(x) -> bool`
  works for any variant; `unwrap_<Variant>(x) -> T` exists only for a payload-carrying one and
  is a runtime error if `x` holds a different variant (same fatal-by-default contract as
  `unwrap`). The generated name keeps the variant's exact declared casing. Consume by
  test-then-unwrap, or the exhaustive `switch` statement below (no `match` *expression* yet --
  `switch` is a statement; ROADMAP R21 tracks expression forms).
- **`==`/`!=` compares variant then payload** — different variants are never equal regardless
  of payload; same variant is equal iff payloads are, legal only when every payload type is
  itself recursively comparable.
- Cross-module works too: `mod.Shape.Circle(5.0)`, `mod.Shape.is_Circle(s)`.
- `enum Shape { ... }` above is concrete/monomorphic; a generic one (the real `Option<T>`)
  needs type parameters on top -- see "Generics".

### `switch` — exhaustive consumption of an enum or a Result

```ziman
enum Shape { Circle(float) Square(float) Empty }
fn area(s: Shape) -> float {
    switch s {
        Circle(r) { return 3.14 * r * r }
        Square(w) { return w * w }
        Empty { return 0.0 }
    }
}
```

- **One arm per variant, none omitted or repeated, checked at compile time.** Adding a variant
  makes every `switch` over it fail to compile instead of crashing later. Deliberately **no
  default arm** -- it would bring back the silent case this exists to remove.
- **Arms name variants bare** (`Circle`, not `Shape.Circle`) and bind a payload with `Circle(r)`
  (required for a payload-carrying variant, forbidden otherwise); the binder is an immutable
  `val` scoped to the arm.
- **Works on any enum** (generic, cross-module, `Option<T>` with `Some(x)`/`None`) **and on
  `Result<T, E>`** (`Ok(v)`/`Err(e)`). A statement, not an expression -- returning from every
  arm satisfies the "returns on every path" rule.
- The scrutinee is evaluated once; `switch` is checked then rewritten into the `is_X`/`unwrap_X`
  calls you could write by hand, so `if Shape.is_Circle(s)` remains fine for a single-variant
  test. Not yet supported: nested patterns, guards, several variants per arm, or as a value.

## Modules

One file is one module, identified by its filesystem path — no separate
`module Foo` declaration to keep in sync with the filename.

```ziman ignore: the first half of a two-file example; `main.zm` below imports it
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

```ziman ignore: the second half of a two-file example, importing the `models/user` above
// main.zm
use models/user

fn main() {
    val u = user.User { id: 1, name: "Ada" }
    print(user.greet(u))
}
```

- **`use path/to/mod [as alias]`** — slash-separated, maps directly onto the filesystem
  (`models/user` → `models/user.zm`), resolved relative to the entry file's own directory
  first, then falling back to a bundled *core module* (below) if no project file exists; a
  project file of the same name always wins. A folder may hold its own module under its own
  name (`controller/controller.zm` for `use controller`, the way bundled `http` is
  `lib/http/http.zm`); the shorter file wins when both exist. Slash, not dot, since a module
  path never shares a token with `.field` access. The referenced name is the path's last
  segment, or the `as` alias — either way casing-enforced lowercase. Two `use` lines that would be
  referenced by the same name are a compile error naming both paths and suggesting an alias (`use
  entity/patient as entity_patient`), as is importing one module twice; before that check existed the
  first `use` silently won and the second did nothing. Nothing is auto-renamed: a name you did not
  write is what the rule below about bare names exists to prevent.
- **`use path/to/mod.Name [as alias]`** imports one function/struct/enum, bound to a bare
  local name (`use uuid.Uuid` then just `Uuid`) — one name per import, no `{A, B, C}` list.
  That last part is a decision, not an omission, and the reasons are worth having here because the
  obvious complaint about it is correct: the reference backend has 1,275 `use` lines and one file
  imports fifteen names from `time`. Three things outweigh that. `--fmt` already sorts runs of
  consecutive `use` lines, so nothing has to find the right slot in an alphabetised run by hand. One
  name per line is independently valid, so a mistake costs one line and the error points at it, where a
  malformed group breaks the whole import. And one fact per line stays greppable: `use service.is_email`
  is a searchable statement about one name, which a group is not. Against those, "one way to do a thing"
  (DESIGN.md's second principle) decides it — a group would be a second way to import three names. See
  DESIGN.md's "Grouped imports, declined" for the full argument.
  The bare name's casing must match `Name`'s own, checked at parse time (the parser already
  relies on casing alone to tell a struct literal from a call). Privacy-checked once, at the
  `use` line, exactly as a dotted reference would be. A compile-time error to import the same
  local name twice, or to collide with something the importing module declares itself.
- **Every cross-module reference is qualified or per-name imported — never a bare name that
  silently resolves to an import.** No wildcard import form; a per-name import always names
  the one thing it binds, so a file's `use` lines account for every bare name its body could
  have come from.
- **Three levels of visibility, ascending: private (no modifier), `internal`, `pub`.** Private
  is callable/constructible only within its own module (cross-module reference is a compile
  error naming both the declaration and module) — so names only need to be unique *within*
  their module, not the whole program. `pub` exports to any module anywhere. `internal` sits
  between: visible to any module in the same *package*, invisible outside it — today "package"
  means one of exactly two things, the bundled standard library as a whole or your own project
  as a whole, not a per-directory boundary (see docs/DESIGN.md's visibility entry for why a
  directory-based definition would have broken the actual motivating case). Violating it is
  worded distinctly from plain private: `'x' in module 'y' is internal to its own package`.
  `internal` is a *soft* keyword — recognized only immediately before `fn`/`native fn`/
  `struct`/`enum`/`val` (the same position `pub` occupies; they're mutually exclusive, `pub`
  checked first) — everywhere else, including a function's own name or an import, it's an
  ordinary identifier (the same technique `unit` uses as a type).
- **`[pub] val NAME[: Type] = <expr>` at module top level declares a global value** — evaluated
  once before `main` runs, in file order, never per-call. No global `var` (Ziman's concurrency
  model is single-writer actors over `Channel<T>`; see docs/DESIGN.md's "Global val" entry).
  Reachable unqualified from any function in the module (or via `use mod.NAME`), but never
  shadowable — reusing its name as a local `val`/`var`/parameter/loop-binder anywhere in the
  module is a compile error, not silent shadowing. No `mod.NAME` dotted bare access; use a
  per-name import for that.

  ```
  val max_retries = 3
  fn should_retry(attempt: int) -> bool { return attempt < max_retries }
  ```
- **No manifest file.** The dependency graph is the entry file's `use` statements, followed
  transitively; each module loads at most once (a diamond dependency shares one instance), and
  a circular `use` is a compile error rather than infinite recursion. The entry file is never
  itself a `use` target, so a program with no `use` at all behaves exactly as before modules
  existed.
- **Where `use foo` looks**: `foo.zm` beside the importing file first, then the directory of the
  nearest `ziman.toml` at or above it, then each directory in `ZIMAN_MODULE_PATH` (colon-separated,
  in order, first match winning), then the bundled core modules. Beside the file always wins, so
  nothing that resolved before either of the middle two resolves differently now. Both exist for the
  same reason -- letting tests live in a directory of their own -- and are two ways to point one
  search path at a directory, not two mechanisms: `ZIMAN_MODULE_PATH=src/main/ziman` makes
  `src/test/ziman/calc_test.zm` able to `use calc`, and a `ziman.toml` at a project's root lets
  anything under it write the project-relative path instead (`use src/main/ziman/calc`), which is
  the same path a file beside the subject would use. A module found either way resolves its own
  `use` lines the same way, so it may import too. `ZIMAN_MODULE_PATH` has the same shape and
  spelling as `ZIMAN_RESOURCE_PATH`, which does this for resources.
- **`ziman.toml`** marks a project's root and **nothing reads its contents** -- an empty file is the
  usual case. It is the name reserved for a manifest that does not exist yet, taken now so that
  manifest later lands in the file already marking the root rather than a second file appearing
  beside it. The search for it stops after eight levels, so a stray one in a home directory cannot
  quietly claim every file beneath it.
- **`native fn name[<TypeParams>](params) -> type`** declares a function with no body — its
  implementation is a C function registered under its exact fully-qualified name, looked up at
  call time; calling one with nothing registered is a runtime error. A generic native fn takes
  explicit type arguments like any other generic (no inference), and its C side never needs to
  know which concrete type a call instantiated it with (`Value` is already runtime-type-erased
  regardless of static types — see "Generics"). `fs.read_text(path) -> Result<string, string>`
  is the standard example (a missing/unreadable file is a `Result`, not a crash).
- **A bundled *core module*** is ordinary `.zm` source embedded in the `ziman` binary, resolved
  by `use` when no project file by that name exists (docs/CORELIB.md lists every one and every
  `pub` signature): `collections`, `fs`, `os`, `clock`, `random`, `uuid` (`Uuid`, RFC 4122 v4,
  satisfies `Eq`/`Hash`/`Ord`), `time`, `number`, `crypto`, `json` (`Json` is a recursive enum,
  `JArray([Json])`/`JObject(Map<string, Json>)`; satisfies `Eq`/`Hash` but not `Ord` — no
  `json_compare`, so `compare`/`sort` on `Json` is a type error), `properties`, `log`, `net`,
  `http`/`server`/`client`, and the database layer `db`/`sql`/`sqlite`/`pg`/`mysql`. Each is an
  ordinary qualified module (`use fs`, always `fs.read_text`) except the prelude
  (`lib/prelude.zm`), which merges bare into every program with no `use` at all (`Option<T>`,
  the builders, `parse_int`/`concat`/`split`/...; see docs/DESIGN.md's "Prelude" entry).

### Naming conventions for module functions

Two usage modes (`use net` then `net.connect(...)`, or a per-name import binding a bare
name), each with a rule the bundled modules follow and new ones should too:

1. **Called qualified: a short verb, no module prefix** (`net.connect`, `fs.read_text`,
   `server.route`, `client.get`) — the qualifier already says where it's from. Don't import
   such a name bare (`use server.response` collides with an application name).
2. **Imported bare, or found by naming convention: carry the noun** — self-describing and
   unique across modules (`byte_from_hex`, `net_error_message`, and everything `#[derive]`
   dispatches on: `<snake_type>_to_json`/`_compare`/`_to_sql`).
3. **A module defining types names operations after the type** (`uuid_new`,
   `localdate_add_days`) — the type is the noun, so rule 2 applies and the name is safe bare
   or qualified. Generic combinators on built-in types (`collections.map`/`filter`/`sort`)
   are rule 1.
4. **Conversions are `<from>_to_<to>`/`<to>_from_<from>`**, parsing is `parse`/`<type>_parse`,
   and a fallible call returns a `Result`, never a `try_` prefix.

The thing to avoid: a short generic verb also meant for bare import — the collision the two
modes are otherwise designed around.

### `if` and `switch` as values

An `if` or `switch` can produce a value, which removes the "declare a `var`,
then assign it in every branch" pattern:

```ziman ignore: `n`, `e` and `text` are the reader's own; the point is where such an expression may go
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

- **Where:** the entire right-hand side of a `val`/`var`, an assignment, or `return` -- an arm
  there may `return`/`break`/`continue`/`try` to leave. Anywhere else an expression can appear
  (a call argument, an operand, an array element, a struct field) it's also allowed, but no arm
  may leave (the interpreter can leave a statement, not mid-expression) -- a `break` inside a
  loop the arm itself contains is still fine. As an operand it needs parentheses when anything
  follows on the line (`(if a { 1 } else { 2 }) + 3`), else the following tokens read as part of
  the last arm's value. At the start of a statement, `if`/`switch` are still plain statements.
- **An arm's value is its last expression** (may be `foo()?`); a trailing `if`/`switch` is
  itself the value, so `else if` chains and nested choices work. An arm may instead leave
  (`return`/`break`/`continue`); at least one arm must produce a value and every arm must do
  one or the other, or it's a compile error. `if` needs an `else`; `switch` stays exhaustive.
- **All arms share one type**, checked against a declared/expected type when there is one (so a
  bare value auto-wraps to `Some`/`Ok` per arm) or decided by the first arm otherwise -- no
  auto-wrap *between* arms. `switch` payload names scope to their own arm only.

## Date and time

The `time` core module (`use time.Name`) provides seven types, modeled on `java.time` but
scoped down. Every type is `{ ...: int }`-shaped (`Instant`/`Duration` are `bigint`-free
`{ seconds: int, nanos: int }`), never a packed number, and has a fixed ISO-8601-shaped
`_to_string`/`_from_string` pair, each the other's exact inverse. `LocalDate`, `LocalTime`,
`LocalDateTime` and `Instant` also get `_format(value, fmt: DateTimeFormat) -> string` /
`_parse_pattern(s, fmt) -> Result<Value, string>`; the ISO pair is implemented in terms of
these, via one of five `pub val iso_*_format` presets. `Duration`/`ZoneOffset`/`ZoneId`
only get the ISO pair -- their shapes (`"PT...S"`, `"+HH:MM"`, a bare zone name) aren't a
calendar breakdown.

**`DateTimeFormat`** is compiled once (`datetime_format_of(pattern: string) ->
DateTimeFormat` scans it into an array of field/literal parts) and reused across many
`_format`/`_parse_pattern` calls. The pattern language is small and fixed-width, one run
length per letter (unlike `java.time.DateTimeFormatter`, where `M`/`MM`/`MMM`/`MMMM` differ):
`yyyy` (4-digit year), `MM`/`dd`/`HH`/`mm`/`ss` (2-digit, `HH` is 0-23), `SSSSSSSSS`
(9-digit nanosecond). Anything else is a literal, copied through verbatim -- no escaping,
no month/day names, no locale-awareness; an unrecognized run length of a token letter (three
`S`s) is just folded into the surrounding literal rather than erroring.

```ziman
use time.LocalDate
use time.localdate_add_months
use time.localdate_of
use time.localdate_to_string

fn main() {
    val d = unwrap(localdate_of(2024, 1, 31))
    print(localdate_to_string(localdate_add_months(d, 1))) // 2024-02-29
}
```

- **`LocalDate { year, month, day: int }`** — a calendar date, no time, no zone.
  `localdate_of(y, m, d) -> Result<LocalDate, string>` validates `month` (1-12) and `day`
  against that month's real length (leap years correct). `localdate_add_days`/`_months`/
  `_years` (month/year forms clamp the day to the target month, like `java.time`'s
  `plusMonths` -- `2024-01-31` plus a month is `2024-02-29`), `localdate_diff_days`,
  `localdate_compare`, `localdate_to_epoch_day`/`_from_epoch_day` (days since 1970-01-01,
  negative before it), `localdate_is_leap_year`, `localdate_to_string`
  (`"YYYY-MM-DD"`)/`_from_string`, `localdate_format`/`_parse_pattern`.
- **`LocalTime { hour, minute, second, nano: int }`** — a time of day, no date, no zone.
  `localtime_of(...)` validates ranges (`nano` is 0-999999999). `localtime_add_hours`/
  `_minutes`/`_seconds`/`_nanos` wrap at 24h and discard day overflow (use `LocalDateTime`
  when that needs tracking), `localtime_diff_nanos`, `localtime_compare`,
  `localtime_to_string` (`"HH:MM:SS"`, or with `.nnnnnnnnn` when `nano != 0`)/`_from_string`
  (auto-detects which shape), `localtime_format`/`_parse_pattern`.
- **`LocalDateTime { date: LocalDate, time: LocalTime }`** — the two combined, still no
  zone. `localdatetime_of(date, time)`, `_add_hours`/`_minutes`/`_seconds`/`_nanos` (these
  DO carry into the date on overflow, unlike `LocalTime`'s own), `_add_days`/`_months`/
  `_years` (delegate to `LocalDate`, time untouched), `_compare`, `_diff(a, b) -> Duration`
  (elapsed wall-clock time on one fixed calendar, not a real cross-zone computation),
  `_to_string` (`"YYYY-MM-DDTHH:MM:SS"`)/`_from_string`, `_format`/`_parse_pattern`.
- **`Duration { seconds, nanos: int }`** — a length of time, not a point. `nanos` stays in
  `[0, 999999999]`; `seconds` alone carries the sign (`-0.5`s is `{-1, 500000000}`, not
  `{0, -500000000}`) -- which is also why plain `seconds` comparison is a correct total
  ordering, `nanos` only breaking ties. `duration_of_seconds(s, n)` normalizes rather than
  rejects, `duration_zero()`, `_add`/`_subtract`/`_negate`, `_compare`, `_to_string`
  (`"PT{seconds}[.{nanos}]S"`; a negative duration prints its true magnitude with a leading
  `-`, not a misleading field-by-field negative) and its exact inverse `_from_string`.
- **`Instant { seconds, nanos: int }`** — a point on the UTC timeline, same shape and
  invariant as `Duration` (not a packed `bigint` nanosecond count). `instant_now()` reads
  the real system clock (`clock` module's `clock_gettime`, Ziman's only non-determinism
  besides `random`); `localdatetime_now(offset)`/`localdate_now(offset)`/
  `localtime_now(offset)` read the current wall-clock time on a clock in `offset`.
  `instant_of(s, n)` (normalizing), `_add`/`_subtract` (with a `Duration`), `_diff(a, b) ->
  Duration`, `_compare`, `instant_to_localdatetime(i, offset: ZoneOffset) -> LocalDateTime`
  / `localdatetime_to_instant(dt, offset)` (the only place `ZoneOffset` does something --
  shifting a UTC instant to/from wall-clock time), `instant_to_string` (UTC ISO-8601 via
  `zoneoffset_utc()`) and its inverse `_from_string` (accepts `"Z"` or a real offset
  suffix), `instant_format`/`_parse_pattern` (offset handled separately from the pattern).
- **`ZoneOffset { total_seconds: int }`** — a fixed UTC offset (`+05:30`), not a named zone.
  `zoneoffset_utc()`, `zoneoffset_of_seconds(s) -> Result<ZoneOffset, string>` (validated to
  `±18:00`), `zoneoffset_of_hours_minutes(h, m)` (rejects mismatched signs -- `(-5, 30)` is
  invalid, must be `(-5, -30)` or `(-5, 0)`), `_compare`, `_to_string` (`"Z"` for UTC,
  `"+HH:MM"`/`"-HH:MM"` otherwise)/`_from_string`.
- **`ZoneId`** — an IANA time zone (`"Europe/Paris"`) with its rules, read from the OS zone
  files (`$TZDIR`, else `/usr/share/zoneinfo`); `zoneid_of(name)` is an `Err` for an unknown
  name or no zone database, `zoneid_utc()`/`"UTC"` never need the files.
  `zoneid_offset_at(z, instant)` is the offset in force then, with `zoneid_is_dst_at`/
  `zoneid_abbreviation_at` (`EST`/`EDT`). `instant_to_localdatetime_in(i, z)`/
  `localdatetime_to_instant_in(dt, z)` apply daylight saving: an ambiguous wall-clock time
  (clocks going back) is the earlier instant, a nonexistent one (clocks going forward) is
  moved forward by the gap's length; a zone file's trailing rule string covers dates past
  its last listed transition. `zoneid_to_string`/`_from_string` are the bare name; two
  `ZoneId`s of the same zone compare equal.
- **No `<`/`>` on any of these seven types** -- use `compare`/`Ord` instead: each has a
  `_compare(a, b) -> int` function, which is what makes it `Ord` (works in `collections.sort`,
  as an `Ord`-bounded type argument). `==`/`!=` come free from structural equality on the
  plain `int`/struct fields. Calendar year is assumed non-negative throughout (no BCE).

`clock.sleep_millis(ms)` (`use clock`) blocks the calling thread only, for at least `ms`
milliseconds (a non-positive value returns at once) -- for retry backoff and tests needing
overlapping work.

## JSON

The `json` core module (`use json.Name`) is a parser/serializer, pure Ziman.

```ziman
use json.Json
use json.json_object_get
use json.json_to_string
use json.parse_json

fn main() {
    val j = unwrap(parse_json("{\"name\": \"Ada\", \"age\": 210}"))
    print(json_to_string(j))                 // {"name":"Ada","age":210}
    val name = json_object_get(j, "name")     // Option<Json>, not Result -- unwrap_Some, not unwrap
    print(Json.unwrap_JString(Option.unwrap_Some(name)))  // Ada
}
```

- **`Json`** is a recursive enum: `JNull`, `JBool(bool)`, `JNumber(Number)`, `JString(string)`,
  `JArray([Json])`, `JObject(Map<string, Json>)`.
- **Objects are Maps** — near-O(1) lookup, insertion-order iteration (a repeated key keeps its
  first position with the last value; `parse_json_strict` errors on one instead).
  `parse_json_bytes`/`_bytes_strict` are the same over `[byte]`, no string made first. Build one
  with `Json.JObject(Map<string, Json> { "k": v })`.
- **Numbers.** `JNumber` holds a `json.Number` (`enum Number { Int(int) Float(float)
  Bigint(bigint) Decimal(decimal) }`) recording how it was written. A 64-bit-fitting integer
  parses to `Int`; a larger one to `Bigint` with exact digits (never a float that's lost low
  digits); a fraction/exponent to `Float`. `parse_json_exact`/`_bytes_exact` read a
  fraction/exponent as `Decimal` instead, keeping every digit and the scale exactly;
  `decimal_to_json`/`json_to_string` round-trip it, and a `Bigint` writes as its digits. Readers
  decide losslessness: `int_from_json` (any in-range whole number), `bigint_from_json` (any
  whole number), `decimal_from_json`/`float_from_json` (any number, rounding a bigint/decimal
  like a cast would); `#[derive(Json)]` may have `bigint`/`decimal` fields. `json_number(j)` is
  the `Number` of a `JNumber`, or `None`; `switch` on it (no nested patterns) to branch on kind.
  Grammar is JSON's (`01`, `+1`, `.5`, `5.`, a dangling exponent are errors); an integer or exact
  decimal caps at 1000 digits (exponent at 100000).
- **`parse_json(s) -> Result<Json, string>`** — native, errors give the byte position, nesting
  past 512 levels errors. All JSON escapes read (`\" \\ \/ \b \f \n \r \t`, `\uXXXX` with
  surrogate pairs, decoded to UTF-8); `\u0000` errors (a string can't hold a 0 byte).
- **`json_to_string(j) -> string`** — native, compact, shortest-round-trip floats, NaN/infinity
  as `null`, control chars as `\b \f \u00XX`. `json_to_pretty_string(j, indent)` indents.
- **Lookups**: `json_object_get(j, key) -> Option<Json>`, `json_get_path(j, ["items", "0",
  "name"]) -> Option<Json>` (array segment = decimal index), `json_require_field`/
  `_string_field`/`_int_field`/`_float_field`/`_bool_field` (all `Result`, message names the field).
- **Map helpers**: `json_to_map<V>(j, decode)`/`json_from_map<V>(m, encode)` convert an object
  to/from `Map<string, V>`; `json_to_string_map(j)` is the `Map<string, string>` case.
- **No `==`/`!=` on `Json`** — array equality needs a scalar element type, and `Json` isn't one.
- **A general parsing engine, not a schema-specific request parser** — decoding into one
  known struct is application code built on `parse_json` + `json_object_get` + validation.

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

```ziman
use json.Json
use toml

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

- **Errors** are `Err(string)` with a `line N:` prefix (CSV: `row N`).
- **CSV**: RFC 4180 quoting; LF/CRLF/lone-CR endings; blank lines skipped; a leading UTF-8 BOM
  ignored. `parse_csv` keeps every cell a string; `parse_csv_typed` reads an unquoted cell as a
  number (`42`, `-1.5`, not `007`), boolean, or null (empty) -- a quoted cell stays a string.
  `parse_csv_columns`/`_typed` take column names for a headerless file. `_with(text, delimiter)`
  variants take a one-character delimiter (`"\t"` for TSV; invalid is `Err`). Errors name the
  line a row started on (including an unclosed quote's own start). The writer unions the rows'
  keys as columns (first-seen order), flattens an object one level into `a.b` columns, rejects
  deeper nesting, and quotes a string that would otherwise read back typed (`parse_csv_typed
  (csv_to_string(x))` round-trips); lines end in `\n`.
- **TOML 1.0**: tables, arrays of tables, dotted/inline tables, all string forms, hex/octal/
  binary/`_` integers, floats, booleans, comments. Dates/times stay `JString` text; `inf`/`nan`
  rejected; a duplicate key/table errors. The writer omits `JNull` fields (no TOML null),
  writing scalars, then sub-tables, then `[[arrays]]`.
- **YAML** is a strict subset of 1.2: block maps/sequences, flow `[..]`/`{..}`, plain/quoted
  scalars, `|`/`>` block scalars, comments, a leading `---`; core-schema typing (`null`/`~`,
  `true`/`false`, numbers; `yes`/`no` stay strings, `007` stays a string). Anchors, aliases,
  tags, `<<` merge keys, `? ` keys, multiple documents and tab indentation are errors, not
  misreadings. The writer quotes exactly what would re-read as another type.
- Ordinary Ziman on the tree-walking interpreter -- fine for config/export-sized data, slow for
  megabytes.

## Ordering (`Ord`, `compare`, sorting)

```ziman ignore: `people`, `patients` and `ages` stand for the reader's own arrays
use collections

#[derive(Ord)]
struct Person { last: string  first: string }

val sorted = collections.sort<Person>(people)               // by last, then first
val by_age = collections.sort_by_keys<Patient, int>(patients, ages)
```

- **`< > <= >=`** work on `int`/`float`/`bigint`/`decimal`/`byte`/`string` (byte-by-byte then
  length for strings) and stay non-overloadable; a struct/enum uses `compare(a, b)`.
- **`compare<T: Ord>(a, b) -> int`**: negative/zero/positive -- only the sign is meaningful, not
  the magnitude (a hand-written `_compare` may return `a.score - b.score`; nothing relies on
  exactly `-1`/`0`/`1`). `Ord` is satisfied by the six scalars, by a struct/enum with
  `<snake_case_type>_compare(a, b) -> int` (`Person` looks for `person_compare`; the spelling
  without inserted underscores, `localdate_compare`, is also accepted, used by `time`), by an
  `Ord`-bounded type parameter, and recursively by **arrays** (lexicographic), **`Option<T>`**
  (`None` before every `Some`) and **`Result<T, E>`** (every `Ok` before every `Err`) whose
  element/payload is `Ord` -- so `[Option<Result<int, string>>]` sorts. `bool` and channels have
  no order; `float`'s order is total (NaN sorts after everything, equals itself).
- **`#[derive(Ord)]`** writes `<type>_compare`: a struct compares fields in declaration order
  (first difference decides); an enum by variant position then payload. Every field/payload must
  itself be `Ord`; a fieldless struct can't derive it. Generic: `#[derive(Ord)] struct Pair<A,
  B>` writes `pair_compare<A: Ord, B: Ord>`, so `Pair<int, string>` is `Ord` but `Pair<int,
  bool>` is rejected.
- **`collections.sort<T: Ord>(items) -> [T]`** is a stable merge sort (new array). Sort by
  something other than the type's own order with **`sort_by_keys<T, K: Ord>(items, keys) ->
  [T]`** (a fatal error, not `Result`, on mismatched lengths -- a caller's own bug; see
  docs/DESIGN.md's "Programmer errors are fatal") or a comparator function via
  **`sort_with<T>(items, cmp: fn(T, T) -> int)`**. Descending: `sort_desc`/`sort_by_keys_desc`/
  `sort_indices_desc` -- also stable, not the same as reversing ascending (which flips ties). A
  packed `[int]`/`[float]`/`[byte]` or string array sorts natively (~10x faster); anything else
  calls `compare` in a merge sort. Also `sort_indices`, `is_sorted`, `reverse`, and for sorted
  arrays `lower_bound`/`upper_bound` (insertion points) and `binary_search` (`Option<int>`).
  `Uuid` and every `time` type are `Ord`.
- **`error_message(e) -> string`** dispatches like `compare` but for a different suffix: an enum
  `e: NetError` looks for `net_error_message`, and so on for any enum with its own
  `<snake_case_type>_message(e) -> string` (`NetError`/`DbError`/`JwtError`/`ClientError` already
  have one). No bound to name and no array/Option/Result recursion -- one concrete enum in, one
  string out.

## Attributes and derive

`#[...]`, above a declaration, attaches compile-time metadata to it. Four are interpreted: `derive`
(below) and `thread_bound` on a `struct`/`enum`, and `test` and `deprecated` on a function; other
names parse but have no effect yet.

`#![...]`, with the `!`, attaches to the *whole file* instead of to the declaration after it (the
same inner/outer split Rust draws), so it must appear before the first declaration -- writing one
later is an error that says so. One name is interpreted: `#![experimental]`, which marks a module
whose surface is more likely to have a gap than the rest of the library. It changes nothing at
compile time or runtime by design; its job is to be the single place that claim lives, so that
docs/CORELIB.md and docs/lib/*.md can generate the "Experimental" callout from the source rather
than each repeating it by hand, and `make experimental-check` can hold the README's own table to
it. `pg`, `mysql`, `toml` and `yaml` carry it.

`#![deprecated]`, or `#![deprecated("use newmod instead")]`, says the whole module should not be
used in new code. It is reported **at each `use` of it** -- the one place the dependency is
written, and so the one line that dropping it means editing -- rather than at every call into the
module, which would bury the answer under repetition. Both import forms count, since each is a
`use` that has to go:

```
$ ziman app.zm
app.zm:8:1: warning: module 'oldmod' is deprecated: use newmod instead
app.zm:9:1: warning: module 'oldmod' is deprecated: use newmod instead
```

A `use` from inside a bundled module is silent, the same same-source rule the declaration-level
attribute follows. On the program's own entry file the attribute warns nowhere, because nothing
imports an entry file -- there is no line for a warning to point at.

`#[deprecated]`, or `#[deprecated("use X instead")]`, marks a function, `struct`, `enum` or
module-level `val` that still works but should not be used in new code. Using it *from another
module* -- calling it, naming it as a value, writing a struct literal, reaching an enum variant,
reading a val -- is a **warning on every compile**, quoting the message when one was given:

```ziman ignore: a signature with no body, showing what the attribute is written on
#[deprecated("use open_session_with_timeouts, which also bounds the request head")]
pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session
```

```
$ ziman app.zm
app.zm:12:18: warning: 'server::open_session_with_timeout' is deprecated: use open_session_with_timeouts, which also bounds the request head
```

**A warning never changes the exit code**, so a deprecation cannot break a build that was passing;
it is reported because a warning nobody can see without running a second command moves nobody off
the old API. `ziman --lint` reports the same warnings *and* exits 1, since there finding things is
the whole request -- that is where the style findings (a `var` never reassigned, a declaration
nothing reads, a redundant `as` alias) live, and those stay behind the flag.

Use from *within* the declaring module is deliberately silent: a library keeping a superseded
function working for its existing callers would otherwise warn on every internal call it makes to
itself, and whoever wrote the deprecated thing is the one person who does not need telling. A
**type annotation naming a deprecated type is also silent**: a function migrating away from one
still has to name it to accept one, so warning there would be loudest in exactly the code doing the
migration. All four declaration kinds carry the attribute; there is no kind it is rejected on.

`#[thread_bound]` marks a struct that belongs to the thread that made it (as `MutableString`/
`MutableArray`/`MutableMap`/`MutableSet`/`Iter` are). A type containing one is refused as a
`Channel` element, `spawn` argument, or module-level `val` type -- including a lambda that captured
one, whose type records that fact, so sending or spawning the lambda is refused where it is written
rather than failing on the receiving thread. Each builder and iterator also remembers its own thread
and a use from another is a runtime error, which stays as a backstop for anything the type checker
cannot see. Hand over the finished value -- `snapshot(x)` for any of the four, an iterator's items
as an array -- not the handle itself. For a struct written in Ziman this attribute gives only the
compile-time refusals (no native check).

**A thread-bound type is neither comparable nor hashable**, and neither is anything holding one:
`==` on two handles is a type error, `hash` of one is, and a struct with a handle field is refused
as a `Map` key or `Set` element. The contents live behind an opaque index into a native table, so
two handles say nothing about what is in them -- comparing the snapshots (`snapshot(a) ==
snapshot(b)`) is the thing that was meant. Until 2026-10-13 this was accidentally *allowed*,
because a handle is a struct whose one field is an `int`: two empty `MutableArray`s compared
unequal, `hash` of one was stable only within a run, and such a struct keyed a `Map` on a pointer
value.

**A type named `Mutable*` must be thread-bound**, and the compiler checks it: `Mutable` followed by
an uppercase letter (or on its own) is a claim of family membership, and a plain value wearing the
name would make "every `Mutable*` type belongs to the thread that made it" untrue. A `struct`
satisfies it with the attribute *or* by holding something that has it, so
`struct MutableRingBuffer { buf: MutableArray<int>, head: int }` needs no annotation -- the error
fires only on a `Mutable*` name with nothing mutable inside. An `enum` carries no `#[thread_bound]`
of its own, so its only way to satisfy the rule is a thread-bound variant payload. `MutablesRegistry`
is not caught, which is why the check reads the character after the prefix. The converse does not
hold: a thread-bound type need not be named `Mutable*`, or `Iter` would have to be renamed.

```ziman
use json.Json
use json.json_to_string

#[derive(Json)]
pub struct Point { x: int  label: Option<string> }

fn main() {
    val p = Point { x: 42, label: Option<string>.Some("hi") }
    print(json_to_string(point_to_json(p)))          // {"x":42,"label":"hi"}
    print(unwrap(point_from_json(point_to_json(p))).x)  // 42
}
```

- **`#[derive(Json)]`** generates `<snake_case>_to_json(v) -> Json` and `<snake_case>_from_json(j)
  -> Result<Type, string>` (both directions always, via a real multi-word `PascalCase` ->
  `snake_case` conversion: `ValidationError` -> `validation_error`). On a **struct** it also generates
  `<snake_case>_from_json_all(j) -> Result<Type, [string]>`, which reports *every* bad field instead of
  stopping at the first -- a document with four wrong types gives four messages. Use it where a client
  should see all of its mistakes at once; `_from_json` is unchanged and remains the cheaper call. A nested
  struct contributes one message (its own first), since it is decoded by its single-error function, and an
  enum gets no companion: its decode either matches a variant or does not, so there is nothing to collect.
- **Per-field conversion, in priority order**: (1) the field type's own `_to_json`/`_from_json`
  (hand-written or derived); (2) a `_to_string`/`_from_string` pair (every `time`/`uuid` type has
  one, wrapped thin -- really tier 1 one step removed); (3) a plain scalar, or `decimal` (written
  as the nearest float, read from its shortest text -- `#[derive(Row)]` stores it as TEXT,
  keeping scale, instead); (4) `Option<U>`/`[U]`, recursing into `U`. Anything else (`bigint`,
  `byte`, `Result`, `Channel`, a generic struct/enum) is a compile error, not a wrong answer.
- **Field names map `snake_case` -> `camelCase`** for the JSON key (Jackson's default), fixed,
  not configurable. **`Option<U>` fields tolerate absence**: a missing key or explicit `null`
  both decode to `None`; every other field is required. **Deserialization is lenient**: an
  unrecognized extra key is silently ignored.
- **A decode error names the field**: `{"email": 5}` against `{ email: string }` reports
  `"email: expected a string"`, composing one level per nesting (`"address: zipCode: expected a
  string"`); an array/set/map-value element has no field name, so decodes with the bare message.
- **An enum derives an externally tagged shape**: `{"type": "<Variant>"}` (unit) or `{"type":
  "<Variant>", "value": ...}` (payload, following the same per-field priority). `Option<T>` keeps
  its own `null`/unwrapped handling even as a variant's payload type, never tagged this way.
- **A derive imports what its generated code calls**, only where the module doesn't already bind
  the name: `#[derive(Json)]` brings `json.Json`/`json_object_get`/`json_require_field`/
  `json_require_string_field`; `#[derive(Row)]` brings `db.SqlValue`/`DbError`/`Row`/`row_get`/
  `sql_as_*`; `#[derive(Properties)]` imports `properties`. It can't supply a field type's own
  conversion, except `Uuid` and every `time` type get their `_to_json`/`_from_json` (or
  `_to_sql`/`_from_sql`) imported automatically; any other field type's conversion must already
  be imported, or the error says to derive or write it. Imported names are visible module-wide.

## Testing

`#[test]` marks a function for `ziman test file.zm [...]` (a separate CLI mode from `ziman
file.zm`) to run. It takes no parameters and returns nothing or `Result<unit, E>` for any `E`
(so it can `try` freely); the file needs no `main`.

`ziman test` loads and type-checks each file, finds every directly-declared `#[test]` function,
and runs each inside its own per-thread runtime-error boundary (the mechanism `spawn` uses --
see "Concurrency") so one failing test doesn't stop the run. A runtime error or an `Err` result
both count as failure, message shown; anything else passes. Prints `PASS <file>::<name>` or
`FAIL <file>::<name>  <message>`, then a `N passed, M failed` summary; exit 0 only if all pass.

**Parametrized: `#[test(cases: NAME)]`** runs a one-parameter function once per array element:

```ziman
use test

struct AddCase { a: int  b: int  expected: int }
val add_cases = [AddCase { a: 1, b: 1, expected: 2 }, AddCase { a: 2, b: 2, expected: 4 }]

#[test(cases: add_cases)]
fn test_add(c: AddCase) { test.assert_eq(c.expected, c.a + c.b) }
```

`NAME` is looked up in the same file only, as a `val` or zero-parameter function of type `[T]`
(`T` the test's parameter type). A `val`'s array is used as-is; a function is called once,
inside its own boundary (a broken builder is one reported failure, not a crash). Each case
reports independently as `<file>::name[i]` -- unlike a plain `for` loop over cases inside an
ordinary `#[test]`, where the first failure ends the whole test.

**The `test` module** (`lib/core/test.zm`) fails every assertion through `unwrap` on a
manufactured `Err` -- no new interpreter primitive needed:

- `fail(message)` -- unconditional. `assert(cond)`/`assert_false(cond)`.
- `assert_eq<T: Eq>(expected, actual)`/`assert_ne<T: Eq>(unexpected, actual)` -- structural
  equality, message shows both values.
- `assert_lt`/`assert_le`/`assert_gt`/`assert_ge`, each `<T: Ord>(actual, bound)`.
- `assert_some<T>(opt) -> T`/`assert_none<T>(opt)`; `assert_ok<T, E>(r) -> T`/`assert_err<T,
  E>(r) -> E` -- the `_some`/`_ok` forms return the unwrapped value for chaining.

Failure messages use `format`'s `{}` (not `str`, which rejects a bare generic `T` -- see
"Builtins"), so `assert_eq`'s only requirement is the `T: Eq` bound `==` already needs.

## Databases

Two modules: `db` (shared by the drivers -- `SqlValue`, `Row`, `DbError`, `sql_as_*` readers,
the URL parser) and `datasource` (what a program calls -- connections, `execute`, `query`,
`rows`, pools, transactions). SQLite (`sqlite::memory:`/`sqlite:<path>`) and PostgreSQL
(`postgres://user:password@host:port/database`) are the two backends, chosen by URL scheme
behind the same calls.

```ziman
use datasource
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

- **Bound parameters only** -- SQL text carries `$1, $2, ...` (the SQLite driver rewrites to
  its own `?N`), values go in a `[SqlValue]` argument, nothing is ever spliced into SQL text.
- **`SqlValue`** is `Null | Int(int) | Float(float) | Text(string) | Bool(bool) | Bytes([byte])`.
  Nothing dialect-specific crosses the driver: `Uuid`/`LocalDate`/`Instant`/`decimal`/`bigint`
  all travel as `Text`, converted by the mapping layer. A `bool` column reads back as an
  integer under SQLite; `sql_as_bool` accepts either.
- **`DbError`** is `NotFound | Constraint | Connection | Sql`. `Constraint` is a unique/
  foreign-key/not-null/check violation (SQLite turns foreign keys on, matching Postgres).
  `NotFound` is never raised by the driver itself -- a repository returns it for an empty
  lookup, so one error type covers the whole data-access layer. `db_error_message(e)` renders any.
- **Postgres** (module `pg`, pure Ziman on `net`/`number`/`crypto`) speaks protocol 3.0:
  trust/cleartext-password/**SCRAM-SHA-256** auth, simple query protocol for parameterless
  statements (`execute` takes a `;`-separated DDL script), extended protocol for `$1, $2, ...`.
  Result values travel as text and are interpreted by column type (int/float/bool/bytea map to
  the matching `SqlValue`, everything else -- text, uuid, dates, numeric, json -- arrives as
  `Text`), so the same repository SQL/schema/`#[derive(Row)]` runs on both backends. A `Bool`
  parameter sends `1`/`0` (Postgres accepts it for `BOOLEAN` or `INTEGER`). One connection is
  owned by one thread serializing requests, so sharing across threads is safe statement by
  statement. **TLS**: an `SSLRequest` then in-place upgrade, chosen by `sslmode` with libpq's
  meanings -- `disable` (the default here, unlike libpq's `prefer`), `prefer` (TLS if offered,
  unverified), `require` (TLS or fail, certificate **not** verified), `verify-ca` (+ chain),
  `verify-full` (+ hostname -- use this for a hosted database); `sslrootcert=<path>` names a
  trusted-CA PEM, else the system store. `allow` unsupported; a failed `prefer` doesn't retry
  plaintext. **Not supported:** MD5 auth, Kerberos/GSS, client certificates, IPv6 URL literals,
  COPY, LISTEN/NOTIFY, prepared-statement reuse, SASLprep of a non-ASCII password.
- **`datasource.rows(conn, sql, params)`** is `query` as a lazy `Iter<Result<Row, DbError>>`:
  SQLite streams from a cursor (a million rows: 72 MB vs. 1.7 GB materialized), closed when
  exhausted, left early, or via `iter_close`; on a pool the connection is borrowed for the whole
  iteration (finish or close it -- not GC-closed). Postgres/MySQL stream 500 rows at a time
  (Postgres: a portal, connection carries no other statement until done/closed; MySQL: a
  server-side cursor, other statements may interleave but one cursor at a time); an unfinished
  cursor ends when garbage collected. 300k rows: 51 MB vs. 609 MB on Postgres, 43 MB vs. 706 MB
  on MySQL.
- `datasource.query` returns `[Row]`; `query_one` returns `Option<Row>`; `execute` returns the
  last statement's row count, and takes a `;`-separated script when there are no parameters.
- **Connection pools.** `datasource.open_pool(url, size)` opens `size` connections up front
  (fails fast, closing any already opened) and returns a pool-backed `Connection` -- the same
  type `open` returns, so repository code is unchanged. Statements each borrow and return a
  connection; a multi-statement unit of work (a transaction, above all) borrows one exclusively:
  ```
  val tx = datasource.acquire(conn)?
  defer datasource.release(conn, tx)      // returned on every path out
  datasource.begin(tx)?
  defer datasource.rollback(tx)           // runs before the release
  datasource.commit(tx)?
  ```
  The pool is a channel of idle connections (a borrow blocks while all are busy -- natural
  backpressure). `tx` is an ordinary non-pool `Connection`, so a repository called with it never
  borrows again (avoiding a small-pool deadlock). `begin`/`commit`/`rollback` on the pool itself
  are refused (each statement would borrow a different connection). On a plain `Connection`,
  `acquire` returns it and `release` no-ops, so the same code runs against either.
  `datasource.close(pool)` waits for every connection to be released then closes them --
  `defer datasource.release` is what prevents an unreleased borrow hanging it forever;
  `acquire_timeout(pool, ms)` gives up with `DbError.Connection` instead. **A dying connection
  is replaced**: a pool statement failing with `DbError.Connection` closes and reopens from the
  same URL (not re-sent, since it may have run); `datasource.discard(pool, tx)` does the same
  after a failure on an acquired connection (`with_transaction` does it automatically). **Health
  check**: a connection idle 30+ seconds is tested with `SELECT 1` when lent out and replaced on
  failure (`open_pool_with(url, size, check_after_seconds)`, 0 to disable; SQLite never tested).
  **Limits**: no min/max sizing or idle timeout, a released connection isn't reset (goes back
  dirty if released mid-transaction), and a pool over `sqlite::memory:` is always a single
  connection (every in-memory connection is its own private database; SQLite itself waits up to
  five seconds for the writer lock instead of failing, making a file-backed pool usable).
- **Transactions** are plain `begin`/`commit`/`rollback` on the connection. Every call takes the
  connection's own lock (safe to share a `Connection` across threads statement by statement, not
  mid-transaction -- other threads' statements can interleave inside it). A `try` returning
  early inside one leaves it open unless the caller uses `defer datasource.rollback(tx)` or
  `with_transaction` (see "Transactions").
- **`#[derive(Row)]`** on a struct generates `<name>_from_row(row) -> Result<Name, DbError>` and
  `<name>_to_params(v) -> [SqlValue]` (field declaration order, matching a hand-written
  `INSERT`'s column order; field names map directly, already `snake_case`). `int`/`float`/
  `bool`/`string` map directly; a struct/enum field dispatches to `<name>_to_sql`/`_from_sql`
  (provided for `Uuid` and every `time` type); `Option<U>` is NULL-or-value. An enum, generic
  struct, array field, or `bigint`/`decimal`/`byte` field is a compile error. The derive imports
  what its generated code calls; a field type of your own needs its conversion imported by the
  deriving module.

## Networking

The `net` core module is blocking TCP; `[byte]` helpers are prelude builtins, plus binary
encodings in `number`.

`net.connect_unix(path)` and `net.listen_unix(path)` do the same over a Unix-domain socket, which is
a filesystem path rather than a host and port -- a local PostgreSQL or MySQL usually listens on one.
The `Socket` behaves identically afterwards (`read`, `write`, `lines`, `set_timeout`, `close`, even
`start_tls`). Two differences, both reported rather than hidden: the path is limited to 107 bytes by
the operating system, and binding a path that already exists is refused -- a socket left by a
process that has gone looks exactly like a live one from here, so it is not removed for you
(`fs.remove` it once you know which it is). `listener_port` answers -1 for such a listener.

```ziman
use net
fn serve(l: net.Listener) {
    val client = unwrap(net.accept(l))
    unwrap(net.write_text(client, concat("echo:", unwrap(net.read_text(client, 1024)))))
    net.close(client)
}
fn main() {
    val l = unwrap(net.listen("127.0.0.1", 0))        // port 0: the OS picks one
    spawn serve(l)
    val s = unwrap(net.connect("127.0.0.1", net.listener_port(l)))
    unwrap(net.write_text(s, "hi"))
    print(unwrap(net.read_text(s, 100)))               // echo:hi
    net.close(s)
}
```

- **Blocking, thread-per-connection** -- a blocked `accept`/`read` blocks only its own thread;
  `spawn` a handler per connection. No event loop, no async.
- **Handles.** `Socket`/`Listener` are `{ handle: int }` over a native table, like
  `datasource.Connection`. Closing is explicit (`net.close`/`close_listener`) -- no destructors,
  so a `try` returning early between `connect` and `close` leaks the socket. Close from the
  owning thread.
- **Reading.** `read(s, max)` blocks for at least one byte, returns up to `max`. `read_exact(s,
  n)` returns exactly `n` or errors (length-prefixed protocols). `read_text`/`write_text` are
  for text (a NUL is reported, not truncated). `write` sends every byte before returning.
- **EOF is an error**: a clean peer close makes a read return `Err(Closed)`, never an empty
  array -- match `NetError.is_Closed` where end-of-stream is legitimate.
- **`NetError`** is `ConnectionRefused | HostNotFound | Timeout | Closed | Io`, each with a
  message; `net_error_message(e)` renders one.
- **Timeouts**: `net.set_timeout(s, millis)` applies to reads/writes (else a dead peer waits
  forever); no connect timeout beyond the OS default.
- **`string_to_bytes`/`bytes_to_string`/`bytes_concat`** are prelude builtins: `bytes_to_string`
  is a `Result` (`Err` on invalid UTF-8, naming the byte offset, or a 0 byte) -- the one real
  boundary where untrusted bytes become a `string`, so the one place UTF-8 is checked (see
  "Strings are bytes" in "Builtins"). `bytes_concat(parts...)` joins many byte arrays in one
  pass; sub-ranges are `slice<byte>(b, start, end)` (zero-copy).
- **`number`**: big-endian `u16_to_bytes`/`u32_to_bytes`/`u16_from_bytes`/`u32_from_bytes` and
  float encodings network protocols are built from.
- **Lines**: `net.lines(socket)`/`lines_with(socket, max_line_bytes)` is a lazy
  `Iter<Result<string, NetError>>`, ended when the peer closes; each item has no `\n`/`\r\n`
  (a final line with none still counts), or an `Err` for a NUL byte, a line over 1 MiB (ends the
  iteration), or a read error. Reads ahead in blocks (take all lines from one iterator); doesn't
  close the socket.
- **TLS (client)**: `net.connect_tls(host, port)` is `connect` + `start_tls(sock, host)`,
  upgrading in place -- the same `read`/`write`/`set_timeout`/`close` work after. Verified by
  default (system CA store, hostname, TLS 1.2 minimum, SNI sent). `_with` forms take a
  `TlsVerify` (`Full`/`Chain`/`Off`) and an optional trusted-CA PEM. A handshake/verification
  failure is `NetError.Tls(message)` (OpenSSL's reason); `connect_tls` closes its socket on
  failure. A timeout still surfaces as `Timeout`, a peer close as `Closed`, over TLS.
- **Server-side TLS**: `listen_tls(host, port, cert_file, key_file)`. Not yet: client
  certificates, ALPN, Unix-domain sockets, UDP, a connect timeout beyond the OS default.

## HTTP

The `http` core module is an HTTP/1.1 **server toolkit** on `net`: parse a request off a
socket, route it, write a response -- plus a ready-made server loop.

```ziman ignore: `{ ... }` stands in for the handler's body
fn route(conn: datasource.Connection, req: http.Request) -> http.Response { ... }

val options = server.ServeOptions { idle_timeout_ms: 30000, header_timeout_ms: 5000, max_connections: 64, log_requests: true }
os.install_shutdown_handler()
server.serve(listener, fn(req) { route(conn, req) }, options)   // returns after SIGINT/SIGTERM
```

`server.serve(listener, handler: fn(Request) -> Response, options)` runs the accept loop and
the per-connection request loop, one thread per connection, at most `max_connections` at a
time (a channel as a counting semaphore; a client past the limit waits in the kernel's accept
queue). The handler is a function value (a lambda's captures are immutable, so safe to share
across connection threads). `serve` returns once a shutdown is requested
(`os.install_shutdown_handler()`) and every in-flight connection finishes; it neither closes
the listener nor installs the handler. Write your own accept/connection loop from the pieces
below (`open_session`, `read_request`, `write_response`) when `serve` doesn't fit.

- **Reading.** `read_request(session)` returns an `Exchange` (`Request` plus updated
  `Session`) or an `HttpError`. `Request` has `method`, `target`, `path`, `query`, `version`,
  `headers`, `body`; `http.header(req, "Name")` ignores case. `Err(Closed)` is a normal
  keep-alive end. Keep-alive (including pipelined requests) and `Expect: 100-continue` both
  work; `Content-Length` and `Transfer-Encoding: chunked` bodies are supported (any other
  transfer encoding is 501); `HEAD` gets headers with `Content-Length` and no body.
- **Bounded.** Headers total 16 KiB, a body 1 MiB (else `HeadersTooLarge`/`BodyTooLarge`, 431/
  413). `net.set_timeout` on the socket turns a stalled read into `Timeout` (408) -- but a
  per-read timeout alone doesn't stop a slowloris client trickling one byte at a time, so
  `server.open_session_with_timeouts(sock, idle_ms, head_ms)` bounds the *whole* request (from
  its first byte) to `idle_ms`, with a shorter `head_ms` for how long a new connection may take
  to send its first byte and head -- an honest client needs milliseconds, so a client that
  drips or says nothing frees its slot in seconds, not `idle_timeout_ms`, and can't starve
  `max_connections`. `Request`/`Response.body` is `[byte]` (may hold a NUL); the request line
  and headers are read as text, so a NUL there is a 400 (a Ziman `string` can't hold one). A
  flood from many addresses is for a firewall/proxy, not this layer.
- **No banner, one hardening header.** Every response carries `Date` and
  `X-Content-Type-Options: nosniff`, never `Server`; a handler setting either itself is not
  overridden. `Strict-Transport-Security` is the application's to add (it, not the library,
  knows whether TLS ends at the process or a proxy in front).
- **Strict about what a proxy might read differently** (request smuggling): 400 on differing
  `Content-Length` headers (identical repeats are fine), a non-digit `Content-Length` (`+5`,
  `5, 5`, `0x5`), two `Transfer-Encoding` or `Host` headers, no `Host` on HTTP/1.1, a
  non-token header name or method, or a control character (other than tab) in a header value
  or request line (a bare CR/LF would let one header line pass for two).
- **Response headers are checked on the way out.** `write_response` refuses a header whose
  name isn't a token or whose value has a control character (CRLF injection from unchecked
  client text) -- sends 500 in its place, closes the connection, logs to stderr, never the
  offending text. `with_header` itself never fails; the check is on the way out, where every
  response passes, since a `Response` can also be built as a plain struct literal.
- **Responses:** `server.response(status, body)`, `json_response`, `text_response`,
  `bad_request`, `not_found`, `method_not_allowed`, `with_header`. `write_response(sock, req,
  resp)` adds `Content-Length`/`Connection` and returns whether the connection stays open
  (HTTP/1.1 keeps alive unless either side said `close`; HTTP/1.0 closes unless asked to keep).
- **Methods:** `Request.method` is an `http.HttpMethod` -- `Get`/`Head`/`Post`/`Put`/`Patch`/
  `Delete`/`Options`, or `Other(name)` for anything else (case-sensitive: `get` is
  `Other("get")`). `http.method_parse`/`method_name` convert. `client` takes the same type.
- **Routing.** `server.route(req, method, pattern)` is `Some(params)` on a match, else `None`;
  `{name}` captures one non-empty, percent-decoded segment, a trailing slash is ignored, and
  `server.param(params, "name")` reads it (a name the pattern doesn't have is a fatal error --
  a bug in the route function, not a value to check). `not_found_or_method_not_allowed` is 405
  with `Allow` if the path matches a different method, else 404. `server.query_param(req, "q")`
  reads the query string (percent-decoded, `+` is a space). A `{id}` capture is always a
  *string*; converting it and answering 400 on a bad one stays in the application.
- **Typed endpoints.** `server.Endpoint<C> { method, pattern, handler }`, built by one of nine
  builders, chosen by where the input comes from -- `handler` (none), `path_handler` (`path:`
  turns captured `Params` into a `P`, an `Err` is 400), `body_handler` (`decoders:`),
  `path_body_handler` (both, for `PUT /things/{id}`), `*query_handler` (the query string,
  `query_param` the tool) -- crossed with who may call, `public_` or `secure_`. Every builder
  takes `encoders:`, `run:`, `api:`, and optional `status:` (default 200). `api:` is one
  `server.Api<E> { failure, fault }` per application: `failure(e)` renders an `Err` of the
  endpoint's own error type `E`, `fault(status, message)` renders what the library itself
  rejects. A `secure_` builder also takes `who: fn(C, Request) -> Result<U, Denied>`
  identifying the caller, passed to `run` as its second argument (`public_`'s `run` has no
  caller parameter); `Denied` is `Unauthenticated`/`Forbidden`/`Failed` (401/403/500),
  answered before the body is read, the path decoded, or a format negotiated. `run` returns
  `Result<O, E>` -- an ordinary function, no request-awareness needed. `encoders: []` answers
  with an empty body and no `Content-Type` (what a 204 needs). A `Decoder<I> { media, decode }`
  turns request bytes into an `I` (`Err` is 400); an `Encoder<O> { media, encode }` turns an
  answer into bytes -- built with `server.decoder`/`encoder` (bytes) or `text_decoder`/
  `text_encoder` (text; a body NUL is 400 there). An endpoint answers 415 with no decoder
  matching `Content-Type`, 406 with no encoder matching `Accept`. `Request`/`Response.body` is
  `[byte]` (`request_text`/`response_text` give text, or an `Err` on a NUL). `server.routes<C>`
  compiles a table once (a module-level `val`, not per request), splitting each pattern into
  segments and grouping by method/segment-count so a request only meets endpoints that could
  match; `server.dispatch<C>(table, ctx, req, fault)` calls the first match, 404, or 405 with
  `Allow`. Table order still decides `{param}` vs. a literal (`/items/{id}` before
  `/items/new`); `server.route`/`path_params` remain for a hand-written router.
- **Not yet:** HTTP/2, multipart, compression, trailers.

## Transactions (`datasource.with_transaction`)

`datasource.with_transaction<R>(conn, work: fn(Connection) -> Result<R, DbError>) ->
Result<R, DbError>` runs `work` in a transaction on one exclusive connection: borrows one from
a pool, begins, calls `work(tx)`, commits on `Ok` or rolls back on `Err`/a failed commit, and
hands the connection back on every path out, returning `work`'s result unchanged. `work` (a
function value, usually a lambda) must not call `begin`/`commit` itself.

```ziman ignore: `...` stands in for the statement's arguments
fn transfer(tx: datasource.Connection, t: Transfer) -> Result<int, DbError> {
    try datasource.execute(tx, "UPDATE acct SET balance = balance - $1 WHERE id = $2", ...)
    try datasource.execute(tx, "UPDATE acct SET balance = balance + $1 WHERE id = $2", ...)
    return ok(t.amount)
}
val moved = try datasource.with_transaction<int>(conn, fn(tx) -> Result<int, DbError> { return transfer(tx, t) })
```

## Regular expressions (`regex`)

```ziman fragment
use regex

val re = try regex.compile("(\\w+)@(\\w+)\\.com")           // Result<Regex, string>
val m = regex.find(re, "mail bob@site.com now")             // Option<Match>
regex.replace_all(re, "bob@site.com", "$2: $1")             // "site: bob"
```

`compile(pattern)` is `Err` with a message naming the problem's position; a `Regex` is
immutable (compile once, share across threads). `is_match`, `find`, `find_from`, `find_all`,
`group`, `group_named`, `replace`, `replace_all`, `split`, `escape` do the rest. A `Match` has
`start`/`end` (byte offsets), `text`, `groups` (`[Option<string>]`, 1..n). Replacements expand
`$0`, `$1`-`$9`, `${12}`, `$$`.

**Perl-flavoured, deliberately without backreferences or lookaround** -- matching runs in time
proportional to text length times pattern size, so a pattern from a request can't hang a
program (`(a+)+$` against many `a`s answers immediately). Supported: literals, `.`, classes
`[a-z]`/`[^...]` (with `\d \w \s` inside), `\d \D \w \W \s \S` (ASCII), `\b \B`, `^ $`, groups
`( )`/`(?: )`/named `(?P<year>...)`/`(?<year>...)`, `|`, `* + ? {n} {n,} {n,m}` (greedy, or lazy
with `?`). POSIX classes (`[[:alpha:]]` etc., ASCII, `[:^alpha:]` negates) and Unicode
properties (`\p{L}`/`\p{Lu}`/`\p{Ll}`/`\p{N}`/`\p{Nd}`/`\p{P}`/`\p{S}`/`\p{Sc}`/`\p{Z}`/`\p{M}`
and the rest of the general categories, plus main-block scripts `Latin Greek Cyrillic Armenian
Hebrew Arabic Devanagari Thai Hiragana Katakana Hangul Han`; `\P{L}`/`\p{^L}` negate, `\pL` is
`\p{L}`). A named group reads with `group_named(m, "year")`; a replacement uses `${year}` or
`$year` (`${year}_x` when text follows). Flags first: `(?i)` ASCII case-insensitive, `(?s)` `.`
matches newline, `(?m)` `^`/`$` match at line breaks. Text is UTF-8, `.`/classes match a whole
character, but `\d \w \s`/`(?i)` stay ASCII (use `\p{L}`/`\p{Nd}` for Unicode). `$` matches only
the very end unless `(?m)` (`^...$` for the whole text). A repetition count caps at 1000, a
compiled pattern at 20,000 instructions, and a pattern whose matching would need more than 64 MB of
capture buffers is refused when it compiles -- that last one is a cap on *capture groups* in
practice (about 1,170 of them), and the error says so and suggests `(?:...)` for a group whose text
you do not need. A match is also bounded in *work*, which that budget does not bound: a pattern that
is cheap to compile can still be expensive per input byte, so a search that would do more work than
its subject's length allows stops with a runtime error naming the subject's size, and a `find_all`
is budgeted as one scan rather than per match. Real patterns use a ten-thousandth of what is
allowed; what hits this is hundreds of capture groups over a short subject. `find_all` steps one
character past an empty match; `split` ignores empty matches.

### Trace context

`http` reads and writes W3C Trace Context's `traceparent`, so one request can be followed across
services. `traceparent_of(req)` answers the incoming trace, or a new one when the header is absent
or malformed -- a restart is what the spec requires of a receiver that cannot parse it, so this is
not an error path. `traceparent_header_for(p)` is the `Header` to put on an outgoing request: the
same `trace_id`, a new `span_id`, and the sampling decision inherited rather than retaken.
`traceparent_parse`/`_new`/`_child`/`_value` are the pieces underneath, and `p.trace_id` is what
goes to `log.with_request_id` so every line of a request carries it.

Four validation rules are easy to get wrong and are all enforced: an all-zero trace or span id is
invalid even though it is well-formed hex; version `ff` is reserved; an upper-case hex field is
rejected rather than lower-cased, because the header is compared as bytes elsewhere; and undefined
flag bits are ignored rather than refused. A *future* version parses when the rest of the value
does, since refusing one would break against a future sender for no gain.

## HTTP client (`client`)

`client.get(url)`, `client.post_json(url, body)`, and the general `client.request(method, url,
headers, body, timeout_ms, max_redirects, ca_file)` (everything after `url` optional, called
with named arguments) return `Result<http.Response, ClientError>`. **Any** HTTP status
(404, 500 included) is `Ok`; `Err` means the exchange itself failed: `BadUrl`/`Network`/
`Timeout`/`BadResponse`/`TooLarge`/`TooManyRedirects` (`error_message` renders one). `http://`
and `https://` (chain and hostname verified; `ca_file` for a private CA). A keep-alive
connection is parked and reused for the next request to the same host (`reuse: false` disables;
`close_idle()` drops parked ones; a dead one is detected and the request re-sent once). Reads
`Content-Length`, chunked, and until-close bodies; follows 301/302/303/307/308 up to
`max_redirects` (default 5, 0 returns the 3xx; 303 and a POST answered 301/302 re-send as a
body-less GET); caps a body at 8 MiB. Body is `[byte]` both ways (`request`'s `body:
Option<[byte]>`, the response's `[byte]` body -- untouched even with a 0 byte);
`http.response_text` reads text (`Err` on NUL); `post_json`/`put_json`/`post_form` take text.

For a response too large to hold or that never ends, `client.stream(method:, url:)` returns
status/headers at once and the body as an `Iter<Result<[byte], ClientError>>` of up-to-8192-byte
blocks, pulled by a `for` loop (a timeout/read error is a last `Err` item; leaving early closes
the connection) -- no redirects followed, no cookies sent, no compression decoded (asks `identity`).

Also built in: `gzip`/`deflate` response decoding (`Accept-Encoding` sent unless `compress:
false`); proxies from `proxy` (`""` = none) or `HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY`, `CONNECT`
tunnel for `https://` targets (an `https://` proxy itself unsupported); Basic auth from
`http://user:pass@host/` (Authorization/Cookie dropped on a cross-host/port/scheme redirect);
cookies via `request_with_jar(jar, method, url, ...)` (returns the updated jar; `cookie_jar()`
makes an empty one, no jar means nothing remembered); `post_form(url, Map)`/`form_encode`;
`response_header`; public `parse_url(url)`. Not yet: HTTP/2.

## Files (`fs`)

`fs.read_text`/`write_text`/`append_text`/`exists`/`is_dir`/`size`/`remove` (file or empty
directory)/`make_dir`/`rename`/`list_dir` (sorted). `fs.lines(path)` reads a file lazily line by
line as an `Iter` (memory for one line, any file size); `fs.chunks(path, size)` the same for
byte blocks (`Iter<Result<[byte], string>>`); `fs.walk(dir)` finds files under a directory depth
first, sorted, without listing them all first (`Iter<Result<string, string>>`; directories not
listed, a symlink to one not followed). Everything fallible returns `Result<_, string>` with
the OS message.

`fs.resolve(name) -> Option<string>` finds `name` by searching `ZIMAN_RESOURCE_PATH` (a
colon-separated list, first match wins, like `PATH`/a Java classpath). Unset, it returns
`Some(name)` unchanged; set with no match, `None`. `properties.load`/`migrate.run` both resolve
through this, so e.g. `ZIMAN_RESOURCE_PATH=src/main/resources` for an app and
`.../test/resources:.../main/resources` for its tests both find `application.properties` with
no code change.

## Logging (`log`)

No global logger (no mutable globals): build one with `log.logger(level: Level.Info, file: ...,
timestamps: ..., prefix: ...)` and pass it around. `log.debug/info/warn/error(logger, message)`
write `<timestamp> LEVEL prefix message` to stderr, or append to `file` (falling back to
stderr); below the logger's level is dropped. `with_prefix` makes a per-component logger,
`level_parse("warn")` reads a level from configuration.

`log.logger(..., json: true)` writes one JSON object per line instead -- `{"time":...,"level":
"info","prefix":...,"request_id":...,"message":...}` -- for a log something else reads. The level is
lowercased and the prefix trimmed, since the text format's column padding is presentation and has no
business in an indexed field; `prefix` and `request_id` are left out entirely when empty rather than
written as `null`. `with_request_id(logger, id)` stamps every line a logger writes from then on, so a
request's handler sets it once instead of threading it through every call -- the same shape as
`with_prefix`, and an empty id clears it. Escaping goes through `json`, so a message carrying
arbitrary bytes from a user is still a document another parser accepts (the text format's `\xNN` for
a control byte is deliberately not valid JSON, and a hand-rolled escaper here would reproduce the
invalid-UTF-8 bug A8 fixed).

A logger with `file` can rotate: `log.logger(..., max_bytes: 10485760, keep_files: 5)`. A file
at `max_bytes` is renamed `<file>.1` before the next write to a fresh `<file>` (`.1`→`.2`, ... up
to `.<keep_files>`, the one past that deleted; `keep_files: 0` just deletes). Without
`max_bytes` (default), nothing rotates. Safe with several threads logging to one file -- a lock
around rotation, size re-checked under it.

## Cryptography

The `crypto` core module is hashes, MAC, key derivation, password hashing and base64 on
OpenSSL's libcrypto (already linked for TLS) and, for bcrypt, libxcrypt; `[byte]` in and out.

- **Hashes**: `sha1` (broken -- legacy formats and MySQL's login only), `sha256`, `sha512`.
- **Keyed**: `hmac_sha256`, `hmac_sha512`, `pbkdf2_hmac_sha256`/`pbkdf2_hmac_sha512(password,
  salt, iterations, length)` (password to key or storable hash).
- **Passwords**: `password_hash(password)` (Argon2id, PHC format, fresh random salt inside),
  `password_verify(password, stored)` (checks Argon2id or bcrypt `$2a$`/`$2b$`/`$2y$`, the
  hashes Spring Security's `BCryptPasswordEncoder` writes), `password_needs_rehash(stored)`
  (true for bcrypt or weaker Argon2id). Parts are public too: `bcrypt_hash(password:, cost:)`/
  `bcrypt_verify`, `argon2id_hash(password:, memory_kib:, iterations:, parallelism:)`/
  `argon2id_verify`/`argon2id_raw`. Comparisons are constant-time.
- **Bytes**: `bytes_xor` (no bitwise operators in the language) and `bytes_equal`
  (constant-time -- use for MACs/tokens, not `==`).
- **base64**: `base64_encode`/`base64_decode` (standard, padded, strict);
  `base64url_encode`/`base64url_decode` (URL-safe, unpadded -- JWTs). Hex is in `number`
  (`bytes_to_hex`/`bytes_from_hex`).
- **Errors**: a wrong argument (mismatched lengths, an out-of-range iteration count) is a
  runtime error; malformed data (`base64_decode`, `bytes_from_hex`) is `Err(string)`.
- Not a general crypto library -- no ciphers, no public-key operations.

### JSON Web Tokens (`jwt`)

`jwt` signs/verifies with a shared secret (HS256/384/512), on `crypto`/`json`/`clock`.

```ziman fragment
use crypto.base64_decode
use json.Json
use jwt

val secret = "c2hhcmVkLXNlY3JldC1vZi0zMi1ieXRlcy0xMjM0NTY="   // 32 random bytes, base64
val key = unwrap(base64_decode(secret))
val claims = jwt.with_claim(jwt.claims("ada@example.com", 3600), "role", Json.JString("admin"))
val token = unwrap(jwt.sign(claims, key))
switch jwt.verify(token, key) {
    Ok(c) { print(unwrap_Some(jwt.subject(c))) }
    Err(e) { print(error_message(e)) }
}
```

- **`claims(subject, expires_in_seconds)`** gives `sub`/`iat`/`exp`; `with_claim` adds more.
  `sign` picks the algorithm from key size (32/48/64 bytes -> HS256/384/512); `sign_with(claims,
  key, "HS512")` chooses explicitly.
- **`verify(token, key)`** returns claims or a `JwtError` (`Malformed`, `UnsupportedAlgorithm`,
  `BadSignature`, `Expired`, `NotYetValid`, `MissingClaim(name)`, `WrongIssuer`,
  `WrongAudience`). Refuses `alg: none` and non-HS algorithms, compares the signature
  constant-time, checks `exp`/`nbf`; `verify_leeway`/`verify_at(token, key, now, leeway)`
  forgive/choose a clock. **A token must have `exp`** (else `MissingClaim("exp")`); a
  non-numeric `exp`/`nbf` (`"exp": "never"`) is `Malformed`, never "no expiry".
- **`verify_claims`** is `verify` with every check named: `jwt.verify_claims(token:, key:,
  issuer:, audience:)`, plus `leeway_seconds` (0), `now` (current time), `require_exp` (true --
  `false` only for an open-ended token). A given `issuer` must equal `iss`, a given `audience`
  must equal or be one of `aud`; absence is `MissingClaim("iss"/"aud")` -- what stops a token
  for one service being accepted by another sharing the key. `jwt.issuer`/`audience(claims)`
  read them (audience always a list); `sub` is `jwt.subject`.
- Tokens from other libraries verify and vice versa (checked against RFC 7515's example).

### One-time passwords (`otp`)

`otp` makes/checks TOTP codes (RFC 6238: HMAC-SHA-1, 30-second steps), pure Ziman on `crypto`.

```ziman fragment
use otp

val typed_code = "492039"
val now_seconds = 1760000000
val secret = otp.generate_secret()                                   // base32, shown to the user once
val key = unwrap(otp.base32_decode(secret))
otp.totp_valid(key, typed_code, now_seconds, 1)                      // this step and one either side
```

`hotp(key, counter, digits)`/`totp(key, unix_seconds)` give the codes; `provisioning_uri("My
App", email, secret)` builds an `otpauth://totp/...` URI for a QR code; `base32_encode`/`_decode`
are public. Checks compare constant-time; tests use the RFC 4226/6238 vectors.

## Configuration (`properties`, `os`)

`os.getenv(name) -> Option<string>` reads an environment variable (`Some("")` set-but-empty,
`None` unset); `properties` builds Spring-Boot-style configuration on it.

The rest of `os` is for ending cleanly: `os.exit(code)` flushes stdout/stderr then exits;
`os.write_stderr(text)` (no newline added); `os.install_shutdown_handler()` makes SIGINT/SIGTERM
set a flag instead of killing the process, `os.shutdown_requested() -> bool` reads it, and
`net.accept` errors within ~0.25s of the signal so an accept loop can stop, drain, and exit:

```ziman ignore: `{ ... }` stands in for what the loop does with each client
os.install_shutdown_handler()
while !os.shutdown_requested() {
    val client = net.accept(listener)
    if is_ok(client) { ... }
}
```

The signal can arrive on any thread, so only `accept` notices it automatically -- other blocking
work needs its own arrangement (a socket timeout, say).

```ziman
use properties

#[derive(Properties)]
struct ServerConfig { port: int  idle_timeout_ms: int  banner: Option<string> }

fn load_config() -> Result<ServerConfig, string> {
    val loaded = try properties.load("application")     // application.properties (+ -<profile>)
    return server_config_from_properties(loaded, "server")
}
```

- **Format**: `key=value` lines, `#` comments only at line start, lowercase dotted kebab-case
  keys, trimmed values, no quoting/escapes/inline comments. A duplicate key, uppercase key, or
  malformed line is an error naming `file:line`.
- **Loading**: `load("application")` reads `application.properties` (required), then
  `application-<profile>.properties` if a profile is active (env `PROFILE_ACTIVE`, else
  `profile.active` in the base file; a missing profile file errors). `parse(text, source)` is
  the pure form. Both file names resolve through `fs.resolve` first, so `ZIMAN_RESOURCE_PATH`
  finds them wherever it points, unchanged code otherwise.
- **Precedence**: environment (`server.port` -> `SERVER_PORT`) > profile file > base file.
- **Placeholders** (resolved once at load/parse, so a bad one fails startup): `${name}`/
  `${name:default}` resolve against properties, then the environment, then the default (which
  may contain `:`); unresolved or cyclic is an error.
- **Getters**: `get_string/int/float/bool` (required); an optional key is `lookup(props, key)
  -> Option<Entry>` plus `entry_int/float/bool/string`; errors name the key and source (`server.
  port: '80x' is not an integer (application.properties:3)`).
- **`check_unknown(props, known_keys)`** fails on keys outside the list, suggesting the closest
  match; `profile.active` is always allowed.
- **`report(props, keys)`** returns effective values with sources for a startup log; secrets
  (keys containing `password`/`secret`/`token`/`key`, and URL credentials) are masked.
- **`#[derive(Properties)]`** on a struct of `int`/`float`/`bool`/`string` (or `Option` of
  those) generates `<snake>_from_properties(props, prefix) -> Result<T, string>` and
  `<snake>_keys(prefix) -> [string]`. `idle_timeout_ms` binds to `<prefix>.idle-timeout-ms`;
  `Option<T>` fields are optional, others required. Needs only `use properties`.

## Number formatting and parsing

The `number` core module is shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/
`byte`, pure Ziman on top of each type's own `str()`.

```ziman
use number.format_int
use number.parse_pattern

fn main() {
    val fmt = parse_pattern("#,###.00")
    print(format_int(1234567, fmt))    // 1,234,567.00
}
```

- **One engine, not five** (Ziman has no argument-based overloading). `format_number(s, fmt) ->
  string`/`parse_number(s, fmt) -> string` operate on the *string* each type already produces
  via `str()`/expects via `parse_int`; `format_int`/`_float`/`_bigint`/`_decimal`/`_byte` are
  thin wrappers over `format_number`.
- **`NumberFormat { thousands_separator: string, decimal_separator: string, decimal_places:
  Option<int> }`** -- empty `thousands_separator` disables grouping, `decimal_places: None`
  leaves the value's own digit count. `number_format_plain()` is the no-grouping, no-forced-
  places default.
- **`parse_pattern(pattern) -> NumberFormat`** builds one from a `"#,###.00"`-style pattern
  (Java `DecimalFormat`/Excel-shaped, a small subset): a comma anywhere in the integer part
  turns on group-by-3 grouping, the digit count after `.` forces that many decimal places. No
  `0`-vs-`#` distinction, no `%`, no `E`, no `;`-separated subpatterns, no literal prefix/suffix
  text, no custom grouping sizes.
- **Forcing fewer decimal places truncates, never rounds** (rounding is `math.round`/
  `decimal_round`'s job). **Scientific notation passes through unchanged** -- `float`'s `str()`
  switches to it past 6 significant digits, where grouping/place-forcing has no sensible
  meaning, so `format_number` checks for `"e"`/`"E"` first rather than mangling it.
- **Hex covers the three integral types**: `byte_to_hex`/`_from_hex`, `int_to_hex`/`_from_hex`,
  `bigint_to_hex`/`_from_hex` (not `float`/`decimal`, no natural hex for a non-whole value).
  `byte` is trivial (2 unsigned digits). `int` uses fixed-width two's complement like C's
  `printf("%x")` (`-1` is `"ffffffffffffffff"`, via `bigint` arithmetic since the unsigned
  64-bit equivalent doesn't fit a signed `int64`). `bigint` has no fixed width, so it uses
  sign-prefixed magnitude (`"-ff"`), matching Java's `BigInteger.toString(16)`.
- **`parse_byte(s) -> Result<byte, string>`** -- delegates to `parse_int`, range-checks 0-255.
- **Limits, as global vals**: `byte_min`/`_max` (0, 255), `int_min`/`_max` (the `int64` range),
  `float_min`/`_max` (most-negative/largest finite double, `float_min` exactly `-float_max`),
  `float_min_positive` (smallest positive nonzero, denormalized -- named distinctly from
  `float_min` unlike Java's `Double.MIN_VALUE`). No `bigint`/`decimal` limits (arbitrary
  precision, no finite bound). Crossing `float_min`/`_max` overflows to infinity (doesn't
  error); under `float_min_positive` underflows to `0.0`. Crossing `int_min`/`_max` *is* a
  runtime error (doesn't wrap), same as any `int` overflow. Bring a limit in with `use
  number.int_max` -- no `number.int_max` dotted access on a global val.

## The `math` module

`sqrt`, `pow`, `exp`, `log` (natural), `sin`/`cos`/`tan` (radians), constants `pi`/`e` (global
vals, `use math.pi` to bring in bare). All `float` in/out, one-line libm wrappers.

```ziman
use math
fn main() {
    print(math.sqrt(2.0))          // 1.4142135623730951
    print(math.is_nan(math.sqrt(-1.0)))  // true
}
```

- **A domain error is NaN, not a `Result`** (`sqrt` of a negative, `log` of zero/negative --
  like C/Java/JS). `compare<T: Ord>` already gives NaN a total order, so check it with
  `math.is_nan(x)` (`x != x`) rather than unwrapping. Unlike `1.0 / 0.0`, which is a runtime
  error in Ziman (not infinity) -- `math` never produces a value your own arithmetic couldn't.
- **`math.floor`/`ceil`/`round`** round a `float` to an `int` (ties away from zero: `round(2.5)`
  is 3, `round(-2.5)` is -3); **`floor_float`/`ceil_float`/`round_float`** apply the same rules
  but keep the result a `float` (for a magnitude past `int`'s range, or NaN/infinity). These
  moved here (with the `decimal_*` functions below) from bare prelude builtins -- `floor(x)`
  alone is now undefined; write `math.floor(x)`.
- **No decimal equivalent for the irrational ones** (`sqrt`/`exp`/`log`/`sin`/`cos`/`tan`) --
  cast through `float` and round with `decimal_round`. `math.decimal_round`/`_floor`/`_ceil`
  (to a given scale: nearest-away-from-zero, toward -inf, toward +inf) and
  `math.decimal_pow(base, exponent)` (non-negative integer power) *are* exact. A negative
  `exponent` is a runtime error naming `numerator / decimal_pow(base, -exponent)` instead --
  there's no scale to pick for that division on `decimal_pow`'s own.

## The `bits` module

Bitwise `and`/`or`/`xor`/`not`/`shift_left`/`shift_right` on `byte`/`int`/`bigint`, plus two
scalar-backed sets: `BitSet` (an `int`, indices 0-63) and `BigBitSet` (a `bigint`, unbounded).
Ziman has no bitwise operators in its own syntax -- this module is the only way to touch bits.

```ziman
use bits
fn main() {
    print(bits.int_and(12, 10))          // 8
    print(bits.int_shift_right(-8, 1))   // -4, sign-extending

    var bs = bits.bitset_set(bits.bitset_new(), 3)
    bs = bits.bitset_set(bs, 5)
    print(bits.bitset_to_string(bs))     // {3, 5}
}
```

- **Three scalar flavors, one convention**: `byte_*`/`int_*`/`bigint_*`, each with
  `and`/`or`/`xor`/`not`/`shift_left`/`shift_right`/`popcount` (`bigint` also `bit_length`) --
  the same type-suffixed pattern `int_to_hex` uses.
- **`byte`** is unsigned (`shift_right` is logical); shift amount 0-7.
- **`int`** is a direct native 64-bit two's-complement op; shift amount 0-63 (else runtime
  error, not C's undefined behavior); `shift_right` is arithmetic/sign-extending.
- **`bigint`** matches Java's `BigInteger`/Python's `int` exactly: `and`/`or`/`xor`/`not` are
  defined against *infinite* two's complement, so a negative bigint has infinitely many leading
  1-bits (`bigint_and(-1n, 5n)` is `5n`). Shift amount must be non-negative, no upper bound.
  Both `int_shift_right` and `bigint_shift_right` floor (toward -infinity), unlike `/`'s
  truncation: `int_shift_right(-3, 1)` is `-2`, not `-1`.
- **`bigint_popcount`/`bit_length` are a runtime error for a negative bigint** (infinite 1-bits,
  no finite answer); `BigBitSet` never produces one (a set of indices is never negative).
- **`BitSet`**: a value type over one `int` -- `bitset_new()`, `bitset_of_int`/`_to_int`,
  `set`/`clear`/`flip`/`get` (index 0-63, else runtime error), `and`/`or`/`xor`/`and_not`
  (mirroring Java's `BitSet`), `is_empty`, `cardinality`, `length` (highest member + 1, 0 if
  empty), `to_string` (`"{0, 3, 5}"`). No mutable builder -- the whole state is one scalar, so
  there's no O(n)-copy cost to save; `var` plus these functions already do the job.
- **`BigBitSet`** mirrors `BitSet` over a `bigint` -- unbounded (only a negative index rejected),
  `length()` via `bigint_bit_length` in O(digits) rather than an unbounded scan.

## Result

```ziman
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

- **`Result<T, E>` is a builtin, not a declared enum** -- manipulated through `ok`, `err`,
  `is_ok`, `unwrap`, `unwrap_err` (see "Builtins") or `switch` (arms `Ok(v)`/`Err(e)`, a
  statement not an expression). You can't declare your own `Result`-shaped type, only an
  `enum` (see "Enums").
- **`ok(x)`/`err(e)` can't fully infer their own type** -- each determines only half of
  `Result<T, E>`. Legal in exactly two places that supply the missing half: a `val`/`var` with
  an explicit `Result<T, E>` annotation, or a `return` inside a function declared `-> Result<T,
  E>` (the common shape). Elsewhere (a bare statement, a call argument) it's a compile error --
  the same restriction empty array literals have, for the same reason (nothing to infer the
  missing type from); `[]` gets inferred in a couple more positions (struct fields, `with`
  overrides) since it only needed one type, not two to disambiguate.
- **`unwrap`/`unwrap_err` are the explicit, opt-in escape hatch to a fatal error** -- `unwrap(r)`
  returns the success value or raises a runtime error on `Err` (mirror for `unwrap_err`);
  crashing on failure becomes the caller's explicit choice, not something forced unconditionally.
- **`==`/`!=` compares variant then payload** -- `Ok`/`Err` never equal each other; same
  variant is equal iff payloads are. Legal only when both `T` and `E` are comparable
  (recursively, same rule as structs/arrays), since the variant isn't known statically.
- **`T`/`E` can be any Ziman type**, including another `Result`, an array, or a struct --
  letting application code layer a richer error type over a simpler native one, converting at
  the boundary rather than forcing one universal error shape.
- Printing shows `Ok(...)`/`Err(...)`, recursively displaying the payload like `print` always does.

## Defer

`defer f(args)` runs a call when the enclosing *block* exits, by any path:
falling off the end, `return`, a `try` that propagates an error, `break`, or
`continue`. It is how a resource is released without destructors.

```ziman
use datasource
use db.DbError
use net

fn read_reply(sock: net.Socket) -> string { return "0" }   // stands for the caller's own reading

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

- **Block-scoped** -- runs when its own block ends (an `if`/loop body, not just the function),
  so one inside a loop body runs every iteration (unlike Go, where defers pile up to the return).
- **Last in, first out** across a block's defers.
- **Arguments are evaluated when `defer` runs, not when it fires**: `var n = 1; defer show(n);
  n = 2` shows `1` -- only the call itself is postponed.
- **The return value is computed first**: `return x * 2` evaluates `x * 2`, runs the defers,
  then returns -- a deferred call can't change what's returned; `try` propagates the same way.
- **A single call only** -- not a block, not `f()?`; put cleanup logic in a helper function
  instead. The argument-expression-reading builtins (`format`, `ok`, `err`, `channel_new`)
  can't be deferred.
- **The result is discarded, including a `Result`** -- a deferred `net.close`/`rollback` has
  nowhere to report failure. `defer` is the one exception to must-use `Result` (see "Must-use
  `Result`"), since there's nowhere left to propagate to by the time it runs -- deliberate, and
  what lets `defer datasource.rollback(conn)` stay harmless after a successful commit.
- **A fatal runtime error skips defers** -- `unwrap` on `Err`, an out-of-bounds index, exit the
  process instead of unwinding, so nothing deferred runs (the OS closes sockets, a DB server
  rolls back a dropped connection's transaction, but nothing else deferred happens).
- **Threads**: a `spawn`ed function gets its defers like any other.

## Concurrency

`Channel<T>` and `spawn` — a third hardcoded parametric type (after
`[T]`/`Result<T, E>`) plus one new statement, giving Ziman real OS-thread
concurrency built on a single-writer / actor pattern: one thread owns a
piece of mutable state and is the only thing that ever touches it; every
other thread reaches it only by sending a message and waiting for a
reply.

```ziman
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

- **Builders and iterators stay in their thread.** `MutableString`, `MutableArray`,
  `MutableMap`, `MutableSet` and `Iter` are mutable, so each belongs to the thread that made
  it: not a `Channel` element, `spawn` argument, or module-level `val` (compile errors), and
  any other cross-thread use is a runtime error. See "Attributes and derive" (`#[thread_bound]`).
- **`Channel<T>` is a thread-safe queue of one element type**, unified rather than split into
  sender/receiver types -- any value supports both `send`/`receive`. **Unbounded**
  (`channel_new()`: `send` never blocks) or **bounded** (`channel_new(8)`: `send` blocks while
  full -- backpressure). `receive(chan) -> T` blocks until available; `try_send`/`try_receive`
  never block (`false`/`None` when full/empty); `channel_len(chan) -> int`. A bounded `send`
  with nobody receiving waits forever; there is no deadlock detection.
- **Closing, timed receives, two-channel waits.** `channel_close(chan)` stops new sends (closing
  twice is harmless); already-queued values still deliver. `send`/`try_send` on a closed channel
  is a runtime error, and a `send` blocked on a full channel is woken by the close and fails the
  same way. Plain `receive` on closed-and-empty is a runtime error too; `receive_option(chan) ->
  Option<T>` blocks like `receive` but returns `None` once closed and drained (no sentinel
  needed). `receive_timeout(chan, millis) -> Option<T>` gives `None` on timeout (indistinguishable
  from closed-and-empty). `select2(a, b) -> Selected<A, B>` (`First(x)`/`Second(y)`, possibly
  different element types) returns whichever is ready first, `a` on a tie; polls both with a 1 ms
  pause (up to ~1ms latency), same no-deadlock-detection property.
- **`channel_new()` can't infer its own type** -- same context-inference mechanism as
  `ok`/`err`/`[]`, legal in the same four positions (an annotated `val`/`var`, a struct field, a
  `with` override, a function's declared return type).
- **Not comparable**: `==`/`!=` on a `Channel` is a type error (a stateful handle, not data).
- **`spawn name(args)` starts `name` on a new OS thread**, `args` evaluated on the calling
  thread first. `name` must be a declared function (never a function value -- `spawn mod.name`
  for an imported one, subject to the same `pub` rule as a qualified call; a generic spawn
  writes its type arguments, `spawn worker<int>(args)`). **The spawned function must return
  `void`** -- there's no join/thread-handle, so its result is only observable through a
  `Channel` it's explicitly given, and `spawn` is a statement, not an expression.
- **A runtime error in a spawned thread ends that thread, not the program**: written to stderr,
  the thread's `defer`red calls run (innermost first), everything else continues (a main-thread
  error still ends the program). `spawn worker(x) on_error handler` (`handler: fn(string)`) runs
  first, while resources are still open, then the defers; `on_error os.fatal` makes that thread's
  failure end the whole program like a main-thread one. `server.serve` uses this per connection:
  a crashing handler gets a 500, its slot and socket release, and the server continues. Nothing
  but the defers runs -- a lock a native held at the moment of the error stays held (none of the
  library's natives hold one across a possible error). If that thread was the only one that could
  have sent on a channel someone is blocked reading, the reader does not wait for ever: when every
  live thread is blocked on a channel, nothing can ever send, and that is reported as a runtime
  error (`deadlock: every thread is blocked on a channel ...`) on each blocked thread. The failed
  thread's own error is printed first, so the two together say what happened and why the program
  stopped. A thread that is computing, sleeping or in a syscall counts as live, so this fires only
  when every thread really is waiting on a channel.
- **`thread` module**: `thread.id()` (0 for main, then 1, 2, ... in start order), `thread.name()`
  (`main` or `<function>-<id>`; `set_name` renames), `is_main()`, `stack_remaining()` (bytes
  before "stack depth exceeded"). No join or thread handle. `thread.ThreadLocal<T>` is a
  per-thread slot (like Java's `ThreadLocal`): make the key once as a module-level `val` via
  `thread.local<T>(init: fn() -> T)`, then `thread.get(key)`/`set(key, v)`/`has(key)`/
  `reset(key)` (forgets it, so the next `get` re-runs `init`) per thread. A slot holds an
  immutable value (`set` to update; keep a `MutableMap`/`MutableArray` in it for a large mutable
  cache -- a builder must stay on its own thread, which a thread-local slot guarantees; marked
  `#[thread_key]`, it's the one type allowed to sit at module level). Slots drop when the thread
  ends -- right for per-request scratch space (`server.serve`'s one-thread-per-connection), not
  for a cache that must outlive connections (an actor thread owning it over a channel is that tool).
- **Sharing a `Value` across threads needs no locking of its own.** An `ArrayObj`/`StructObj`/
  `EnumObj`/`ResultObj` is immutable once built, so crossing a channel is just a pointer copy --
  no data race, and no use-after-free since the collector only reclaims what's unreachable
  everywhere. The one thing that would break this, a mutable handle, is refused wherever it could
  cross a thread boundary (`#[thread_bound]`) instead of being allowed to race.

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
| `int`/`bigint` arithmetic overflow | traps, never silently wraps or saturates (`+`/`-`/`*`/`/`, `abs(MIN)`) | the interpreter |
| An `int` literal past `int`'s range | a compile-time error, not silent saturation | the parser |
| `bigint`/exact-`decimal` digit count (`parse_json_exact`, literals) | 1,000 digits | `json`'s number parser |
| A parsed number's exponent size | 100,000 | `json`'s number parser |
| `parse_json` nesting depth | 512 levels | `json` |
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

## Builtins

Builtin names are reserved — redefining any of them as a user function is
a type error.

- `print(x)` — one argument of any non-void type, writes its display form plus a newline to
  stdout. A `float` displays as the shortest decimal text that reads back exactly, always with
  a `.` or `e` (`1.0`, never bare `1`); plain notation from `1e-7` to `1e21`, scientific outside
  that. `format`'s `{}` displays a `float` the same way.
- `concat(pieces: variadic string) -> string` — every piece joined with no separator (`""` for zero
  pieces); no auto-stringify (use `str` first), same anti-implicit-coercion reasoning as `+`.
  `join` adds a separator/prefix/suffix; a `MutableString` avoids `concat`'s own O(n) recopy
  when building one piece at a time in a loop (`--lint` flags `x = concat(x, ..)` there).
- `str(x) -> string` — converts `int`/`float`/`bool`/`bigint`/`decimal`/`byte` to display text
  (a `float` the same way `print` does). Rejects an already-`string` argument, and a bare
  generic type parameter `T` (a fixed allowlist checked before type-parameter resolution) --
  use `format`'s `{}` inside a generic function instead.
- `printf(fmt, args...)` — `format`, written straight to stdout with no newline added and
  flushed immediately (for a prompt or progress line). Same compile-time checks as `format`.
- `format(fmt, args...) -> string` — the only variable-arity call in Ziman. `fmt` must be a
  string literal with zero or more `{}` placeholders, their count checked at compile time
  against the remaining arguments. Placeholders are untyped (no `%d`/`%s` verbs) -- the
  argument's own declared type is already known. `{:.N}` (`N` 0-100) fixes a `float`/`decimal`
  argument's digits after the point (`{:.2}` of `3.14159` is `3.14`) -- a `decimal` rounds half
  away from zero exactly, a `float` by its binary value (like C's `printf("%.2f")`); a compile
  error on any other type, and nothing else follows the colon. `{{`/`}}` write a literal
  `{`/`}` (a lone `}` or an unmatched `{` is a compile error).
- `format_dynamic(template: string, args: [string]) -> Result<string, string>` — `format` for a
  template that's a runtime *value* (a message catalog), string arguments only; what `format`
  checks at compile time (malformed template, placeholder/argument count, `{:.N}` on a string)
  becomes an `Err` instead.
- `len(arr) -> int` / `len(s: string) -> int` — element count, or byte length. Also answers for a
  `Map`, a `Set`, and any of the four builders (`MutableArray`, `MutableString`, `MutableMap`,
  `MutableSet`), and for a `Mutable<T>` cell, whose length is always 1. So asking a collection its
  size never needs a per-type name or a conversion. It answers for a *handle*, not for a struct
  that holds one, which has no length of its own.
- `mutable_new(v) -> Mutable<T>` / `mutable_get(c) -> T` / `mutable_set(c, v)` /
  `mutable_update(c, f: fn(T) -> T) -> T` — one cell holding one value: the root the four mutable
  collections specialise, and what lets a new mutable shape be written in Ziman instead of added to
  the language (a ring buffer is a struct over a `MutableArray` plus two `Mutable<int>`s). An
  update returns the new value, so a counter is `mutable_update(n, fn(x) { x + 1 })`. Always
  initialised -- there is no empty `mutable_new<T>()`, because `T` may have no zero -- and `T` is
  anything, including another handle. It answers `len(c)` and `contains(c, v)` but **not**
  `snapshot` or `clear`: crossing out is `mutable_get`, an O(1) read rather than an O(n) copy, and
  emptying would need exactly the zero its type may not have. So a cell has no `from`/`snapshot`
  pair at all -- `mutable_new` takes a value and `mutable_get` returns one, which *is* the crossing
  between the two worlds.
- `clear(b)` — empties a builder and leaves it usable, for reusing one across the passes of an
  outer loop instead of allocating a fresh one each time. Takes a builder only: an empty `Map` or
  array is a value you write down (`Map<K, V> {}`, `[]`), not something you do to one you have. A
  snapshot taken before the clear is undisturbed by it.
- `snapshot(b)` — a builder's contents copied out as the immutable value it is building: an
  `MutableArray<T>` gives `[T]`, a `MutableString` gives `string`, a `MutableMap<K, V>` gives
  `Map<K, V>`, a `MutableSet<T>` gives `Set<T>`. One name for `snapshot`/`snapshot`/
  `snapshot`/`snapshot`, which all still work and do the same thing: an O(n) copy, after
  which the builder stays usable and later changes do not touch the snapshot. Takes a builder
  only -- an array is already a value, so there is nothing to copy out of.
- `substring(s, start, end) -> string` — `[start, end)`, `0 <= start <= end <= len(s)` else a
  runtime error. Does not copy -- a string is a pointer+length over immutable bytes, so this
  (and `trim`/`char_at`/`split`'s pieces) is a view, free to make, `len` O(1); it keeps the
  whole parent buffer alive, which matters once memory is reclaimed.
- `split(s, sep) -> [string]` — splits on every `sep` occurrence (`sep` must not be `""`,
  runtime error). Empty pieces (adjacent/leading/trailing separators) are kept.
- **`[byte]`/`[bool]`/`[int]`/`[float]` are stored packed** (one real byte/byte/int64/double
  per element, not an 80-byte boxed value) -- invisible except for cost (a million-`int` array
  is 8 MB); indexing/`for`/`push`/`slice`/`==`/`hash`/`compare` behave identically either way.
- `slice<T>(a, start, end) -> [T]` — elements `[start, end)`, same bounds rule as `substring`.
  Does not copy (shares the original's storage, like `substring`); `push`/`insert`/`set` on
  either build a new array, so neither changes under the other.
- `concat_arrays<T>(parts: variadic [T]) -> [T]` — one array from many in a single allocation
  (`concat_arrays<int>(a, b)` or `concat_arrays<int>(parts: arrays)`; an empty `[]` literal
  can't be inferred as a variadic argument -- pass a typed `val`).
- `char_at(s, i) -> string` — the one-byte string at `i` (`0 <= i < len(s)`, else runtime error).
- `split_once(s, sep) -> Option<[string]>` — `Some([before, after])` at the *first* `sep`,
  `None` if absent -- for `key=value` text whose value may itself contain the separator.
- `index_of(s, needle, from) -> int` — byte index of the first `needle` at/after `from`
  (`0 <= from <= len(s)`, else runtime error), or `-1`; an empty `needle` matches at `from`.
- `last_index_of(s, needle) -> int` — byte index of the last `needle` (`len(s)` if empty), or `-1`.
- `starts_with(s, prefix) -> bool` / `ends_with(s, suffix) -> bool`.
- `is_char_at(s, i, c) -> bool` — whether `c` appears at `i`; `false` (never an error) when `i`
  is out of range or `c` doesn't fit, replacing a guarded `p < len(s) && char_at(s, p) == "x"`.
- `replace(s, old, new) -> string` — swaps every non-overlapping `old` (must not be empty).
- `equals_ignore_case(a, b) -> bool` — ASCII case-insensitive equality, allocating nothing
  (unlike `to_lower(a) == to_lower(b)`). None of the functions above build intermediate strings.
- **Strings are bytes, with code points opt-in.** The functions above count/cut bytes, not
  characters (`len("héllo")` is 6; `substring` can split a multi-byte character). Three
  functions give the code-point-aware alternative (slower -- they decode UTF-8 -- and a runtime
  error naming the byte offset on invalid UTF-8, reachable only by chaining a byte-level
  function into one, since `bytes_to_string` already rejects invalid UTF-8 at the one real
  boundary): `chars(s) -> [string]` (split into code points: `chars("héllo")` is `[h,é,l,l,o]`),
  `char_count(s) -> int` (cheaper than `len(chars(s))`), `truncate_chars(s, max_chars) ->
  string` (shortened without cutting a code point in half; `max_chars >= char_count(s)` returns
  `s` unchanged, negative is a runtime error).
- `trim(s) -> string` — leading/trailing whitespace removed.
- `lines(text) -> [string]` — the lines, without their terminators. Not the same as
  `split(text, "\n")`: a `\r` is removed from each line of CRLF text, and a final newline
  *terminates* the last line rather than starting an empty one, so `"a\nb\n"` and `"a\nb"` both
  have two lines. A blank line in the middle is a real line and stays; an empty string has none.
- `reverse_string(s) -> string` — `s` with its characters in the opposite order, reversing *code
  points* rather than bytes, so text outside ASCII comes back as text. `collections.reverse` is the
  array version. `is_empty(s) -> bool` (`s == ""`);
  `is_blank(s) -> bool` (empty or only whitespace). `to_upper`/`to_lower(s) -> string` — ASCII only.
- `parse_int`/`parse_float`/`parse_bigint`/`parse_decimal(s) -> Result<_, string>` — parses all
  of `s`; `Err` on leading/trailing junk (blanks, a leading `+`) or a non-parse. A number is an
  optional `-` and digits (a float also a point/exponent; `parse_float` reads `nan`/`inf`/`-inf`,
  not hex or `Infinity`/`NaN`). All four agree.
- `contains(s, needle) -> bool` / `contains(arr: [T], x: T) -> bool` — substring test, or
  array membership via `==` (`T` must support `==`, same rule as array `==` itself). Also answers
  for a `Map`/`Set` key and for any of the four builders, each the same question its immutable
  counterpart answers: a substring of a `MutableString`, a key of a `MutableMap`, an element of a
  `MutableSet` or `MutableArray` -- and the one value a `Mutable<T>` holds. The `MutableArray` is
  the one O(n) case, and it and `Mutable<T>` are the only two where the `==` requirement can fail:
  a `MutableMap` key and a `MutableSet` element are already bounded `Eq + Hash` by their
  declarations.
- `compare(a: T, b: T) -> int` — negative/zero/positive; `T` is any `Ord` type (see "Ordering")
  -- the way to order a struct/enum, where `< > <= >=` (numbers/strings/bytes only) don't reach.
- `hash(x: T) -> int` — `T` is any type supporting `==` with only hashable parts (the six
  scalars; an array/`Option`/`Result`/struct/enum built from hashable parts, however deep,
  including self-referential ones like `Json`; or a `Hash`-bounded type parameter). Structural,
  so `a == b` implies `hash(a) == hash(b)` for free; `-0.0`/`0.0` hash alike, `NaN` is never
  found (doesn't equal itself). Not cryptographic, not stable across language versions --
  never persist a hash, use it only within one program run.
- `abs(x: T) -> T` — `T` is `int`/`float`/`bigint`/`decimal`, not `byte`.
- `min(items: variadic T) -> T` / `max(items: variadic T) -> T` — variadic and `Ord`-bounded like
  `compare`/`sort`: two-or-more arguments or one array (the `variadic T` convention); at least one
  value required (`min()` is a compile error, an empty array a runtime one); every argument the
  same `T`, no implicit mixing.
- Rounding (`floor`/`ceil`/`round`, `decimal_round`/`_floor`/`_ceil`) and `decimal_pow` live in
  the `math` module now, not here.
- `push(arr: [T], x: T) -> [T]` — new array with `x` appended; `arr = push(arr, x)` in a loop
  is amortised O(1) (the result keeps spare room the *newest* version's next `push` reuses; an
  older version's `push` still copies, so no version changes under another).
- `insert(arr, i, x) -> [T]` — `x` inserted at `i` (`[0, len(arr)]`, else runtime error).
- `set(arr, i, x) -> [T]` / `remove(arr, i) -> [T]` — element at `i` replaced/removed
  (`[0, len(arr))`, else runtime error). `insert`/`set`/`remove`/`concat_arrays` always copy.
  No "pop" -- indexing already covers peeking.
- **Array helpers, callable with no import** — the counterparts of the bare `map_*`/`set_*`
  helpers that `Map` and `Set` already have: `array_map<T, U>(items, f)`,
  `array_filter<T>(items, keep)`, `array_flat_map<T, U>(items, f)`, `array_reverse<T>(items)`,
  `array_sort<T: Ord>(items)`, `array_sort_desc<T: Ord>(items)` and
  `array_sort_with<T>(items, cmp)`. All return a new array. The sorts are stable in both
  directions, so `array_sort_desc` is not `array_sort` reversed (that would flip ties).
  `collections.map`/`filter`/`flat_map`/`reverse`/`sort`/`sort_desc`/`sort_with` are thin
  wrappers over these and still work; these are the canonical spelling. The rest of
  `collections` — `fold`, `group_by`, `index_by`, `binary_search`, `min_by` and the others —
  stays behind `use collections`.
- `ok(x) -> Result<T, E>` / `err(e) -> Result<T, E>` — construct a `Result` (only where the
  missing type parameter is inferable from context; see "Result").
- `is_ok(r) -> bool`; `unwrap(r) -> T` (runtime error if `Err`); `unwrap_err(r) -> E` (runtime
  error if `Ok`).
- `channel_new() -> Channel<T>` / `channel_new(capacity: int) -> Channel<T>` — unbounded /
  bounded (`capacity >= 1`, else runtime error); same context-inference rule as `ok`/`err`.
- `send(c, value)` — enqueue, blocking while a bounded channel is full. `receive(c) -> T` —
  dequeue, blocking until available. `try_send(c, value) -> bool`, `try_receive(c) ->
  Option<T>`, `channel_len(c) -> int` — non-blocking forms and queue length.

None of `push`/`insert`/`set` mutate their `arr` argument, regardless of `val`/`var` (see
"Arrays"). `+` stays numeric-only in v0 (see docs/DESIGN.md on why string concatenation didn't
just overload it).
