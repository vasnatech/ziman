# Ziman code style and colouring architecture — 2026-11-16

Two deliverables in one report: **how Ziman code should be formatted**, verified against the
formatter rather than imagined, and **a strategy for colouring it** across the three layers that
now exist. This is a read-only investigation; the only file written is this one.

Every statement is tagged **[V]** (verified: I ran it, or read the cited `file:line`) or **[O]**
(opinion). Three things stay distinguishable throughout: what the formatter **does**, what I
**recommend** where it is silent, and what I think should **change** (the latter lives in
*Findings*, never in Part 1 or Part 2 as though it were the style).

## Provenance

| | |
|---|---|
| Commit | `4e710159`, tree clean apart from this report [V] |
| `make fmt-check` | **exit 0** [V] — both `--fmt --check` and `--fmt --canonical --check` over 347 `.zm` files |
| `make test` | **exit 0** [V] — 1,529 passed, 0 failed, 1 skipped (`ibus_composition`, needs a composing engine) |
| Oracle | `./ziman --fmt` (`src/format.c`, 767 lines) [V] |
| Second corpus | `health-record-backend-ziman`, 343 files / 59,300 lines: `--fmt --check` **exit 0**, `--fmt --canonical --check` **exit 0** [V] |

`fmt-check` is clean, so the oracle and the committed tree agree and the rest of this report is
trustworthy. One correction to the brief that sent me: `make fmt-check` covers **`gallery` and
`docs/reviews/benchmark/reference` as well** as the directories named in the prompt
(`make/ziman.mk:62-64`) [V], and the sample backend is 59,300 lines, not 13,000 [V].

## Executive summary

1. The formatter is clean on both corpora and every rule in Part 1 below is a run, not a guess [V].
2. Its governing rule is **it never joins lines**: it only *adds* breaks (two mechanisms), while
   `--canonical` removes exactly two and `use`-sorting reorders whole lines [V].
3. The most surprising behaviour is unchanged and is now documented in the journal: a call,
   parameter list or struct/map literal broken between items at all goes fully vertical — **even at
   `--width=0`** [V]. An array or set literal does not [V].
4. Seven of the 2026-10-11 review's eight findings are closed or explicitly decided; the eighth,
   `documentHighlight`, is mostly fixed and survives in a changed shape [V].
5. Layer 1 has grown from ~32 to **35 scopes**, and `internal`, `#![…]` and `val`/`var` — three
   findings then — are all fixed [V].
6. **But `builtin-types` and `builtin-functions` match nothing.** `tools/gen_vscode_grammar.py`
   writes `\\\\b` into the JSON where every hand-written rule writes `\\b`, so both regexes require
   a literal backslash [V]. `print`, `len`, `Map`, `Option` all fall through; `mutable_array_new<int>()`
   gets no scope at all [V]. The gate cannot see it: it diffs the generator against itself [V].
7. Layer 3 exists and the prompt's premise is out of date: the LSP advertises semantic tokens with
   five types and five modifiers, `mutable` among them [V]. `documentHighlight` really does resolve
   bindings, not words [V].
8. Both layer-3 features are **wrong around a shadowing block**, in opposite directions: a use
   after the block reads `mutable`, and the shadowing declaration itself reads as the outer
   binding [V].
9. The gallery's new runtime colourer takes the right **partition** and the wrong **palette**: the
   three hexes are tuned for a dark code block and land on a light control face at 2.20–3.37:1 [V].
10. The site itself fails there too — in light theme a literal in a code block is **2.66:1** [V].

---

# Part 1 — the formatting reference

Read front to back, this is enough to format Ziman by hand the way `--fmt` would. Each numbered
section cites the run that established it; the runs are reproduced in the appendix.

## 0. The one rule that governs the rest: the formatter never joins lines

Establish this first, because most of Part 1 follows from it. `--fmt` decides **where to add** a
line break and **never where to remove one** [V]. Three mechanisms touch line structure:

| Mechanism | Direction | `src/format.c` |
|---|---|---|
| `wrap_once` | adds a break after a comma, when a line is over the width | `:416` [V] |
| `normalize_vertical` | adds breaks so a half-broken list becomes one-per-line | `:472` [V] |
| `sort_use_runs` | reorders whole `use` lines | `:552` [V] |
| `canonicalize_braces` | **removes** a break before `{` and before `else` — `--canonical` only | `:648` [V] |

So: an author's break survives plain `--fmt` [V]. A two-line empty body stays two lines
(`fn empty_body() {` / `}`) [V]. A one-field struct literal the author spread over three lines
stays over three [V]. Nothing is ever pulled back up without `--canonical` [V].

The practical consequence for a programmer: **you choose the line structure, the formatter
chooses the spacing, the indentation, and whether your structure needs more breaks** [O].

The implementation is a **text** formatter, not a tree printer: it scans the source into items
(words, numbers, strings, punctuation, comments) with the whitespace between them and re-emits
them (`src/format.c:32` `scan`, `:295` `emit_pass`) [V]. That is why it works on a file the
compiler rejects — a wrong-typed program formats fine, exit 0 [V] — and why an unterminated string
literal is the only input it refuses as unlexable [V]. After formatting, the output is re-scanned
and must contain exactly the same sequence of code tokens and comments, or nothing is printed
(`src/format.c:749-764`) [V].

## 1. Indentation

**Four spaces per open bracket** (`src/format.c:340`) [V]. Nothing else increases it: there is no
extra level for a `switch` arm, an `else`, a visibility marker or a continuation of a declaration.

```
fn main() {                       // 0 open brackets on this line's start → 0
    if true {                     // 1 → 4
        val x = 1                 // 2 → 8
    }
    for i in 0..3 {
        if i > 1 { print(i) }     // a one-line body takes no second level
    }
    val nested = [[1, 2], [3, 4]]
    print(nested[0][1])
}
```
Input was the same code flush left; output is the above [V] (experiment **E01**).

Three refinements, each verified:

- **A bracket opened on the same line as another adds no level** (`src/format.c:365-371`) [V]. So
  `f(g(h(` on one line indents its contents once, not three times, and a lambda passed to a call
  indents its body one level from the statement:
  ```
  val r = map<int, int>(xs, fn(n: int) -> int {
      return n * 2
  })
  val q = f(g(h(
      1
  )))
  ```
  (experiment **E20**) [V]. This is also why 190 parentheses written on one line cost one level,
  not 190 — which is what made my first attempt at the output-size limit fail to trip it [V].
- **A line that begins with a closing bracket sits at its opener's level**
  (`src/format.c:324`) [V] — that is what puts `)` and `}` under the `fn`/`val`, not under the
  arguments.
- **A continuation line gets exactly one extra level, and every line of a run gets the same
  one.** A line "continues" when its last token was a binary operator, `->`, or the `\` that joins
  string-literal pieces (`src/format.c:392-394`) [V]:
  ```
  val sum = a +
      b +
      c +
      d
  val s = "one " \
      "two " \
      "three"
  ```
  (experiment **E15**) [V]. The `+1, +0, +1, +0` zigzag the 2026-10-11 review found is gone; the
  cause and the fix are recorded at `src/format.c:327-339` [V].

## 2. Line length and wrapping

**The default target is 120 columns** (`--width=N`, `0` = never wrap) [V], and `./ziman --help`
confirms the spelling [V]. The editor extension sets `editor.rulers: [120]` to match
(`editors/vscode/package.json`) [V].

**Comments are not counted.** A 139-column line whose code is 24 columns and whose trailing
comment is the rest is left alone [V]; `wrap_once` measures to the last *code* item
(`src/format.c:424-426`) [V]. That is why 175 lines in `lib/` exceed 120 columns while the tree
passes `fmt-check` [V] — and why only 54 of those are not comment-only [V].

**What `--width=N` changes, and what it does not.** It changes only `wrap_once`. It does *not*
change indentation, spacing, `use` sorting, comment alignment, or `normalize_vertical` [V] — the
last is the one most people will be caught by, because it means **`--width=0` is not "never add a
line break"** [V].

### Where a long line breaks

A break is only ever inserted **after a comma of the shallowest bracket group on the line**, and
only where the parser accepts a newline (`src/format.c:427-440`) [V]. Excluded, and verified:

- a comma inside a generic list (`Result<A, B>`) — tracked as an unmatched `<` [V];
- a comma inside an `fn(A, B) -> R` type — the `after_fn` flag [V];
- any line that starts with `#`, i.e. an attribute [V];
- a comma whose next item is already a line start, or is a comment [V].

A line with no legal break is left long [V]. That is why a long `&&` condition and a long `use`
line both stay over the width:

```
    if first_condition_is_true && second_condition_is_true && third_condition_is_true && fourth_condition_is_also_true {
        print(1)
    }
```
(experiment **E10**, default width) [V]. The library's own 54 over-width code lines are exactly
this shape: long string literals and `||`-chains (`lib/core/url.zm:87`, `lib/core/otp.zm:71`) [V].

A generic list is likewise never broken, even when the call beside it is:

```
    val v: Result<SomeVeryLongTypeNameHere, AnotherEquallyLongErrorTypeNameHere> = f(
        aaaaaaaaaa,
        bbbbbbbbbb,
        cccccccccc
    )
```
(experiment **E33**, `--width=60`) [V].

### The two shapes, and which bracket decides

A bracket is either **vertical** (one item per line, brackets on lines of their own) or **greedy**
(fill to the width and wrap, continuation indented one level). The set of vertical brackets is
fixed at `src/format.c:374-376` [V]:

| Bracket | Shape | Verified |
|---|---|---|
| `name(` — a declaration's parameters, or a call's arguments | vertical | E09, E10 [V] |
| `>(` with no space — a generic call's arguments | vertical | `src/format.c:375` [V] |
| `{` that opens a struct or map **literal** (an entry with `:` before any `,`) | vertical | E11 [V] |
| `[` — an array literal | greedy | E10, E28 [V] |
| `{` that opens a **set** literal (`Set<int> { 1, 2 }`, no `:`) | greedy | E12, E34 [V] |
| `{` that opens a block, a `struct`/`enum` **declaration** body, or a `with` list | untouched (no commas to break at) | E21 [V] |

Vertical, from a real run (experiment **E09**, default width):

```
fn a_very_long_function_name_indeed(
    first_parameter: string,
    second_parameter: int,
    third_parameter: bool,
    fourth_parameter: float
) -> Result<string, string> {
```

Greedy, same width (experiments **E28** and **E34**, `--width=60`):

```
    val z = [alpha, bravo, charlie, delta, echo, foxtrot,
        golf, hotel, india, juliet, kilo, lima, mike]

    val s = Set<string> { "alpha", "bravo", "charlie",
        "delta", "echo", "foxtrot", "golf", "hotel",
        "india", "juliet" }
```

The asymmetry is deliberate and the reason is recorded: an array's elements are homogeneous and
forty integers one per line is a column nobody reviews, while a call's arguments are heterogeneous
(`docs/design/journal/part-06.md:451-465`) [V]. I agree with the reasoning [O]; see Finding 6 for
the one place it has not been carried through.

### The rule that catches everyone: a half-broken list is completed

`normalize_vertical` runs on every pass, **independently of the width** [V]:

