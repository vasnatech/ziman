# Ziman code style and colouring architecture

**Date:** 2026-10-11 · **Commit:** `f71e4d9`, working tree clean at the start.
**Binary:** rebuilt from clean at that commit — `./ziman --version` → `ziman f71e4d9`. [V]
**`make fmt-check`:** clean, exit 0, over the 99 `.zm` files it covers. [V]
**`make test`:** `956 passed, 0 failed`, exit 0, 24.4 s. [V]

> The checked-in binary first reported `ziman 18c402e-dirty`, three commits behind `HEAD`, and
> `make -j8 ziman` said "up to date". `git diff --stat 18c402e..HEAD -- src Makefile` is empty
> and `ziman`'s mtime matched `lib/http/server.zm`'s to the second, so only the embedded version
> string was stale — but I rebuilt from clean and re-ran every result below against that binary,
> because a report whose oracle might be a different build than its commit is worth nothing. [V]

The oracle agrees with the committed tree, so what follows is trustworthy on that basis. Two
corpora count as evidence throughout: `lib/**/*.zm` (40 files, 18,876 lines) and the reference
application at `…/health-record/health-record-backend-ziman` (173 files, 21,177 lines, clean at
`1c85089`). Both are `--fmt --check` clean, and — unexpectedly — both are
`--fmt --canonical --check` clean too. [V]

**Tagging.** Every statement is **[V]** (I ran it, or read the cited `file:line`) or **[O]**
(opinion). Part 1 describes only what the formatter *does*; where it is silent I say so and
label the recommendation **[O]**; everything I think should *change* is in Findings and nowhere
else.

**One correction to the task framing:** the width flag is `--width=N`, not `--fmt-width N`
(`./ziman --help`, parsed at `src/main.c:577`); `--fmt-width` is not accepted. [V]

## Executive summary

1. `--fmt` is a **text** formatter: it decides spacing, indentation, `use` order, comment
   alignment and where to *break* a long line, and never *moves* a break you wrote. [V]
2. That one property explains nearly every surprise below, including the real bug: three or more
   operator-continued lines get a **zigzag** indent, idempotently, so `fmt-check` enforces it. [V]
3. Allman and K&R braces **both** pass `--fmt --check`; `--canonical` settles it and is already a
   no-op on all 40,053 lines of library and application code. [V]
4. Parameter lists, call arguments and struct/map literals go **fully vertical** when they
   overflow; array literals **fill greedily**. The asymmetry is in the code, not the docs. [V]
5. Both documented limits fire as documented (200 brackets, 16 MB); the 2026-10-08 C review's
   144 MB LSP reply is fixed — same input now gives an 814-byte error at 28 MB RSS. [V]
6. `docs/SYNTAX.md:840`'s own `struct RectDims { width: float  height: float }` is **not**
   `--fmt` clean (two spaces collapse to one), and nothing checks it. [V]
7. The TextMate grammar names **30 distinct scopes over 32 assignments** and is better than most
   first grammars, because enforced casing makes its boldest rules sound, not heuristic. [V]
8. Its gaps are specific: `internal` has **no scope at all**, `#![…]` is not matched by the
   attribute rule, an exponent literal gets **no numeric scope**, and `use a/b`'s separator
   colours as **arithmetic division**. [V]
9. `val` and `var` share one scope, so no theme can distinguish them — the exact thing
   `DESIGN.md:204` asks a future tool to do. [O] The highest-value colouring change available.
10. There are **no semantic tokens** (`semanticTokens/full` → `-32601`), and
    `documentHighlightProvider` is a word match — 7 hits for an `x` with 2 real uses — so it is
    not a foundation to build them on. [V]

---

# Part 1 — The formatting reference

## 1. Indentation

**Four spaces per open bracket, and nothing else.** A line's level is
`stack[depth-1].opener_level + 1`; a line *starting* with a closer is pulled back to its
opener's level (`src/format.c:307-310`). Input tabs are discarded (`:39`). [V]

```
$ cat e01_indent.zm      $ ziman --fmt e01_indent.zm
fn main() {              fn main() {
val a = 1                    val a = 1
      if a > 0 {             if a > 0 {
val b = [1, 2, 3]                val b = [1, 2, 3]
  for i in 0..3 {                for i in 0..3 {
print(b[i])                          print(b[i])
}                                }
    }                        }
}                        }
```

Increased by `(`, `[`, `{`. **Not** increased by:

- **A second bracket opened on the same line.** `f(x, fn() {` … `})` indents once, capped at
  `src/format.c:342-345`. Verified (`e25_lambda.zm`, which runs → `5`): [V]
  ```
      val ys = collections.map<int, int>(xs, fn(n: int) -> int {
          return n * 2
      })
  ```
