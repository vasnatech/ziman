# Functions, structs, generics and enums

Part of the [v0 syntax reference](README.md).

## Function values

A function can be passed around: its type is `fn(A, B) -> R` (no `-> R` for a
function that returns nothing). Three things are values of that type: the name of
a top-level function, a *lambda* — an anonymous function written in place — and
`Type::field`, the field-accessor shorthand below.

```ziman

fn double(n: int) -> int { return n * 2 }
fn is_even(n: int) -> bool { return n % 2 == 0 }

fn apply(f: fn(int) -> int, x: int) -> int {
    return f(x)                       // a call through a local of function type
}

fn main() {
    print(apply(double, 21))          // 42
    val keep = is_even                // a val of type fn(int) -> bool
    val evens = filter<int>([1, 2, 3, 4], keep)
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
- **In the standard library**: `map`/`filter`/`flat_map`/`fold`/`count`/`any`/`all`/
  `find`/`first`/`last`/`min_by`/`max_by`/`distinct`/`group_by`/`for_each`/`sort_with`, type
  arguments written at the call. Pass a lambda for anything needing the enclosing scope.

### `Type::field`, the field-accessor shorthand

`Type::field` is a function value that answers that field of that type —
`Row::dept` is `fn(r: Row) { r.dept }`, with the field's type taken from the
struct. It exists for the key-taking family (`sort_by`, `min_by`, `max_by`,
`group_by`, `index_by`, `map`), where the lambda names the type twice and the
field once to say one field.

```ziman

struct Row {
    dept: string
    age: int
}

fn main() {
    val rows = [Row { dept: "b", age: 30 }, Row { dept: "a", age: 40 }]
    print(map<Row, string>(rows, Row::dept))                      // [b, a]
    print(len(sort_by<Row, string, int>(rows, Row::dept, Row::age)))
    val key = Row::age                // an ordinary fn(Row) -> int
    print(key(rows[0]))               // 30
}
```

The separator is `::` and not `.` because `Type.member` already means
enum-variant access (`Option<T>.Some`). A module-qualified type works the same
way — `model.Item::qty`. One field only: `Row::addr::city` is refused, and the
message gives the lambda to write instead. Since Ziman has no methods, `::`
means exactly this and nothing else.

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
  `a.distance_squared(b)`) -- no `impl` blocks, no inheritance (see docs/design/journal/'s
  classes-vs-traits discussion). A field that *holds* a function value is the one exception to the
  shape, not to the rule: `h.run(21)` calls the function in `h`'s `run` field, which is the field's
  value doing the work rather than a method dispatched on `h`'s type. A field of any other type says
  so when called (`'h.name' is a string, which cannot be called`), and a qualifier that is neither an
  imported module nor a local with a callable field still reports the module error.
- **`==`/`!=` compares every field in declaration order**, short-circuiting on the first
  mismatch; both sides must be the same declared type. Works only when every field's type
  itself supports `==`, recursively (an incomparable field, like `Channel`, makes the whole
  struct a type error to compare); a struct referring back to its own type is assumed
  comparable rather than checked forever (see docs/design/journal/'s "Recursive Eq/Hash" entry).
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
- **`#[at_least_one]` on the function raises that floor to one.** Zero is the default because a
  variadic with an identity reads perfectly well empty — `concat()` is `""`, and a SQL
  `query(conn, sql)` with no bind parameters is the common case, not a mistake. It is the
  *mutations* that have no empty reading: `add(s)`, `remove(s)` and `push(xs)` change nothing,
  so the prelude marks those. An **empty array still passes**, as one argument rather than none —
  that is how a caller spreads a list that may be empty, deliberately rather than by omission.
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
- **A program's own function shadows a builtin of the same name.** `fn count(conn, table) -> int`
  is legal beside the builtin `count`, and a bare `count(...)` in that file reaches the program's
  one. The shadow is invisible to the library: the prelude's own calls always reach the builtin, so
  shadowing `len` cannot break `lib/prelude/`. A `use`d module could always do this; the main
  file could not until 2026-10-18, which was an asymmetry rather than a rule.
- **`prelude.name(...)` reaches the shadowed one.** `prelude` is the one qualifier that names no
  module: it resolves against the prelude's declarations and the builtins only, so a file that has
  taken `format` or `len` for its own type can still call the original — `prelude.format("{} of {}",
  a, b)` beside its own `format(t: Thing)`. It is strictly an escape, and it reaches *nothing* of
  yours: `prelude.helper(...)` for a `helper` you declared is a compile error rather than a call,
  and `prelude.len(my_struct)` resolves against the prelude's nine `len` declarations only. A
  module you actually imported under the name `prelude` still wins. The same applies inside a
  bundled module, which is where the need showed up: `lib/time/time.zm` declaring a `format`
  overload set made the interpolating builtin unreachable from its own file.
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

- **Only an `Option<T>`-typed parameter may be omitted** — a required parameter can never be left
  out, however the call is written (`mixed()` on `fn mixed(a: int, b: Option<int> = 2)` is
  "missing required argument 'a'"). Two call shapes may omit one: a **named** call, leaving out any
  subset, and a call with **no arguments at all**, which leaves out every parameter at once
  (`mutable_array_new<int>()`) and so needs every parameter to be omittable. A positional call that
  passes *at least one* argument must supply every fixed parameter — `mutable_array_new<int>(100)`
  is refused, and `mutable_array_new<int>(capacity: 100)` is how to set one and leave the rest.
  Nothing can bind to the wrong parameter when no argument is passed at all, which is why that
  shape is not the exception it looks like.
- **A variadic tail is not an omission**, so it is always allowed to be empty: `reqvar(1)` is a
  complete positional call to `fn reqvar(a: int, rest: variadic int)`.
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
  exception to "no implicit coercion" (see docs/design/journal/'s "Rationale consolidated from
  docs/syntax/" entry). `Option<Option<T>>` is the one ambiguous case (would a bare `Option<T>`'s
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

Structs, enums and functions can all be generic, cross-module included (see docs/roadmap/'s
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
  docs/design/journal/'s "Rationale consolidated from docs/syntax/" entry for why). A unit variant is
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
