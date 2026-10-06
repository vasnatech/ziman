# Your first hour with Ziman

This is a guided walkthrough, not a reference — it builds one small program up
piece by piece, introducing a feature exactly when the program needs it. For
the full rules see [docs/SYNTAX.md](SYNTAX.md); for the standard library see
[docs/CORELIB.md](CORELIB.md); for the reasoning behind a design choice see
[docs/DESIGN.md](DESIGN.md). Every snippet below is real Ziman, checked
against the interpreter while this page was written.

## Hello, ziman

```sh
make
echo 'fn main() { print("hello, ziman") }' > hello.zm
./ziman hello.zm
```

`main` is the entry point, `print` takes one argument of any printable type.
There is no `import os` ceremony and no `public static void` boilerplate —
this is the whole program.

## Values, types, and no implicit coercion

```
fn main() {
    val name = "Ada"        // val: never reassigned
    var count = 0            // var: may be reassigned
    count = count + 1
    print(format("{} has {} unread messages", name, count))
}
```

`val` and `var` differ by one letter on purpose — see DESIGN.md if you want
the reasoning — so pick the one you mean. Types are inferred here (`name` is
`string`, `count` is `int`), but there is no coercion between them anywhere:
`1 + 1.0` is a compile-time error, not a silent `2.0`. `str(count)` converts a
number to text explicitly; `format("{}...", ...)` does the equivalent for you
in one call, and it is checked at compile time against how many `{}`
placeholders the string literal has.

## Functions

```
fn shout(text: string) -> string {
    return concat(to_upper(text), "!")
}

fn main() {
    print(shout("hello"))   // HELLO!
}
```

Every parameter and return type is declared, always — there is no inference
across a function boundary. `concat` joins two strings (`+` is numeric-only,
so `"a" + "b"` is a compile error that tells you to use `concat`). Ziman has
no methods: nothing is ever spelled `text.shout()` — a function takes the
value it works on as an ordinary argument instead.

## Arrays

```
fn total(nums: [int]) -> int {
    var sum = 0
    for n in nums {
        sum = sum + n
    }
    return sum
}

fn main() {
    var nums: [int] = []
    nums = push(nums, 10)
    nums = push(nums, 20)
    nums = push(nums, 30)
    print(total(nums))   // 60
}
```

An array is a value, like everything else in Ziman — there is no
`nums[i] = x` and no in-place `.push`. `push(arr, x)` returns a *new* array
with `x` appended; the idiom `arr = push(arr, x)` is amortised O(1) per call,
not a copy every time (see SYNTAX.md's "Other limits" if you want the exact
cost of `push` versus `insert`/`set`/`remove`, which do always copy). `for n
in nums` binds each element in turn; `for i in 0..len(nums)` binds an index
into a range instead — the two are deliberately different syntax, so a model
or a human can't silently write an index loop that means an element loop.

## Structs, and `with` instead of field assignment

```
struct Expense {
    label: string
    cents: int
}

fn main() {
    val coffee = Expense { label: "coffee", cents: 350 }
    val bigger_coffee = with coffee { cents: 500 }
    print(bigger_coffee.label)   // coffee
    print(bigger_coffee.cents)   // 500
}
```

A struct literal names every field — no positional `Expense("coffee", 350)`,
which would be exactly the kind of swapped-argument bug that class of
constructor invites. There is no field assignment (`coffee.cents = 500` is a
syntax error): `with coffee { cents: 500 }` builds a new struct with the
named fields replaced and everything else copied from `coffee`. Like arrays,
a struct value never changes after it's built — only a binding (`val`/`var`)
can be given a new one.

## `Result`, and no exceptions

Ziman has no `try`/`catch` and nothing ever throws. A function that can fail
says so in its return type:

```
fn parse_expense(label: string, text: string) -> Result<int, string> {
    val cents = try parse_int(text)
    if cents < 0 {
        return err(format("{}: amount cannot be negative", label))
    }
    return ok(cents)
}

fn main() {
    print(parse_expense("coffee", "350"))   // Ok(350)
    print(parse_expense("oops", "-5"))      // Err(oops: amount cannot be negative)
    print(parse_expense("oops", "abc"))     // Err(invalid digit found in string)
}
```