- **`<` and `>`**, which are not counted at all, not even by the nesting limit. [V]
- **A `{` opening on a continuation line** — the block indents from the *statement's* level:
  ```
  $ ziman --fmt e36_misc.zm     # input: `fn long_return(a: int) ->` / `int {`
  fn long_return(a: int) ->
      int {
      return a
  }
  ```
  `ziman --check e36_misc.zm` exits 0, so this is valid Ziman, not a malformed probe. [V]

Increased by one, once: a line whose last code token is a binary operator or `->`
(`src/format.c:364`) — see the zigzag in §2.

## 2. Line length and wrapping

**Default target: 120 columns of *code*; comments do not count.** `--width=N` changes it,
`--width=0` disables wrapping (`./ziman --help`; the parameter reaches `format_source` at
`src/format.c:690`). The boundary is exact — I
generated `    val x = f(1, 22…2)` at code widths 119/120/121 and fed each through `--fmt -`:
119 no, 120 no, **121 wraps**. [V]

Breaks happen **only after a comma**, and only one the parser already accepts a newline behind:
inside a call, an array literal or a struct/map literal. Commas inside a generic list, inside an
`fn(A, B) -> R` type, and inside `#[…]` are skipped (`src/format.c:401` for the comma test,
`:396` for the attribute guard). [V]

| Construct | What `--fmt` does | Verified by |
|---|---|---|
| long function signature | **vertical**: one parameter per line, both parens on their own lines, `) -> R {` together | `e04_wrap_sig.zm` |
| long call | **vertical**; inner calls that fit stay on one line | `e06_wrap_call2.zm` |
| long struct or map literal | **vertical**, one field per line | `e34_lit.zm` (runs) |
| long array literal | **greedy fill**, brackets hug the items | `e05`, `e07_arr.zm` |
| long type | left long — its commas are inside `<>` | `e11` (121 cols) |
| long condition | left long — `&&` is not a break point | `e10` (131 cols, runs) |
| long `use` line | left long — no legal break | `e38` (104 cols) |
| long `#[…]` line | left long, at any width | `e35` (121 cols) |

```
$ ziman --fmt e04_wrap_sig.zm          $ ziman --fmt e05_wrap_call.zm
fn process_patient_record(                 val arr = [100000, 200000, …, 1200000,
    identifier: string,                        1300000]
    given_name: string,
    family_name: string,               $ ziman --fmt --width=30 e07_arr.zm
    insurance_number: string,              val arr = [10, 20, 30, 40,
    notes: Option<string>                      50, 60, 70, 80, 90,
) -> Result<int, string> {                     100, 110, 120]
    return 0
}
```
With `--width=0` the signature returns as one 170-column line. [V]

The mechanism behind the asymmetry is `vertical` in `Open` (`src/format.c:239`): set for a
`name(` call paren, a generic call's paren and a literal `{`, **never** for `[`. [V] I think
that is wrong — Finding 2.

**So 120 is a target, not a guarantee.** In the 40,053 formatted lines of library plus
application, **112 code lines exceed 120 columns**, the longest 451 characters — almost all one
long string literal or line of embedded SQL. [V] A guide claiming 120 as a hard limit would be
contradicted by its own corpus. **`--width` changes nothing else:** indentation, spacing, `use`
order, comment alignment, blank-line collapsing and brace placement are width-independent, and
re-running every §3-§10 experiment at `--width=0` changed only the wrapping. [V]

### Already-wrapped lists get normalised

The behaviour most likely to surprise a reader of `DESIGN.md`, which describes only the
width-triggered break. Break a call or literal **between its arguments at all** and
`normalize_vertical` (`src/format.c:442-467`) finishes the job, regardless of width:

```
$ cat e26_vert.zm         $ ziman --fmt e26_vert.zm
    val a = f(1, 2,           val a = f(
        3)                        1,
                                  2,
    val b = f(1, // why           3
        2, 3)                 )
                              val b = f(1,  // why      <- a comment in the list
    val d = [1, 2,                2, 3)                    leaves it as written
        3]                    val d = [1, 2,            <- an array is never
                                  3]                       normalised
```
A call whose only break is inside a nested bracket is also exempt — that is why §1's lambda
keeps its arguments on the header line. [V]

### The continuation-indent zigzag [V]

One continuation line indents as you would want. Three or more alternate:

```
$ cat e03_cont4.zm      $ ziman --fmt e03_cont4.zm
    val sum = 1 +           val sum = 1 +
2 +                             2 +
3 +                         3 +
4 +                             4 +
5                           5
```
`ziman e03_cont4.zm` prints `15`, and re-formatting the output changes nothing, so the zigzag is
a fixed point that `--fmt --check` would *require*. The cause is `cont_from_prev`
(`src/format.c:311-312`), set on the first continuation line and cleared on the next. The
library escapes it with exactly three operator-continued lines
(`lib/http/client.zm:1054`, `lib/db/mysql.zm:442`, `:645`), each with one continuation. [V]
Finding 1.

**Where the formatter is silent [O]:** until it is fixed, keep an operator-continued expression
to one continuation line, as all three library sites do. If you need more, break inside a call's
parentheses, where the bracket rule indents correctly.

## 3. Declarations

One run (`e12_decls.zm`, which type-checks and runs) covers every form:

| Written | Formatted |
|---|---|
| `struct` body | one field per line, four spaces, **no commas** |
| `struct Inline { a: int  b: string }` | `struct Inline { a: int b: string }` — **two spaces collapse to one** |
| `enum` body / `enum Flat { A B(int) C }` | one variant per line / unchanged |
| `pub val max_retries = 3`, `val greeting: string = "hi"` | unchanged; `:` tight left, spaced right |
| `native fn read_raw(path: string) -> Result<string, string>` | unchanged (no body, no brace) |
| `pub native fn …`, `internal fn …` | unchanged; modifier order preserved |
| `pub fn generic_pick<T: Eq + Hash, U>(a: T, b: U, rest: int...) -> Option<U> { … }` | unchanged — bounds, `+`, variadic `...`, one-line body all survive |
| `fn opt(…, priority: Option<int> = 3) -> string` | unchanged; `= 3` spaces as an operator |

[V] The variadic `...` survives a special case: `need_space` detects a `.` whose predecessor is
`..` and suppresses the member-access rule (`src/format.c:166`). [V] Visibility markers —
`pub`, `internal`, neither — are passed through verbatim and never moved, added or removed. [V]

**Where the formatter is silent [O].** It never inserts a blank line between declarations and
never expands a one-line body. The corpus is near-unanimous, so follow it:

| | `lib` | backend |
|---|---|---|
| function with a braced multi-line body | 1001 | 705 |
| function with a one-line body | 4 | 0 |
| `struct`/`enum` multi-line / **on one line** | 131 / **0** | 128 / **0** |
| function preceded by a blank line | 283 | 490 (all) |

[V] So: **blank line before every declaration, multi-line body for every function, never a
one-line `struct` or `enum`** — not because `--fmt` says so, but because 40,000 lines of Ziman
do, and because the one-line struct form is exactly where the two-space collapse bites. [O]

## 4. Statements and expressions

One run (`e15_stmts.zm`). The formatter re-indents and re-spaces; it adds no structure:

```
$ ziman --fmt e15_stmts.zm
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
    if Some(x) = o { print(x) } else { print(0) }
    if Some(x) = o && x > 2 { print(x) }
    val label = if 1 > 0 { "pos" } else if 1 < 0 { "neg" } else { "zero" }
    val st = switch Shape.Empty {
        Circle(r) { 1 }
        Square(w) { 2 }
        Empty { 3 }
    }
    defer print("bye")
    spawn worker(1)
    val f = fn(n: int) -> int { return n * 2 }
    val g = fn(n) { n * 2 }
}
```
[V] Read as rules: **`if`/`else`** — `} else {` stays joined or split exactly as written; plain
`--fmt` has no opinion, `--canonical` does (§11). **`switch`** — one level for arms, one more for
a braced body; a value-`switch` indents identically to a statement one. **`for`/`while`** —
header, `{`, body; `0 .. 3` tightens to `0..3`. **`defer`/`spawn`/`try`** — no layout at all; a
keyword and a call, or a prefix keyword and one space. **Lambdas** — left as written, and as a
call argument the body indents one level from the statement (§1). **Binding forms** — preserved
verbatim, because the parser desugars them *after* the formatter has run on tokens, exactly as
`SYNTAX.md:742` says. [V]

**There are no methods, so there is no method-chain layout question.** Ziman has free functions
only (`SYNTAX.md:521-527`) and a call may chain only off a plain or single-qualified identifier
(`:148-151`). The `.map().filter().fold()` layout problem that dominates Prettier's and
rustfmt's rulebooks does not arise, and a Ziman guide should say so rather than borrow a rule it
has no use for. [V]

## 5. Literals and collections

From `e34_lit.zm` (runs → `10`/`Ada`) and `e23_ops.zm` (runs): [V]

- **Arrays** — `[1, 2, 3]`, tight brackets, one space after each comma; nested `[[1, 2], [3, 4]]`;
  indexing tight (`arr[ 1 ]` → `arr[1]`).
- **Maps and sets** — `Map<string, int> { "a": 1 }`: one space between `>` and `{`, braces spaced
  inside, `:` tight left and spaced right. `Set<int> {}` — an empty literal keeps nothing inside.
- **Struct literals** — `Patient { identifier: "a", given_name: "b" }`. The formatter tells a
  literal brace from a block brace by scanning for a `:` at the group's own level before any `,`
  or the close (`is_literal_brace`, `src/format.c:248-263`).
- **`with` overrides** — `with base { given_name: "Ada" }`: prefix `with`, one space, the base,
  then a literal-braced list spaced as a struct literal. `with` is prefix, so no postfix question.
- **Long lists** — struct and map vertical, arrays greedy (§2).
- **Text blocks are byte-for-byte untouched**: `scan` consumes `"""…"""` as one item
  (`src/format.c:48-62`). This matters because the block's own indent-stripping rule
  (`SYNTAX.md:35-37`) makes that indentation semantically load-bearing.
- **No trailing comma is ever added** when a list goes vertical — one before the close is a
  syntax error (`SYNTAX.md:48`), unlike `gofmt` or Prettier. Confirmed in every vertical output.

## 6. `use` lines

**A run of consecutive `use` lines — no blank line, no comment line between — is sorted by
text** (`sort_use_runs`, `src/format.c:522-583`). Order has no meaning in Ziman, so it is safe
(`DESIGN.md:3903`).

```
$ ziman --fmt e16_use.zm          # input: time.localdate_to_string, json, http.Request,
use a/b/c                         # http, collections, db.SqlValue, entity/patient as
use collections                   # entity_patient, time, http.Response, a/b/c, zz as aa
use db.SqlValue
use entity/patient as entity_patient      $ ziman --fmt e17_use2.zm
use http                                  use http          # bare module   "use http "
use http.Request                          use http.Request  # symbol        '.' = 0x2e
use http.Response                         use http.zebra
use json                                  use http/client   # path child    '/' = 0x2f
use time                                  use http/server
use time.localdate_to_string              use http as h     # alias         'a' = 0x61
use zz as aa                              use httpx         # other module

use fs                            # a blank line freezes the groups
use os

// a comment breaks the run
use clock
use random
```
[V] The sort key is the line's code tokens joined with a trailing space each (`:550-559`), so
intra-module order falls out of ASCII on the separator. Read off that run: **bare module first,
then its symbol imports alphabetically, then its path children, then its aliased forms, then any
longer module name.** Module and symbol imports interleave *by module*, not in two groups —
`use http`, `use http.Request`, `use json`, `use time`, `use time.localdate_of`. An alias does
not change where a line sorts by target; it sorts after every unaliased form of the same prefix,
because `as` begins with a letter and the separators do not. [V]

A blank line or comment line **freezes** the groups either side of it — the only control you
have over the order. [V] And because `SYNTAX.md:946` chose one-name-per-import partly *because*
`--fmt` sorts the runs, the sorting is load-bearing for the import design, not a convenience. [V]

## 7. Attributes

**The formatter re-indents an attribute to its declaration's level and never moves it relative
to anything.** From `e18_attr.zm`, an input `   #[test]` came back at column 0 and every
comment/attribute/declaration ordering survived. [V] Spacing *inside* an attribute is the one
thing it decides, and it is fully normalised (`need_space` suppresses a space after `#` and
after `!`, `src/format.c:169`):

```
$ cat e39_attrspace.zm      $ ziman --fmt e39_attrspace.zm   # and `ziman e39_attrspace.zm` → 1
# ! [ experimental ]        #![experimental]
# [ derive ( Json ) ]       #[derive(Json)]
```
[V] `#![…]` attaches to the file and must precede the first declaration (`SYNTAX.md:1340`);
`#[…]` attaches to the declaration after it. The formatter treats both as ordinary tokens and
relocates neither, so that ordering rule is the parser's to enforce. [V] An attribute line is
never wrapped, at any width (121 columns came back untouched). [V]

**The real hazard.** The generated manual associates a doc comment with the declaration that
follows, and an attribute sits *between* them. `tools/gen_lib_manual.py:119-125` carries the
pending comment across an attribute line rather than clearing it, and its own comment records
the cost: *"Without this, every documented declaration carrying one loses its whole description
from the manual"*, naming `thread.zm`'s `#[thread_key]` and `server.zm`'s `#[deprecated]`. [V]

So the order that matters is **doc comment → attribute → declaration**, and the library is
unanimous: all 7 top-level attribute lines under `lib/` are immediately preceded by a `//` line,
e.g. `lib/http/server.zm:188-190`. [V] **But `--fmt` does not enforce it and cannot** — it
preserves either order, because it does not know which comment documents what. The guard is
`make lib-manual-check`, over `lib/` only. **Recommendation [O]: treat comment-then-attribute as
a rule, because the tool that would catch you is a documentation generator, not the formatter,
and it does not cover application code.**

## 8. Comments

Only `//` exists — no block comments in v0 (`SYNTAX.md:7`). [V]

```
$ ziman --fmt e19_comments.zm
    val a = 1             // first            <- a run, aligned to 2 spaces past
    val bbbbbbbbbbbb = 2  // second              the longest code line in the run
    val c = 3             // third

    val d = 4                            // a blank line starts a new run
    val eeeeeeeeeeeeeeeeeeeeeeeeeee = 5  // long code line
    val f = 6                            // short
    // a full-line comment, written at column 0, re-indented to the block
    val g = 7
    // This is a very long comment that goes well past the hundred and twenty column target width to see whether the formatter reflows it at all.
```
[V] (`align_trailing_comments`, `src/format.c:471-517`.) A run is broken by a blank line **or by
any line without a trailing comment**, including a full-line comment line — verified separately
because the two cases look alike until the code lengths differ: in `e22_runbreak2.zm` the
comments either side of a full-line comment each sit at their own two-space gap. [V]

**What it will not do: reflow.** The 139-column comment above is untouched; comments are not
counted toward the width at all; a comment is never split, joined, re-wrapped or moved. [V] One
consequence: a comment **pins a brace** — `--canonical` refuses to move a `{` that follows a
trailing comment, which would comment the brace out (`src/format.c:623-625`). [V]

## 9. Blank lines

From `e21_blank.zm`: [V]

| Situation | Formatter |
|---|---|
| two or more blanks in a row | **collapsed to one** |
| a blank immediately after `{` / immediately before `}` | **removed** |
| a single blank inside a block, or between declarations | **preserved** |
| no blank where you might want one | **nothing inserted, ever** |

The mechanics (`src/format.c:304`) emit a second newline only when the previous line did not
end with an opening bracket and the next token is not a closing one — so "none just inside a
block" is really "none just inside *any* bracket group", a vertical argument list included. [V]
**Where the formatter is silent [O]:** it will never add the blank line between declarations
that the whole corpus uses (§3). Add it by hand.

## 10. Operators and spacing

From `e23_ops.zm` and `e24_angle.zm`, both of which run: [V]

| Construct | Formatted | Construct | Formatted |
|---|---|---|---|
| binary `+ - * / %` | `1 + 2 * 3 - 4 / 5 % 6` | `:` annotation or field | `val e: int`, `"a": 1` |
| `== != < > <= >= && \|\|` | one space each side | `->` | `fn f(x: T) -> T` |
| `=`, and a default `= 3` | one space each side | `..` range | `0 .. 3` → `0..3` |
| unary `-` | `-a`, tight | `...` variadic | `nums: int...`, tight |
| unary `!` | `!(a == 1)`, tight | `as` cast | `a as float` |
| call paren | `f(1)`, no space before `(` | index | `arr[ 1 ]` → `arr[1]` |
| `{}` | `Set<int> {}`, stays empty | `{ … }` | `Foo { a: 1 }`, `{ return x }` |
| two operator chars together | `i++`, `=-` left alone, so no token pair becomes another | | |

### The `<` / `>` exception, which a style guide has to own

**`--fmt` keeps whatever spacing the source had around `<` and `>`, collapsed to at most one
space.** It cannot tell a generic bracket from a comparison without parsing and refuses to guess
(`src/format.c:189`, `DESIGN.md:3530-3537`):

```
$ ziman --fmt e24_angle.zm                          # the file runs: true/true/true/0
    print(a < b)                                    # written spaced -> spaced
    print(a<b)                                      # written tight  -> tight
    print(a  <  b)                                  # two spaces     -> one
    val c: Map < string, int > = Map < string, int > {}    # left alone
    val d: Map<string, int> = Map<string, int> {}          # left alone
```
All five lines pass `--fmt --check`. [V]

**Recommendation [O]: generics tight (`Map<string, int>`), comparisons spaced (`a < b`).** Not
taste — the corpus, checked: grepping `lib/**/*.zm` for a spaced generic returns exactly one
hit, inside a string literal (`lib/time/zone.zm:145`, the text `"unterminated <name>"`). [V]
Because it is unenforced, this is the one layout rule a reviewer rather than a tool must hold.

## 11. What `--fmt --canonical` changes

**Two things a lexer can decide alone: a `{` that starts a line joins the line before it with
one space, and an `else` alone on a line after the `}` that closed its `if` joins that line.
Nothing else moves, and without the flag nothing moves at all.** [V]

```
$ ziman --fmt e27_canon.zm   $ ziman --fmt --canonical e27_canon.zm
fn main()                    fn main() {
{                                val a = 1
    val a = 1                    if a > 0 {
    if a > 0                         print(a)
    {                            } else {
        print(a)                     print(0)
    }                            }
    else                         val p = Point {
    {                                x: 1
        print(0)                 }
    }                        }
    val p = Point
    {
        x: 1
    }
}
```
The Allman file is unchanged by plain `--fmt` and passes `--fmt --check` with exit 0; the same
file fails `--fmt --canonical --check` with exit 1. [V] The rule covers a `switch` arm's pattern
and a struct literal's brace as well as a construct's; the one brace left alone follows a
trailing comment.

**Why the split exists.** Because `--fmt` never moves a break, it is safe on any lexable text —
including a file the compiler rejects (`DESIGN.md:3534`). Confirmed directly: `e02_cont.zm`
contains a leading-`+` continuation that `--check` rejects at 9:9, and `--fmt` formatted it
anyway. [V] That safety is the point, and also exactly why plain `--fmt` cannot settle brace
placement the way `gofmt` does — so `--canonical` is the opt-in for the part that *is*
decidable. `DESIGN.md:3929-3937` records the discovery that made the rule simple rather than
complicated: Ziman has **no bare block statement**, so a line-leading `{` always belongs to what
precedes it and pulling it up can never change meaning.

**`--canonical` is currently a no-op on every line of Ziman in either repository** — exit 0 over
all 99 `fmt-check` files and all 173 backend files. [V] So its value today is not cleanup; it is
the guarantee that a contributor who writes Allman braces is corrected before review. [O]
Finding 8.

## 12. The limits

Both ceilings exist, fire where documented, and have arithmetic rather than aesthetic reasons.
Both are errors, not crashes, and both print the input back unchanged so filtering an editor
buffer never empties it. [V]

**Nesting: 200 brackets.** Every line is indented four spaces per open bracket, so formatting
input that is *only* nesting is **quadratic in its own size** — `DESIGN.md:3487-3492` records 32
KB of nested blocks producing 256 MB of output at 9.7 GB peak RSS. Hence a cheap early refusal.
`<`/`>` are not counted, so generics are free.

```
$ ziman --fmt e28_deep.zm      # 201 nested `if true {`   -> exit 2
e28_deep.zm: line 2: nesting is too deep to format (203 open: 1 '(', 0 '[', 202 '{';
  the limit is 200, and the indentation alone would be 812 spaces a line)
```
[V] The count is **every** open bracket at the deepest point, with a per-kind breakdown so the
number is actionable; 199 nested `if`s plus the `fn` brace plus the `print(` paren is 201 and is
already refused. [V] For calibration, the deepest real nesting across all 40,053 lines of
library and application code is **19** (`lib/http/client.zm:1295`) — about a tenth of the
budget. [V]

**Output: 16 MB.** The depth limit bounds only one factor; the cost is O(lines × depth), so a
document of ordinary depth and very many lines passes the depth check and still grows without
limit (`src/format.c:206-216`, the ceiling itself at `:216`, enforced in `put` at `:220-224`).
Checked as the output is appended, not afterwards.

| input (272 KB each) | result |
|---|---|
| depth 151, 20,000 lines | formats to 12,352,414 bytes, exit 0 |
| depth 151, 30,000 lines | **refused**, exit 2: *"would produce more than 16 MB of output — the indentation is four spaces per open bracket…"* |

[V] 16 MB is about 180× the largest file in the project (`lib/http/server.zm`, 89 KB, which
formats to exactly its own size).

Two further bounds: an unterminated string literal or text block is the only *lexical* error the
formatter reports (`src/format.c:57, 71`); and after formatting, the output is re-scanned and
must carry exactly the same code tokens and comments, or nothing is printed or written
(`:722-736`) — which is why the whole design is trustworthy on files nobody has type-checked. [V]

**Adjacent tools.** `--lint` holds the non-formatting style findings and exits 1 so it can gate;
on one small file it found all four of its rules — a redundant `as` alias, a quadratic `concat`
in a loop, a never-reassigned `var`, an unread `val`. [V] `ziman --fmt -` reads stdin and writes
stdout, and the language server answers `textDocument/formatting` with the identical text — I
sent a real request over stdio and got back exactly what `--fmt -` prints, so
`DESIGN.md:3909`'s "not done" on an LSP format-document is stale. [V]

---
# Part 2 — The colouring strategy

Not a palette: what the colours *mean*, and which distinctions are worth paying for. Three
layers, kept apart because they can do different things.

## Layer 1 — What the TextMate grammar already does

`editors/vscode/syntaxes/ziman.tmLanguage.json` assigns **32 scopes under 30 distinct names**
(`entity.name.function.ziman` and `constant.character.escape.ziman` each appear twice). [V] I
verified every claim below by reimplementing TextMate's *leftmost match, ties broken by pattern
order* rule in Python against the grammar file (`/tmp/ziman_style/tmscan.py`, 60 lines) and
running it over `editors/vscode/showcase.zm` plus four samples of my own that all run and are
all `--fmt` clean. Its one limitation is named in "What I did not check".

Six categories a theme author can reason about:

| Category | Scopes | What they match |
|---|---|---|
| **A. Lexical atoms** (9) | `comment.line.double-slash`, `string.quoted.double`, `string.quoted.triple`, `constant.character.escape` ×2, `constant.numeric.suffixed`/`.float`/`.integer`, `constant.language.boolean` | `//`; `"…"`; `"""…"""`; `\n \t \r \" \\`; `123n`/`19.99d`/`200b`; `1.5`; `42`; `true`/`false` |
| **B. Declaration keywords + the names they bind** (7) | `storage.type.function` + `entity.name.function`; `storage.type.struct` + `entity.name.type.struct`; `storage.type.enum` + `entity.name.type.enum`; `storage.type` | `fn`/`struct`/`enum` immediately followed by a correctly-cased name, captured separately; the bare fallback catches a lambda's `fn(` |
| **C. Type references** (3) | `support.type.primitive`, `support.type.builtin`, `entity.name.type` | the 8 scalars; the 9 built-in generics; any other `[A-Z]…` |
| **D. Control flow + modifiers** (4) | `keyword.control`, `keyword.control.import`, `keyword.other`, `storage.modifier` | `if else while for in break continue spawn return switch defer try`; `use as`; `with`; `pub native val var` |
| **E. Operators** (6) | `.arrow`, `.range`, `.comparison`, `.logical`, `.assignment`, `.arithmetic` | `->`; `..`; `== != <= >= < >`; `&& \|\| !`; `=`; `+ - * / %` |
| **F. Attributes** (1) | `meta.attribute` | `#\[[^\]]*\]` as one flat span — `Json` inside `#[derive(Json)]` is deliberately *not* a type name [V] |

**Justifications, briefly. [O]** (A) is the only group a regex gets *right* rather than
usually-right, and splitting the numerics three ways is earned: the `n`/`d`/`b` suffix is the
whole difference between `int` and `bigint`, `float` and `decimal`, and mixing them is a compile
error (`SYNTAX.md:199-258`) — a suffix you can see is a bug you do not file. (B) is the
grammar's best idea, because Ziman *enforces* casing (`SYNTAX.md:8-12`), so `struct` + `[A-Z]…`
is the language's own rule, not a heuristic, and a declaration site and a reference end up with
different scopes at no analytical cost. (C) draws the right axis — "the language gave you this"
versus "someone declared this". (D) makes two good calls: `spawn`/`defer`/`try` belong with
`if`/`return`, since each is a non-local start or exit and Ziman's error story is that those are
visible (`SYNTAX.md:652-698`); and `with` deserves its own scope, being a construction form
rather than control flow. (E) costs nothing if ignored and lets a theme lift `->` out of a
signature if it wants.

### The gaps — constructs with no scope, or the wrong one

Finding these was the work. Each row verified by running the harness over a file that compiles
and runs.

| # | Construct | `SYNTAX.md` | What the grammar gives it |
|---|---|---|---|
| G1 | **`internal`**, the third visibility level | `:963-974` | **nothing at all** — `storage.modifier` lists only `pub\|native\|val\|var` |
| G2 | **`#![…]`**, the file-level attribute | `:1338-1345` | **nothing** — `#\[` cannot match `#![`; the `!` becomes `keyword.operator.logical`, the name is bare |
| G3 | **An exponent literal** (`1e3`, `2.5E-2`) | `:12-19` | **no numeric scope** — `\b[0-9]+\b` finds no boundary before `e`, so `1` and `e3` are both bare |
| G4 | **A `use` path separator** (`use models/user`) | `:932` | `keyword.operator.arithmetic` — **coloured as division** |
| G5 | **A variadic marker** (`nums: int...`) | `:543-569` | `keyword.operator.range` for the `..` — coloured as a range |
| G6 | **An `as` cast** (`a as float`) | `:195` | `keyword.control.import` — coloured as import machinery |
| G7 | **A field name, parameter name or named-argument label** | `:516`, `:571` | nothing |
| G8 | **A module qualifier** (`fs` in `fs.read_text`) | `:932` | nothing — only the function after the dot is scoped |
| G9 | **An enum variant** (`Shape.Circle`, `Some`, `None`) | `:834-873` | `entity.name.type` — indistinguishable from a struct name |
| G10 | **`concat`, `join`** and other prelude builtins | `:2420+` | `entity.name.function` — the `support.function.builtin` list omits them |
| G11 | **A soft keyword as an identifier** (`struct Measurement { unit: string }`) | `:175`, `:972` | `unit` → `support.type.primitive` — **miscoloured as a type** |

```
$ ziman gaps3.zm && ziman --fmt --check gaps3.zm      # both exit 0; output: mg/1000.0/6/ab/2
$ python3 tmscan.py full gaps3.zm | grep -E "internal|experimental|e3|/|\.\."
gaps3.zm:1:4: UNSCOPED 'experimental'
gaps3.zm:1: '!' -> keyword.operator.logical.ziman
gaps3.zm:3:1: UNSCOPED 'internal'
gaps3.zm:7: '..' -> keyword.operator.range.ziman     # from `nums: int...`
gaps3.zm:16:21: UNSCOPED 'e3'                        # from `1e3`
gaps3.zm:20: '/' -> keyword.operator.arithmetic.ziman
```
[V] G1 and G2 are the two I would fix first: they are the only cases where a deliberate,
documented, load-bearing marker in the source is **invisible**. [O] G4, G5 and G6 are actively
misleading rather than merely absent — a reader sees a division operator inside a `use` line.
G11 is inherent to a regex layer and belongs to layer 3; Ziman's docs explicitly promise that a
JSON field called `unit` is fine, so it will be hit.

**A gap in the prior art itself:** `showcase.zm` exists to make a grammar change eyeballable and
exercises **0 of the 11** constructs above — no `internal`, no `#![…]`, no `try`/`defer`/
`switch`, no variadic parameter, no named arguments, no lambda, no text block, no exponent
literal, no symbol import. Fixing the grammar without extending that file leaves the regression
unguarded. [V]

## Layer 2 — What a regex grammar structurally cannot do

Verified against `layer2.zm`, which runs (`5/1/31/3`) and is `--fmt` clean. [V] The structural
reason in one sentence: a TextMate grammar sees a window of characters on one line and has no
symbol table.

| Distinction | What the grammar actually produces | Worth colouring? |
|---|---|---|
| **type parameter vs. struct name** | `T` in `fn identity<T>(x: T)` and `Box` at a use site are **both** `entity.name.type.ziman` | **Mildly.** Mistaking `T` for a real type misreads a signature's generality — but type parameters are single letters by convention and bounds are written inline, so: a modifier, not a colour. [O] |
| **`var` vs. `val` at a use site** | `mutable` and `fixed` are **both unscoped**; at the declaration, `var` and `val` share one scope, so even *that* cannot be themed | **Yes — this is the one.** `DESIGN.md:204-206` names it and names colour as the answer. The lint half shipped; the colour half is still impossible. Finding 5. |
| **deprecated vs. live symbol** | nothing — `#[deprecated]` is a flat span on the *declaration*; a call site in another file carries no marker | **Yes, strongly.** The compiler already computes it and warns on every compile (`SYNTAX.md:1366-1372`). Strikethrough at the call site converts a warning people scroll past into one they cannot. [O] |
| **imported vs. locally declared name** | `localdate_of` (from `use time.localdate_of`) and `local_fn` (declared here) are **both** `entity.name.function.ziman` | **Marginal — I would not.** Ziman guarantees every bare name is accounted for by a `use` line in the same file (`SYNTAX.md:959-962`), so the answer is always ten lines up. [O] |
| **shadowed binding vs. what it shadows** | nothing — no identifier at a use site gets any scope | **No.** The language already narrows this: loop binders are immutable and loop-scoped, a global `val` can never be shadowed (`SYNTAX.md:979-981`), and `for i in a { a[i] }` is a compile error. What remains is small. Worth a hover, not a colour. [O] |

## Layer 3 — The semantic-token layer

**Confirmed: no semantic tokens.** A real `initialize` over stdio returns
`textDocumentSync`, `documentFormattingProvider`, `hoverProvider`, `definitionProvider`,
`documentSymbolProvider`, `workspaceSymbolProvider`, `documentHighlightProvider` and
`completionProvider` — **no `semanticTokensProvider`** — and
`textDocument/semanticTokens/full` answers `{"code":-32601,"message":"method not found"}`. [V]
(`src/lsp.c:1581-1584` is the literal.) The prompt's premise is correct at this commit.

**A correction that matters for planning.** `documentHighlightProvider` is **not** evidence that
the server resolves identifiers. `handle_document_highlight` is a word match over the document
text that skips only `//` comments. On a nine-line file where the outer `x` has two real uses it
returns **seven** hits: [V]

```
line 0 col 15: 'struct Point { x: int }'   <- a field declaration
line 2 col  8: 'val x = 1'                 <- the binding itself
line 3 col 20: 'val p = Point { x: 2 }'    <- a field label
line 5 col 12: 'val x = 3'                 <- an inner binding that SHADOWS it
line 6 col 14: 'print(x)'                  <- a use of the inner one
line 8 col 10: 'print(x + p.x)'            <- a real use
line 8 col 16: 'print(x + p.x)'            <- a field access
```

It is wrong in precisely the cases layer 2 said a regex is wrong in. So semantic tokens are
**new work on the resolved AST** that `--check` already builds — the one `hoverProvider` and
`definitionProvider` consult — not a re-export of something that exists.

**Tokens I would emit** (standard LSP types where one fits; named explicitly where none does):

| Token type | Applies to | Closes |
|---|---|---|
| `namespace` | a module qualifier, an alias | G8 |
| `type`, `struct`, `enum` | type references, with the two kinds distinguished at last | — |
| `typeParameter` | `T` in `<T: Eq>` and its uses | layer-2 row 1 |
| `enumMember` | `Circle` in `Shape.Circle`, `Some`, `None`, `Ok`, `Err` | G9 |
| `function` | a function reference | — |
| `parameter` | a declared parameter, and a named-argument label | G7 |
| `property` | a struct field, at declaration and at `.x` | G7 |
| `variable` | a `val`/`var` local, a loop binder, a `switch`/`if Some(x)` binder | — |
| `keyword` | including `internal`, which a regex cannot place | G1 |
| `decorator` | an attribute, `#[…]` and `#![…]` alike | G2 |

| Modifier | Meaning | Standard? |
|---|---|---|
| `readonly` | a `val`, a parameter, any loop/`switch` binder | yes |
| `declaration` / `definition` | this occurrence is the binding site | yes |
| `deprecated` | carries `#[deprecated]`, or its module carries `#![deprecated]` | yes |
| `defaultLibrary` | from a bundled core module or the prelude — closes **G10** | yes |
| **`mutable`** | a `var`, at its declaration *and every use* | **non-standard; Ziman must name it** |
| **`threadBound`** | a value whose type carries `#[thread_bound]` | **non-standard** |
| **`optional`** | an `Option<T>` parameter omissible in a named call | **non-standard** |

What each buys that layer 1 cannot: **`mutable`** is the headline, and the only way to satisfy
`DESIGN.md:204`, because a regex cannot find a `var`'s *uses* — and the use site is where the
typo does its damage. **`defaultLibrary`** replaces a hand-maintained 60-name regex with a fact,
which is how `concat` and `join` stop being miscoloured and stay fixed as the prelude grows.
**`deprecated`** gives the compiler's existing warning a place in the editor. **`namespace`**,
**`property`**, **`parameter`** and **`enumMember`** are the four that turn "every lowercase word
is one colour" into structure. **`threadBound`** and **`optional`** I would build last. [O]

## The strategy

### Category-to-role mapping

Loudest for what a reader must not miss; quietest for what is on every line.

| Tier | Role | Categories |
|---|---|---|
| **Loud** — highest contrast, the only place I would use bold | being wrong here is a bug you cannot otherwise see | `mutable` (`var` and every use), `deprecated` (strikethrough + dimming), `decorator`, non-local control flow (`return spawn defer try break continue`) |
| **Mid** — distinct hues, normal weight | the structure you navigate by | declaration keywords and names, `type`/`struct`/`enum`/`enumMember`, `namespace`, strings, numbers |
| **Quiet** — low contrast, near body text | what is everywhere | ordinary locals and parameters, `property`, operators, punctuation, `support.type.primitive`, comments |

**On explicitness, deciding rather than hedging: yes, make mutability prominent; no, do not make
visibility or thread-affinity prominent.** [O]

The reason is consequence, not symmetry. Mutability is written in one character; the project's
own log records that as an accepted hazard awaiting a tooling answer; nothing else in the source
tells you, and `--lint` catches a `var` never reassigned but not a `val` that should have been
one. Making `var` loud at *every* occurrence turns a one-character difference into a
whole-expression one, and it costs nothing, because `var` is rare — the corpus is overwhelmingly
`val`, which is exactly what makes a loud marker cheap. **Mutability earns loudness because it
is scarce, consequential and invisible.**

Visibility fails two of those three. `pub` and `internal` sit at the left margin of a
declaration, one per declaration, with nothing competing, and the reader's question ("can I call
this from another module?") is asked while looking straight at it. `internal` needs a *scope* —
G1 is a real bug — but a shouting `pub` on every exported function in `lib` is noise with no
decision attached.

Thread-affinity goes further still: `#[thread_bound]` is a property of a *type*, declared once,
and the compiler already refuses the mistakes it exists to prevent — as a `Channel` element, a
`spawn` argument or a global `val` it is an error, not a warning (`SYNTAX.md:1391-1397`). Colour
is for what the compiler cannot catch. Emit `threadBound` so a theme that wants it has it; do
not spend a loud role on it. [O]

### Light and dark, and colour-blind safety [O]

- **Define the mapping once in category terms and bind two value sets.** Every rule above is
  stated as a category, so light and dark differ only in lightness assignments.
- **Contrast floors:** body text and every Mid hue **≥ 7:1** against the background (AAA, for
  text this small and dense); comments and Quiet **≥ 4.5:1** — dimmed, never faint. Loud **≥ 7:1**
  *and* separated from its Mid neighbours by a lightness step, not only a hue step, so it
  survives a dim projector.
- **In dark themes do not drop saturation to signal "quiet"** — lower lightness instead.
  Desaturated mid-grey on dark is the commonest readability failure.
- **No distinction may rest on red-versus-green alone.** Concretely: `mutable` must not be "red
  `var`, green `val`", and `deprecated` must not be "red identifier". Both carry a **non-colour**
  channel — `mutable` gets weight plus a hue shift, `deprecated` gets strikethrough plus reduced
  opacity. Either is legible in greyscale, which is the test.
- **At most seven hues.** Deuteranopia and protanopia together make roughly 8% of men unable to
  separate a 12-hue palette; seven hues plus two non-colour channels covers every distinction
  above.
- **Keep `string` and `comment` maximally far apart** in hue *and* lightness: they are the two
  categories read peripherally, and the text-block scope makes a long string look structurally
  like a comment block.

### The worked sample

`showcase.zm` reaches only part of the grammar, so I wrote `palette.zm` — 55 lines that **run**
and are `--fmt --check` clean and reach **all 30 scope names**. The harness confirms 29; the
thirtieth, `constant.character.escape.ziman`, is a sub-pattern of the two string begin/end rules
my harness does not descend into, so I verified it by matching its own regex `\\[ntr"\\]`, which
finds the `\"`, `\"` and `\n` in `print("done \"ok\"\n")`. [V]

Token → category → role (the rows that carry distinct information; full dump in the appendix):

| Line | Token | Layer-1 scope | Category | Role | Layer 3 adds |
|---|---|---|---|---|---|
| 1 | `#![experimental]` | **none (G2)** | attribute | **Loud** | `decorator` |
| 4 | `json` / `Json` | **none (G8)** / `entity.name.type` | qualifier / type ref | Mid | `namespace` / `enum` |
| 6-7 | `// A doc comment…` / `#[derive(Json)]` | `comment.line.double-slash` / `meta.attribute` | comment / attribute | Quiet / **Loud** | — / `decorator` |
| 8 | `pub` / `struct` / `Patient` | `storage.modifier` / `storage.type.struct` / `entity.name.type.struct` | visibility / decl kw / decl name | Mid | `keyword` / — / `struct`+`declaration` |
| 9 | `identifier` / `string` | **none (G7)** / `support.type.primitive` | field name / primitive | Quiet | `property`+`declaration` / — |
| 14-15 | `enum Status` / `Active` | `storage.type.enum`+`entity.name.type.enum` / `entity.name.type` **(G9)** | decl / variant | Mid | `enum`+`declaration` / `enumMember` |
| 19 | `internal` / `max_retries` | **none (G1)** / none | visibility / global `val` | Mid / Quiet | `keyword` / `variable`+`readonly` |
| 21 | `native` / `read_raw` / `Result` | `storage.modifier` / `entity.name.function` / `support.type.builtin` | decl kw / decl name / builtin type | Mid / Mid / Quiet | `keyword` / `function`+`declaration` / — |
| 23-24 | `#[deprecated("…")]` / `describe` / `extra: int...` | `meta.attribute` / `entity.name.function` / `..`→`keyword.operator.range` **(G5)** | attribute / decl name / variadic | **Loud** / Mid / Quiet | `decorator` / `function`+`deprecated` / `parameter` |
| 25-28 | `var` / `count` / `count = count + i` | `storage.modifier` — **same scope as `val`** / none / none | **mutability** | **Loud** | `variable`+`mutable`(+`declaration`) |
| 26 | `val` / `len` | `storage.modifier` / `support.function.builtin` | immutability / builtin call | Mid / Quiet | `variable`+`readonly` / `function`+`defaultLibrary` |
| 30-35 | `defer` / `switch` / `Active(n)`, `Some(text)` | `keyword.control` ×2 / `entity.name.type` **(G9)** | non-local / control / patterns | **Loud** / Mid / Mid | `keyword`; `enumMember`; binder `variable`+`readonly` |
| 38-43 | `2d`, `200b` / `1.5e-3` / `as` / `with` / `collections` | `constant.numeric.suffixed` / **`1.5` only (G3)** / `keyword.control.import` **(G6)** / `keyword.other` / **none (G8)** | literals / cast / construction / qualifier | Mid | — / — / `keyword` / `keyword` / `namespace`+`defaultLibrary` |
| 44-51 | a text block / `concat` / `true` | `string.quoted.triple` / `entity.name.function` **(G10)** / `constant.language.boolean` | text block / builtin call / literal | Mid / Quiet / Mid | — / `function`+`defaultLibrary` / — |

Reading down the third column is the argument for layer 3 in one glance: eleven rows are a gap
or a miscolour, and every one is closed by a token type or modifier the LSP could emit from what
`--check` already knows.

### How to verify a theme is right

Steps 1-5 need no editor.

1. **`python3 /tmp/ziman_style/tmscan.py full palette.zm`** — all 30 scope names must appear. A
   grammar edit that silently stops matching shows up here, not three weeks later.
2. **`ziman palette.zm && ziman --fmt --check palette.zm`** — a colouring sample that has
   drifted out of the language is worse than none, because it teaches the grammar dead syntax.
3. **`python3 tmscan.py gaps gaps3.zm`** — every gap you claimed to close: `internal`,
   `experimental` and `e3` must no longer be unscoped, and `/` in a `use` line must no longer be
   `keyword.operator.arithmetic`.
4. **Contrast-audit numerically**, not by eye: compute the WCAG ratio of every foreground against
   both backgrounds and fail the build under the floors above.
5. **Greyscale test.** Screenshot `palette.zm` in both themes and desaturate completely. `var`
   must still be findable (weight) and the `#[deprecated]` function must still read as deprecated
   (strikethrough). If either vanishes, the distinction rested on colour alone. Then repeat
   through a deuteranopia and a protanopia simulation.
6. **In the editor, use the scope inspector** (VS Code: *Developer: Inspect Editor Tokens and
   Scopes*) on each row of the table above, confirming the scope *and* the resolved colour. Only
   this step catches a theme rule shadowed by a more specific one.
7. **Once semantic tokens exist, re-run step 6 with them off and on.** The file must be readable
   both ways — a user whose server has not started yet must not get an unreadable buffer.
8. **Diff the real corpus, not the sample:** `lib/http/server.zm` (89 KB, the largest) and the
   backend's `src/main/ziman/repository/*.zm`. The sample proves coverage; only real code at
   scale shows whether the Quiet tier is actually quiet.

---

# Findings — questions for the maintainer

Each is something I believe is wrong or under-specified. None is written up in Part 1 or 2 as
though it were the style, and I changed no file in either repository to suit my taste.

**1. Should a chain of operator-continued lines zigzag? [V]** Three or more lines each ending in
a binary operator indent `+1, +0, +1, +0` (§2); I expected every continuation one level deeper,
as the first already is. It is idempotent, so `--fmt --check` would *require* the zigzag of
anyone who wrote the aligned version, and the library escapes it only by having exactly three
such lines, each with one continuation — the bug is one long boolean condition away from a
committed file. The cause looks like a one-line fix: `cont_from_prev` (`src/format.c:311-312`)
is cleared on the line after it is set, where I would expect it to stay set while
`prev_line_continues` holds. **Is the alternation intentional, and if not, is the guard meant to
read "do not add a *second* level" rather than "do not add a level on alternate lines"?**

**2. Should an array literal wrap vertically like a call and a struct literal? [V]** An
over-width `f(…)` or `Foo { … }` goes one-item-per-line with brackets on their own lines; an
over-width `[…]` fills greedily. I expected one shape applied to all three. The mechanism is
explicit (`vertical` is never set for `[`, `src/format.c:239`), so it reads as a decision — but
it is recorded nowhere: F11 describes only the greedy comma break. So a reader cannot predict
what `--fmt` will do to a long literal without knowing which bracket it is, and an author who
hand-breaks an array keeps their layout while one who hand-breaks a call loses theirs. **Is the
greedy fill deliberate — on the grounds that array elements are homogeneous and a call's
arguments are not — and if so, should that reason sit in `DESIGN.md` beside F11?**

**3. Should `docs/SYNTAX.md`'s own examples be `--fmt` clean? [V]** `struct RectDims { width:
float  height: float }` (`:840`) fails `--fmt --check` with exit 1, because the two-space field
separator collapses. `make syntax-check` verifies that extracted examples still *compile*;
nothing verifies that they are formatted. Two spaces is a real readability device for an inline
field list, and collapsing it is arguably the formatter's loss rather than the document's — but
today the two disagree silently and the document is the louder voice. A third fact: the
single-line `struct`/`enum` form appears **0 times** in 259 real declarations, so SYNTAX.md
teaches a layout nobody uses in a spelling the formatter rejects. **Should
`check_syntax_examples.py` also run `--fmt --check` on each block — or should `need_space`
preserve a deliberate multi-space run between a field list's entries, the way it already
preserves one around `<`?**

**4. `internal` and `#![…]` have no syntax scope at all. [V]** `internal` matches nothing in
`ziman.tmLanguage.json`; `#![…]` is not matched by `meta.attribute.ziman`, whose `#\[[^\]]*\]`
requires `[` immediately after `#`, so the `!` colours as logical-not and the name is bare. I
expected `internal` to colour like `pub` and `#![…]` like `#[…]`. These are the two cases where
a documented, deliberate, load-bearing marker is invisible: `internal` is one of exactly three
visibility levels (`SYNTAX.md:963`), and `#![…]` is how four shipped modules declare themselves
experimental and how a module declares itself deprecated (`:1338-1360`). Both look like two-line
fixes — add `internal` to the alternation, relax the match to `#!?\[[^\]]*\]`. **Was `internal`
left out because of its soft-keyword status, i.e. a worry about miscolouring `val internal = 1`?**
If so, note that `unit` is a soft keyword too and *is* matched, so a field named `unit` already
miscolours as a primitive type (G11) — the grammar has accepted that trade once.

**5. `val` and `var` share one scope, so no theme can distinguish them. [V]**
`storage.modifier.ziman` matches `pub|native|val|var` as one alternation, and a `var`'s uses get
no scope at all. `DESIGN.md:204-206` names this exact hazard and names colour as the answer:
"`val`/`var` differ by one character, which is a one-keystroke typo that silently flips
mutability … a future formatter/linter should visually distinguish them (e.g. color, lint warning
on the near-miss)". The lint half shipped. The colour half is still impossible, because the
grammar gives a theme nothing to hook. The declaration half is one line (split `val|var` into
`storage.modifier.mutable.ziman` and `…immutable.ziman`); the use-site half needs layer 3's
`mutable` modifier. **Is splitting that alternation acceptable, and is the non-standard
`mutable` semantic-token modifier the intended long-term home for the use-site half?**

**6. `documentHighlightProvider` is advertised but is a word match. [V]** Seven hits for an `x`
with two real uses, including a field declaration, a field label, an inner shadowing binding and
`p.x` (Layer 3); I expected a capability named "highlight this symbol's occurrences" to use the
resolver `definitionProvider` and `hoverProvider` already consult. An editor renders those hits
as "other uses of this", so the feature is actively wrong on the shadowing case — where being
wrong is most expensive. Worse for planning: anyone reading `documentHighlightProvider: true`
might reasonably conclude occurrence resolution is done and semantic tokens are a re-export.
They are not. **Was the textual implementation a deliberate cheap first cut, and should the
capability stay advertised while it is textual?**

**7. Three behaviours exist only in source comments. [V]** Not bugs — rules a reader following
`DESIGN.md` would not know and would be surprised by. (a) **`normalize_vertical`**: breaking a
call or struct literal between arguments *at all* forces it fully vertical regardless of width —
the most surprising behaviour in Part 1, documented only at `src/format.c:438-441`. (b) **The
`<`/`>` convention**: `--fmt` is deliberately silent, so the corpus's rule (generics tight,
comparisons spaced, zero counterexamples in `lib`) is unwritten and unenforced. (c) **The
attribute-after-doc-comment order**: it lives in `tools/gen_lib_manual.py:119-125`, enforced by
`make lib-manual-check` over `lib/` only, invisible to `--fmt` and to application code.
**Should these move into `DESIGN.md` or a short style section in `SYNTAX.md`, so that "what the
formatter will not decide" is as documented as what it will?**

