# The twenty things

[docs/SYNTAX.md](SYNTAX.md) is the reference: 308 rules over 38 sections and 25,000 words, and
the right page to be reading when you have a specific question. This page is the other thing --
what you need *before* you have a specific question, in one pass.

Everything here is checked: every example below is compiled by `make syntax-check` on each build,
so none of it can quietly stop being valid Ziman.

## 0. Before you write a helper, look here

This is first because it is the mistake that actually gets made. Ziman has **no methods** —
nothing is ever `text.trim()` — so a reader coming from another language goes looking for a
string class, does not find one, and writes their own `contains`. Two developers in a reviewed
codebase did exactly that.

The string and collection toolkit is **builtin and unqualified**. No `use`, no module prefix:

| | |
| --- | --- |
| text | `len` `substring` `slice` `char_at` `is_char_at` `index_of` `last_index_of` `contains` `starts_with` `split` `split_once` `lines` `trim` `replace` `reverse_string` `equals_ignore_case` |
| building text | `concat` `sort` `filter` `reverse` `format` `printf` `str` `join` |
| arrays & maps | `len` `push` `contains` `insert` `slice` `concat` `snapshot` `clear` |
| numbers | `abs` `min` `parse_int` `compare` |
| results | `ok` `err` `is_ok` `unwrap` `unwrap_err` |

`contains` answers for a string, an array, a `Map` and a `Set`. So does `len`. If you are about
to write a loop that searches a collection, check this table first.

For anything else: [docs/CORELIB.md](CORELIB.md) is the generated index of every module, and
`grep -rn 'pub fn ' lib/` is the honest two-second version.

## 1. A program is `fn main()`

```ziman
fn main() {
    print("hello, ziman")
}
```

## 2. `val` never changes, `var` does

```ziman fragment
val name = "Ada"
var count = 0
count = count + 1
print(format("{} has {} unread", name, count))
```

Prefer `val`. They differ by one letter so that choosing is cheap.

## 3. There is no implicit coercion. Anywhere.

`1 + 1.0` is a compile error, not `2.0`. `"a" + "b"` is a compile error that tells you to use
`concat`. `str(x)` converts a number to text explicitly; `format("{}", x)` does it inline.

## 4. Every parameter and return type is declared

```ziman
fn shout(text: string) -> string {
    return concat(to_upper(text), "!")
}

fn main() {
    print(shout("hello"))
}
```

There is no inference across a function boundary. Inside one, `val`/`var` infer fine.

## 5. Values do not mutate — `push` returns a new array

```ziman fragment
var nums: [int] = []
nums = push(nums, 10)
nums = push(nums, 20)
print(len(nums))
```

There is no `nums[i] = x`. Reassigning the variable is the idiom, and it is cheap -- see item 6
for the one case where it is not.

## 6. `Mutable*` is for the operations that copy

Appending is already fast: `arr = push(arr, x)` in a loop is amortised O(1), because the array
keeps spare room and the next `push` on that same array writes into it. 400,000 of them take
0.09s. Do not reach for a builder just to append.

What copies is `set`, `insert` and `remove` -- each rebuilds the whole array, so replacing every
element one `set` at a time is O(n²). Strings are the other trap: `s = concat(s, piece)` in a
loop *is* quadratic, and `--lint` names the variable and tells you to use a `MutableString`.

So `MutableArray`, `MutableString`, `MutableMap`, `MutableSet` and the one-value `Mutable<T>`
cell are what you reach for when you are *editing* a large collection, or building a string.

```ziman fragment
val b = mutable_array_new<int>()
for i in 0..5 {
    mutable_array_push(b, i * i)
}
print(len(b))
```

## 7. `for` iterates values; `a..b` counts

```ziman fragment
for n in [1, 2, 3] {
    print(n)
}
for i in 0..3 {
    print(i)
}
```

`for i in a { ... a[i] ... }` is a compile error on purpose: in `for i in a`, `i` is the
*element*, not its index. Write `for i in 0..len(a)` when you want the index.

## 8. Structs are values, and `with` makes a changed copy

```ziman
struct User {
    name: string
    age: int
}

fn main() {
    val u = User { name: "Ada", age: 36 }
    val older = with u { age: 37 }
    print(format("{} {}", older.name, older.age))
}
```

Fields are separated by whitespace, **not commas** — a newline is the usual one.

## 9. `enum` plus `switch`

```ziman
enum Shape {
    Circle(float)
    Square(float)
}

fn area(s: Shape) -> float {
    switch s {
        Circle(r) { return 3.14159 * r * r }
        Square(side) { return side * side }
    }
}

fn main() {
    print(area(Shape.Circle(2.0)))
}
```