`ok(x)`/`err(e)` build the two cases; `try expr` unwraps an `Ok` and returns
the `Err` immediately if there is one (`try parse_int(text)` above: a bad
number returns `parse_int`'s own error without an explicit `if`). There is no
`?`-chaining across expressions yet — `try` must be a `val`/`var`'s whole
initializer or its own statement — so a function with several fallible steps
reads top to bottom, one `try` per line, rather than one deeply nested
expression.

## Enums and exhaustive `switch`

```
enum Category {
    Food
    Transport
    Other
}

fn category_name(c: Category) -> string {
    return switch c {
        Food { "food" }
        Transport { "transport" }
        Other { "other" }
    }
}

fn main() {
    print(category_name(Category.Transport))   // transport
}
```

Construction is enum-qualified (`Category.Transport`); consumption inside a
`switch` uses the bare variant name. `switch` requires one arm per variant
and has no `default` — add a fourth variant to `Category` and every `switch`
over it stops compiling until you handle the new case too, which is the
point: a machine-written change that forgets a case is a compile error here,
not a bug found in production.

## Putting it together

```
enum Category {
    Food
    Transport
    Other
}

struct Expense {
    label: string
    cents: int
    category: Category
}

fn category_name(c: Category) -> string {
    return switch c {
        Food { "food" }
        Transport { "transport" }
        Other { "other" }
    }
}

fn parse_expense(label: string, text: string, category: Category) -> Result<Expense, string> {
    val cents = try parse_int(text)
    if cents < 0 {
        return err(format("{}: amount cannot be negative", label))
    }
    return ok(Expense { label: label, cents: cents, category: category })
}

fn add_expense(items: [Expense], label: string, text: string, category: Category) -> [Expense] {
    val parsed = parse_expense(label, text, category)
    if Ok(expense) = parsed {
        return push(items, expense)
    }
    print(parsed)
    return items
}

fn total_cents(items: [Expense]) -> int {
    var sum = 0
    for item in items {
        sum = sum + item.cents
    }
    return sum
}

fn main() {
    var items: [Expense] = []
    items = add_expense(items, "coffee", "350", Category.Food)
    items = add_expense(items, "bus ticket", "275", Category.Transport)
    items = add_expense(items, "book", "1899", Category.Other)
    items = add_expense(items, "oops", "-5", Category.Other)

    for item in items {
        printf("{}: {} cents ({})\n", item.label, item.cents, category_name(item.category))
    }
    printf("total: {} cents\n", total_cents(items))
}
```

```
Err(oops: amount cannot be negative)
coffee: 350 cents (food)
bus ticket: 275 cents (transport)
book: 1899 cents (other)
total: 2524 cents
```

Nothing here is a toy restriction you'll outgrow: no methods, no exceptions,
explicit `Result`, exhaustive `switch` and value-only structs/arrays are the
whole language, all the way up to the multi-thousand-line reference backend
(`health-record-backend-ziman`) that exercises every module in the standard
library. What you *will* outgrow in your first hour: `if`/`switch` can also
be used as expressions in a restricted set of positions (see "`if` and
`switch` as values" in SYNTAX.md), generic types and functions exist
(`Option<T>`, `Map<K, V>`, `collections.map`/`filter`/`fold`), and there's a
standard library for JSON, files, HTTP, databases, time, crypto and more —
[README.md](../README.md)'s module table is the map.

## Where to go next

- **`ziman --check file.zm`** type-checks without running; **`ziman --lint
  file.zm`** flags a few common mistakes (an unused `var`, a loop that
  quadratically rebuilds a string or array); **`ziman --coverage file.zm`**
  (or `ziman test --coverage`) reports which of your lines never ran, and
  exits 1 if any did not; **`ziman --profile file.zm`** reports which
  functions the time went to, hottest first; **`ziman --locals file.zm`**
  adds each stack frame's variables and their values to a runtime error's
  trace, which is usually the fastest way to see what a failure was holding;
  **`ziman --fmt --write file.zm`**
  formats it.
- **[docs/SYNTAX.md](SYNTAX.md)** is the normative reference: every construct,
  every builtin, every operator, with the exact rules.
- **[docs/CORELIB.md](CORELIB.md)** and **docs/lib/*.md** document the
  bundled standard library, generated from its own source so they can't drift.
- **[docs/DESIGN.md](DESIGN.md)** is a dated decision log — read an entry
  when you want to know *why* something works the way it does, including the
  designs that were tried and rejected.
- **`health-record-backend-ziman`** is a real, multi-thousand-line
  application written in Ziman — the best example of how the pieces above
  compose into a real backend.