**8. `--canonical` is a no-op on the entire corpus. [V]** `--fmt --canonical --check` exits 0
over all 99 `fmt-check` files *and* all 173 backend files. F12 predicted this for `lib/`; the
backend result was not known then. Not a defect — the flag is worth having as the thing that
corrects Allman braces before review — but it means the flag is untested by use and is not in
`make check`. **Should `make fmt-check` run `--canonical --check` too, since both corpora already
pass it?** It costs one word on one line and converts "we happen to agree" into "we are held to
it". If no, the reason — keeping plain `--fmt` as the only gate, so the formatter never has an
opinion a lexer had to guess at — is worth a sentence in F12.

---

# Earlier reviews — what is fixed, what still holds

Read after forming the view above, and re-checked by re-running the reproductions.

| Source | Finding | Status |
|---|---|---|
| `developer-experience-code-review-2026-10-05.md:206,645` | "`--fmt` is idempotent on all 37 library files and on 138 of 152 application files; 14 would be rewritten"; "`--fmt --check` is evidently not in CI" | **Fixed.** `--fmt --check` over all 173 backend files exits 0 — 0 would change. [V] |
| same, `:836` (item 25) | "`--fmt --check` in CI" | **Fixed for the library** — `make fmt-check` is a prerequisite of `make check` (`Makefile:45`) over 99 files. The backend is clean but I found no target gating it; `fmt-check-backend` exists as a phony name (`Makefile:33`). **Partially open.** [V] |
| `language-design-review-2026-10-05.md:652`, `:791` (P2-9) | "add a `--fmt --canonical` mode that normalises brace placement" | **Fixed** (F12); verified pulling up Allman braces and `else`. See Finding 8. [V] |
| `c-implementation-review-2026-10-08.md:76`, `:544`, `:743`, `:884-885` | "`--fmt` and the LSP's formatter buffer a quadratic result. `lspfmt.py 6000` → 144,064,589-byte reply, 1,036,196 kB RSS, from a 24 KB document" | **Fixed.** I re-ran the same shape — 6,000 nested blocks, 72 KB — through the real LSP: **814-byte** reply (a `-32803` error naming the depth limit) at **28,852 kB** peak RSS. Both limits fire. [V] |
| `language-design-review.md:179`, `:339` (o2) | "`--fmt` works, wraps long arrays, has `--check`/`--write`, enforced via `make fmt-check`"; "wraps at width 120" | **Still accurate**, and I pinned the boundary: 120 code columns pass, 121 wraps. [V] |
| `language-design-review.md:213` | "the formatter output is canonical" | **I disagree as written.** Plain `--fmt` output is *idempotent*, not canonical: two files in opposite brace styles both pass `--fmt --check`, which I verified. `DESIGN.md:3918` says so plainly, and F12 exists because of it. True only of `--fmt --canonical`. [V] |
| `pl-expert-review-2026-09-23.md:389` | "DESIGN.md itself flags `val`/`var` … and says 'a future formatter/linter should visually distinguish them.' Worth more than an LSP here." | **Half fixed, half still holds.** Formatter, LSP and the `--lint` rule all shipped [V]. The *visual* distinction is still impossible — one scope for both, no scope on uses. This is Finding 5, and the oldest open formatting-adjacent finding in the reviews. [V] |
| `pl-expert-review-2026-09-23.md:369` | "`editors/vscode/` — a TextMate grammar. Not published." | **Still holds** as to publication; I would add that `showcase.zm`, which exists to make a grammar change eyeballable, reaches none of the 11 gaps. [V] |
| `DESIGN.md:3908-3909` (F11's own "still not done") | joining lines that would now fit; wrapping at binary operators or `&&`/`\|\|`; wrapping long `use` or `if` conditions; an LSP "format document" | **The LSP half is now done** — `documentFormattingProvider: true`, and a real request returns exactly what `--fmt` prints [V]. The other four still hold: I verified a long condition and a long `use` line are both left long [V]. F11's list is stale on that one item. |

---

# Appendix — the experiments

Every file is in `/tmp/ziman_style/`; nothing was written inside either repository except this
report. "Runs" = executed to completion, exit 0. "fmt-clean" = `--fmt --check` exits 0.

| # | File | Input (the point) | Result | Section |
|---|---|---|---|---|
| 1 | `e01_indent.zm` | wild indentation, nested `if`/`for`/`while` | normalised to 4 spaces per bracket | §1 |
| 2 | `e02_cont.zm` | a leading-`+` continuation | formatted fine; `--check` rejects it at 9:9 | §11 — proves `--fmt` works on input the compiler refuses |
| 3 | `e03_cont4.zm` | 4 operator-continued lines; runs (`15`) | **zigzag**; re-formatting changes nothing | §2, **Finding 1** |
| 4 | `e04_wrap_sig.zm` | a 170-column signature | vertical; `--width=0` restores one line | §2 |
| 5 | `e05`, `e06_wrap_call*.zm` | long calls, arrays, struct literals | calls+struct vertical; **array greedy** | §2, **Finding 2** |
| 6 | `e07_arr.zm` | an array at `--width=30` and `=20` | greedy fill at both | §2 |
| 7 | `e08`, `e10_longcond.zm` | a 131-column `&&` condition; runs (`1`) | left long | §2 |
| 8 | `e09`, `e11_longtype.zm` | a 121-column generic return type | left long | §2 |
| 9 | `e12_decls.zm` | every declaration form; runs | preserved; **two-space separator collapses** | §3, **Finding 3** |
| 10 | `e13_inline.zm` | the collapsed one-space struct; runs | parses and runs | confirms the collapse is legal, not corruption |
| 11 | `e14_syntaxmd.zm` | `docs/SYNTAX.md:840` verbatim | `--fmt --check` **exits 1** | **Finding 3** |
| 12 | `e15_stmts.zm` | `switch`, `if Some(x) =`, value-`if`, `defer`, `spawn`, lambdas; runs | re-indented only | §4 |
| 13 | `e16`, `e17_use*.zm` | 11 then 7 `use` lines, mixed forms | sorted; separator ASCII decides intra-module order | §6 |
| 14 | `e18_attr.zm` | attributes, doc comments, `#![…]`, an indented `#[test]` | re-indented; **never moved** | §7 |
| 15 | `e19`, `e20`, `e22` | trailing comments, a 139-column comment | aligned per run; never reflowed; a full-line comment breaks the run | §8 |
| 16 | `e21_blank.zm` | 3 blanks; blanks after `{` and before `}` | collapsed / removed; none inserted | §9 |
| 17 | `e23_ops.zm` | every operator, badly spaced; runs | normalised; `a<2` stays tight | §10 |
| 18 | `e24_angle.zm` | `a < b`, `a<b`, `Map < string , int >`; runs | spacing **preserved**, collapsed to one | §10, **Finding 7b** |
| 19 | `e25_lambda.zm` | a lambda as a call argument; runs (`5`) | indents once; call not made vertical | §1, §2 |
| 20 | `e26_vert.zm` | hand-broken call / call-with-comment / struct / array | vertical / **left alone** / vertical / **left alone** | §2, **Finding 7a** |
| 21 | `e27_canon.zm` | a full Allman-brace file | plain check **exits 0**; `--canonical` check exits 1 | §11, **Finding 8** |
| 22 | `e28`, `e29_deep*.zm` | 201 and 199 nested `if true {` | both refused, exit 2, per-kind breakdown | §12 |
| 23 | `e30`, `e31`, `e32.zm` | depth 151 × 20,000 and × 30,000 lines | 12,352,414 bytes / **refused** | §12; also shows same-line brackets add no level |
| 24 | `e33_lint.zm` | redundant alias, quadratic `concat`, unused `val`, never-reassigned `var` | all four reported, exit 1 | §12 |
| 25 | `e34_lit.zm` | map, set, struct, `with`, nested arrays; runs | map vertical; `with` spaced as a literal | §5 |
| 26 | `e35_attr.zm` | a 121-column `#[derive(…)]` | left long | §2, §7 |
| 27 | `e36_misc.zm` | lines ending in `->`, `=`, `:`; `--check` exits 0 | `->` and `=` continue; `:` does not | §1, §2 |
| 28 | `e37_write.zm` | `--fmt --write` on a copy | rewritten in place | §12 |
| 29 | `e38_use.zm` | a 104-column `use` | left long | §2 |
| 30 | `e39_attrspace.zm` | `# ! [ experimental ]`; runs | → `#![experimental]` | §7 |
| 31 | `e40_cw.zm` | over-width call with a 60-column trailing comment | wrapped on code width only; the comment travels with `)` | §2 |
| 32 | width sweep | code widths 119/120/121 via `--fmt -` | no / no / **yes** | §2 |
| 33 | `palette.zm` | 55 lines reaching all 30 scopes; runs, fmt-clean | 29 confirmed by harness, the 30th by its own regex | Part 2 sample |
| 34 | `gaps.zm`, `gaps2.zm`, `gaps3.zm` | `internal`, `#![…]`, `1e3`, `int...`, `use a/b`, `as`, `concat`, `unit` as a field | all 11 gaps reproduced; `gaps3.zm` runs and is fmt-clean | Layer 1, **Finding 4** |
| 35 | `layer2.zm` | type parameter, imported vs. local call, `var` vs `val`; runs, fmt-clean | `T`≡`Box`; `localdate_of`≡`local_fn`; both locals unscoped | Layer 2, **Finding 5** |
| 36 | LSP `initialize` | a real request over stdio | no `semanticTokensProvider`; `semanticTokens/full` → `-32601` | Layer 3 |
| 37 | LSP `formatting` | `fn main(){val a=1` / `print(a)}` | returns exactly what `--fmt` prints | `DESIGN.md:3909` stale |
| 38 | LSP `documentHighlight` | the outer `x` in a 9-line file with a field, a label and a shadow | **7 hits**, 2 real | **Finding 6** |
| 39 | LSP format, 6,000 nested blocks | the C review's F5 reproduction, 72 KB | 814-byte error, 28,852 kB RSS | that P1 is fixed |
| 40 | corpus measurement | 213 files, 40,053 lines of `lib` + backend | max depth **19**; 112 code lines over 120 cols, longest 451; **0** one-line structs; 1,706 of 1,710 functions braced | grounds every [O] recommendation |
| 41 | corpus `--canonical` | all 99 `fmt-check` files, all 173 backend files | both exit 0 | **Finding 8** |

**Tooling I wrote, and its one real limitation.** `/tmp/ziman_style/tmscan.py` reads
`ziman.tmLanguage.json` and reimplements TextMate's leftmost-match-then-pattern-order rule. It
handles the flat `match` rules and the two `begin`/`end` rules. It does **not** descend into a
`begin`/`end` rule's own sub-patterns, and it scans one line at a time, so it cannot see
`constant.character.escape.ziman` inside a string and cannot carry a text block's scope across
lines. Both are stated wherever they affect a claim, and I verified the escape scope by matching
its own regex instead. It approximates Oniguruma on these patterns; it is not a substitute for
the editor's scope inspector — which is why step 6 of the verification procedure exists.

---

# What I did not check

Named plainly, because a report implying coverage it lacks is worse than a short one.

- **I did not render anything in an editor.** No VS Code, no theme applied, no scope inspector,
  no screenshot, no colour-blindness simulation. Every layer-1 and layer-2 claim comes from the
  grammar file plus my harness, whose limitation is above. My contrast and greyscale guidance is
  reasoning from constraints, **not** a measurement of a palette — there is no palette to
  measure, which is deliberate: I did not propose concrete colours, because assigning hex values
  without rendering them is the one part of this I could not verify.
- **I did not read `editors/vscode/extension.js`, `package.json`, `snippets/ziman.json` or
  `models/` in any depth.** A snippet teaching a layout that contradicts §3 would have escaped me.
- **I read `src/format.c` in full and `src/lsp.c` only around the handlers I exercised**
  (`initialize`, `documentHighlight`, `formatting`). Hover, completion, definition and the symbol
  providers were not audited.
- **No sanitizers, no fuzzing.** The `fuzz/crash2/` inputs the C review mentions were not re-run;
  I reproduced only the one documented shape.
- **No non-ASCII source.** `char_count` handles UTF-8 continuation bytes (`src/format.c:375-378`), so
  width is measured in characters not bytes, but I tested no multi-byte identifier or string, no
  grapheme cluster and no double-width CJK character — where "column" and "character" stop
  agreeing and trailing-comment alignment could visibly drift. I also did not test CRLF input,
  though `scan` treats `\r` as a blank (`:39`).
- **I did not verify the `#![deprecated]` warning-at-each-`use` behaviour**, only read it at
  `SYNTAX.md:1347-1360`. My G2 claim is about the *grammar*, which I did verify.
- **The corpus numbers (112 over-width lines, max depth 19) come from my own Python
  approximation** of comment stripping and bracket counting, not the formatter's accounting.
  They are right to within the handful of lines where a `//` sits inside a string literal; treat
  them as measurements of scale, not exact counts.
- **`make check` in full was not run** — only `make fmt-check` and `make test`, as asked.
  `api-check`, `docs-check`, `libc-free-check` and the sanitizer targets were not exercised.

---

*Nothing in either repository was modified except this file. No commit, no push. Scratch files
are confined to `/tmp/ziman_style/`.*