`switch` is a statement, and it must cover every variant -- there is no default arm, on purpose,
so adding a variant turns every switch that has not been updated into a compile error.

## 10. Failure is a `Result`, and `try` is how you pass it up

```ziman
fn safe_div(a: int, b: int) -> Result<int, string> {
    if b == 0 {
        return err("division by zero")
    }
    return ok(a / b)
}

fn quarter_of(n: int) -> Result<int, string> {
    val half = try safe_div(n, 2)      // on Err, returns it from quarter_of
    return safe_div(half, 2)
}

fn main() {
    print(unwrap(quarter_of(20)))      // 5
}
```

`try` unwraps an `Ok` and returns the `Err` from the enclosing function. `unwrap` is the
opt-in crash. A discarded `Result` is a compile error, so you cannot ignore one by accident.

## 11. Absence is an `Option`, matched with `if Some(x) =`

```ziman fragment
val found: Option<int> = Some(3)
if Some(n) = found {
    print(n)
} else {
    print("nothing")
}
```

## 12. `defer` runs on the way out

```ziman
fn note(m: string) {
    print(m)
}

fn main() {
    var stage = "first"
    defer note(stage)
    stage = "second"
    print(stage)
}
```

That prints `second` then `first`: the deferred call runs last, but its argument was evaluated
**when the `defer` was registered**. There is no way to defer a call over a value you have not
computed yet -- put the value in a `Mutable<T>` cell and read the cell inside the deferred
function if you need the late one.

## 13. A module is a file, and `use` names what you take from it

```ziman
use json.Json
use time.LocalDate

fn main() {
    print("modules are files under lib/ or beside your program")
}
```

`--fmt` sorts `use` lines, so write them sorted and they will stay where you put them.

## 14. Named arguments at the call site

```ziman
fn at(hour: int, minute: int, second: int) -> string {
    return format("{}:{}:{}", hour, minute, second)
}

fn main() {
    print(at(9, 30, 0))                              // positional
    print(at(hour: 9, minute: 30, second: 0))        // named, same call
    print(at(second: 0, hour: 9, minute: 30))        // any order
}
```

A call is **all positional or all named, never mixed**. A label must match the parameter name
exactly, and named arguments may come in any order. This is how a call with three numbers in a
row stays readable.

## 15. Generics look like you expect

```ziman
fn first_or<T>(items: [T], fallback: T) -> T {
    if len(items) == 0 {
        return fallback
    }
    return items[0]
}

fn main() {
    print(first_or<int>([], 7))
}
```

## 16. `#[derive(...)]` writes the boring code

There are four: `#[derive(Json)]` for JSON in and out, `#[derive(Row)]` for a database row,
`#[derive(Properties)]` for a config file, and `#[derive(Ord)]` for sorting and comparison. Look
here before hand-writing a serializer -- SYNTAX.md's "Attributes and derive" has the rules.

## 17. Tests are `#[test]` functions

```ziman
use test

#[test]
fn two_plus_two() {
    test.assert_eq(2 + 2, 4)
}
```

Run them with `ziman test <file>` -- a subcommand, not a flag, and the file needs no `main`.
`#[test(cases: NAME)]` runs the same function once per element of an array.

## 18. Concurrency is `spawn` and channels

`spawn f(x)` runs a declared function on a new OS thread, with `args` evaluated on the calling
thread first. There is no thread handle and no join: a spawned function returns nothing, so its
result reaches you through a `Channel<T>` or not at all. The intended shape is one thread owning
a piece of state and everyone else sending it messages.

The rule that will catch you: **builders and iterators are thread-bound.** `MutableArray`,
`MutableString`, `MutableMap`, `MutableSet` and `Iter` belong to the thread that made them, and
any other thread touching one is a runtime error. Ordinary values are immutable, so they travel
freely.

## 19. The tools that will tell you the answer faster than this page

| | |
| --- | --- |
| `./ziman prog.zm` | run it |
| `./ziman --check prog.zm` | typecheck without running |
| `./ziman --lint prog.zm` | the slow-pattern and dead-code warnings |
| `./ziman --fmt prog.zm` | print it formatted (`--write` to rewrite in place) |
| `./ziman test prog.zm` | run its `#[test]` functions |
| `make check-fast` | the whole gate except the North Star suite |

## 20. Where to go next

| | |
| --- | --- |
| [docs/TUTORIAL.md](TUTORIAL.md) | one small program, built up feature by feature |
| [docs/SYNTAX.md](SYNTAX.md) | the full rules, when you have a specific question |
| [docs/CORELIB.md](CORELIB.md) | every module and every function in it |
| [docs/DESIGN.md](DESIGN.md) | why it is this way |