```
# input                          # output at --width=0
val a = f(1,                     val a = f(
    2, 3)                            1,
                                     2,
                                     3
                                 )
fn g(a: int,                     fn g(
    b: int) -> int { ... }           a: int,
                                     b: int
                                 ) -> int { ... }
```
(experiment **E12**) [V]. Two escapes, both verified: a list that **contains a comment** is left
exactly as written [V], and a call whose only breaks are **inside a lambda body or another
bracket** is not verticalised [V] (experiments **E13**, **E14**) — which is what keeps the
`map(xs, fn(n) { ... })` idiom readable.

**Recommendation** [O]: write a call on one line, or break it after *every* comma. Breaking after
one comma to "help" is the one edit the formatter will always undo and redo differently.

## 3. Declarations

Spacing is uniform and there is no special layout rule for any declaration kind beyond §2's
vertical/greedy split. From one run (experiment **E21**) [V]:

```
#![experimental]
use fs
pub val max_retries = 3
internal val internal_thing: int = 4
pub native fn read_it(path: string) -> Result<string, string>
native fn hidden(a: int)
pub struct Box<T> { value: T }
pub enum Shape { Circle(float) Rectangle(RectDims) Point }
struct RectDims { width: float
    height: float }
pub fn generic_thing<T: Eq + Hash, U>(a: T, b: U, rest: variadic int, opt: Option<int> = 3) -> Result<T, string> {
    return ok(a)
}
internal fn small() {}
fn empty_body() {
}
```

Reading that off:

- `pub`, `internal`, `native`, `variadic` are ordinary words to the formatter: one space, no
  alignment, no reordering [V]. `is_keyword` lists all four (`src/format.c:142-143`) [V].
- A parameter is `name: type` — no space before the `:`, one after [V]. `variadic` sits before the
  type, separated by one space [V]. An `Option<T> = 3` default takes spaces around `=` [V].
- A generic parameter list keeps the source's `<`/`>` spacing (see §10) and its bounds are spaced
  `T: Eq + Hash` [V].
- `fn name(` never takes a space before the `(` (`src/format.c:194`) [V].
- A `struct`/`enum` **declaration** body has no commas, so it is never verticalised — a
  half-broken one stays half-broken [V]. The library writes one field per line in all of its
  declarations; `docs/syntax/functions-and-types.md:131` states that as the convention [V].
  **Recommendation** [O]: one field per line, and reserve the single-line form for the
  two-field illustrative struct the way `docs/syntax/` does.
- `{}` stays `{}` on one line; `{` / `}` on two lines stays two [V].

## 4. Statements and expressions

One run (experiment **E16**) covers the lot [V]:

```
enum Shape { Circle(float) Square(float) Empty }
fn area(s: Shape) -> float {
    switch s {
        Circle(r) { return 3.14 * r * r }
        Square(w) {
            return w * w
        }
        Empty { return 0.0 }
    }
}
fn main() {
    val o: Option<int> = Option<int>.Some(1)
    if Some(x) = o { print(x) }
    else { print(0) }
    while true { break }
    defer print(1)
    val f = fn(n: int) -> int { return n }
    val g = fn(n) { n * 2 }
    val r: Result<int, string> = ok(try parse_int("1"))
    for i, x in [1, 2] { print(x) }
}
```

- **`switch` arms** are one bracket level inside the `switch`'s brace, and an arm's own brace is a
  brace like any other: a one-line arm stays one line, a broken arm indents [V]. There is no
  alignment of arm bodies.
- **`if`/`else`**: plain `--fmt` leaves `else` where the author put it, including on its own line
  [V]. `--canonical` pulls it up (§11).
- **`if Some(x) = o`** is sugar the parser rewrites, but `--fmt` never sees the rewrite — it is a
  text formatter, so what you wrote is what you keep [V].
- **`for`**, **`while`**, **`defer`**, **`spawn`**, **`try`** take one space after the keyword and
  nothing else [V]. `for i, x in xs` spaces the comma normally [V].
- **A lambda** is `fn(params) -> R { body }` with `fn(` tight (`src/format.c:195`) [V]; an
  omitted-type lambda `fn(n) { n * 2 }` formats the same way [V]. A lambda whose body is broken
  indents from the statement's level, not the argument's (§1) [V].
- **There are no methods in Ziman**, so there is no method-chain layout question at all [V]
  (`docs/syntax/functions-and-types.md:133-145` — functions operate on structs). Nothing in
  `src/format.c` has a rule for a chain, and nothing needs one. A style guide for Ziman should say
  this rather than borrow a chaining rule from a language that has one [O].

## 5. Literals and collections

```
    val c = Config { host: "localhost", port: 8080, timeout_ms: 30000, retries: 5 }
    val e = with d { port: 9090, retries: 1 }
    val m = Map<string, int> { "alpha": 1, "beta": 2, "gamma": 3 }
    val s = Set<int> { 1, 2, 3 }
    val arr = [1, 2, 3]
```
(experiment **E11**, default width — unchanged from input) [V]. At `--width=60` the struct literal
and the map literal go vertical; the set literal and the array do not [V]:

```
    val c = Config {
        host: "localhost",
        port: 8080,
        timeout_ms: 30000,
        retries: 5
    }
    val s = Set<int> { 1, 2, 3 }
```

- **`Foo { … }` is spaced inside the braces**, `[1, 2]` and `(a, b)` are not
  (`src/format.c:186-189`) [V].
- **`with` is a prefix form** — `with base { field: value }` — and the formatter treats `with` as
  an ordinary keyword, so a `with` override list formats exactly like a struct literal, including
  going vertical when broken [V].
- **A map or struct literal is recognised by text**: `is_literal_brace` (`src/format.c:264`) looks
  for a `:` at the group's own level before any `,` [V]. That is why a set literal is greedy — it
  has no `:` — and it is the one place the shape of a literal depends on its *contents* [V].
- **A literal's own spelling survives.** A hex constant is not rewritten as decimal, a `_`
  separator is kept, and `19.99d`/`200b`/`…n` suffixes are untouched [V] — the scanner re-prints a
  literal's source text (`docs/syntax/lexical-and-grammar.md`) [V].
- **A text block is one token and is reproduced byte for byte**, interior indentation included [V]:
  ```
      val q = """
          SELECT id, name
            FROM person
      """
  ```
  (experiment **E27**) [V]. So a text block's indentation is yours to get right; the formatter will
  not help and will not interfere.
- **An empty array literal needs a type from context** (a language rule, not a formatting one), and
  `--fmt` has no opinion about it [V].

## 6. `use` lines

A **run** of consecutive `use` lines — no blank line and no standalone comment line between them —
is sorted by the line's text, token by token with a space appended after each
(`src/format.c:552-610`) [V]:

```
# input                 # output
use time                use a/b/c
use fs                  use db.DbError as DbErr
use http.Request        use fs
use http                use http
use db.DbError as DbErr use http.Request
use models/user         use models/user
use a/b/c               use time
                        (blank line preserved)
use zzz                 use aaa
use aaa                 use zzz
```
(experiment **E04**) [V]. Reading the ordering off that run:

- It is a **text** sort, so `use http` precedes `use http.Request` because `http ` < `http.` [V].
  Module imports and symbol imports therefore interleave by spelling, not by kind [V].
- An **alias** sorts by the whole line including `as`, so `use db.DbError as DbErr` sorts under
  `d` [V]. The alias never moves the line.
- A `/` path sorts as its text, so `use a/b/c` comes first [V].
- The sort is **stable** (insertion sort, `src/format.c:596-601`) [V], and order has no meaning in
  Ziman because imports resolve after all are read [V], which is what makes it safe.
- A **blank line splits the run**; so does a standalone comment line [V]. A **trailing** comment
  does *not* — the comment travels with its line and is excluded from the sort key [V]:
  ```
  use zzz
  // a comment splits the run
  use aaa
  use bbb
  use mmm  // a trailing comment
  ```
  (experiment **E31**) [V].

**Recommendation** [O]: one import per line is a language decision, not an omission
(`docs/syntax/modules.md`), so never try to group — and never hand-maintain the order, because the
formatter owns it.

## 7. Attributes

`#[…]` on a declaration and `#![…]` on a file are indented like the declaration they precede and
otherwise left alone [V]:

```
#![experimental]
use fs

// A doc comment for the struct.
#[derive(Json)]
pub struct Thing {
    #[json(name: "the_id")]
    id: int
    name: string
}
```
(experiment **E07**, input had the field attribute flush left) [V]. Verified behaviour:

- `#` and `[` are tight, and `#` takes no space before what follows (`src/format.c:185`) [V].
- A field attribute is indented to the field's level [V].
- **An attribute line is never wrapped**, however long: `in_attr` excludes it
  (`src/format.c:426`) [V], which is right because a newline inside `#[…]` does not parse. A
  130-column `#[when_unmatched(…)]` is left as written [V] (experiment **E32**).
- The formatter **preserves whatever order you wrote** between a doc comment, an attribute and the
  declaration — it will not move either [V].

### The real hazard here, and where it still lives

The convention is **doc comment, then attribute, then declaration**, and it is load-bearing
because `tools/gen_lib_manual.py` carries the pending comment across an attribute line and would
otherwise drop it: *"Without this, every documented declaration carrying one loses its whole
description from the manual"* (`tools/gen_lib_manual.py:261-267`) [V]. 45 declarations under `lib/`
are written that way [V].

**`ziman --api` has the bug the manual generator fixed.** `print_doc_comment`
(`src/main.c:411-425`) walks up from `decl_line - 1` and stops at the first line that is not a
comment — an attribute line is one [V]. So:

```
$ ./ziman --api thread | grep -B3 ThreadLocal
struct ThreadLocal<T> {            # no description
$ grep -n -B2 thread_key lib/core/thread.zm
52: // A key for a value that is separate on every thread. `init` makes a thread's first value, ...
54: #[thread_key]
$ grep -n "separate on every thread" docs/lib/thread.md
127: A key for a value that is separate on every thread. `init` makes a thread's first value, ...
```
[V]. The generated manual has the description; `--api`, the surface a model or a reader reaches
first, does not — for all 45 [V]. `math.min`/`math.max` lose three lines of real behaviour each
(`lib/core/math.zm:70-80`) [V]. This is Finding 1.

**Recommendation** [O]: write the doc comment above the attribute (that is what `lib/` does and
what the manual expects), and treat the convention as a rule rather than a habit — it is currently
documented in exactly one Python comment and nowhere a Ziman programmer would look.

## 8. Comments

- **Line comments only** — `//` to end of line; Ziman has no block comment [V].
- **A standalone comment is re-indented to its line's level** [V]: an over-indented comment inside
  a block is pulled back (experiment **E05**) [V].
- **A lone trailing comment is set to two spaces** from the code, however far out it was
  written [V].
- **A run of trailing comments on consecutive lines is aligned to one column**, two spaces past the
  longest code line in the run (`src/format.c:501-548`) [V]:
  ```
  # input                          # output
  val a = 1 // one                 val a = 1         // one
  val bbbbbbbb = 2       // two    val bbbbbbbb = 2  // two
  val c = 3  // three              val c = 3         // three
                                   (blank)
  val d = 4 // four                val d = 4  // four
  ```
  (experiment **E05**) [V]. **A blank line, or a line without a trailing comment, starts a new
  run** [V] — which is why a hand-aligned column that spans a blank line will always be
  re-aligned as two groups.
- **What it will not reflow**: the text of a comment, at any length. A 139-column trailing comment
  is untouched [V] (experiment **E27**). A comment is never joined to or split from another.
- **A comment protects the brace after it from `--canonical`** (`src/format.c:653-658`) [V], the
  only exception to that flag's rule — moving the brace would comment it out.

**Recommendation** [O]: keep trailing comments short enough that the aligned column does not push
real code off the screen, and put long prose on its own line above the code, where the formatter
has no opinion at all.

## 9. Blank lines

- **At most one in a row** [V].
- **None just inside a block**, at either end [V]:
  ```
  # input          # output
  fn a() {         fn a() {
                       val x = 1
      val x = 1    }
  }
  ```
  (experiment **E06**) [V].
- **None just after an opening bracket or just before a closer** — `line_ends_with_open` and
  `next_closes` suppress them (`src/format.c:317-319`) [V] (experiment **E30**).
- **One between two declarations is preserved** [V].
- **The formatter never inserts a blank line.** It left `}` and `struct S` adjacent
  (experiment **E06**) [V].

**Recommendation** [O]: one blank line between top-level declarations, and blank lines inside a
function only to separate phases — the formatter will keep exactly what you write, so this one is
entirely a house convention and should be stated as such rather than implied.

## 10. Operators and spacing

From one run (experiment **E02**, input was unspaced) [V]:

```
fn f(a: int, b: int) -> int { return a + b * 2 - (-a) }
    val m: [int] = [1, 2, 3]
    val s = m[0]
    val t = !true
    val v: Result<int, string> = ok(1)
    val w = f(1, 2)
    print(s + 1)
```

| Construct | Rule | Where |
|---|---|---|
| binary `+ - * / % == != <= >= && \|\| = ->` | one space either side | `src/format.c:160-166, 213` [V] |
| unary `-` and `!` | tight to the operand, decided from the token before | `:208-210` [V] |
| `:` in an annotation or a literal | nothing before, one space after | `:184, 211` [V] |
| `,` | nothing before, one space after | `:184, 211` [V] |
| `.` and `..` | tight both sides | `:184-185` [V] |
| `::` | tight both sides — `Row::dept` | `:182` [V] |
| indexing `m[0]`, call `f(1)` | no space before the bracket | `:191-203` [V] |
| `as` | an ordinary word: one space either side | `:212-215` [V] |
| `/` inside a `use` line | tight — a path, not a division | `:175` [V] |
| two operator characters written together (`++`, `=-`) | kept together | `:178` [V] |
| `<` and `>` | **the source's spacing, collapsed to at most one space** | `:206` [V] |

### The `<`/`>` exception, which a style guide has to own

`--fmt` cannot tell `a < b` from `List<int>` without parsing, so it leaves the spacing alone. The
consequence is asymmetric spacing survives verbatim [V]:

```
    val a: Map < string, int > = Map<string, int> { "a": 1 }
    val c = 1 <2
    val d = 1< 2
```
(experiment **E03**, output identical to input except for the comma) [V].

The corpus's own convention is **generics tight, comparisons spaced**, stated in the journal with
the note that nothing checks it (`docs/design/journal/part-06.md:467-472`) [V].

**Recommendation** [O]: write `Option<int>` and `short < 5`, and accept that this is the single
place where `--fmt --check` will not catch you. If this ever becomes worth enforcing, the cheapest
honest enforcement is a `--lint` rule, not a formatter change — the formatter's value is that it
needs only lexable text [O].

## 11. What `--fmt --canonical` changes

Exactly two things, and nothing else [V]:

1. **A `{` that starts a line joins the line before it**, with one space. *Every* line-leading
   brace, not only a construct's — Ziman has no bare block statement, so this cannot change
   meaning (`docs/design/journal/part-02.md:55-75`) [V].
2. **An `else` on its own line after the `}` that closed its `if` joins that line** [V].

```
# plain --fmt                  # --fmt --canonical
fn main()                      fn main() {
{                                  if true {
    if true                            print(1)
    {                              } else {
        print(1)                       print(2)
    }                              }
    else                           val c = Config {
    {                                  host: "h"
        print(2)                   }
    }                              print(1)  // a trailing comment
    val c = Config                 {
    {                                  host: 2
        host: "h"                  }
    }                          }
    print(1)  // a trailing comment
    {
        host: 2
    }
}
```
(experiment **E17**) [V]. Note the one exception working: the brace after a trailing comment stays
put [V].

**Why the split exists at all.** Plain `--fmt` keeps every break, which is what makes it safe to
run on anything lexable and also what stops it settling style — a file in Allman braces and a file
in K&R braces both pass `--fmt --check` [V]. `--canonical` settles the part a lexer can decide
alone [V]. The division is sound [O]: the formatter's trustworthiness comes from needing no parse,
and brace placement is the one layout question a scanner *can* answer, so putting it behind a flag
keeps the default honest while still giving the project a canon.

And the project does hold itself to the canon: `make fmt-check` runs **both** passes over all 347
files, and both exit 0 [V], as does `--canonical --check` over the 343 backend files [V]. So the
repository requires K&R braces, and says so in `make/ziman.mk:46-50` [V]. That closes the
2026-10-11 review's Finding 8.

## 12. The limits

Both are real, both are reached by refusing rather than by misbehaving, and both have arithmetic
reasons.

**Nesting: 200 brackets** (`FORMAT_MAX_NESTING`, `src/format.c:679-717`) [V]. Every line is
indented four spaces per open bracket, so formatting input that is *only* nesting is quadratic in
its own size — 32 KB of nested blocks once produced 256 MB of output at 9.7 GB peak
(`docs/design/journal/part-01.md:1428-1440`) [V]. Verified at the boundary:

```
$ ./ziman --fmt e18_deep200.zm >/dev/null; echo $?
0
$ ./ziman --fmt e18_deep202.zm >out 2>err; echo $?
2
$ cat err
e18_deep202.zm: line 2: nesting is too deep to format (202 open: 201 '(', 0 '[', 1 '{';
the limit is 200, and the indentation alone would be 808 spaces a line)
$ wc -c out        # the input, printed back unchanged
418 out
```
[V]. Note the error counts brackets **per kind**, so a reader knows whether to unnest a call chain
or a block nest (`src/format.c:681-685`) [V]. `<`/`>` are not counted, so generics are free [V].

**Output: 16 MB** (`FORMAT_MAX_OUTPUT`, `src/format.c:236-249`) [V], checked in `put` as the text
is appended rather than afterwards — because the cost is O(lines × depth) and the nesting limit
bounds only one factor [V]. Verified: 240 KB of input at depth 190 over 60,000 lines is refused,
at 61 MB peak RSS, with the input printed back unchanged [V]:

```
$ ./ziman --fmt --width=0 e19b_big.zm >out 2>err; echo $?
2
$ cat err
e19b_big.zm: formatting this would produce more than 16 MB of output -- the indentation is
four spaces per open bracket, so a deeply nested document of many lines grows much faster
than its own size
```

**Both refusals print the file back unchanged, with the reason on stderr and exit 2** [V], which is
what makes filtering an editor buffer through `--fmt -` safe: it can never empty the buffer [V].

One stale number worth correcting: the 16 MB cap is described as "about 180x the largest file here
(`lib/http/server.zm`, 89 KB)" in both `src/format.c:225-232` and the journal [V]. That file no longer
exists at that size; the largest `.zm` is now `lib/db/memdb.zm` at 185,735 bytes, which formats to
**exactly its own size** [V], making the cap **90.3x** [V]. See Finding 7.

**Also worth stating in a style guide** [O]: a document the language server analyses is capped at
4 MB (`docs/syntax/semantic-rules.md`) [V]. A style guide that pretends its tooling has no limits
is one somebody will walk into at the worst moment.

---

# Part 2 — the colouring strategy

The deliverable is what the colours **mean** and which distinctions are **worth** making. Three
layers now exist, with different capabilities — and since 2026-11-16 there is a fourth surface
reusing the first's decisions.

| Layer | Artefact | Sees |
|---|---|---|
| 1 | `editors/vscode/syntaxes/ziman.tmLanguage.json` — 35 scopes [V] | spelling |
| 1′ | `tools/gen_highlight.py` (ziman.dev, README) and `gallery/ui/desktop/highlight.zm` (the gallery's Code pane) [V] | spelling, four categories |
| 2 | — the distinctions a regex cannot draw at all | |
| 3 | `src/lsp.c` — semantic tokens, 5 types × 5 modifiers [V] | what a name *refers to* |

## Layer 1 — what the TextMate grammar already does

**35 distinct scopes** [V] (the brief said "roughly 32"; three of the increase are the `internal`,
`#![…]` and `val`/`var` fixes the last review asked for). The top-level pattern order is
`comments, text-blocks, strings, numbers, booleans, attributes, primitive-types, builtin-types,
builtin-functions, field-accessors, declarations, keywords, function-calls, type-names,
operators` [V]. Grouped into the categories a theme author can actually reason about:

| Category | Scopes | What it matches | Why this grouping |
|---|---|---|---|
| **Prose** | `comment.line.double-slash` | `//…$` | It is not program text; it should recede. [O] |
| **Inert data** | `string.quoted.double`, `string.quoted.triple`, `constant.character.escape`, `constant.numeric.{integer,float,hex,suffixed}`, `constant.language.boolean` | literals; `.triple` is a text block; `.suffixed` is `…n`/`…d`/`…b` | A value written in the source, not a name. Escapes and suffixes deserve their own scopes because both change meaning: `\n` is one character, `200b` is a different type from `200`. [V]/[O] |
| **Structure words** | `keyword.control`, `keyword.control.import`, `keyword.other` (`with`), `storage.type{,.function,.struct,.enum}` | `if else while for in break continue spawn return switch defer try`; `use as`; `with`; `fn struct enum` | The skeleton a reader skims for. [O] |
| **Declared properties** | `storage.modifier` (`pub internal native builtin variadic`), `storage.modifier.immutable` (`val`), `storage.modifier.mutable` (`var`) | the markers Ziman makes you write | Ziman's central design value: mutability, visibility and thread-affinity are *written*. These deserve their own category precisely because they are not keywords in the control-flow sense. [V]/[O] |
| **Names** | `entity.name.function`, `entity.name.type`, `entity.name.type.struct`, `entity.name.type.enum`, `variable.other.property` | a lowercase name before `(`; any capitalised word; the name at a `fn`/`struct`/`enum` declaration site; the field after `::` | Separating a *declaration site* from a *reference* is what makes "where is this defined" visible without a resolver. [V] |
| **Library surface** | `support.type.primitive`, `support.type.builtin`, `support.function.builtin` | `int float bool string bigint decimal byte unit`; the generated type list; the generated function list | "This is the language, not your program." [O] |
| **Punctuation with meaning** | `meta.attribute`, `keyword.operator.{arrow,range,comparison,logical,assignment,arithmetic,accessor}` | `#[…]`/`#![…]`; `->`; `..`; `== != <= >= < >`; `&& \|\| !`; `=`; `+ - * / %`; `::` | Each is a distinct *reading* of a symbol. [V] |

Two rules in this grammar are worth singling out because they encode hard-won lessons, both
recorded in the file's own comments [V]:

- **`attributes` is a `begin`/`end` pair that includes `#strings`**, because a single
  `#!?\[[^\]]*\]` cannot see past a `]` inside the attribute's own message — and `clear`'s
  `#[when_unmatched("… an empty one ('[]', 'Map<K, V> {}') …")]` has one, which once coloured 191
  lines of `lib/prelude/array.zm` as a single string [V].
- **`strings` ends at `"` *or* `$`**, because a Ziman string cannot span lines, so a stray quote
  must not swallow the file [V].

Both are right, and both are the kind of thing a hand-written grammar gets wrong once [O].

### Gap A — the two generated rules match nothing at all

**This is the single most consequential thing in this report.** `builtin-types` and
`builtin-functions` are the two largest rules in the grammar and the only two generated from the
compiler, and **neither can ever match** [V].

```
$ python3 -c '...'    # the match strings as JSON decodes them
builtin-types     -> '\\\\b(Channel|Guard|Iter|...|Set)\\\\b'
builtin-functions -> '\\\\b(add|all|any|...|values)(?=\\\\s*[(<])'
keywords          -> '\\b(if|else|while|...|try)\\b'
type-names        -> '\\b[A-Z][A-Za-z0-9_]*\\b'
```
[V]. Every hand-written rule carries `\\b` in the JSON — regex `\b`, a word boundary. The two
generated rules carry `\\\\b` — regex `\\b`, **a literal backslash followed by `b`**. The cause is
`tools/gen_vscode_grammar.py:86-87`, which inserts `r"\\\\b(%s)\\\\b"` **textually** into the JSON
instead of letting `json.dumps` do the escaping [V]:

```
$ python3 -c "import json,re; g=json.load(...); print(bool(re.search(g['repository']['builtin-functions']['match'], 'print(len(xs))')))"
False
$ # the same pattern with \\b collapsed to \b
True
```
[V]. The consequences, measured with the repository's own `tools/check_grammar.py:scopes_of`
(not my own simulator) [V]:

| Token | Scope it gets | Scope it is meant to get |
|---|---|---|
| `print`, `len`, `union`, `concat` | `entity.name.function.ziman` (the fallback) | `support.function.builtin.ziman` |
| `Map`, `Set`, `Option`, `Result`, `Channel`, `MutableArray` | `entity.name.type.ziman` (the fallback) | `support.type.builtin.ziman` |
| `mutable_array_new` in `mutable_array_new<int>()` | **(no scope)** | `support.function.builtin.ziman` |

The third row is the visible one: `function-calls` requires `(?=\s*\()`, so a generic call's name
falls through entirely [V] — a programmer's eye sees the name go colourless the moment they add
type arguments.

`Channel`, `Map`, `Result` and `Set` are **reserved words in `src/lexer.c`** [V] and are
indistinguishable from a user's own struct in the editor today [V].

**Why no gate caught it.** `make vscode-grammar-check` runs the generator and `git diff
--exit-code`s the file [V] — it proves the file equals the generator's output and can never prove
the output matches anything. `make grammar-check` runs 27 scope cases, and **not one of them tests
`support.type.builtin` or `support.function.builtin`** [V]; its single `support.type` case is on
`string`, which `primitive-types` handles [V]. This is Finding 2.

### Gap B — constructs in `docs/syntax/` with no scope at all

Established by running `check_grammar.scopes_of` over each line [V]:

| Construct | `docs/syntax/` reference | Scope today | Worth colouring? |
|---|---|---|---|
| `on_error` in `spawn f(x) on_error h` | `control-flow.md` (concurrency) | **(none)** | **Yes** [O] — it is the difference between a thread whose failure ends the program and one whose failure is silent. A contextual word in the parser (`src/parser.c:2009-2010`), so no keyword list reaches it [V]. |
| a module qualifier (`fs` in `fs.read_text`, `u` in `u.User`) | `modules.md` | **(none)** | **Yes** [O] — `namespace` is a standard LSP type and the qualifier is how a reader tells a cross-module call from a local one. |
| `prelude` | `modules.md` ("the one qualifier with no module behind it") | **(none)** | **Yes** [O] — it means "reach past my own shadow", which is worth being visible exactly once per file. |
| a field after `.` (`x` in `p.x`) | `functions-and-types.md` | **(none)** | Yes [O] — and the grammar already has `variable.other.property` for the `::` form, so the category exists and is half-populated. |
| a field label in a literal (`id` in `User { id: 1 }`) | `functions-and-types.md` | **(none)** | Yes [O] — same scope as above; it is the same field. |
| a `use … as` alias | `modules.md` | **(none)** | Yes [O] — it is a declaration site. |
| `_`, the deliberate discard | `functions-and-types.md` ("must-use `Result`") | **(none)** | **Yes** [O] — `val _ = risky()` is a documented assertion that an error was considered. Colouring it keeps the assertion legible. |
| `\`, the string-literal continuation | `lexical-and-grammar.md` | **(none)** | Yes [O] — it is `constant.character.escape` in spirit; unscoped, it reads as stray punctuation. |
| a `switch` arm's payload binder (`r` in `Circle(r)`) | `functions-and-types.md` | **(none)** | Yes, via layer 3 [O] — it is an immutable local. |
| an enum variant (`Circle` in `Shape.Circle(5.0)`, `Some`) | `functions-and-types.md` | `entity.name.type` — **wrong kind** | Yes [O] — `enumMember` exists in the LSP's standard set. A variant is not a type. |
| a generic **bound** (`Ord`, `Eq`, `Hash`) | `functions-and-types.md` | `entity.name.type` — **wrong kind** | Yes [O] — and `gen_vscode_grammar.py:46-49` says in as many words that colouring `Ord` as a type "would be wrong wherever it is written", then leaves the fallback rule doing it [V]. |
| a **type parameter** (`T`, `K`, `V`) | `functions-and-types.md` | `entity.name.type` — **wrong kind** | Yes [O] — the same generator says at `gen_vscode_grammar.py:26-29` that the single letters "are not types and must not be highlighted as one -- highlighting `T` would paint every generic signature in the editor" [V]. It does [V]. |
| `/` in `use a/b` | `modules.md` | `keyword.operator.arithmetic` — **wrong kind** | Yes [O] — a path separator painted as division. `need_space` already special-cases `use` lines (`src/format.c:175`), so the precedent for a `use`-line context exists in the formatter [V]. |
| `<`/`>` around type arguments | everywhere | `keyword.operator.comparison` — **wrong kind** | No [O] — it is the one case I would leave alone. A regex cannot decide it and a wrong decision is worse than a neutral one; this is layer 3's job. |

The last four rows are the honest shape of the gap: the grammar has no "wrong" category, so an
unresolvable name lands in `entity.name.type` and looks deliberate.

### Layer 1′ — the two non-editor colourers, and whether they agree

`tools/gen_highlight.py` marks up `.zm` for ziman.dev and the README; `gallery/ui/desktop/highlight.zm`
(new today) colours the gallery's Code pane at runtime. Both take a deliberately coarser partition
than the grammar: **keywords, literals, comments and attributes coloured; types and identifiers
plain** [V]. Each has a gate against `src/lexer.c`'s keyword table —
`make highlight-check` and `tools/check_gallery_keywords.py` (which reports "37 keywords, split 24
coloured, 11 left plain, 2 literal") [V].

**I think the coarse partition is right and the gallery was right to copy it** [O]. The argument in
both files' headers is sound: a signature whose every type is emphasised is a signature nobody
reads, and telling a type from an identifier needs the compiler. A tokenizer that colours only
what it can know is one that never lies.

Where the three surfaces **agree** [V]: keywords; `true`/`false` as literals, not keywords;
strings, text blocks and numbers as literals; `//` to end of line as a comment; `int`/`string`/`Map`
left out of the keyword colour; a `//` inside a string is not a comment and a `"` inside a comment
does not open a string (verified on both the gallery and the website) [V].

Where they **depart** — two measured divergences [V]:

```
# ziman.dev (tools/gen_highlight.py)              # the gallery (highlight.zm)
#![experimental]                      (plain)     [#![experimental]] = dim
<span class="dim">#[derive(Json)]</span>          [#[derive(Json)]]  = dim
<span class="grn">0</span>..<span class="grn">3</span>   [0..3] = literal
```

1. **`#![…]` is not an attribute on the website.** `gen_highlight.py:108` requires
   `startswith("#[")`, which is false for `#![` [V], so a module-level attribute is uncoloured on
   ziman.dev while `#[…]` beside it is dim. **The gallery is right here and the website is wrong**,
   and it is the *same* defect the TextMate grammar fixed in F75 by relaxing its match to `#!?\[`
   [V]. Four shipped modules declare themselves experimental this way [V].
2. **`0..3` is one literal span in the gallery.** `highlight.zm:141` consumes any `.` after a
   digit; `gen_highlight.py:115-121` deliberately stops at a `.` not followed by a digit, with the
   reason in a comment [V]. So the gallery paints the range operator as part of the number.
   **The website is right here and the gallery is wrong**, and `for i in 0..n` is the commonest loop
   in the language [V].

Each surface has the other's bug. That is what a copied partition without a shared implementation
costs [O]; see Finding 3.

### The forced deviation: `Ink` has a colour and no face

The gallery's own header says ziman.dev draws a keyword in **bold**, that `controls.Ink` carries a
colour and not a face index, and that "a keyword is drawn in the palette's accent instead"
(`gallery/ui/desktop/highlight.zm:16-19`) [V]. My judgement, which the brief asked for:

**The call is right, and the comment overstates the deviation.** ziman.dev's rule is
`pre b { color: var(--accent); font-weight: 700 }` [V] — the site's keyword is **already** accent
*and* bold. So the gallery does not substitute a colour for a weight; it keeps the colour and drops
the weight. That is the smallest possible loss and the right one to take [O].

**But one real distinction is lost, and the comment does not name it.** On the site, a comment is
`--code-dim` *italic* and an attribute is `--code-dim` upright
(`index.html:128-129`) [V]. In the gallery both are `dim_ink()` [V], so **an attribute and a comment
are indistinguishable** — two categories collapsed into one. That is the cost worth recording,
not the keyword's weight [O].

**Should `Ink` carry a face?** Not a raw face index [O]. An `Ink` holding `face: int` lets a caller
name a face the window never loaded, which fails as wrong glyphs at paint time rather than as a
compile error — and `Style` already owns `text_font`/`icon_font` precisely so that a control does
not choose a face (`lib/ui/controls.zm:3930-4046`) [V]. What the website needs is **two** variations
(bold and italic), not one, so a single index would not close the gap anyway.

My recommendation [O]: give `Ink` an **emphasis enum** (`Regular`/`Strong`/`Quiet`) and let `Style`
resolve it against the faces the window actually loaded, falling back to colour alone when the face
is absent. That is the same shape `Style.collapsed_mark` already uses for "this layer cannot know
what is in the font bytes" (`lib/ui/controls.zm:3958-3969`) [V], it restores the
attribute/comment distinction, and it introduces no way to ask for a face that does not exist.

## Layer 2 — what a regex grammar structurally cannot do

Each claim verified against the grammar file with `check_grammar.scopes_of` [V].

| Distinction | Why a regex cannot | Scope today | Worth colouring? |
|---|---|---|---|
| **a type parameter vs. a struct name** | both are `[A-Z][A-Za-z0-9_]*`; `T` is a parameter in `fn f<T>(x: T)` and a struct anywhere else | both `entity.name.type.ziman` [V] | **Yes, loudly** [O]. Ziman writes type arguments explicitly everywhere, so generic signatures are dense; painting `T` as a type is the specific harm the grammar's own generator predicted [V]. `typeParameter` is standard LSP. |
| **a `var` vs. a `val` at its *use* site** | the declaring keyword is elsewhere in the file, possibly in an enclosing block | no scope on a use at all [V] | **Yes — this is the single most valuable one** [O]. `val`/`var` differ by one character and that typo silently flips mutability; the use site is where it does damage. Layer 3 ships this. |
| **a deprecated symbol vs. a live one** | `#[deprecated]` is on the declaration, in another file | none [V] | **Yes** [O]. The compiler already warns on every compile; the editor should show the same fact where the name is written. `deprecated` is standard. |
| **an imported name vs. a locally declared one** | the `use` line is elsewhere; Ziman has no wildcard import, so the information exists but not locally | none [V] | **Yes, but quietly** [O]. `defaultLibrary` for the bundled library is the high-value half; a `namespace` scope on the qualifier does most of the rest with no resolution at all. |
| **a shadowed binding vs. what it shadows** | needs scope tracking | none [V] | **Yes** [O], and it is the case where being wrong is most expensive — see the layer-3 defect below. |
| **`<` generic vs. `<` comparison** | genuinely ambiguous to a one-token lookahead; the parser needs it too | `keyword.operator.comparison` for both [V] | **Yes, but only at layer 3** [O]. I would *not* guess at layer 1. |
| **an enum variant vs. a type** | `Shape.Circle` and `mod.Type` are the same shape | `entity.name.type` for both [V] | Yes [O]. `enumMember` is standard and a variant is a value, not a type. |
| **a generic bound vs. a type** | `Eq`/`Hash`/`Ord` are capitalised words in a `<…>` list | `entity.name.type` [V] | Yes [O], and cheaply: they are a closed set of three. This one a regex *could* do with a bounded lookbehind — it is a gap, not a structural limit. |
| **a field vs. a local** | `p.x` and `x` differ only in a preceding `.`, which the grammar does look at for `::` | none for `.x` [V] | Yes [O]. `property` is standard. |
| **a thread-bound handle vs. an ordinary value** | `#[thread_bound]` is on the struct, in `lib/prelude/` | none [V] | Yes [O] — but as a **non-standard** modifier, because nothing standard means "this may not cross a thread", and Ziman turns that into a compile error. |

The honest summary [O]: of ten distinctions, nine are worth colouring and eight need a resolver.
Only the generic-bound row is a layer-1 gap rather than a layer-1 limit.

## Layer 3 — the semantic-token layer

**The brief's premise is out of date and I am correcting it.** `src/lsp.c:1884-1889` advertises
`documentFormattingProvider`, `hoverProvider`, `definitionProvider`, `documentSymbolProvider`,
`workspaceSymbolProvider`, `documentHighlightProvider`, `completionProvider` **and**
`semanticTokensProvider` with a full legend [V], confirmed by driving the real server:

```
"semanticTokensProvider": { "legend": {
  "tokenTypes":     ["variable","parameter","function","struct","enum"],
  "tokenModifiers": ["declaration","readonly","mutable","deprecated","defaultLibrary"] },
  "full": true }
```
[V]. `mutable` is deliberately non-standard, and the file says why: *"nothing standard says 'this
name refers to something you may reassign'"* (`src/lsp.c:1789-1791`) [V]. The implementation's own
comment names the deferred set — `namespace`, `property`, `enumMember`, `typeParameter`, and
non-standard `threadBound` and `optional` — citing "the style report's own ordering"
(`src/lsp.c:1772-1776`) [V]. So the previous review's layer-3 proposal was taken up, in order, with
the standing finding closed first.

### Does `documentHighlight` resolve, or merely match the word?

**It resolves, for locals.** `handle_document_highlight` calls `find_local` at each candidate
occurrence and keeps it only when it resolves to the *same* declaration position, and it skips
`p.x` and `name:` labels (`src/lsp.c:1724-1733`) [V]. Driven against the real server on

```
 1  struct Point { x: int }
 2  fn helper(x: int) -> int { return x }
 3  fn main() {
 4      val x = 1
 5      print(helper(x))
 6      val p = Point { x: 2 }
 7      print(p.x)
 8      if true {
 9          var x = 3
...
```

a highlight on the `x` at line 4 returns **two** ranges — line 4 and line 5 — not the seven the
2026-10-11 review measured [V]. The field declaration, the field label, `p.x` and the inner
shadowing binding are all correctly excluded [V]. So a token layer is **new** work, not a
re-export; but occurrence resolution does exist, and that earlier finding is fixed in substance.

### The defect both features share: a shadowing block

Both features are wrong around a shadowing block, in **opposite** directions. On

```
 1  fn main() {
 2      val v = 1
 3      if true {
 4          var v = 2
 5          print(v)
 6      }
 7      print(v)
 8  }
```

which really prints `2` then `1` [V] — so line 7's `v` is the outer `val` — the server answers [V]:

| Line | Token | Semantic token says | Truth |
|---|---|---|---|
| 2 | `val v` | `variable declaration,readonly` | correct |
| 4 | `var v` (the shadowing declaration) | `variable readonly` — **no `declaration`, and readonly** | it *is* a declaration, and it is mutable |
| 5 | `print(v)` inside the block | `variable mutable` | correct |
| 7 | `print(v)` after the block | `variable mutable` | **wrong — it is the outer `val`** |

and `documentHighlight` on **either** `v` (line 2 or line 4) returns **only line 2** [V] — so the
inner declaration highlights the outer binding and no use is ever highlighted.

The mutability marker is the one thing this layer exists to provide, and it is **off by one
declaration** across a shadowing boundary. The two features also disagree with each other: the
token layer thinks line 7 belongs to the inner binding, `documentHighlight` thinks line 2's binding
has no uses. This is Finding 4.

Two further mis-attributions in the token path, both already handled in `documentHighlight` and
both missing here [V]: the field label `x` in `Point { x: 2 }` and the field access `x` in `p.x`
are each emitted as `variable readonly` [V]. `looks_like_label` and the `char_before == '.'` test
exist a few hundred lines above (`src/lsp.c:1727-1729`) [V].

### What Ziman should emit, and what each buys

Shipped (keep as is) [V]: `variable` + `readonly`/`mutable`/`declaration`; `parameter` +
`readonly`; `function`, `struct`, `enum`, each + `defaultLibrary` and + `deprecated`.

Proposed additions, in the order I would ship them [O]:

| Token type / modifier | Standard? | Maps to | What it buys over layer 1 |
|---|---|---|---|
| `typeParameter` | standard type | `T`, `K`, `V` in a generic declaration's scope | Stops the grammar painting every generic signature as full of types — the exact harm `gen_vscode_grammar.py` named. Highest value per line of code [O]. |
| `namespace` | standard type | a module qualifier, and a `use … as` alias | Gives a cross-module call a visible boundary; closes two Gap-B rows at once with resolution the loader already has. |
| `property` | standard type | a field after `.`, and a field label in a literal/`with` list | Closes two more Gap-B rows and would have prevented the two token-path mis-attributions above by construction. |
| `enumMember` | standard type | a variant at construction and in a `switch` arm pattern | A variant stops reading as a type. |
| `operator` + `generic`-ish disambiguation | standard type | `<`/`>` when they bracket type arguments | The only honest answer to the `<` ambiguity; layer 1 should keep guessing neutrally. |
| `interface` | standard type | `Eq`, `Hash`, `Ord` as bounds | A bound is a constraint, not a type. (Or close it at layer 1 — it is a closed set of three.) |
| **`threadBound`** | **non-standard** | a `#[thread_bound]` type, and a local holding one | Ziman turns a cross-thread use into a compile error or a runtime error; nothing standard means "this value may not leave this thread", and the restriction is the single most Ziman-specific fact about a value [O]. |
| **`optional`** | **non-standard** | an `Option<T>` parameter that may be omitted, and a parameter with a default | "You may leave this out" is a property of the *declaration* that the call site cannot see. No standard modifier says it. |
| `defaultLibrary` on `prelude.` | standard modifier | the `prelude` qualifier | Marks the escape hatch as library surface rather than an undefined name. |

What layer 3 can **never** add, and should not try [O]: anything that needs the program to run.
Deprecation, mutability, visibility, thread-affinity and optionality are all *written in the source*
— which is exactly why Ziman is unusually well served by this layer.

## The strategy

### Category-to-role mapping

Ziman's design value is explicitness: mutability, visibility and thread-affinity are written, not
inferred. **I argue for making those written markers prominent, and I would make them the second
loudest thing on the screen** [O] — after the literals and before the structure words.

The reasoning is not a general aesthetic preference. In most languages a visibility or mutability
marker is noise you learn to skip: the compiler enforces the property anyway and the marker is
redundant with a convention. In Ziman the marker **is** the property's only statement, and two of
them — `val`/`var`, and `pub`/`internal`/(nothing) — differ by one token from a different program
that still compiles. The project has already decided colour is the answer: the grammar split the
two scopes and the LSP added a non-standard `mutable` modifier for the use site [V]. Making them
quiet would undo a decision the codebase has already made and paid for. Against that, making five
categories loud makes none of them loud — so what follows is a ranking, not a wish list.

| Rank | Category | Role | Reason |
|---|---|---|---|
| 1 — **loudest** | Inert data (strings, text blocks, numbers, booleans) | strong hue, distinct from every name | A literal is the one thing in a file that is *not* a name, and mistaking one for a name is the costliest confusion. Every mature theme does this; it is also the only decision all three of Ziman's colourers already agree on [V]. |
| 2 | Declared properties — `val`/`var`, `pub`/`internal`, `variadic`, `native`, and layer 3's `mutable`/`threadBound` | **a second channel**: weight or a distinct hue family, *not* a shade of the keyword colour | The argument above. Critically, `val` and `var` must differ **from each other**, not merely from identifiers — which is what the grammar's split now permits and no theme yet uses [V]. |
| 3 | Structure words (`fn struct enum if else for switch use with try defer spawn`) | one hue, moderate weight | The skeleton. Loud enough to skim, quiet enough not to dominate a dense signature. |
| 4 | Library surface (`support.*`) | a muted variant of the name colour | "Not yours" is useful and secondary. Today this rank is unreachable because both `support.*` generated rules are dead (Gap A) [V]. |
| 5 | Names (functions, types, declaration sites) | near the foreground colour, declaration sites one step stronger | In a language with 268 public prelude names, colouring every name loudly colours everything. |
| 6 | Attributes (`meta.attribute`) | dim, but **distinct from a comment** | An attribute changes compilation; a comment does not. The gallery currently collapses these [V]. |
| 7 — **quietest** | Prose (comments) | dimmest that still passes contrast | It should recede — but see the contrast failure below: "recede" is routinely implemented as "fail AA". |
| — | Punctuation, operators, `<`/`>` | foreground, unstyled | Styling every operator is how a file becomes a rainbow. The exception is `->` and `::`, which are *readings* rather than arithmetic and may take the structure hue. |
| — | Type parameters (`T`) | **foreground, deliberately unstyled** | They are placeholders. The current behaviour — full type colour — is actively worse than nothing [V]. |

### Light and dark, and colour-blind safety

Constraints I would hold a Ziman theme to [O]:

1. **Code text is 14px mono, which is not "large text", so every category needs ≥ 4.5:1** against
   its own background (WCAG AA). The 3:1 large-text allowance does not apply.
2. **Comments get a floor, not an exemption.** The temptation is to dim prose until it vanishes.
   I would hold comments to 4.5:1 and achieve "recede" with hue and weight, not luminance alone —
   comments are the longest prose in a source file and the one most often read by someone who did
   not write it.
3. **No distinction may rest on red-versus-green alone.** Ziman has a natural temptation here:
   `Ok`/`Err`, `val`/`var`, `pub`/private. Every such pair must also differ in **luminance or
   weight**, so that it survives deuteranopia and a greyscale diff.
4. **Light and dark are separate palettes, not one palette inverted.** A hue darkened for paper is
   not legible on near-black, and the site proves it (below).
5. **Verify the pair that will actually be adjacent**, not each colour against the page background.
   A literal beside a keyword inside a `#[when_unmatched("…")]` is the real test.

**The site's own palette fails constraint 1 and 4 today** [V]. `pre` uses
`--code-bg: #15201b` with `--code-fg: #e8e4da` in **both** themes
(`websites/ziman.dev/index.html:117-120`) [V], while `--accent` and `--structure` are swapped per
theme — and the light-theme values are commented "darkened for contrast on paper" [V]:

| | light-theme value on `#15201b` | dark-theme value on `#0c110e` |
|---|---|---|
| keyword (`--accent`) | **3.98:1** ✗ | 8.79:1 ✓ |
| literal (`--structure`) | **2.66:1** ✗✗ | 8.68:1 ✓ |
| comment (`--code-dim`) | 6.09:1 ✓ | 5.46:1 ✓ |
| body (`--code-fg`) | 13.19:1 ✓ | 15.01:1 ✓ |
[V]

So in light theme — the default on most machines — a literal in a code block on ziman.dev sits at
2.66:1, below even the 3:1 large-text floor [V]. The fix is one line: give `--code-bg`/`--code-fg`
no theme variants *and* pin the two code hues to the dark-theme values, since the code block is
always dark [O].

**The gallery inherits the wrong half of that palette.** `highlight.zm` copies the three
**light-theme** hexes — `#a8701a`, `#2e6b4f`, `#8fa096` [V] — onto `controls.default_style()`,
whose surfaces are light (`ink: rgb(0.1,0.1,0.1)`, `face: rgb(0.9,…)`, `panel: rgb(0.96,…)`)
[V]:

| Gallery ink | on white | on `panel` #f5f5f7 | on a control `face` #e6e6e6 |
|---|---|---|---|
| keyword `#a8701a` | 4.20:1 ✗ | 3.86:1 ✗ | **3.37:1** ✗ |
| literal `#2e6b4f` | 6.30:1 ✓ | 5.79:1 ✓ | 5.05:1 ✓ |
| comment/attribute `#8fa096` | **2.75:1** ✗✗ | **2.53:1** ✗✗ | **2.20:1** ✗✗ |
[V]

The Code pane is a `text_area_of` (`gallery/ui/desktop/state.zm:147-154`) [V], so comments are
drawn at about 2.2:1. **The partition travelled correctly and the palette did not** [O]: a value
that reaches 6.09:1 on the site's dark code block reaches 2.20:1 on a light control face. This is
Finding 5, and it is the answer to "is taking the palette from ziman.dev the right call" — taking
the *roles* was right, taking the *hexes* was not.

### A worked sample

Lines from `editors/vscode/showcase.zm` and `docs/syntax/`, each token's scope read with the
repository's own `tools/check_grammar.scopes_of` [V]. The role column is my recommendation [O].

| Token | Layer-1 scope [V] | Category | Role [O] |
|---|---|---|---|
| `pub` | `storage.modifier.ziman` | declared property | rank 2 |
| `internal` | `storage.modifier.ziman` | declared property | rank 2 |
| `variadic` | `storage.modifier.ziman` | declared property | rank 2 |
| `val` | `storage.modifier.immutable.ziman` | declared property | rank 2 — **must differ from `var`** |
| `var` | `storage.modifier.mutable.ziman` | declared property | rank 2 — **must differ from `val`** |
| `fn` | `storage.type.function.ziman` | structure word | rank 3 |
| `spawn` | `keyword.control.ziman` | structure word | rank 3 |
| `use`, `as` | `keyword.control.import.ziman` | structure word | rank 3 |
| `distance_squared` (at `fn`) | `entity.name.function.ziman` | name, declaration site | rank 5, one step stronger |
| `Point` | `entity.name.type.ziman` | name | rank 5 |
| `int`, `string`, `unit`, `byte`, `bigint` | `support.type.primitive.ziman` | library surface | rank 4 |
| `Map`, `Set`, `Option` | `entity.name.type.ziman` | **should be** library surface | rank 4 — **Gap A: unreachable** |
| `print`, `len`, `union` | `entity.name.function.ziman` | **should be** library surface | rank 4 — **Gap A: unreachable** |
| `mutable_array_new` in `…<int>()` | **(no scope)** | library surface | rank 4 — **Gap A** |
| `"ann"` | `string.quoted.double.ziman` | inert data | rank 1 |
| text block | `string.quoted.triple.ziman` | inert data | rank 1 |
| `12` / `2.0` / `200b` / `…n` | `constant.numeric.{integer,float,suffixed}` | inert data | rank 1 |
| `true` | `constant.language.boolean.ziman` | inert data | rank 1 |
| `#[deprecated("…")]` | `meta.attribute.ziman` (whole span) | attribute | rank 6 — **distinct from a comment** |
| `//` line | `comment.line.double-slash.ziman` | prose | rank 7, ≥ 4.5:1 |
| `->` | `keyword.operator.arrow.ziman` | punctuation with meaning | structure hue |
| `::` | `keyword.operator.accessor.ziman` | punctuation with meaning | structure hue |
| `x` after `::` | `variable.other.property.ziman` | name | rank 5 |
| `..` | `keyword.operator.range.ziman` | punctuation | unstyled |
| `<`, `>` | `keyword.operator.comparison.ziman` | ambiguous | unstyled at layer 1 |
| `/` in `use models/user` | `keyword.operator.arithmetic.ziman` | **wrong kind** | should be `namespace`/path |
| `T` in `fn identity<T>` | `entity.name.type.ziman` | **wrong kind** | **unstyled** |
| `Ord`, `Eq` as bounds | `entity.name.type.ziman` | **wrong kind** | constraint hue |
| `Circle` in `Shape.Circle` | `entity.name.type.ziman` | **wrong kind** | `enumMember` |
| `on_error` | **(none)** | structure word | rank 3 |
| `os` in `os.fatal`, `u` in `u.User` | **(none)** | namespace | rank 4, quiet |
| `prelude` | **(none)** | namespace / library | rank 4 |
| `id` in `User { id: 1 }` | **(none)** | property | rank 5 |
| `x` in `p.x` | **(none)** | property | rank 5 |
| `_` in `val _ = f()` | **(none)** | declared property | rank 2 — it asserts a decision |
| `\` continuing a literal | **(none)** | inert data | rank 1, as an escape |

`showcase.zm`'s header says "a rule with nothing to colour in this file is a rule nobody will
notice breaking" [V], and it is compiled and `--fmt`-checked by `make showcase-check` [V]. It is
still true that it reaches **none** of the rows marked "(none)" above, and it contains no `::`-free
field access worth colouring. **Recommendation** [O]: add one line per Gap-B row to `showcase.zm` —
it is the cheapest possible coverage for a layer that otherwise has none.

### How to verify a theme is right

Steps someone can run, in the order that catches the most per minute [O]:

1. **Prove the rules fire before judging the colours.**
   `python3 tools/check_grammar.py` — 35 rules, 27 cases, 717 corpus attributes scanned [V]. Then,
   for any scope you are about to style, assert it:
   ```
   python3 - <<'EOF'
   import importlib.util, sys, json
   spec = importlib.util.spec_from_file_location("cg", "tools/check_grammar.py")
   cg = importlib.util.module_from_spec(spec); sys.argv = ["x"]
   try: spec.loader.exec_module(cg)
   except SystemExit: pass
   g = json.load(open("editors/vscode/syntaxes/ziman.tmLanguage.json"))
   rules, repo = cg.rules_in_order(g), g["repository"]
   line = "    print(len(xs))"
   print(cg.scopes_of(line, rules, repo))
   EOF
   ```
   This is how Gap A was found [V]; a theme author who styles `support.function.builtin` today will
   see nothing change and will not know why.
2. **Run the real engine once per grammar change.** `npm i vscode-textmate vscode-oniguruma` in
   `editors/vscode/`, tokenise `lib/**/*.zm`, and diff the scope stream against the previous run.
   `check_grammar.py`'s own header says in as many words that it is a check on the grammar's rules
   and not a renderer [V]. I did **not** run the real engine (no network); that is a stated gap.
3. **Measure contrast for every (category, background) pair you will actually ship** — not category
   against page background. The script in the appendix produces the two tables above [V]. Reject
   anything under 4.5:1.
4. **Convert a screenshot to greyscale and to a deuteranopia simulation.** Every pair in the rank
   table must still be two things. `Ok`/`Err` and `val`/`var` are the pairs to check first.
5. **Drive the LSP and compare its tokens to the truth.** Open a file with a shadowing block, ask
   for `textDocument/semanticTokens/full`, and check the `mutable` modifier line by line against
   what the program prints. That is how Finding 4 was found, and it took one 40-line script [V].
6. **Read `showcase.zm` with the theme applied**, then read a real file — `lib/db/memdb.zm`
   (185 KB) [V] or `lib/ui/controls.zm`. A theme that is pleasant on a curated 207-line file and
   exhausting on a real one has failed the only test that matters.
7. **Check the other two surfaces agree.** `make highlight-check` and
   `python3 tools/check_gallery_keywords.py` [V] prove the keyword lists match the compiler; they
   prove nothing about the four partition edges. Run the three colourers over the same file and diff
   the spans — that is how both divergences in Layer 1′ were found [V].

---

# Findings — questions for the maintainer

Each is something I believe is wrong or under-specified, phrased as a question. None is written up
above as though it were the style, and I changed no file but this report.

**1. Should `ziman --api` carry a doc comment across an attribute line, as the manual generator
does? [V]** `tools/gen_lib_manual.py:261-267` deliberately carries the pending comment across a
`#[…]` line, with the reason stated: without it "every documented declaration carrying one loses
its whole description from the manual". `src/main.c:411-425` does the opposite — it walks up from
`decl_line - 1` and stops at the first non-comment line. So 45 declarations under `lib/` have a
description in `docs/lib/*.md` and none in `--api` [V]: `ThreadLocal<T>`, `math.min`/`max`,
`concat`, `push`, `snapshot`, `clear`, `narrow`, `server.open_session_with_timeout` among them. The
asymmetry matters more than the count, because `--api` is what the 2026-10-09 benchmark identified
as the surface a model reads, and `print_doc_comment`'s own comment argues at length that "a
signature is not a behaviour" [V]. The fix looks like four lines — skip a line that begins `#[`
while walking up. **Is the inconsistency known, and is `--api` or the manual the intended
behaviour?** A second half: the convention "doc comment, then attribute, then declaration" is
documented in **exactly one Python comment** and nowhere in `docs/syntax/` [V] — should it be
stated where a Ziman programmer would find it?

**2. Are `builtin-types` and `builtin-functions` meant to match anything? [V]** They do not.
`tools/gen_vscode_grammar.py:86-87` writes `r"\\\\b(…)\\\\b"` textually into the JSON, so after
JSON decoding the regex is `\\b` — a literal backslash — where every hand-written rule in the file
is `\b` [V]. Measured with `check_grammar.scopes_of`: `print` and `len` get
`entity.name.function`, `Map` and `Option` get `entity.name.type`, and `mutable_array_new<int>()`
gets **no scope at all** [V]; `Channel`, `Map`, `Result` and `Set` — reserved words in the lexer —
are indistinguishable from a user struct [V]. Neither gate can see it: `vscode-grammar-check`
diffs the generator against itself, and `grammar-check`'s 27 cases include no
`support.type.builtin` or `support.function.builtin` case [V]. This is the same shape as the
failure the generator was written to fix — a list that asserts nothing — one level down.
**Should the generator use `json.dumps` for the pattern instead of textual insertion, and should
`check_grammar.py`'s table gain one case per generated rule so that "the list is current" and "the
list fires" are both checked?**

**3. Should the three colourers share one partition implementation rather than one keyword list?
[V]** The keyword lists are gated three ways and agree [V]. The partition *edges* are not gated and
disagree twice: `#![experimental]` is an attribute in the gallery and in VS Code but **plain** on
ziman.dev, because `gen_highlight.py:108` tests `startswith("#[")` [V] — the same F75 defect the
TextMate grammar fixed by relaxing to `#!?\[`; and `0..3` is **one number span** in the gallery but
two on the site, because `highlight.zm:141` consumes any `.` after a digit while
`gen_highlight.py:115-121` deliberately stops [V]. Each surface has the other's bug, and `for i in
0..n` is the commonest loop in the language. **Should the three be held to a shared corpus of edge
cases — a list of (source, expected spans) that all three must agree on — rather than only to a
shared keyword list?**

**4. Is the semantic-token layer's `mutable` modifier meant to be wrong after a shadowing block?
[V]** On a `val v` shadowed by a `var v` inside an `if`, the server marks the use **after** the
block `mutable`, and marks the shadowing declaration itself `readonly` with no `declaration`
modifier [V] — verified against a run of the program, which prints `2` then `1` [V]. So the one
fact this layer exists to publish, by its own comment ("the use site is where a one-character
`val`/`var` typo does its damage"), is reported backwards on both sides of the boundary. Separately,
`documentHighlight` on **either** `v` returns only the outer declaration and no use at all [V], so
the two features also disagree with each other. And the token path emits `variable readonly` for a
field label (`Point { x: 2 }`) and a field access (`p.x`) [V], both of which
`handle_document_highlight` already excludes 100 lines above (`src/lsp.c:1727-1729`) [V].
**Is `find_local` block-scoped, and if not, is the right fix to make it so, or to have
`handle_semantic_tokens` reuse `documentHighlight`'s resolution rather than its own?**

**5. Should the gallery take ziman.dev's palette *values*, or its palette *roles*? [V]**
`highlight.zm` copies the site's three light-theme hexes onto `controls.default_style()`, whose
surfaces are light. The site's code block is always dark, so those values were tuned against
`#15201b`. On the gallery's actual surfaces the result is: comments and attributes at **2.20:1**
on a control face, **2.53:1** on the panel, **2.75:1** on white; keywords at **3.37:1** / 3.86:1 /
4.20:1 [V] — all below WCAG AA 4.5:1, and the comment case below the 3:1 large-text floor as well.
The partition was right to copy; the hexes travelled from a dark background to a light one
unchanged. A related, separate defect: **the site itself fails in light theme**, because `pre`
keeps `--code-bg: #15201b` in both themes while `--accent`/`--structure` swap, putting a literal
at **2.66:1** and a keyword at **3.98:1** [V]. **Should `highlight.zm` derive its three colours
from `controls.Style` (so a dark theme and a light theme each get legible ones), and should
`index.html` pin the two code hues to their dark-theme values since the code block is never
light?**

**6. Should `highlight.zm`'s dim colour distinguish an attribute from a comment? [V]** On the site
a comment is `--code-dim` **italic** and an attribute is `--code-dim` upright
(`index.html:128-129`) [V]; in the gallery both are `dim_ink()` [V], so two categories collapse
into one. The file's header names the keyword's lost **bold** as "the one deviation" [V], but the
site's keyword is already `--accent` *and* bold (`index.html:127`) [V] — so dropping the weight
loses nothing a theme can see, while dropping the comment's italic loses a real distinction the
header does not mention. **Is the collapse acceptable, and if `Ink` is to grow, should it carry an
emphasis the `Style` resolves against loaded faces (`Regular`/`Strong`/`Quiet`) rather than a raw
face index a caller could name without the window having loaded it?** I would argue for the enum
[O]: `Style` already owns `text_font`/`icon_font` so that no control picks a face
(`lib/ui/controls.zm:3930-4046`) [V], and a raw index reintroduces exactly the "this layer cannot
know what is in the font bytes" problem `Style.collapsed_mark` exists to avoid [V].

**7. Should the output-limit rationale name a file that still exists? [V]** `src/format.c:225-232` and
the R17 journal entry both justify the 16 MB cap as "about 180x the largest file here
(`lib/http/server.zm`, 89 KB, which formats to exactly its own size)" [V]. That file is now 276
lines; the largest `.zm` is `lib/db/memdb.zm` at 185,735 bytes, which formats to exactly its own
size, making the cap **90.3x** [V]. The limit is still the right order of magnitude and I am not
suggesting it change. **Should the figure be re-derived — or stated as a ratio to a measured
maximum rather than to a named file, so that it cannot go stale again?**

**8. Is anything meant to notice when `--fmt` and a documentation example disagree? [V]** 33 of the
38 non-`ignore` `ziman` blocks under `docs/syntax/` would be reformatted by `--fmt --check` [V];
`check_syntax_examples.py:31-34` records the decision not to run a blanket check, with the
measurement ("40 of the 46 blocks") and the reason (hand-aligned comment columns, not errors) [V].
I agree with the decision [O] — the docs' aligned columns are a readability device and the
formatter's run-based alignment is a different, equally defensible one. But the measurement has
drifted with the docs split (46 → 38 blocks) and the number is quoted in a source comment as a
current fact. **Should that comment's figure be derived by the script itself, so the recorded
measurement cannot be the stale half of a decision?**

---

# Earlier reviews — what is fixed, what still holds

Read after forming the view above, and re-checked by re-running the reproductions rather than
trusting the text.

| Source | Finding | Status |
|---|---|---|
| `code-style-and-colouring-2026-10-11.md` F1 | continuation lines zigzag `+1, +0, +1, +0` | **Fixed.** Every line of a run now takes one level; cause and fix recorded at `src/format.c:327-339`. Re-ran a four-term `+` chain and a three-piece `\` literal: uniform [V]. |
| same, F2 | should an array literal wrap vertically like a call? | **Decided, not changed.** The asymmetry stands and the reason is now in the journal (`part-06.md:451-465`): homogeneous elements versus heterogeneous arguments, confirmed by the maintainer 2026-10-12 [V]. Re-measured both shapes [V]. I agree with the decision [O]. One loose end the recorded reason does not cover: a **set** literal is greedy too [V] (E34), and it is greedy not because its elements are homogeneous but because `is_literal_brace` finds no `:` in it [V] — the mechanism and the stated reason happen to agree here, which is worth knowing before either is relied on. |
| same, F3 | should `SYNTAX.md`'s examples be `--fmt` clean? | **Decided, not changed.** `check_syntax_examples.py` checks `use` order only, deliberately, with the measurement recorded [V]. My re-measurement: 33 of 38 blocks [V]. See Finding 8. |
| same, F4 | `internal` and `#![…]` have no scope | **Fixed.** `internal` is in `storage.modifier`; `attributes` is a `begin`/`end` pair matching `#!?\[` and including `#strings` [V]. Both verified with `scopes_of` [V]. |
| same, F5 | `val`/`var` share one scope | **Fixed, both halves.** `storage.modifier.immutable.ziman` / `…mutable.ziman` at layer 1 [V], and the non-standard `mutable` semantic-token modifier at layer 3 [V]. |
| same, F6 | `documentHighlightProvider` is a word match | **Mostly fixed; a new shape survives.** It now resolves locals through `find_local` and excludes `p.x` and `name:` labels; the review's seven-hit repro returns two [V]. But on a shadowing block it returns the outer binding for both declarations and no uses at all [V] — Finding 4. |
| same, F7 | three behaviours live only in source comments | **Fixed.** `normalize_vertical`, the `<`/`>` house rule and the trailing-comment rule are all written up in `docs/design/journal/part-06.md:434-481` [V]. The attribute-after-doc-comment convention is the one that did **not** move and still lives only in `gen_lib_manual.py` [V] — Finding 1. |
| same, F8 | `--canonical` is a no-op and not in `make check` | **Fixed.** `make fmt-check` runs both passes over all 347 files [V], and `make/ziman.mk:46-50` records the consequence: the repo now *requires* K&R braces [V]. |
| `developer-experience-code-review-2026-10-05.md` (`:836`, item 25) | "`--fmt --check` in CI", backend not gated | **Fixed for the library, still ungated for the backend.** `fmt-check-backend` exists and skips cleanly when the sibling checkout is absent (`make/ziman.mk:65-72`), but it is not in `check-fast` [V]. I ran it by hand: 343 files, both passes exit 0 [V]. |
| `c-implementation-review-2026-10-08.md` (A18/A27) | `--fmt` and the LSP buffer a quadratic result | **Fixed, and I re-derived both limits from scratch** [V]: 202 brackets refused with a per-kind count, 240 KB at depth 190 refused at 61 MB peak, both printing the input back unchanged at exit 2 [V]. |
| `language-design-review.md:213` | "the formatter output is canonical" | **Still wrong as written, but now harmless here.** Plain `--fmt` output is idempotent, not canonical — two brace styles both pass [V]. What changed is that `make fmt-check` now also runs `--canonical --check`, so *this repository's* corpus is canonical even though the tool's default is not [V]. |
| `pl-expert-review-2026-09-23.md:389` | `val`/`var` deserve a visual distinction | **Fixed.** This was the oldest open formatting-adjacent finding; both halves shipped [V]. |
| `pl-expert-review-2026-09-23.md:369` | `editors/vscode/` is not published | **Still holds** [V] — `package.json` names a publisher and a version but there is no marketplace entry in the tree. I would add that `showcase.zm` is now compiled *and* `--fmt`-checked by `make showcase-check` [V], which is more than it had, and that it still reaches none of Gap B. |
| R17's own "not done" list | joining lines that would fit; wrapping at binary operators or `&&`/`\|\|`; wrapping long `use` or `if` conditions; an LSP format-document | **One fixed, four still hold.** The LSP advertises `documentFormattingProvider` [V]. I re-verified that a long `&&` condition and a long `use` line are both left long [V], and that no line is ever joined [V]. |

---

# Appendix — the experiments

All inputs are under `/tmp/ziman_style/`; nothing was written inside either repository except this
report. `Z=/home/mfg/workspace/vasna.tech/repos/ziman/ziman`.

| # | Input | Command | What it established |
|---|---|---|---|
| **E01** | `e01_indent.zm` — nested `if`/`for`/array, flush left | `$Z --fmt` | 4 spaces per open bracket; no extra level for a one-line body. §1 |
| **E02** | `e02_spacing.zm` — `fn f(a:int,b:int)->int{...}`, `m [ 0 ]`, `f ( 1 , 2 )` | `$Z --fmt` | Binary/unary/`:`/`,` spacing; call and index tight. §10 |
| **E03** | `e03_angles.zm` — `Map < string , int >`, `1 <2`, `1< 2` | `$Z --fmt` | `<`/`>` keep the source's spacing; asymmetric spacing survives verbatim. §10 |
| **E04** | `e04_use.zm` — 7 unsorted `use` lines, blank line, 2 more | `$Z --fmt` | Text sort per run; `use http` < `use http.Request`; alias sorts by whole line; blank line splits the run. §6 |
| **E05** | `e05_comments.zm` — ragged trailing comments, blank line, over-indented standalone | `$Z --fmt` | Run alignment at longest+2; blank line starts a new run; lone comment → 2 spaces; standalone re-indented. §8 |
| **E06** | `e06_blank.zm` — blanks inside blocks, 3 in a row, blanks in a struct body | `$Z --fmt` | At most one blank; none just inside a block; none ever inserted. §9 |
| **E07** | `e07_attrs.zm` — `#![…]`, doc/attr both orders, flush-left field attribute | `$Z --fmt` | Attributes indented to their declaration; order preserved either way. §7 |
| **E08** | `e08_api.zm` — doc above vs. below `#[deprecated]` | `$Z --api` | Doc **above** the attribute is lost; doc **below** is printed. Finding 1 |
| — | `lib/core/thread.zm`, `lib/core/math.zm` | `$Z --api thread`, `--api math`, `grep docs/lib/*.md` | The manual has the description, `--api` does not; 45 such declarations in `lib/`. Finding 1 |
| **E09** | `e09_wrap_sig.zm` — 150-column signature | `--fmt`, `--width=60`, `--width=0` | Vertical parameters; `--width=0` suppresses only the width wrap. §2 |
| **E10** | `e10_wrap_call.zm` — long call, long `&&` condition, long array | `$Z --fmt` | Call vertical; condition left long (no legal break); array greedy. §2 |
| **E11** | `e11_literals.zm` — struct/map/set/array literals and `with` | `--fmt`, `--width=60` | Struct and map vertical; set and array greedy; `with` formats as a struct literal. §5 |
| **E12** | `e12_vertical.zm` — half-broken call, struct literal, array, set, parameter list | `$Z --fmt --width=0` | `normalize_vertical` fires at width 0; array and set untouched. §2 |
| **E13** | `e13_vertical_comment.zm` — a comment inside an argument list | `$Z --fmt --width=0` | A list holding a comment is left as written. §2 |
| **E14** | `e14_lambda.zm` — calls broken only inside a lambda body | `$Z --fmt --width=0` | Not verticalised. §2 |
| **E15** | `e15_cont.zm` — 4-term `+` chain, `&&`, 3-piece `\` literal, split `->` | `$Z --fmt` | Every continuation line gets one level (no zigzag). §1 |
| **E16** | `e16_stmts.zm` — `switch`, `if Some(x) =`, `while`, `defer`, lambdas, `try`, two-binder `for` | `$Z --fmt` | Statement layout; `else` on its own line survives plain `--fmt`. §4 |
| **E17** | `e17_canon.zm` — Allman braces, `else` alone, brace after a trailing comment | `--fmt`, `--fmt --canonical` | Both canonical rules, and the trailing-comment exception. §11 |
| **E20** | `e20_samelineopen.zm` — `f(g(h(` and a lambda on one line | `$Z --fmt --width=0` | A bracket opened on the same line as another adds no level. §1 |
| **E21** | `e21_decls.zm` — every declaration kind, generics, variadic, defaults, `{}` | `$Z --fmt` | Declaration spacing; declaration bodies never verticalised; `{}` and `{`/`}` both preserved. §3 |
| **E18** | `e18_deep200.zm` / `e18_deep202.zm` — 199 and 201 nested `(` on separate lines | `$Z --fmt` | 200 formats (exit 0); 202 refused (exit 2) with a per-kind count; input printed back. §12 |
| **E19** | `e19b_big.zm` — 240 KB, 60,000 lines at depth 190 | `/usr/bin/time $Z --fmt --width=0` | 16 MB cap fires; 61 MB peak; input printed back unchanged. §12 |
| **E22** | `e22_lint.zm` — a `var` never reassigned, `1 <2` | `$Z --lint` | Lint reports the `var`; it has nothing to say about `<` spacing. §10 |
| **E23–E25** | `e23_lsp.zm`, `e24_lsp2.zm`, `e25_lsp3.zm` + `lsp_probe.py` (52 lines, driving `--lsp` over stdio) | `initialize`, `didOpen`, `documentHighlight`, `semanticTokens/full` | Capabilities include semantic tokens; `documentHighlight` resolves locals; the shadowing defect and the field-label/field-access mis-attributions. Layer 3, Finding 4 |
| — | `e25_lsp3.zm` | `$Z e25_lsp3.zm` → `2`, `1` | Proves line 7's `v` is the outer `val`, so `mutable` there is wrong. Finding 4 |
| **E26** | `e26_hl.zm` — `#![experimental]`, `#[derive(Json)]`, `0..3`, `1.5` | `python3 tools/gen_highlight.py`, and `ZIMAN_MODULE_PATH=gallery/ui/desktop $Z hl_probe.zm` | The two Layer-1′ divergences. Finding 3 |
| **E27** | `e27_misc.zm` — text block, `as`, `::`, `..`, unary, a 139-column trailing comment | `$Z --fmt` | Text block byte-for-byte; no comment reflow. §5, §8 |
| **E28** | `e28_greedy.zm` — 13-element array | `$Z --fmt --width=60` | Greedy fill, continuation one level. §2 |
| **E29** | `e29_nested.zm` — calls inside a call | `$Z --fmt --width=60` | Breaks at the outermost depth only. §2 |
| **E30** | `e30_blank2.zm` — blanks just inside `(` and before `)` | `$Z --fmt --width=0` | Both removed. §9 |
| **E31** | `e31_usecomment.zm` — a standalone and a trailing comment inside a `use` run | `$Z --fmt` | Standalone splits the run; trailing does not. §6 |
| **E32** | `e32_attrwrap.zm` — a 130-column `#[when_unmatched(…)]` | `$Z --fmt` | An attribute line is never wrapped. §7 |
| **E33** | `e33_generic.zm` — long generic type beside a long call | `$Z --fmt --width=60` | A generic list is never broken; the call beside it is. §2 |
| **E34** | `e34_setwrap.zm` — a 10-element `Set<string>` literal | `--fmt`, `--width=60` | A set literal is greedy, not vertical. §2, §5 |
| — | `chk.zm`, `w.zm`, `bad.zm`, `nottc.zm` | `--fmt --check`, `--fmt --write`, `--fmt -` | `--check` names the file and exits 1; `--write` rewrites in place; stdin works; an unterminated string is the one lexical refusal (exit 2); a type-incorrect program formats fine (exit 0). §0 |
| — | grammar | `python3 tools/check_grammar.py`; `scopes_of` driven directly on 30 lines | 35 rules, 27 cases; every scope quoted in Part 2. Gap A, Gap B |
| — | grammar | `re.search` on the two generated `match` strings, as written and with `\\b`→`\b` | `False` → `True`: the escaping alone is the defect. Finding 2 |
| — | lexer vs. grammar | `src/lexer.c` KEYWORDS table (37) vs. every grammar keyword/type/boolean alternation | `Channel`, `Map`, `Result`, `Set` reachable only through the dead rule; `internal` and `unit` correctly covered as soft keywords. Gap A |
| — | palettes | a 12-line WCAG relative-luminance script over `index.html`'s tokens and `default_style()`'s surfaces | Every contrast figure in Part 2. Finding 5 |
| — | corpora | `awk '{print length}'` over `lib/**/*.zm`; `grep -n '.\{121,\}'`; `ls -S`; `--fmt \| wc -c` | p50 31, p90 96, p99 105, max 229 columns; 175 lines over 120, 54 of them code; largest file `lib/db/memdb.zm` 185,735 bytes, formats to its own size. §2, §12, Finding 7 |
| — | docs | extract every non-`ignore` ```ziman block and `--fmt --check` each | 33 of 38 would be reformatted. Finding 8 |

---

# What I did not check

Stated plainly, because a report that names its gaps is worth more than one implying coverage it
does not have.

- **I did not run a real TextMate engine.** `vscode-textmate` and `vscode-oniguruma` are not
  installed and `editors/vscode/node_modules/` holds only the language-client packages [V]. Every
  scope in Part 2 comes from `tools/check_grammar.py:scopes_of` — the repository's own simulation of
  leftmost-match-wins over the rule list, which its header is explicit about being a check on the
  rules and not a renderer [V]. Gap A is a regex fact (a pattern that cannot match a string cannot
  match it in any engine) and I regard it as certain; the *fallback* scopes I quote are
  simulator output and could differ in a renderer's handling of multi-line state.
- **I did not open VS Code**, so I have not seen any of this rendered, and I have not checked how a
  popular theme actually styles the 35 scopes.
- **I did not open the gallery's window.** `make gallery-check` type-checks it, lints it, runs
  `highlight_test.zm` and runs the keyword gate, but cannot run the application — a window needs
  `make gui`, a separate binary, and a headless gate has no display
  (`make/ziman.mk:51-61`) [V]. I drove `highlight.inks_of` directly from a probe program instead
  [V]. The contrast figures for the Code pane are computed from
  `controls.default_style()`'s declared colours and the `text_area_of` path, not measured off
  pixels.
- **I did not read all of `src/lsp.c`** — only `initialize`, `handle_semantic_tokens`,
  `handle_document_highlight` and their helpers. In particular I did not establish *why*
  `find_local` resolves a shadowing block the way it does; Finding 4 reports the behaviour, not the
  cause.
- **I did not measure the formatter's cost on the real corpora**, only at the two limits.
- **I did not verify the 45-declaration count by hand** — it comes from a script that looks for a
  `//` line immediately above a `#[` line immediately above a declaration, so it may include a
  declaration whose comment was a section banner.
- **I did not check `editors/vscode/snippets/ziman.json`** (109 lines) against the current library,
  though the `showcase.zm` history suggests that is exactly the kind of list that goes stale.
- **I did not re-run the LSP's 4 MB document limit**, taking it from
  `docs/syntax/semantic-rules.md` and `src/lsp.c:342-345`.
- **I did not read the whole 59,300-line sample backend**; I ran both `--fmt` passes over it and
  read nothing else.
