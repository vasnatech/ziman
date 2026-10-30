# Design journal, part 1 (entries dated 2026-09-01 to 2026-10-13)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## R14: derives on generic types

`derive(Ord)` on `Pair<A, B>` emits `pair_compare<A: Ord, B: Ord>`; the
typechecker's `find_compare_function` accepts a generic compare function and
checks each type argument against the bounds it declares. `derive(Json)`
cannot rely on a bound (Json has no trait), so instead each type parameter
becomes a converter function-value parameter, which is what function values
(R13) made possible. Converters for nested generic fields are synthesised from
the concrete type arguments (type parameter -> the enclosing converter, scalar
-> `json.int_to_json` etc., plain struct/enum -> its derived function).
Arrays/Options as type arguments are rejected rather than guessed at. Row and
Properties stay non-generic: a row has a fixed column list.


## R15: stdlib gaps -- HTTP client, filesystem, logging

- `fs` is a thin native layer; every failure is a `Result` with `strerror`
  text. Actions with nothing to report return `Ok(unit)` (`remove`, `make_dir`,
  `make_dirs`, `rename`; the writes return the bytes written).
- `log` has no global logger because Ziman has no mutable globals; a `Logger`
  value is passed instead, which also makes per-component prefixes trivial.
  Named arguments + `Option` omission give it a small constructor.
- `client` is pure Ziman over `net` (so TLS comes free). It deliberately
  treats every HTTP status as `Ok`: a 404 is an answer, not a failure of the
  exchange. One request per connection keeps it stateless. Building it found a
  real bug in its own first draft (connecting plain *and* TLS for https); the
  TLS fixture against `openssl s_server` is what caught it.
- Regex is left open: it wants either a native engine or a sizeable pure-Ziman
  one, and is a design decision of its own.


## `io` folded into `fs`

`io` held two functions (`read_file`, and a fatal `write_file`) next to the
newer `fs`. One domain, two modules; the fatal writer contradicted the
Result-everywhere rule. Now `fs.read_text` replaces `io.read_file` (same
Result; the message names the OS reason, e.g. "cannot read 'x': No such file
or directory") and `fs.write_text` replaces `io.write_file`. Earlier entries in
this file and ROADMAP.md that mention `io.read_file` describe history.


## `sb_append_byte`

`percent_decode` (in `http` and `db`) built an `ArrayBuilder<byte>` (one
boxed Value per byte), copied it to a `[byte]`, then copied that to a string,
allocating a one-element array per plain character on the way. Now it appends
the plain runs between escapes as substring views to a `StringBuilder` and each
decoded byte with `sb_append_byte`, which takes one or more raw bytes (variadic) and,
because a string cannot hold a NUL, appends nothing and returns false if any is `0b`. The general cost, boxed bytes, is roadmap R23.


## R16: naming convention

Survey result: the two usage modes already sort the names. Everything another
module imports bare (`byte_from_hex`, `string_to_bytes`, `net_error_message`,
`sql_lift`, `random_bytes`) carries its noun, and every module's qualified-use
API is short verbs. The review's collision (`use server.response`) is a
consequence of importing a verb-style name bare, which the convention now says
not to do. No mass rename: it would churn derive-generated code and every
fixture for no gain. Rule 3 (type-prefixed operations for types a module
defines) is what lets `derive` find `<type>_compare` and friends by name.


## R17: `--lint`

The hazard is one-directional: assigning to a `val` is already a type error, so
the silent mistake is a `var` that never changes. The lint therefore flags
exactly that (Rust's `unused_mut`), by having the type checker mark each `var`
on `STMT_ASSIGN` and report the unmarked ones at the end of the function. It
needs the merged module to know where code came from, so `FunctionDecl` gained
`source_file` (NULL for bundled modules and the prelude, so a library isn't
linted through the program that uses it) and derived functions are skipped.
`--lint` replaces running: exit 0 clean, 1 with findings, so it drops into CI.
It also adds the first `file` field to a diagnostic (warnings only). A
formatter is a separate, larger project (the parser drops comments and layout).

*Update 2026-10-12 (F84): a third rule, and the sentence above about bundled
modules turned out to be the interesting one.* **Reading a builder by converting
it, inside a loop that keeps adding to it** -- `val seen = mapb_to_map(counts)`
followed by `mapb_set(counts, ..)` -- materialises the whole map on every pass
when `mapb_get_or` reads it in place. The existing quadratic rule cannot see
this: it matches a variable rebuilt *from itself*, and this shape has no
self-reference. **The obvious predicate is unusable, which is the design
content:** "a builder conversion inside a loop" gives 14 hits across `lib/`, the
reference backend and the benchmark and 13 are correct code -- a conversion on
the way out of a loop runs once, and a convert-then-reset converts one batch. So
the rule requires all three of: the result bound by a `val`/`var` in the loop, a
later statement in the same block appending to that same builder, and no
reassignment of the builder in between. **And the `source_file` sentence above
is why the library's own instance had to be found by hand**: `fs.zm`'s
`path_join` converted the whole accumulated path on every piece to read its last
character (0.02s to 0.35s across 2,000 to 16,000 pieces, now 0.01s to 0.03s and
linear), and no amount of `--lint` would ever have reported it, because a
bundled module is deliberately not linted through the program that uses it. That
is the right trade -- a warning about code the caller cannot change is noise --
but it means the library is checked by review, and this rule is for the code
people write.


## Two silences the compiler no longer keeps (F85, F86, 2026-10-12)

Round 2 of the committed generation benchmark took a cold model from 0 of 50 compiling to 50 of 50,
and then found the limit of the compiler-as-documentation premise: **6 of those 50 programs were
compile-clean and silently wrong**, in two shapes. Both are now diagnosed, and in both cases the
corpus decided the rule's shape rather than taste.

**`${name}` or `{name}` in a string literal.** Ziman has no interpolation (see the string-literal
entry above), so `"total: ${t}"` prints those characters. Two attempts printed `${total}` to stdout, and -- the
part that makes this worse than a missing feature -- **two agents reading only `--check` concluded
that interpolation works**, one reporting it "stringifies int/bool". The warning fires only when the
braces hold a name path whose first segment **is a binding in scope at that point**, and that
narrowness is not a preference: a lexical "any `${`" rule fires 19 times across `lib/`, the tests
and the reference backend, and **all 19 are correct code** -- `format("${}", i)` is a literal dollar
plus Ziman's own `{}` placeholder, which is how the backend writes Postgres `$1` parameters, while
`properties` and `regex` both take `${name}` as their own template syntax in data. Scope-gated, the
same sweep reports **zero**.

*Extended 2026-10-12 (F88), because round 3 found a third agent inventing a third spelling.* The
bare `{name}` form is covered too: arm B's 49 concluded from `--check` that `"{expr.field}"`
interpolates, and that attempt was one of its two wrong answers. The extension turned out to need
**no format-string exemption at all**, which was the risk that made it look hard: `format` and
`printf` never pass their format string through `check_expr` -- it is typed directly and scanned by
`scan_placeholders` -- so a real `{}` placeholder is never seen by this check, and a malformed one
like `format("{host}", host)` is already a hard error there. The corpus sweep after the extension
still reports **zero** across 1,303 files.

There is deliberately no escape character: a `\$` was written and then removed, because the checker
sees the literal after escape decoding and so could not have honoured it -- shipping it would have
added a language feature that did nothing. Braces wanted as text are split from the name instead:
`concat("{", "name}")` is quiet and prints `{name}`. Note `"{{name}}"` is **not** a way to write it;
`{{` is a format-string escape, and in a plain literal it prints both braces.

**A discarded pure builtin.** `push(arr, item)` in statement position compiled and did nothing in
four of the fifty attempts -- arrays are values, so the new array went nowhere, and two of those four
printed an empty result. The mechanism was already here for `Result`, which refuses to be discarded
and names `val _ =` and `try`; this extends the same rule to a fixed list of builtins that compute a
value and have no other effect. **The list is a list, not a predicate, and `unwrap` is why:** adding
it reported 30 errors in `docs/lib/examples` alone, because `unwrap(fs.write_text(...))` is the
corpus's own idiom for "abort if this failed" -- the value is genuinely unwanted and the abort is the
point. `print` and the channel builtins are out for the same reason. Both fixtures reintroduce their
fault to prove the guard is live: widening the interpolation rule breaks `dollar_brace_not_
interpolation`, and adding `unwrap` to the pure list breaks `discarded_pure_builtin` **and 59 other
fixtures**, which is the measurement of how deliberate that idiom is.


## `--api` prints doc comments, because a signature is not a behaviour (F89, 2026-10-12)

Round 3 of the generation benchmark measured `--api`'s worth at 897 compiler invocations down to
119, and found the one thing it does not carry. **Two of the twelve tasks turned on behaviour that
a signature cannot state, and both were answerable from the function's own doc comment:**
`46-fold-average` answered **wrongly** because it chose `number.float_to_text` off its signature and
could not know that a whole-number float prints as `18` -- the comment above that function gives
`150` as an example -- and `15-sha256-hex` was right only because it *distrusted*
`number.to_hex`'s unstated output format and hand-rolled hex at a cost of 14 extra compiles,
when that function's comment states the format exactly. The information existed, in the library, and
`--api` hid it.

**The comments come from the lexer, not from re-reading the file.** The parser still drops them, as
the formatter entry above says; the lexer now records each `//` line beside the token stream
(`lex_comments`) and `--api` walks back from a declaration's line to collect the run immediately
above it. So the signature still comes from the parse -- "what it says is what the compiler sees" --
and the prose comes from the same lexing of the same source, rather than a text scan that could
disagree with it. **Contiguity is the whole rule:** a `// ---------- section` banner is separated
from the next declaration by a blank line, which is what keeps it out of that declaration's
documentation, and `make api-check` asserts both halves of that.


## The no-methods hint speaks only about functions that exist (F92, 2026-10-12)

`p.to_string()` parses as a call into a module named `p`, and the usual reason is that the author
reached for a method Ziman does not have -- so the diagnostic names the free-function form that does
work. F28 gave that hint ten hand-written special cases (`to_string`→`str`, `lines`, `join`→
`collections.join`, `sort`, `keys`, `values`, `reverse`, `sum`, ...) on a rule worth keeping: **a hint
that names the wrong function is worse than one that names none**, because the reader spends a round
finding out. Everything else fell back to `member(receiver, ...)` -- the call *form*, with the
comment "not a claim that such a function exists", because the pass had no scope to look a name up
in.

**The variance wave showed what that costs when the form is read as a claim.** `a.iter()` was
answered with `iter(a, ...)`; there is no `iter`; the run spent **194 compiler invocations** and
concluded `Iter<T>` is "visible in the type system and unreachable from source" -- while
`iter_of_array` exists, and round 3's `--api builtins` arm found it in one call. (F170 renamed
that function to `iter`, so the hint the run was given, `iter(a, ...)`, is now the right advice
and the fixture pins both halves. The point this entry makes is unchanged: the form was read as a
claim, and the claim happened to be false on the day.)

So the pass now has a scope: **the parsed prelude**, kept aside as the hint's own reference. That is
the same source `--api builtins` prints from, and for the reason recorded there -- it is the only
honest answer to "does this exist", where `BUILTINS_NO_NAMED_ARGS` is the subset taking no named
arguments and would report `chars` as absent. When the prelude has the function, the form is named
exactly as before, which keeps the common `s.trim()` case clean and is why this is **not** the
blanket "if there is such a function" hedge this diagnostic had already carried and shed as noise.
When the prelude does not have it, the message says so and points at `--api builtins`. While the
prelude is itself being parsed the answer is *unknown* rather than *absent*, and the hint falls back
to the old behaviour. Two fixtures, one per half: dropping the lookup fails the absent-name one and
leaves the real-name one passing, which is what makes the pair a guard rather than a transcript.


## Three diagnostics now say that `a..b` stops before `b` (F93, 2026-10-12)

`a..b` has been exclusive of `b` since the beginning and `docs/SYNTAX.md` has said so in those words
the whole time. The benchmark measured what that is worth to someone who cannot read it: **`..=` got
a bare "expected an expression, found '='" plus a bogus "expected 'fn'" on the next line, and four
runs across three waves avoided ranges altogether rather than risk an off-by-one**, one of them
writing that "`--check` alone cannot distinguish the two" and switching to `while` with explicit
counters. Nothing was *wrong* with those programs; the cost was paid in the shape of the code and in
compiler calls spent hedging.

Two kinds of fix, and the second is the one that reaches people. **`..=` is now rejected in the
lexer** with the semantics stated -- which also ends the cascade, since a lexical error stops before
the parser can invent a second complaint about the following line. But an author who writes `0..n`
directly, correctly, and simply wonders never sees that message at all. So **the two diagnostics that
already named the range form now say what it means**: the stray-semicolon lexical error, which
already volunteered "loops are `for i in 0..n`" and which **30 of 50 cold attempts hit** -- by far the
widest-reach sentence in the compiler -- and the `for`-iterable type error. The rule worth
generalising: when a message names a construct in passing, that is the cheapest place in the whole
compiler to say what the construct means.

`..` followed by a third `.` is still the variadic `xs: int...`; only `..=` is refused.
**Superseded in part (2026-10-12, F94)**: that spelling is gone -- `..` plus a third `.` is now
recognised in a parameter list only in order to be refused by name (see the next entry). The `..=`
rule itself is unchanged.


## The variadic marker is a keyword after all: `xs: variadic int` (F94, 2026-10-12)

**This reverses the syntax half of "Variadic parameters" above, and the entry worth reading first is
that one**, because its reasoning was explicit and is what came apart. It chose `pieces: string...`
over a keyword on an AI-generation argument: a keyword "invites a subtly-wrong synonym a model could
plausibly generate instead (`varargs`, `repeated`)", while `...` "is either present or a syntax
error, nothing in between". Both halves of that are still true. What was missing is that they were
never weighed against anything measured -- and the thing to weigh them against is not generation at
all.

**The decision is readability, and it is the user's.** `pieces: string...` is three characters of
punctuation doing work a word does better for a human reader, in the one place in a signature a
reader most needs to know what a caller writes. Ziman's stated order is AI-first, human readability
second, and this is a case where the second one was the only one with anything to say: nothing in
the 2026-10-12 benchmark turned on the spelling (one task, `25-variadic-sum`, declares a variadic
at all), so there was no generation effect to trade away -- only a guess about one.

**`variadic` was chosen over `vararg`, `varargs`, `params` and `vararg`-style abbreviations on two
grounds, one of them measured.** It is the only candidate that is both absent from the whole corpus
as an identifier (`variadic_sum`, `collapse_variadic_args` and a dozen comments use it as a *word*,
none as a name, so making it a hard keyword breaks nothing) and one a model actually wrote: a round-2
attempt at `25-variadic-sum` gave up on the ellipsis and wrote `xs: variadic int`, unprompted, which
is also where the position comes from. `rest` (19 binding-position uses) and `args` (22) would break
working code; `varargs` is free but no run guessed it. The earlier entry's synonym worry is real and
is answered by picking the longest, least abbreviatable member of the family -- there is no shorter
form of `variadic` to drift toward the way `vararg` and `varargs` have each other -- not by avoiding
words altogether.

**The known cost, named rather than discovered later: a cold model will write `int...`.** That is
what the training data holds (Java/Swift/Kotlin suffix it; no language spells it `variadic`), and a
future benchmark wave failing on it is this decision's price, not a regression. So the cost is paid
where it is cheapest to pay -- **both abandoned spellings are still recognised, only to be refused
with the keyword written out**: `xs: int...` and Go's `xs: ...int` each report "'...' does not mark a
parameter variadic -- the keyword does, before the type: 'xs: variadic int'", naming the parameter
and the type the parser just read, so the fix can be copied off the message. Without that the error
was `expected ')', found '..'`, which is the F53 failure exactly: the token, not the question. Two
diagnostics for two spellings, pinned by `diag_ellipsis_variadic` with the count as part of the
assertion, and the guard proved by reintroducing the fault.

**Everywhere a signature is printed now says `variadic`, including the one that never said anything.**
`ziman --api` printed `concat(pieces: string)` -- the element type with no marker at all, which was a
silent omission while the marker was punctuation a reader would not miss, and is actively misleading
once it is a keyword that has to be written to declare one. Round 3 measured `--api` at roughly ten
times the value of iterating without it, so it is the one surface where a model learns a keyword it
cannot guess. LSP hover had the same bug in the other direction (it rendered the old `...`), and
`editors/vscode/syntaxes/ziman.tmLanguage.json` now scopes `variadic` as a `storage.modifier`
alongside `pub`/`internal`/`native`, with a case in `tools/check_grammar.py`.

**The keyword is six characters wider than the suffix, and exactly one signature noticed.**
`join(prefix: Option<string>, suffix: Option<string>, separator: Option<string>, pieces: variadic
string)` crossed the formatter's 120-column limit, so `--fmt` now breaks it one parameter per line --
the whole visible cost of the extra width across 42 declarations.

**One formatter rule was deleted rather than kept working.** `need_space` had a special case for "a
`.` whose previous-previous token is `..`" -- `string...` ends in a `.` that is not a member access.
Nothing valid is spelled that way any more, so the rule went; `make fmt-check` holds both corpora to
the result.

**Migration: 42 declarations across 23 files**, all of them in this repository (`lib/`, the fifty
benchmark references, the fixtures) -- the reference backend next door calls variadic functions but
declares none, so it needed no change, which is itself the measure of how small this blast radius
is. One commit, because a half-migrated corpus does not compile: the prelude declares seven
variadics and is built into the binary.


## `let x = 1` is parsed as the `val` it meant (F95, 2026-10-12)

**The cascade was the single noisiest thing the 2026-10-12 benchmark measured: 5 to 9 errors from
one wrong word, in at least eight of the fifty runs.** `let total = 0` is not a declaration to the
parser -- it is two statements, an expression `let` and an assignment to an undeclared `total` -- so
it reported "Ziman has no 'let'" (correctly, and with the fix in it), then "assignment to undefined
variable 'total'" on the same line, and then "use of undefined variable" at every later mention. A
model reading that cannot tell one mistake from eight.

**The suppression machinery already existed and the construct could not reach it.** "Reporting
several errors per run" above poisons the name a failed `val`/`var` declared, precisely so one bad
line is not re-reported at every use. It keys off the statement *being* a declaration. `let` never
was one.

**So the parser now rewrites it.** `let`/`const`/`auto` `name [: T] = expr` parses through
`parse_decl` as a `val` (`mut` as a `var`), exactly as if the right word had been written, and the
wrong word is recorded on the statement (`Stmt.foreign_decl_keyword`) rather than reported on the
spot. **Reporting it in the parser is the thing to avoid**, and the previous two attempts at this
line are both examples of why: a parser error jumps to the recovery point *between declarations*, so
it takes the rest of the function with it -- F28's fix for the annotated `let x: int = 1` bought one
diagnostic for a whole `fn main`, and a file with three `let`s reported one of them. The type
checker reports it instead, from the top of `check_stmt`, once per statement; the recovery that
follows poisons the name the rewrite bound, and the cascade is gone with nothing new written to
suppress it. Eight errors became three on the measured file -- two wrong words and the one genuine
error on a later line, which **has to survive**, or the cure is worse than the noise.

**The field is on `Stmt`, not inside `decl`.** `let x = if c { 1 } else { 2 }` parses to a
STMT_VALUE_BLOCK, not a declaration node, and a rewrite that silently accepted `let` in one shape
would be a far worse defect than the cascade. One field, set on whatever `parse_decl` returns, is
how that shape cannot be forgotten -- and `make test` proves it: removing the `check_stmt` report
makes three fixtures fail, not none.

**The same wrong word one level out gets its own answer**, since `parse_statement` never sees it:
`let top = 1` between declarations reported "expected 'fn', found identifier", naming neither `let`
nor what replaces it. It now reports through the `expect` hook that already answers `import` and
`type`, with the one piece of advice that differs out there -- a top-level binding is `val` or `pub
val`, and there is no `var`, because Ziman has no mutable globals.

**One list, two positions.** `FOREIGN_DECL_KEYWORDS` moved from typecheck.c to token.c as
`ziman_foreign_decl_keyword`, because a second copy in the parser is exactly the kind of list that
drifts: the parser catches the three declaration shapes, the type checker still answers a bare `let`
that reaches an expression on its own.


## ArrayBuilder reads back in place, and iterates (F96, 2026-10-13)

**The gap was an asymmetry nobody had looked at squarely.** `MapBuilder` and `SetBuilder` have had
`_get`/`_contains`/`_len`/`_remove` from the day they were written -- a builder you can ask about. The
`ArrayBuilder` beside them had `ab_new`, `ab_append`, `ab_to_array` and `ab_len`, and nothing else: no way to
read an element, no way to replace one, and `for x in ab` was a type error. Anything that needed what it had
already appended had to call `ab_to_array`, which is **a full copy of the buffer per call** (prelude.c's
`native_ab_to_array`, memcpy for a packed kind and element-by-element otherwise) -- in a loop, exactly the
quadratic cost the builder exists to avoid. The project already knew that shape was a mistake: `--lint`'s
`lint_builder_read_by_conversion` flags it. What it could offer as a remedy, for an array, was "`ab_len`, or
an ArrayBuilder you finish once after the loop", because there was nothing else to suggest.

**What it answers is a real line of the reference backend**, which is where this came from: a loop that
collected matching records into a builder and then converted the whole thing to ask `len()`. Three separable
things were wrong with it, and only the third is the language's: the code wanted a counter, not a builder;
`ab_len` already existed and would have done; and there was no way to read an element back.

**Added: `ab_get`, `ab_set`, `ab_clear`, and `for x in ab`.** `ab_get`/`ab_set` are bounds-checked against
the *length*, not the capacity -- a builder keeps a grown buffer, so reading element 0 of an empty one would
otherwise hand back whatever that buffer happens to hold. `ab_set` overwrites and never extends: appending is
`ab_append`, and setting one past the end is a mistake rather than a push. `ab_clear` keeps the buffer, which
is the point of it -- a builder reused across rounds of work should not pay for the growth again -- and keeps
the packed kind, which is safe because an `ArrayBuilder<T>` is one T for life.

**Iteration reads the live buffer, deliberately, and the alternative was worse.** A snapshot would have been
safe by construction, but `for x in ab` would then have been `for x in ab_to_array(ab)` with nicer syntax and
a hidden O(n) copy -- the exact cost this change exists to remove, moved somewhere a reader cannot see it. So
the loop walks by index and reads the length again on every pass: a body that appends sees its own additions,
as it would in the array the builder is becoming; a body that calls `ab_clear` ends the loop; and neither can
read past the end. Nothing is copied.

**Not added: a pop.** `ab_remove_last`/truncate has an obvious shape and no caller asking for it -- the same
"hardcode the minimum, not general machinery" the `Range` and `BitSet`-builder decisions already took.

**This is step one of two.** The second -- renaming the three builders to `MutArray`/`MutMap`/`MutSet` and
treating them as mutable collections rather than things you finish -- is a separate decision, and the thing
to be careful about is that these are `#[thread_bound]` native handles: that is what keeps them compatible
with single-writer actors over `Channel<T>`, and a name that reads as "an ordinary value I can put in a
struct and send to another thread" would be a worse lie than "Builder" is. Completing the API is worth doing
either way, which is why it went first.

**Extended by** "The builders become mutable collections" (2026-10-13), which settles the second
step: the names are `MutableArray`/`MutableMap`/`MutableSet`, not the `Mut*` spelling guessed here.


## The builders become mutable collections (2026-10-13)

The second step F96 named, decided in full and not yet built: `ArrayBuilder`, `MapBuilder` and
`SetBuilder` become `MutableArray`, `MutableMap` and `MutableSet`, with a surface wide enough that
nothing has to leave the mutable side to get work done. Nothing here exists yet; the names below
are the specification, and the two gates at the end sit in front of the first line of code.

**The disease is the name, and the copies are the symptom.** The builders are named for a phase of
a computation -- accumulate, then freeze -- and the surface enforced it, so the moment a program
wanted to *read* what it had accumulated it had to leave:

```ziman
val entries = ab_new<Entry>()
for row in rows { ab_append(entries, entry_of(row)) }
if len(ab_to_array(entries)) == 0 { ... }     // an O(n) copy to ask what the buffer knows
```

F96 closed the worst of that for arrays. But a handle you can read, index, sort and iterate is not
a builder, it is a mutable array, and the call sites that route around the gap are only half the
cost. The other half is that a model generating Ziman does not look for a builder, so it does not
find the type at all -- which is the measurement that gates this work.

### One prefix per shape, one `mutable_` to cross worlds

*The four type names landed in F103; the function names follow it. So in the commits between,
`ab_new<int>()` returns a `MutableArray<int>` -- the type renamed first because it is the smaller,
self-contained half, and because the bare builtins of F98-F101 mean the busiest call sites
(`ab_to_array`, `ab_len`) collapse into `snapshot` and `len` rather than being renamed twice.*

| shape | value | handle |
|---|---|---|
| array | `[T]` · `array_*` | `MutableArray<T>` · `mutable_array_*` |
| map | `Map<K, V>` · `map_*` | `MutableMap<K, V>` · `mutable_map_*` |
| set | `Set<T>` · `set_*` | `MutableSet<T>` · `mutable_set_*` |
| string | `string` · bare helpers | `MutableString` · `mutable_string_*` |

**`StringBuilder` becomes `MutableString`, and two of the three reasons for keeping its name do not
survive checking.** "Frozen once" is simply wrong: `sb_to_string`'s own doc comment says *"It is a
copy: appending afterwards is fine, and it can be called more than once"* -- the same
snapshot-not-freeze semantics as the other three. "Nothing wants to be read back in place" is the
claim that was true of `ArrayBuilder` right up until F96 found `ab_to_array`-just-to-get-a-length
through the backend, so it is an untested assumption rather than an argument.

What makes the rename cheap is that Ziman's strings are **byte-indexed** already: `len(s)` is bytes,
`char_at(s, i)` is "the one-byte string at `i`", and `char_count` is the separate explicit helper.
The UTF-8 ambiguity that usually makes a mutable string hard was settled for immutable strings long
ago, so `len`, `mutable_string_get` and `mutable_string_set` all mean bytes, consistently.
*(2026-10-23: two of those three moved. `len` on text counts code points now and `byte_len` is the
byte question; `mutable_string_get` is `char_at`'s builder member and `mutable_string_set` is
`set`'s. The byte-indexing itself is unchanged -- `char_at`, `substring`, `set` and the searches
all still take byte offsets, which is what `byte_len` bounds.)*

The honest counterweight: this is the **one rename in the set that loses prior art rather than
gaining guessability**. `StringBuilder` is Java, C# and Go, and a cold model writes it correctly
today; every other rename traded an unguessable name for a guessable one, and this trades a
guessable name for a consistent one. Consistency wins because a model with `MutableArray`,
`MutableMap` and `MutableSet` in front of it extends the pattern -- the same argument as the long
spelling below -- but it is the weakest case of the four. It also drops the thread-bound rule to a
single exception, `Iter`, which is a cursor rather than a container and keeps its name because
nothing better exists.

The long spelling is deliberate. `MutArr` is shorter across 1,200 call sites, but a model that reads
`MutableArray` in a signature writes `mutable_array_push`, and an abbreviation has to be guessed
twice -- once per half. Guessability is the whole point of the exercise, so it wins over typing.

**Each verb means one thing, and matches its immutable twin where one exists.** `push` appends to
an array's tail -- `push(arr, x)` on a value, `mutable_array_push(xs, x)` on a handle. `insert`
puts a thing at a position, shifting what follows (`insert(arr, i, x)`, `mutable_array_insert`).
`set` replaces at a key or index (`set(arr, i, x)`, `set(map, k, v)`, `mutable_map_set`; a
builder's element write is `b[i] = value`, not a `set`, and says so). `add` hands a thing to a set and lets it decide (`mutable_set_add`). `append`
concatenates onto the end of a text buffer (`append`, `append_byte`) and is used by nothing
else -- a fifth verb, taken deliberately, because text genuinely reads as appended and
`sb_append` is what it has always been called. *(Bare since F205; it was
`mutable_string_append`/`_byte` until then, the last builder append still carrying a world
prefix -- this entry chose the verb and never came back for the prefix.)*

> **The prefixed spellings in that paragraph are gone (F208, 2026-10-21).** `mutable_array_push`,
> `mutable_array_insert` and `mutable_array_remove` were the last three, kept as `internal`
> declarations under the bare names that superseded them -- so the verbs this entry chose are now
> the only spelling, and `MutableArray<T>`'s operations are `push(b, x)`, `insert(b, i, v)` and
> `remove(b, i)`. The prefix survives only as the C symbol interpreter.c derives from parameter
> one, which is where this entry's own reasoning put it. What is still prefixed and still a
> declaration: `mutable_array_new` and `mutable_array_filled` (constructors, no bare form),
> their three `mutable_map_*`/`mutable_set_*`/`mutable_string_*` constructor counterparts, and
> `mutable_set_add` beneath `add`. (`mutable_array_get` and `mutable_array_set` were on this list
> for a day: F209 made `b[i]` and `b[i] = value` an index rather than a rewrite onto them.)

The one divergence was that set: immutably `insert(s, x)`, mutably `mutable_set_add`. **Resolved
in F199 (2026-10-20): `add` is the verb, variadic, in both worlds**, and `insert` means
*positional* and only that. The paragraph above judged `add` "unavailable" on the reasoning
below, which no longer holds -- see the correction there.

*The checker caught up in F206 (2026-10-21).* F199 decided the verb and added `add`, but
`insert(aSet, x)` went on compiling for two passes: `push`, `insert` and `set` shared one
`typecheck.c` arm, and it reached the map/set mutator before anything could ask whether a `Set`
had an `insert` declaration. The form was absent from `ziman --api builtins` the whole time and
`docs/SYNTAX.md` still taught it. **A hand-written arm can keep a spelling alive after the
declarations have retired it, and no guard notices, because the arm answers first** -- see
docs/design/full-overloading.md §13.

**`_all` takes the plural of what the singular takes.** `mutable_map_remove(key)` →
`remove_all(variadic K)`; `mutable_map_set(key, value)` → `set_all(Map)`; `mutable_set_add(item)` →
`add_all(variadic T)`; `mutable_array_push(item)` → `push_all(variadic T)`. The singular is never
variadic, which is what keeps `MutableArray<[int]>` from being a special case:
`mutable_array_push(xs, [1, 2])` appends exactly one element, because the only variadic forms are
the `_all` ones.

> **Superseded (F199, 2026-10-20).** The singular *is* variadic now, for `push` and for `add`, and
> the `_all` names they would have taken are gone. The ambiguity this paragraph guards against --
> one element or two? -- turned out to be resolvable rather than structural: the checker compares
> the argument against the **element type** before deciding, so on a `Set<[int]>` the call
> `add(s, [1, 2])` adds the one element `[1, 2]` while `add(s, [[1, 2], [3]])` spreads into two,
> and `push(xs, [1, 2])` on a `[int]` is a type error rather than a guess. `push` had already been
> variadic for some time when this was re-read, so the rule had lapsed in practice before it was
> revisited in principle. What survives is the *reason* the restriction was written: a variadic
> singular needs the element type to disambiguate, and that comparison has to be the thing
> deciding, not the argument's shape alone.

**That restriction applies only to the generic containers.** It exists because of type ambiguity --
one element or two? -- which needs the element type to be a *type parameter*. `MutableString`'s
pieces are concretely `string` and `byte`, and a `byte` can never be an array of bytes, so no
ambiguity is constructible: it keeps variadic `append` and `append_byte`, and needs no `_all`
forms at all.

### The surface

Every mutable collection also answers four bare builtins -- `len`, `contains`, `clear` and
`snapshot`. **`snapshot` is bare rather than per-type**, which is the one place in this design the
call-site count decided the shape: crossing back out is roughly 371 sites (`ab_to_array` 250,
`sb_to_string` 90, `mapb_to_map` 26, `setb_to_set` the rest), second only to the appends and ahead
of everything else combined, so the verbosity of `mutable_array_snapshot` would land precisely on
the operation typed most. Its return type is computed from its argument type -- `MutableArray<T>`
to `[T]`, `MutableMap<K,V>` to `Map<K,V>`, `MutableSet<T>` to `Set<T>`, `MutableString` to `string`
-- which is also not new machinery: `map_keys` already derives `Set<K>` from a `Map<K,V>` argument
the same way. No per-type aliases alongside it: one spelling, the `collections.sort` lesson.

**What that leaves is more work than it sounds, in the other direction.** `len` and `contains` do
**not** accept the builders today -- `len` takes `TY_ARRAY`/`TY_STRING`/`TY_MAP`/`TY_SET` and
nothing else, which is why `ab_len`, `sb_len`, `mapb_len` and `setb_len` exist as separate
functions -- and **`clear` is not a builtin at all**, appearing nowhere in `typecheck.c`,
`interpreter.c` or `prelude.c`. So the bare four means extending two builtins and creating two,
rather than joining a dispatch that was already there. The pattern itself is established: `for`
over an `ArrayBuilder` (F96) already matches a `TY_STRUCT` by `struct_name`, which is exactly what
a handle is.

(As designed in 2026-10; the three `from` constructors below were removed in F151 -- see the
update under rule 2 -- and `mutable(x)` covers the conversion. Everything else still holds.)

```
MutableArray<T>
  mutable_array_new<T>(capacity: Option<int>, grow_factor: Option<float>)
  mutable_array_from<T>(xs: [T] | MutableArray<T>) -> MutableArray<T>
  mutable_array_get<T>(xs, index) -> T                   (removed in F209; `b[i]`)
  mutable_array_set<T>(xs, index, value)                 (removed in F209; `b[i] = value`)
  mutable_array_push<T>(xs, item: T)                     (removed in F208; `push(b, x...)`)
  mutable_array_push_all<T>(xs, items: variadic T)       (removed in F161; `concat(b, parts...)`)
  mutable_array_insert<T>(xs, index, value)              (removed in F208; `insert(b, i, v)`)
  mutable_array_remove<T>(xs, index) -> T                (removed in F208; `remove(b, i)`)
  mutable_array_truncate<T>(xs, length)                  (removed in F158; `narrow(b, s, e)`)
  mutable_array_swap<T>(xs, i, j)                        (removed in F160; `swap(x, i, j)`)
  mutable_array_filter<T>(xs, keep: fn(T) -> bool)
  mutable_array_reverse<T>(xs)
  mutable_array_sort<T: Ord>(xs)
  mutable_array_sort_desc<T: Ord>(xs)
  mutable_array_sort_with<T>(xs, cmp: fn(T, T) -> int)
  len(xs)   contains(xs, v)   clear(xs)   snapshot(xs) -> [T]
  for x in xs    for i, x in xs

MutableMap<K: Eq + Hash, V>
  mutable_map_new<K, V>()
  mutable_map_from<K, V>(m: Map<K,V> | MutableMap<K,V>) -> MutableMap<K, V>
  mutable_map_get<K, V>(m, key) -> Option<V>
  mutable_map_get_or<K, V>(m, key, default_value) -> V
  mutable_map_set<K, V>(m, key, value)
  mutable_map_set_all<K, V>(m, values: Map<K,V> | MutableMap<K,V>)
  update<K, V>(m, key, f: fn(Option<V>) -> V) -> V      (was mutable_map_update; F201)
  update_all<K, V>(m, f: fn(K, V) -> V)                 (was mutable_map_update_all; F201)
  filter<K, V>(m, keep: fn(K, V) -> bool)                 (was mutable_map_filter; F192)
  mutable_map_remove<K, V>(m, key) -> Option<V>
  remove_all<K, V>(m, keys: variadic K) -> bool          (was mutable_map_remove_all; F197)
  mutable_map_keys<K, V>(m) -> Set<K>
  mutable_map_values<K, V>(m) -> [V]
  len(m)   contains(m, key)   clear(m)   snapshot(m) -> Map<K, V>
  for k in m    for k, v in m

MutableSet<T: Eq + Hash>
  mutable_set_new<T>()
  mutable_set_from<T>(s: Set<T> | MutableSet<T>) -> MutableSet<T>
  mutable_set_from_array<T>(xs: [T] | MutableArray<T>) -> MutableSet<T>   (now `to_mutable_set`)
  mutable_set_add<T>(s, item) -> bool                    true if added
  mutable_set_add_all<T>(s, items: variadic T) -> bool   true if at least one was added
  mutable_set_remove<T>(s, item) -> bool                 true if removed
  remove_all<T>(s, items: variadic T) -> bool            (was mutable_set_remove_all; F197)
  filter<T>(s, keep: fn(T) -> bool)                       (was mutable_set_filter; F192)
  union<T>(s, other: Set<T> | MutableSet<T>)             (was mutable_set_union; F196)
  intersect<T>(s, other: Set<T> | MutableSet<T>)         (was mutable_set_intersect; F196)
  mutable_set_difference<T>(s, other: Set<T> | MutableSet<T>)
  mutable_set_is_subset<T>(s, other: Set<T> | MutableSet<T>) -> bool
  mutable_set_items<T>(s) -> [T]
  len(s)   contains(s, x)   clear(s)   snapshot(s) -> Set<T>
  for x in s

MutableString                                      #[thread_bound]
  mutable_string_new(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0)
  append(s, pieces: variadic string)
  append_byte(s, bytes: variadic byte)
  mutable_string_get(s, index) -> string            the one-byte string, as char_at
                                                    (now `char_at`; 2026-10-23)
  mutable_string_set(s, index, value)               (now `set`; 2026-10-22)
  mutable_string_index_of(s, needle) -> int         (now `index_of`; 2026-10-22)
  mutable_string_last_index_of(s, needle) -> int    (now `last_index_of`; 2026-10-22)
  len(s)   contains(s, sub)   clear(s)   snapshot(s) -> string
```

There is no `mutable_string_from`, and it would add nothing: `append(b, s)` already
*is* the from-a-string case, in one call.

`contains` is O(1) on a map and a set and **O(n) on a `MutableArray`** -- one bare name over two
cost classes, which the documentation has to carry because the call site cannot.

### Five rules that decided the rest

**1. A pipeline, not a parallel universe.** The mutable side is `new` → mutate → `snapshot`, and
*no `mutable_*` function returns a handle except the `mutable_*_from*` constructors*. So
`mutable_map_keys` answers `Set<K>` and `mutable_set_items` answers `[T]`: those results are nearly
always consumed by a `for` or returned from a function, and handing back a handle would allocate a
thread-bound object per loop for nothing. The alternative -- every reader staying mutable --
doubles the surface and makes the mutable side somewhere a program lives rather than somewhere it
passes through. This rule is also why there is no `mutable_set_map`: it would have to return
`MutableSet<U>`, and `set_map` already does the job. `set_map`'s own body is
`setb_new`/`setb_add`/`setb_to_set` -- the mutable path, encapsulated, which is where it belongs.

**2. Reading functions accept either world.** `mutable_map_set_all`, both `from` constructors per
type, and `mutable_set_union`/`_intersect`/`_difference`/`_is_subset` take the value *or* the
handle. That keeps the conversion matrix from being thirty functions, and it makes clone free,
since `mutable_map_from(other)` on a handle is a copy. The cost is error messages: a function
accepting two types has to name both when handed a third.

> **Update 2026-10-18 (F151): three of those `from` constructors no longer exist**, so the
> listings above and rule 2's "both `from` constructors per type" describe the design as it was
> built, not the current API. `mutable(x)` now covers immutable-to-builder for all three shapes in
> one word (**four since 2026-10-23, text having joined**), and keeping
> `mutable_array_from`/`mutable_map_from`/`mutable_set_from` beside it would
> have put two spellings on one question — the near-synonym failure mode the AI-first goal exists
> to avoid, and the ground on which `..=` was declined. They were removed, which also removed the
> free clone this rule describes: **a builder can no longer be cloned.** That half had no caller
> anywhere outside its own fixtures, so the clone was a property of the implementation rather than
> something the language was asked for. `mutable_set_from_array` survives, still either-world and
> renamed `to_mutable_set` in F152,
> because it *deduplicates* and so is a different question; rule 3 below still governs it. The
> checker keeps the three as `$`-prefixed internal builtins that `mutable` rewrites onto, so the
> trie-node sharing and the one-allocation buffer sizing described here are all still what runs.

**It needs no new notation**, which is worth stating because the `Set<T> | MutableSet<T>` spelling
above looks like it does. The immutable twins of these nine are *already* hand-checked builtins --
`set_union`, `set_intersect`, `set_difference`, `set_is_subset`, `to_set`, `set_insert_all`,
`set_filter`, `map_filter`, `map_merge`, `map_remove_all` and `map_from_arrays` are all in
`typecheck.c`'s builtin list and **none of them is declared in `lib/prelude.zm`** -- so accepting
two kinds is two branches in `check_call`, not a type-system feature. A union type in the `Type`
representation would be a far bigger change than these nine functions justify, and a language that
refuses general bounds should not acquire general unions for them.

> **None of those eleven names is left (F211, 2026-10-21).** Ten lost the prefix to the bare name
> that now owns the question — `union`, `intersect`, `difference`, `is_subset`, `insert_all`,
> `filter`, `merge`, `remove_all` — over F195 to F201, and `to_set` kept its name because it is
> named for its result. `map_from_arrays` is the last, and it went a different way: it was deleted
> rather than renamed, having no caller anywhere in this repo or the North Star, and a map literal
> says what it said. The paragraph's argument stands, and the hand-checked-builtin state it
> describes is what the conversions have been dismantling ever since.

**3. A fused conversion earns a name only when composition costs an extra full copy.**
`to_mutable_set(xs)` (`mutable_set_from_array` when this was written) is one pass where
`mutable(to_set(xs))` is two, so it exists. `mutable_array_from_set` does not: it is `mutable_array_from(set_items(s))` at one extra
copy and nothing asks for it. Nor does `mutable_map_from_arrays`.

*(2026-10-22: the set-to-builder row exists now, as `to_mutable_array(s)` over both set worlds —
a user asked for it. Worth reading the sentence above again, because the reversal is in its second
clause and not its first: `mutable(to_array(s))` costs exactly the one extra full copy that
`mutable(to_set(xs))` does, so the stated rule admitted this row all along and "nothing asks for
it" is what declined it. The rule is really two tests, cost and demand, and only the cost half was
written as the heading. `mutable_map_from_arrays` is still declined on both.)*

For the two hash types `from` saves something different from a copy: it can duplicate the trie's
nodes with leaves shared, exactly as `mapobj_snapshot` does in the other direction, instead of
hashing and inserting every entry one at a time. Relatedly, **neither `mutable_map_new` nor
`mutable_set_new` takes a capacity hint** where `mutable_array_new` and `mutable_string_new` do: a
bitmap-compressed trie has no single capacity to reserve -- it grows node by node -- while a buffer
has exactly one. The four constructors stay asymmetric for that reason rather than by accident.

**4. Hashed handles iterate a snapshot; indexed handles iterate live.** `for x in xs` over a
`MutableArray` re-reads the length each pass and sees appends made inside the loop (F96). A map and
a set cannot, and the reason is not what it first looks like. `Map` and `Set` are a **persistent
32-way hash array mapped trie** (`src/mapobj.h`, `BITS 5`, bitmap-compressed nodes) -- there is no
table, no load factor and no rehash anywhere in it, because a key's path is fixed by its hash
forever. What forces a snapshot is insertion order: each entry carries the sequence number it was
first inserted with, and `mapobj_entries` sorts by it into fresh arrays, so iterating a
`MutableMap` in insertion order **materialises the entries by construction** rather than by choice.
Secondarily, a node's `slots` array shifts on insert via `realloc`, so there would be no stable
cursor position either. So they walk a snapshot of their keys in insertion order. The consequence for arrays is that sorting or reversing during a `for` reorders
what the loop has not reached, the same class as appending during iteration, and belongs in the
same sentence of the docs.

**5. The library stays in `collections`.** `fold`, `group_by`, `index_by`, `frequencies`,
`binary_search`, `min_by` and the rest return new collections, so routing them through a snapshot
costs one copy and is honest about it; mirroring them onto the handles would double the surface and
hide that. A function is promoted to the prelude only when it has a counterpart already there -- a
`map_*`/`set_*` twin, or a `mutable_*` twin.

### What this changes outside the three types

**`array_sort`, `array_sort_desc` and `array_sort_with` are promoted** to the prelude (**done, F97**), so the
immutable side is no harder to reach than the mutable one. Otherwise `mutable_array_sort(xs)` would
be bare while `collections.sort(xs)` needs an import, which in a value-semantics-first language is
a nudge in exactly the wrong direction -- and it is the rename that creates it. The sorting natives
(`sort_scalars`, `sort_positions`, `sort_scalar_order`) are already in `lib/prelude.zm`, so this is
three thin exports. `collections.sort` and friends stay as the wrappers they already are; 113 call
sites do not move.

**The four that were already bare are documented** (also F97). `array_map`, `array_filter`, `array_flat_map`
and `array_reverse` are natives in `lib/prelude.zm` today with *zero* call sites outside the four
`collections` wrappers that delegate to them -- promoted in fact, invisible in practice, because
SYNTAX.md lists the bare `map_*` and `set_*` helpers and never mentions them. All seven belong in
that list.

Both landed ahead of the rename, because neither needs any of it: see F97 in
docs/reviews/tasks.md.

**`Mutable*` types are thread-bound, as a rule.** One-directional: every `Mutable*` type is
`#[thread_bound]`, but not every thread-bound type is named `Mutable*` -- `Iter` is not, and after
`MutableString` it is the only one. Stating it once beats repeating it in three doc comments, but the cross-thread
error still has to say it at the point of failure, because the name does not. `ArrayBuilder` at
least implied a local scratch object; `MutableArray` implies nothing, which is the lie F96's entry
warned about and the reason the first gate below exists.

**And the rule is checked, not merely documented.** A user type named `Mutable*` must be
thread-bound -- declared `#[thread_bound]`, or transitively containing something that is -- and
otherwise is refused: *"a type named `Mutable*` must be thread-bound -- add `#[thread_bound]`, or
rename it."* Without that, `struct MutableCache { entries: int }` is a plain value, copied on
assignment and freely sendable, wearing a name that promises the opposite, and a reader has no way
to tell which `Mutable*` types the rule covers. That is a false guarantee rather than a style
violation, which is what separates this from R16's documented-but-unenforced convention.

The check is cheaper than it sounds and costs legitimate users nothing, because a struct holding a
handle is *already* thread-bound transitively: `struct MutableRingBuffer { buf: MutableArray<int>,
head: Mutable<int> }` passes with no annotation, and the error fires only on a `Mutable*` name with
nothing mutable inside. The matcher triggers on `Mutable` followed by an uppercase letter or the
end of the name, so `MutablesRegistry` is not caught by accident. Not the converse: a thread-bound
type need not be named `Mutable*`, or `Iter` would have to be renamed and no better name exists for
a cursor.

The four built-in names need no such check -- a builtin type name is already unavailable, though by
two different routes and one bad message: `struct Map` is a syntax error naming a reserved word,
while `struct ArrayBuilder` is *"struct 'ArrayBuilder' is already defined"*, which is misleading
because the user defined nothing. That message should say it is a builtin, whichever names it
guards.

**None of the three is comparable or hashable**, which is a behaviour change rather than a rename.
Today a struct holding a builder handle is accidentally both, comparing pointer identity, which is
never what anyone means. It becomes a typecheck error with its own message. Nothing relies on the
current behaviour: `client.StreamResponse` and the test harness's `Tally` are the only structs with
such a field, and neither compares nor keys them.

### Deliberately absent

- **`push_front`** (`mutable_array_push_front` when this was written) -- it reads as `push`'s
  twin, which is exactly the problem: it would promise amortised O(1) while costing O(n), and a
  loop of them is quadratic. `insert(b, 0, v)` is honest, because nobody reads "insert at an
  index" as constant time. Building in reverse is `push` then one `reverse`. If
  head pushes ever need to be O(1) that is a `start` offset on the buffer -- a representation
  change, roughly twenty lines, and nothing here forecloses it.
- **`mutable_set_map`** -- see rule 1.
- **`is_sorted`, `binary_search`, `lower_bound`, `upper_bound`** on `MutableArray`. The strongest
  case in the not-a-mutation column: reads that would otherwise force an O(n) snapshot to answer an
  O(log n) question. But nothing keeps a sorted `MutableArray` today. *Trigger:* the first program
  that calls `mutable_array_sort` and then searches the result.
- **Deleting the `collections.*` wrappers** in favour of the bare `array_*` spelling. The end state
  is one spelling per operation; the way there breaks 113 call sites for cosmetics. Not now -- but
  the docs should say which spelling is canonical, so the duplication reads as a migration in
  progress rather than a coin flip.

> **Update 2026-10-18 (F161): `mutable_array_push_all` is gone too**, absorbed into `concat`
> along with `concat_arrays`. The overlap this paragraph used to keep -- it overlapped what a
> variadic singular would have covered -- stopped being the question once one name covered
> appending to a builder from either world, any number of parts at a time.

> **Update 2026-10-18 (F157): the `clear(xs)` / `mutable_array_truncate(xs, 0)` overlap is being
> removed**, which this paragraph used to keep for the same "both are cheap" reason. The counting
> is what changed the answer: of twelve `mutable_array_truncate` call sites, **eight were
> `truncate(x, 0)`** -- `clear` written long, byte for byte the same operation, since
> `ziman_builder_clear` on a MutableArray is `length = 0` and so is truncate's clamp. All eight are
> now `clear`. Of the remaining four, two were a fixture's own coverage of truncate and one was
> `truncate(shuffled, wanted)` immediately followed by `snapshot`, which the either-world `slice`
> added in the same pass says in one call and copies only the prefix for. **One caller is left**,
> `lib/graphics/raster.zm`'s `advance_active`, and it is a hand-written
> `mutable_array_filter` -- the same read/write compaction the native does, with the predicate
> inline instead of in a closure. The end state this section already names is "one spelling per
> operation", and the overlap survived only because nothing had counted it.
>
> **Settled in F158: the function is gone.** `narrow(b, start, end)` replaced it rather than
> `mutable_array_filter` -- that site keeps its hand-written compaction, and only the shrink at the
> end of it changed, so no closure enters a rasteriser inner loop and nothing needed measuring.
> `narrow` is the in-place counterpart of `slice`, builder-only for the reason `clear` is, and it
> adds the half truncate could not do: dropping a prefix in one memmove rather than a quadratic
> loop of `remove(b, 0)`. It also does not clamp, where truncate's clamp made `truncate(b, 99)` on
> a length-2 builder a silent no-op.

### Aliasing: three types, three answers

Each needs its own fixture, because the correct behaviour differs:

| call | answer |
|---|---|
| `concat(xs, xs)` | **doubles** the array -- read the source length first, then append. Several aliasing parts append *sequentially*, so `concat(c, c, c)` on one element answers four |
| `mutable_map_set_all(m, m)` | a no-op -- return early on handle identity, never iterate a table being written |
| `mutable_set_union(s, s)` · `_intersect(s, s)` | no-ops, same early return |
| `mutable_set_difference(s, s)` | **empties** the set -- it is `clear(s)`, not a no-op |

### Why `add` is not the array's verb

The tidy answer would have been `add` everywhere: `add(arr, x)` on a value and
`mutable_array_add(xs, x)` on a handle, one verb for "hand it over, the collection decides". It is
impossible, and the reason is worth recording because the result looks arbitrary without it.

A builtin name used to be reserved language-wide -- `fn push(a: int, b: int)` was rejected with
*"'push' is a builtin function and cannot be redefined"*. `push` survived that because nobody names
a function `push`. `add` would not: it is the most common example-function name there is, and it
already appears 14 times in this repo alone -- four `named_args_*` fixtures,
`collections_higher_order`, `fmt_idempotent`, the `38-bits-flags` benchmark and others. Promoting
`add` to a builtin would have made every one of them, and most people's first Ziman function, a
type error.

> **Update 2026-10-18 (F164): that reservation is gone, and with it this argument.** A program's own
> function now **shadows** a builtin of the same name. The asymmetry that prompted it: a `use`d
> module could always declare one, because `typecheck` runs on the root module and an imported
> module's functions never reached the reserved-name check -- so the rule was "the main file may
> not", not "the language may not". F163 made that visible by taking `count`, `sum`, `find` and
> `first`, of which `count` is declared by fifteen files in this tree: five modules that were
> allowed it, ten programs that were not.
>
> So the barrier this paragraph describes no longer blocks `add`. What remains against it is the
> *other* half of the argument above -- that a verb which reads as "hand it over, the collection
> decides" is the wrong shape -- and that half is unaffected.
>
> Shadowing is **not** visible from the library it shadows. The prelude's functions live in the
> root module's own list, so without scoping it to program bodies a program declaring
> `fn len(x: string)` made every `len(...)` inside lib/prelude.zm resolve to it and the prelude
> stopped type-checking. That was found by trying it, after the first two attempts at this had
> already been wrong in two other ways: the builtin arms run before `find_function`, so a declared
> name was accepted and then unreachable; and the registration keyed on a prior `builtin fn`
> declaration, which eleven dispatched builtins do not have.

So the array keeps `push` in both worlds, and `add` survives only where it is namespaced and
collides with nothing: `mutable_set_add`.

### The two gates, and what actually happened to them

1. **Does `#[thread_bound]` survive the name?** Largely discharged by F103/F104, and in the place
   that turned out to matter most: the old name of every renamed type and function now reports its
   replacement, and the type message carries the sentence the name itself cannot -- that a handle
   belongs to the thread that made it and crosses neither a `Channel`, a `spawn`, nor a
   module-level `val`. That is where a reader meets `Mutable*` for the first time, so it is the
   right place for it. The cross-thread *runtime* error wording is still untouched.
2. **Do cold models find the builders at all?** *Never measured, and the migration went ahead
   anyway.* This entry said that number justified the work, so the honest record is that the work
   rests on the other argument instead -- the read-in-place gap F96 found, and a name that
   describes a phase of a computation rather than a type. The benchmark cannot answer the question
   retroactively now: `docs/reviews/benchmark/run.sh` scores attempts rather than producing them,
   and the tasks are scored against whatever the language currently is, so a cold run from here
   measures the new names with nothing to compare against. Anyone wanting the comparison needs a
   cold wave against `2f60699` or earlier.

### Migration

Measured scope, excluding `lib/prelude.zm` itself: **1,103 call sites** (`ab_` 578, `sb_` 436,
`mapb_` 83, `setb_` 6) plus 95 type mentions across `ziman` and `health-record-backend-ziman`, with
docs, fixtures and the C source on top.

Big bang, no deprecation window. A window is for a language with outside users who cannot be
edited, and here it would have cost something specific: while both spellings work, nothing can tell
whether a model reaches for the new name, because the old one still passes. The two repos could not
land together, since renaming the prelude breaks every call site in the same tree.

**What it took, in order** -- F97, then F98-F101, then F103, F104:

1. Promote the three sorts and document the seven `array_*` helpers (F97), which needed none of the
   rest and had to come first for a reason the rename creates: `mutable_array_sort` is bare, and
   shipping that while `collections.sort` still needed an import would make the mutable type easier
   to reach than the immutable one.
2. The four bare builtins (F98-F101), *before* the rename rather than after. That ordering is what
   made the rename cheap: the 250 `ab_to_array` sites collapsed into `snapshot`, and the `*_len`
   sites into `len`, instead of being renamed to a per-type name and then changed again. It also
   found that the entry's claim about these being an existing dispatch was wrong in both directions
   (11b63a9).
3. The types (F103), then the functions (F104). Eleven functions stopped existing rather than being
   renamed, because their question is now one of the four bare ones.

**Three things a mechanical pass cannot do**, each of which bit: it rewrote `derive(Json)`'s
`cb_appendf` format strings without their arguments, which compiled silently because that function
had no `format(printf)` attribute; it turned the `--lint` tables' four conversion names into four
copies of `snapshot`, leaving the lint keyed on nothing; and it wrote "an MutableArray" in 31
messages. None of that was visible without the suite and a reader.

**What was deliberately left alone**: `DESIGN.md`, `ROADMAP.md`, `tasks.md`, the benchmark run logs,
`FEATURE-NOTES.md` and `MEMORY.md`, which hold 285 occurrences of the old names between them. Those
are dated records, true of their date, and rewriting them would have falsified the history that
explains the rename.


## `Mutable<T>`: the root of the family, and why there is no `MutableInt` (2026-10-13)

Designed alongside the entry above and not yet built. One native generic, `Mutable<T>`,
`#[thread_bound]`, holding exactly one value of any type. It is the proposal recorded in
docs/ROADMAP.md as `Cell<T>`; the name changed here, and the reason the name changed is most of
what this entry is about.

**The objection it answers is structural.** Raised while planning to use Ziman on other projects,
UI among them: *"every time we hit a wall we have to change ziman itself to add mutable types
instead of implementing one with ziman."* That is true today. `#[thread_bound]` is already a
user-available attribute whose refusals are transitive -- verified through a struct field, an
`Option<T>` and a `[T]`, on both the `spawn` and the `Channel` boundary -- so the safety story a
new mutable type needs is already there and already generalises. Exactly one thing is closed:
**a field cannot change.** With one cell, it opens: a ring buffer is a struct over a
`MutableArray` plus two `Mutable<int>`s, an LRU is a `MutableMap` plus a `MutableArray` plus a
cell, a UI signal is a cell plus a `MutableArray` of subscribers. The language stops growing a
type each time a new mutable shape is wanted.

### Why `Mutable<T>` rather than `Cell<T>`

The entry above makes "every `Mutable*` type is thread-bound" a rule, and notes it is
one-directional because `Iter` is thread-bound without the name. `Cell` would have made a second
unnamed one, and a rule with a growing exception list stops being worth reading.

`Mutable<T>` is not an exception. It is the **root the other four are specialisations of**:

```
Mutable<T>            a mutable anything
MutableArray<T>       a mutable array
MutableMap<K, V>      a mutable map
MutableSet<T>         a mutable set
MutableString         a mutable string
```

`Mutable` stops being a prefix and becomes a word with a meaning, which is worth more than
matching Rust's and OCaml's spelling: a model that has seen the other four writes `Mutable<int>`
without being told, where `Cell<int>` has to be imported from another language's idiom. And it
costs almost nothing to type, because there is no shape word to carry --
`mutable_set(n, mutable_get(n) + 1)` against `cell_set(n, cell_get(n) + 1)`.

**The cost is one adjacency, accepted deliberately.** `mutable_set(c, v)` sits beside
`mutable_set_new`, `mutable_set_add` and `mutable_set_remove`: the first is `Mutable`'s `set`, the
rest are `MutableSet`'s, and an underscore is all that separates them. Two alternatives were
weighed -- giving the cell a unique verb (`mutable_put`/`mutable_write`), which breaks the
one-verb-one-meaning rule the entry above codifies for a single special case, and `MutableValue<T>`,
which loses the root-of-the-family reading. Neither is worth it: the two names differ in arity and
in first-argument type, so a confusion is a type error at the call site rather than a silent wrong
behaviour, and `set` keeping one meaning across the family is worth a reader's double-take.

### The surface

```
Mutable<T>                                   #[thread_bound]
  mutable_new<T>(value) -> Mutable<T>
  mutable_get<T>(c) -> T
  mutable_set<T>(c, value)
  update<T>(c, f: fn(T) -> T) -> T           the new value, as a MutableMap's update
```

**A cell takes two of the four bare builtins and not the other two.** `len(c)` is 1 and
`contains(c, v)` is "equals the value" (which needs `T: Eq`, checked at the call since `Mutable<T>`
itself is unbounded). `clear(c)` is **not defined**, and that is the same decision as there being no
empty `mutable_new<T>()`: it would need exactly the zero value `T` may not have, and a cell always
holding something is the property worth keeping. `snapshot(c)` is not defined either, because a
cell crosses out through `mutable_get` -- `snapshot` names an O(n) copy and this is an O(1) read, so
they should not share a name. The line for the docs: a cell supports exactly the bare verbs that
need nothing it does not have.

**There is no `from`/`snapshot` pair**, which every other mutable type needs. A handle and a value
are different shapes for an array, a map and a set, so each needs two world-crossings; a cell's
crossing *is* its API -- `mutable_new` takes a value and `mutable_get` returns one. That is why
four functions is enough here where sixteen were needed there.

**Always initialised.** There is no empty `mutable_new<T>()`, because `T` may have no zero. A cell
never holds nothing, so nothing has to say what reading an unset one does.

**`T` is unrestricted, handles included.** `Mutable<MutableArray<int>>` is exactly what the ring
buffer and LRU shapes want. `Mutable<T>` is thread-bound whatever `T` is, and `T`'s own
thread-boundness propagates; both fall out of the existing transitive check without new work.

**Not comparable, not hashable, not a `Map` key**, and neither is a struct holding one -- the same
decision the four builders take in the entry above, for the same reason: a handle's identity is a
pointer value that means nothing across runs, and `==` on it would be identity equality wearing
value equality's clothes.

**No `mutable_swap(c, v) -> T`.** It composes in two lines, it is rare, and the atomicity argument
that justifies it elsewhere does not exist here -- thread-bound means single-threaded by
construction.

**Still not mutable struct fields.** A struct is a value and is copied, so a mutable field means
mutating a copy and wondering why the original did not change. `Mutable` keeps the distinction
visible: the struct stays a value, the cell is plainly a handle. No methods are needed either.

### Why there is no `MutableInt`, `MutableFloat`, …

Two separate questions hide in that one, and the repository has already answered both.

**Speed is a representation question, and Ziman has solved it once already.** `[int]`, `[float]`,
`[bool]` and `[byte]` are stored unboxed behind the ordinary `[T]` type -- a `Value` is 80 bytes,
so a million `[int]` cost 80 MB boxed and 8 MB packed, and `ArrayObj` simply carries an `ints`
field instead of `boxed_items`. There is no `IntArray` type and there does not need to be.
`Mutable<int>` should pick an unboxed slot by `T` the same way, invisibly.

**Convenience is blocked by something else entirely.** `mutable_add(c, 1)` reads better than
`mutable_update(c, fn(x) { x + 1 })`, and it cannot be written generically because the language has
no numeric bound: there are three bounds, `Eq`, `Hash` and `Ord`, and *any other bound name is a
compile error*. That is what makes a specialised type look necessary. It is not the cheapest
answer. Held in reserve, in order of cost: `mutable_add` as a name-special-cased builtin accepting
`Mutable<int>` and `Mutable<float>` only (one function, no bound, like `len` and `push`); then a
fourth bound, which is a language feature with every generic in the stdlib as a candidate; then
specialised types, which is the thing this design exists to avoid. *Trigger:* a real program where
the lambda is measurably in the way, not the aesthetics of `n++`.

A specialised family would not even replace the generic. `Mutable<MyEnum>`, `Mutable<Option<T>>`
and `Mutable<MutableArray<int>>` all need `Mutable<T>` regardless, so it would ship two mechanisms
for one job and leave everyone a choice to get wrong.

### Two things found by testing rather than by argument

**`print` does not diverge on a cycle, and that is not the problem with it.** Library code can now
build one -- `struct Node { name: string, children: MutableArray<Node> }` holding itself -- and
printing it twice is fine, because `print` does not follow a handle:

```
Node { name: a, children: ArrayBuilder { handle: 139806342961664 } }
```

It prints **a raw address**, which is the same defect being removed from `==` and `hash`, in the
one place nobody had looked. A pointer value is not reproducible across runs, so any output
carrying it is unstable by construction -- and nothing depends on it: no file under `tests/` or
`docs/` contains a printed handle address, which is what you would expect of a value that could
never be a stable expectation. So:

> **A handle prints as its immutable counterpart would, and a *nested* handle prints as a summary
> -- never an address.**

*Done in F118. The one thing that needed designing rather than writing was how depth reaches the
renderer: `value_display` recursed without one, and a file-static counter would have been shared
across threads.*

So a `MutableArray<int>` prints `[1, 2, 3]`, a `MutableMap` prints as its `Map` would, a
`MutableString` prints its text (which changes what `sb_to_string`'s comment says today about
`str(sb)` printing the struct), and a cell prints `Mutable(5)`.

Following the contents is what makes a cycle reachable, and summarising a *nested* handle is what
makes it unconstructible: a `MutableArray<Node>` prints its nodes, and each node's own handle field
prints as `MutableArray(2 items)` and stops. That matches the immutable output everywhere the data
is actually tree-shaped, which is nearly everywhere, and needs no new machinery -- where a faithful
seen-set printing `<cycle>` on revisit would be the upgrade if someone later wants real graph
output. Immutable values never need any of this, because a value is a tree and cannot cycle.

This lands with the rename, not after it.

**`#[derive(Json)]` does not refuse a handle field -- it fails incidentally, and invites a fix.**
*(Fixed in F119, for `Row` and `Properties` as well -- the existing field walk covered `Json` and
`Row` only. The check needs two tests for "is this a handle", because derive expansion runs before
the prelude is merged into the module and so cannot look `MutableArray` up at all.)*
A struct with an `ArrayBuilder` field is rejected with "call to undefined function
`array_builder_to_json` ... or write `array_builder_to_json` by hand and import it with `use`".
Write that function and the derive succeeds. Serialising a handle is meaningless, because whatever
is emitted cannot decode back to the same mutable object. This needs to be a refusal **by name**,
saying a handle has no serialisation, for `Json`, `Row` and `Properties` alike -- and for the
existing builders, which have the identical hole today. It matters more after this work than
before, since `Mutable<T>` makes a handle in a struct an ordinary pattern rather than a curiosity.

### Cost, and when

A mutable type built in Ziman is a function call per operation through a handle, where
`MutableArray<int>` is a packed buffer. The native collections stay the fast paths; `Mutable` is
for shapes, not for hot loops.

**Do it when a real program asks** -- the same test every other decision here takes, and not before
something concrete wants a mutable shape the four do not cover. A UI project is the likely first
caller. The cheapest thing to do ahead of it is still a worked actor example: a counter or small
cache owned by a spawned loop and talked to over a `Channel`, which is what shows the thread-bound
story working rather than asserting it.

*Update 2026-10-16: written, as `tests/lang/actor_owns_mutable_state.zm`. **The answer this was
asked for -- whether process-lifetime mutable state needs anything new -- is no, with one exception
that is now fixed.*** The actor owns three `Mutable*` values at once (a `MutableMap` cache, a
`Mutable<int>` counter, a `MutableString` log), serves four threads making a hundred requests
through one `Channel`, and needs no lock, because one thread is the only thing that ever touches
them. Nothing crosses a thread: the state is created *inside* the actor, which is why the
`#[thread_bound]` refusals (the `thread_bound_*` fixtures) never come up -- they are what makes
this the shape that works, not an obstacle to it. The total is exact while the interleaving is not,
and the count is high enough that a lost update would fail the fixture rather than pass by luck.

Two things the exercise found that asserting it would not have.

**A gap, now closed: `send` gave its value argument no type context.** `send(replies, Some(n))` could
not be written at all, even with `replies` declared `Channel<Option<int>>` -- and the error asked
the caller to supply "an argument whose parameter type says which Option it is", which a channel's
element type is. Every reply in the first draft had to be spelled `Option<int>.Some(...)`, which is
the natural shape for a cache `get` and so would have met anyone writing this. `send` and `try_send`
now go through `check_expr_expecting` with the channel's element type, like the `Map`/`Set` builtins
already did; `ok(...)`/`err(...)`/`channel_new()` gain the same context, since they infer the same
way.

**A subtlety worth copying, not a gap: each client needs its own reply channel.** A single shared
one hands a thread somebody else's answer, because a `Channel` delivers to whoever receives first
rather than to whoever asked. The example gives each worker its own and says so, since this is the
kind of thing that works in a single-client test and fails under load.


## What the AI-first goal does and does not justify

(External review, roadmap R20, 2026-09-24.) "Cheap for an LLM to generate
correctly" is a real design forcing function: asking "would a model get this
right?" is a large part of why Ziman is small and orthogonal. It is not a
universal justification, and some restrictions have a different motive that
should be named.

**Restrictions whose motive is the implementation, not clarity.**
- *`?` is not nestable.* The motive is that the tree-walking interpreter
  would otherwise have to unwind out of an arbitrary expression tree. It
  happens to be good style; that is luck, not design. The rule stays.
- *Function values don't capture* (R13 stage 1). This one has a design
  motive, recorded in the R13 entry: a value carrying no environment cannot
  reintroduce shared mutable state or a hidden `var` capture, so the actor
  model is unchanged and the value is safe to send over a channel. It is a
  concurrency argument, not a readability one. The earlier argument that "no
  closures keeps call sites obvious" was weak -- no overloading, no methods and
  no inheritance already deliver that -- and is withdrawn. Capture by value
  (stage 2) stays open.
- *`if` and `switch` are statements.* Same shape: expression forms are an
  interpreter/typechecker change, tracked as R21, not a readability position.

**The training-distribution cost of deliberate divergence.** A model writes
Ziman well in proportion to how much Ziman-shaped code it has seen, and a new
language starts at zero. Every divergence from mainstream syntax, even a
clearer one, costs generation accuracy until a corpus exists. Ziman pays it
in the most common operations: `arr = push(arr, x)` for `arr.push(x)`,
`concat(a, b)` for `a + b`, `switch` with `Enum.unwrap_X` accessors for `match`,
no methods, no `if` expression as an operand, lambdas that must spell out their types and cannot capture a `var`, `for i in 0..n` for the C loop.
Those are accepted for the principles above, knowingly, and the mitigation is
the **diagnostic**: a model's habit should fail at compile time with the
sentence that fixes it. The most-asked-about one of them, `+` on strings, has
its own entry below -- including a measurement that cut its supposed frequency
in half. Probed on 2026-09-24, each habit now does:
`"a" + "b"` (use `concat`/`format`), `a.push(x)` and `s.len()` (no methods; call
`push(a, x)` / `len(s)`), `match` (use `switch`), `s.length` (use `len(s)`),
`for (...; ...; ...)` (no `;`; use `for i in 0..n`), `if` as an expression
(use a `var`), `fn(...) {}` was rejected here until R13 stage 2 made it a lambda, and
`1 + 2.0` (cast with `as`). Adding a habit that produces a vague message is a
defect to fix the way these were.

**Where the goal points the other way.** The errors models make in practice
are cross-file and semantic (a variant added but a consumer missed, an off-by-
one, one identifier mis-pasted in a duplicated block), not `1 + 1.0`. The
features that address those are the ones the goal most supports: exhaustive
`switch` (R4), function values so a route table replaces near-identical
copy-pasted blocks (R13), `--json` diagnostics (R8) and `--lint` (R17). New
work should be weighed against that list before adding another
local-syntax restriction.


## `+` does not join strings (2026-10-10)

The most frequently raised habit collision, and the reason it stays refused is
**principle 1, not coercion**. The earlier note in the backlog called `+` on
strings "implicit coercion by another name"; that argument does not survive
being read closely, and defending a decision with a wrong reason is how a
decision gets reopened every few months.

**The two proposals are different, and only one of them is coercion.**
`"a" + "b"` converts nothing -- both operands are strings, the result is a
string. `"a" + 1` does require a silent `str(1)`, and that one is ruled out
directly by "No implicit coercion" above. So the coercion objection applies to
the second form only, while the habit being argued about is mostly the first.

**What actually rules out `"a" + "b"`:**

1. **Principle 1 forbids it by name:** "no operator overloading". One token
   meaning numeric addition and string joining, selected by operand type, is
   exactly that. This is a principle, not a v0 decision, and it needs no help
   from the coercion argument.
2. **Principle 2, "one way to do a thing"**, says Ziman "doesn't add a second
   way without removing the first" -- and `concat` is the first. The only
   principle-2-compliant version of this change is `+` *replacing* `concat`,
   which would break every call site and is not what anyone proposes.
3. **`+` in a loop is quadratic.** `--lint` already flags `x = concat(x, ..)`
   for precisely this (see R17 and the accumulate-in-a-loop sweep); making it
   an operator makes the trap more inviting and the lint harder to phrase.
4. **A partial `+` teaches "sometimes".** Accepting `"a" + "b"` while still
   rejecting `"a" + 1` splits one habit into two outcomes: a model that learns
   `+` joins strings writes `"n=" + n` next and fails anyway. Today both forms
   fail identically, with one sentence naming `concat`, `format` and `str`. A
   uniform refusal carrying the fix is a better signal than conditional
   success.

**The frequency argument, re-counted 2026-10-10 -- and it was wrong.** The
backlog recorded "24 of 30 cold attempts reach for `+`/`++`", on a split of
16 `+` and 8 `++`. Re-counted by hand from the surviving round-1 attempt files
across all three waves, classifying every `+` by whether its operands are
actually strings (`i = i + 1` is not this habit), the `++` half was right and
the `+` half was not:

| habit | recorded | actual |
|---|---|---|
| `++` (Haskell's operator) | 8 of 30 | **8 of 30** |
| `+` on strings | 16 of 30 | **6 of 30** |
| either operator | 24 of 30 | **14 of 30** |
| no concatenation operator at all | 6 of 30 | **16 of 30** |

Supporting `+` would therefore address **6 of 30 attempts, not 24**, and more
than half of the operator habit that remains is `++` -- which no one proposes
adding, and which principle 2 would refuse on its own as a second spelling of
a thing that already has one. The strongest argument for the change was its
frequency, and the frequency was less than half what the record claimed.

(The two counts disagree because the original appears to have counted `+`
tokens rather than string concatenations: numeric `i = i + 1` and
`(lo + hi) / 2` appear in most of these files. The lesson is the one in the
benchmark README -- a habit count has to name the shape it counted.)

**And the refusal is cheap.** All **14 of 14** attempts that reached for an
operator resolved it: 8 to `format`, 5 to `concat`, and one restructured so it
no longer built a string that way. **None kept the operator.** Agents named
`'+' doesn't join strings ... use concat(a, b), or format("{}{}", a, b)` among
the messages they found decisive, unprompted (see the 2026-10-09 benchmark
report).

**What this does not say.** The round-1 captures hold only the *first*
diagnostic, and only for one of the three waves, so the **number of edits** a
model spends on this specific message is not measured -- an earlier syntax
error usually masks it. If that number ever turns out to be large, the cost
side of this entry is the part to revisit; the frequency and the principle are
not affected by it. Interpolation is not an escape hatch either: 0 of 30 cold
attempts reached for any interpolation form (F38), so adding one would not
absorb this habit.



## Array slices are views

`slice<T>(a, start, end)` returns a new `ArrayObj` whose `items` points at
`parent->items + start`. That is sound for the same reason string views are:
an `ArrayObj` is immutable once built, and `push`/`insert`/`set` always build a
new one. Costs: a slice keeps its whole parent reachable (the GC build's
`libgc` is compiled with `ALL_INTERIOR_POINTERS`, so the interior pointer keeps
the block alive -- the `array_slice` fixture checks a slice outliving its
parent); and anything that later frees or reallocates an array's `items` would
need an ownership flag (nothing does today; a `[byte]` packing, ROADMAP R23,
should design views in from the start). Uses in the library: `bytes_slice`
(which copied element by element through an `ArrayBuilder`) is gone in favour
of `slice<byte>`, which also makes the chunked-response and packet-framing
loops that repeatedly cut a prefix off a buffer O(1) per cut; the merge sort
appends its leftover runs as slices via `ab_append(ab: ..., pieces: ...)`, and
`hashset_items` appends each bucket the same way. The other `ArrayBuilder`
users compute new values (decoders, encoders, `map`/`filter`), which a view
cannot replace.


## `if` and `switch` as values (R21)

**Form.** `val x = if c { a } else { b }`, `x = switch e { ... }`,
`return if ...`. An arm's value is its final bare expression.

**Why only those three positions.** An arm can `return`, `break` or `continue`
(`val n = switch r { Ok(v) { v } Err(e) { return err(e) } }` is the most useful
shape), and a tree-walking interpreter can unwind out of a *statement* but not
out of an arbitrary expression tree; that is the same reason `?` is
restricted (ROADMAP R20 states this plainly). So the value form is a
statement, not an expression node, and `1 + if ...` or `f(if ...)` are syntax
errors with a hint to bind a `val` first. Relaxing it later (arms that cannot
leave, used as operands) is possible without breaking anything.

**Mechanism: no new expression kind.** The parser wraps an ordinary `if` or
`switch` statement in a `STMT_VALUE_BLOCK` (target: val, var, assignment or
return) and turns each arm's last bare expression into a `STMT_YIELD`
(recursively through a trailing `if`/`switch`). Type checking checks the inner
statement as usual, with the target's expected type pushed on a small stack so
each yielded value goes through `check_initializer` -- the val/var initializer
logic, factored out for this -- and therefore gets `?`, `[]`, `ok()`/`err()`,
`channel_new()` and the Option/Result auto-wrap for free. `switch` is rewritten
into its `if` chain as before, so exhaustiveness is unchanged. The interpreter
gets one new control result, `CTRL_YIELD`, which carries an arm's value out of
its blocks the way `CTRL_RETURN` carries a return value (chosen over a
thread-local "yield register" so the value stays on the C stack, visible to the
collector). Scoping, defers and `return`/`break` inside an arm are the
existing statement machinery.

**Decisions.** `if` needs an `else`. Every arm must yield or leave. All arms
have one type; with an expected type they are each coerced to it, without one
the first decides and there is no auto-wrap between arms (write both arms the
same way). A trailing `if`/`switch` in an arm is a value, never a statement
(an arm that must do something conditionally and produce nothing can't be one).

**Effect on the code base.** Nine `var x = ...; if ... { x = ... }` patterns in
the library and the sample backend became `val`s (the backend's
`db_error_response` status, the pg/mysql decoders, `client`'s body
reading, `properties`' profile choice). The lint (`--lint`) counts an
assignment made by a value block.


## Packed byte arrays (R23)

**Before.** Every element of a `[byte]` was an interpreter `Value` (16 bytes:
type header plus union), so a 1 MB buffer cost ~16 MB, and `net.read`, `crypto`,
`string_to_bytes` and `bytes_concat` boxed and copied element by element.
Benchmark (`tests/bench/bytes_bench.zm`: build a 1 MB string, convert to bytes,
sum it, sha256, concat to 2 MB, slice, compare, convert back): **297 MB RSS,
0.33 s.**

**Now.** `ArrayObj` has two storages: `boxed_items` (a `Value` per element, for
every other element type) and `bytes` (a `uint8_t *`, one real byte per
element). The type system is unchanged: a `[byte]` is still `TY_ARRAY` of
`TY_BYTE`; which storage backs a given array is invisible to Ziman code. The
field rename (`items` to `boxed_items`) was done first so the compiler listed
every site that touched storage (37), and all of them now go through
`array_get(a, i)` or check `a->bytes`. Producers pack: array literals of bytes,
`string_to_bytes`, `net.read`/`read_exact`, `random_bytes`, `crypto`, `bytes_xor`,
`concat_arrays`, and a byte-mode `ArrayBuilder` (the first appended element
being a byte switches the builder to raw storage, so `ab_to_array` is one
memcpy and building costs a byte per byte). `push`/`insert`/`set`/`remove` on a
packed array produce a packed array; `==` is `memcmp`, `contains` is `memchr`.
Consumers that need a C buffer (`net.write`, `bytes_to_string`, `crypto`) use
the storage directly or `array_copy_bytes`. Empty arrays stay boxed (there is
no element to type them by), and anything generic that ends up with an
all-byte boxed array is still correct, just unpacked. Storage is allocated
`zalloc_atomic`, so the collector never scans it.

**Views.** `slice<T>` works on both storages (`array_obj_view`): for bytes it
is a pointer into the parent's buffer, an interior pointer that
`ALL_INTERIOR_POINTERS` keeps alive (the `bytes_packed` fixture checks a slice
outliving its parent under the GC build). So the design left the
"anything that frees `items` needs an ownership flag" question exactly as it
was.

**After.** Same program: **66 MB RSS, 0.23 s** (the remainder is interpreter
per-iteration scope garbage from the loop, not bytes). Other results:
`make test`/`test-gc` 507 each, pg, mysql, tls, shutdown and the backend
scenarios (SQLite and Postgres) unchanged.

**Not done.** `ArrayBuilder<T>` for other scalar types (`int`, `float`, `bool`)
still boxes; the same trick would apply, but nothing hot needs it yet.


## Closures: lambdas that capture by value (R13 stage 2)

**Decision.** Ziman gets anonymous functions, `fn(x: int) -> int { ... }`,
capturing enclosing *immutable* locals by value. Captures cannot be `var`s.

**Why by value and no `var`.** The stage 1 reason to have no closures was the
actor model: a value that carries no environment cannot reintroduce shared
mutable state. Capturing a copy of an immutable value keeps that true, since
the lambda and the enclosing function share nothing that either can change, so a
function value is still safe to hand to another thread over a channel. Capturing
a `var` (by reference, or by copy that then diverges) is where the surprises
live, so it is a compile error with the fix in the message. What it costs: a
counter or accumulator cannot be updated from inside a lambda; that state
belongs in a value the lambda returns, or in an actor.

**Why explicit parameter and return types.** Ziman has no inference across
function boundaries (principle 3; a signature is a contract), and a lambda is a
function. It is verbose next to Rust or Kotlin (`collections.map<int, int>(xs,
fn(n: int) -> int { return n * 2 })`), and lifting the annotations when the
expected type is known is a later, separate decision.

**Mechanism.** No free-variable pass. The lambda body is type-checked in a scope
whose parent is the enclosing scope but which is a *capture boundary*
(`Env.is_lambda_boundary`): when `env_lookup` finds a name beyond such a
boundary, in an enclosing function's scopes rather than the global one, it
records the capture on every boundary it crossed (so nested lambdas capture
through each level). The captures then become the lambda's leading parameters
(`FunctionDecl.params[0..ncaptured)`), and the lambda is an ordinary function
body to the interpreter. At run time a function value is a `ClosureObj`
(function plus captured values); evaluating `EXPR_LAMBDA` copies the captured
values out of the current scope, and a call through a function value passes them
ahead of the arguments. A plain reference to a named function is a closure with
nothing captured, so there is one representation. The static type is unchanged:
`fn(A) -> R`, regardless of what is captured.

**Small consequences.** The loader's val-shadowing check sees statements, not
expressions, so a lambda body's declarations are checked while it is walked
(a local may not share a module-level global `val`'s name, as elsewhere). A
lambda is never lifted to the module's function list, so it has no name and
prints as `<function lambda>`. `spawn` still names its function statically (a
lambda can be an *argument* to a spawned function, as the fixture shows).
Generic functions still cannot be used as values; a lambda inside a generic
function may use its type parameters.

**Not done.** Stage 2 does not make `datasource.with_transaction` or `server.serve` drop
their explicit context parameters: those APIs remain correct, and a caller can
now also pass a lambda that closes over what it needs. Expression-bodied lambdas
and inferring lambda parameter types are open.


## Lambdas replace the context-passing workarounds

The `_ctx` and generic-context APIs existed only because function values could
not capture (DESIGN, R13 stage 1 "what this shows about stage 2"). With lambdas
they are redundant, so they were removed rather than kept as a second way:
`collections.map_ctx`/`filter_ctx`/`find_ctx`, `db.with_transaction_ctx`, and
the `<C>` context parameter of `server.serve` (now
`serve(listener, handler: fn(Request) -> Response, options)`, whose per-
connection function is no longer generic). The sample backend's route lookup is
`collections.find<Route>(table, fn(r: Route) -> bool { ... })` capturing the
request, and `server.zm` passes a lambda capturing the connection. Captured
values are immutable copies, so sharing one handler between connection threads
is safe.


## Regular expressions (R15)

**Native, not pure Ziman.** A regex engine is a tight inner loop over bytes;
written in Ziman it would run at tree-walking speed, an order of magnitude or two
slower than the same loop in C, for the one library where users notice. So
`src/regex.c` is a self-contained engine (no PCRE or POSIX `regcomp`: their
semantics differ, and neither is available identically everywhere) and
`lib/core/regex.zm` is the Ziman-side API on top of four small natives
(`compile_raw`, `group_count_raw`, `search_raw`, `search_all_raw`). Matches are
returned as raw offset arrays because a native cannot build an `Option` or a
struct of Ziman enums; the wrapper builds `Match` values with `substring` views.

**No backreferences, no lookaround.** That is the design decision. With them a
regex is NP-hard to match and every backtracking engine can be made to take
exponential time by a crafted pattern or text (ReDoS); a language meant for
servers that read patterns and text from outside should not have that. Without
them the engine is a Pike VM (a Thompson NFA simulation carrying capture
positions per thread): O(text x pattern), with Perl's leftmost/priority
semantics, so results agree with what people expect from `(a|ab)`. The
`regex_no_backtracking_blowup` fixture pins that with the classic pathological
patterns. Repetition counts (`{n,m}`) expand by copying, so they are capped
(1000 each, 20,000 instructions total) instead of allowed to blow memory.

**UTF-8.** Pattern and text are UTF-8, `.` and classes consume a whole
character, offsets are byte offsets (consistent with `substring`, `index_of` and
`char_at`), and `\w \d \s` and `(?i)` are ASCII-only, since Unicode
properties need tables Ziman does not carry.

**Not done.** Named groups, `\p{...}`, POSIX classes (`[[:alpha:]]`),
lookaround, backreferences, and a compiled-pattern cache; a `regex` literal
syntax was considered and rejected (a string plus `compile` keeps one way to do
it, and a bad pattern is a `Result`, not a compile error).


## The formatter (`ziman --fmt`, R17)

**Extended by "Formatter: wrapping, comment alignment, sorted `use`, stdin" below
(2026-10-07, F11), and bounded on 2026-10-08 (A18): nesting deeper than 200
brackets is now refused rather than formatted. The reason is arithmetic rather
than taste -- every line is indented four spaces per open bracket, so formatting
input that is *only* nesting is quadratic in its own size, and 32 KB of nested
blocks produced 256 MB of output at 9.7 GB of peak memory. Real code measured
over 1,119 files reaches depth 10; generated code reaches 7; `<`/`>` are not
counted, so generics are free. The field agrees with refusing: gofmt refuses at
100,000, Prettier dies at about 250 with an uncaught RangeError, clang-format
crashes, and only rustfmt passes the input through unformatted. What the limit
does *not* bound is the output, which was A27 -- **done 2026-10-09: the output is
now bounded too, at 16 MB, checked in `put` where the formatter appends rather
than afterwards.** That is the same cost measured on the axis the depth limit
cannot see: the blowup is O(lines x depth) and A18 had capped one factor, so a
document of ordinary depth and very many lines passed the check and still grew
without limit. At depth 190, inside the nesting limit, 60,000 lines used to
produce 45 MB of output at 235 MB of peak memory and is now refused at 70 MB;
200,000 lines was multiple gigabytes and is refused at 131 MB. 16 MB is about
180x the largest file here (`lib/http/server.zm`, 89 KB, which formats to exactly
its own size). The nesting limit stays, because it fails early and cheaply, which
also means A18's over-refusal of a one-line deeply nested expression stays with
it.** This entry records the formatter as first built, and its
"Not done" list below is what was missing *then*: that later entry is where the
four items on it were added. A reader who stops here gets the state of an earlier
version, which is the hazard F11 set out to fix -- this file is a dated log, so an
entry is true of its date and a later one may be the current answer.

**A text formatter, not a tree printer.** The parser drops comments and layout,
so printing the AST back would delete every comment. `src/format.c` instead
scans the source into items (words, numbers, strings, punctuation, comments)
with the whitespace and newlines between them, and re-emits them under a fixed
set of rules: indentation from bracket nesting (4 spaces per level; a bracket
opened on the same line as another adds no extra level, so `f(x, fn() {` ... `})`
indents once), one space around binary operators and after commas and colons,
none inside brackets or before a call's `(`, `Foo { a: 1 }` and `{ return x }`
spaced inside braces, `{}` empty, at most one blank line in a row and none just
inside a block, trailing whitespace removed, comments re-indented (a trailing
comment keeps its distance from the code so aligned comments survive), a final
newline. It never moves a line break and does not reflow long lines: where you
broke a line is where it stays, and a line ending in an operator indents its
continuation one level. **That sentence was right and the code was not, until
2026-10-12 (docs/reviews/tasks.md F72):** a run of three or more continued lines
indented `+1, +0, +1, +0`, because the flag that stopped the run accumulating
`+1, +2, +3` did it by skipping every other increment. The output was stable, so
`--fmt --check` *required* the zigzag, and 12 files of the reference backend had
already been written that way. Every line of a run now takes the one level this
entry always promised. It needs only lexable text (an unterminated string
literal is the one error), so it works on a file the compiler rejects.

**What it cannot decide without parsing.** `<` and `>` are a comparison or a
generic bracket (`a < b` versus `List<int>`); those keep the spacing the source
had, collapsed to at most one space. That is a deliberate limit rather than a
bug: resolving it needs a real parse, which a text formatter avoids so that it
stays trivial to trust. Everything else it decides from the neighbouring tokens
(a `-` is unary unless it follows an operand; `use a/b` paths keep their `/`;
`i++` stays `i++`; `string...` keeps its dots).

**Safety.** After formatting, the output is scanned again and must contain
exactly the same sequence of code tokens and comments as the input; if not,
nothing is printed or written (an internal error). Formatting is idempotent, and
`--fmt` over the 588 `.zm` files in the repository was checked for it. Only the
library, the sample backend and the extra test programs are formatted
(`make fmt-check`): fixtures that test error positions depend on their exact
layout, so `tests/lang` is left as written.

**Not done.** Line wrapping; alignment of consecutive trailing comments; sorting
`use` lines; an editor integration (`--fmt` reads files, not stdin).


## Lambda shorthand and inference

Two changes to lambdas, both about writing less. **Expression bodies:** when a
lambda's return type is known and not void, its final bare expression is
rewritten to a `return` before checking, so `fn(a: int, b: int) -> int { a + b }`
works and all the usual return handling (autowrap to `Some`/`Ok`, `ok()`/`err()`
resolution) applies unchanged. **Inference from the expected type:** a lambda used
where the context names a function type (an argument of a call whose parameter is
`fn(...)`, a `val`/`var` with a declared type, an assignment, a `return`, a struct
field) takes omitted parameter types and an omitted `->` from it. This is *not*
inference across function boundaries in general: the expected type is written in
a signature or declaration right there, which is the contract Ziman insists on,
and generic calls already spell their type arguments so `fn(T) -> U` is concrete
by the time the lambda is checked. **With no context** the parameter types must
be written; an omitted `->` then means "the type of the final expression" (a void
call as the last statement, or no final expression, makes it void). That rule
changes what `fn(x: int) { foo() }` means when `foo` returns a value: it now
returns it, which is harmless where a void function type is expected (the value
is discarded). A lambda that uses `return` must write its `-> T` (or get it from
context), because inferring a type from scattered `return`s is the kind of
non-local reasoning Ziman avoids.

Mechanism: `Param.type_omitted` and `FunctionDecl.return_omitted` from the
parser; `check_lambda_expecting` takes the expected function type, which the
argument, initializer, assignment and `check_expr_expecting` paths now pass when
the operand is a lambda. Array literal elements do not carry an expected type, so
`[fn(x) { x }]` still needs annotations.


## `if`/`switch` values anywhere an expression can go

R21 restricted the value forms to the right-hand side of `val`/`var`, an
assignment or `return`, because an arm can leave (`return`, `break`, `continue`,
`?`) and a tree-walking interpreter can leave a statement but not the middle of an
expression. Two observations lifted the restriction for the common case:
most arms *don't* leave, and whether one can is decidable by walking the arm's
statements. So there is a second node, `EXPR_VALUE_BLOCK`, wrapping the same
`if`/`switch` statement with yielding arms that `STMT_VALUE_BLOCK` wraps. It is
allowed anywhere an expression is (a call argument, an operand, an element, a
field), and the type checker rejects it if any arm may leave: a `return`, a
`break`/`continue` outside a loop of the arm's own, or a `?` (in a `val`
initializer, a bare statement or a yielded value); nested lambdas are not walked,
their `return` is their own. The interpreter then needs nothing new: it runs the
statement and takes the `CTRL_YIELD` value. Type checking passes the parameter,
declared or field type down as the expected type, so autowrap and `ok()`/`err()`
work in an argument exactly as in a `val`. **Precedence:** a value `if`/`switch`
followed on the same line by a binary operator, `.`, `[`, `as` or `?` is a syntax
error asking for parentheses, because `if c { 1 } else { 2 } + 3` would otherwise
be read either way; the parenthesised form is unambiguous.
Not done: an arm that leaves inside an expression (needs the interpreter to
unwind), which stays statement-only.


## Generic functions as function values

`identity<int>` (or `mod.identity<int>`) is a function value whose type is the
generic function's signature with the type arguments substituted. There is still
no inference: the arguments are written where the function is used as a value,
exactly as at a call, and a missing or wrong-count list is an error that says so.
Parsing: after `name<...>` a following `(` still means a call; anything else makes
it a value (the same one-token lookahead that already tells a generic call from a
comparison). The type arguments ride on the expression (`Expr.fn_ref_type_args`),
are qualified and resolved by the loader like any type, and bounds are checked by
the same routine calls use. At run time a generic function is one erased body, so
the value is an ordinary closure over the same declaration. **Native generics are
excluded**: their C code cannot see the type arguments, so a result's element type
is fixed up from the call site's static type (see the interpreter's generic-native
patch), and a call through a function value has no such site; wrapping one in a
lambda (`fn(a) { slice<int>(a, 0, 2) }`) restores the call site.


## Regex: named groups, POSIX classes, Unicode properties

**Named groups** (`(?P<name>...)`, `(?<name>...)`): names live in the compiled
program; `Regex` and `Match` carry the name list, `group_named(m, name)` looks a
name up, and replacements accept `${name}` and `$name`. Duplicate or malformed
names are compile errors, and `(?<=`/`(?<!` says lookaround is unsupported rather
than being misread as a name.

**POSIX classes** (`[[:alpha:]]`, `[[:^digit:]]`) are ASCII and only valid inside
brackets, as in Perl and Rust.

**Unicode properties** (`\p{L}`, `\P{Nd}`, `\p{^Lu}`, `\pN`, and a few scripts)
need tables. `tools/gen_regex_unicode.py` writes `src/regex_unicode.h` from
Python's `unicodedata` (Unicode 16.0): exact ranges for every general category
and their one-letter unions, about 92 KB of source, checked in so building needs
no Python. **Scripts are approximations:** Python has no script data, so each is
the union of its main blocks (`Han` includes the CJK ideograph blocks, `Greek` the
Greek and Coptic and Greek Extended blocks, and so on); use categories where
exactness matters. To keep a `\p{L}` class (hundreds of ranges) fast, every
character class is now sorted and merged when the pattern compiles and matched by
binary search. `\w \d \s` and `(?i)` are deliberately still ASCII: changing them
would change what existing patterns match, so Unicode-aware matching is opt-in
through `\p{...}`.

**Still not done:** `\p{Script=...}` spelling and script extensions, `(?x)`
verbose mode, case folding beyond ASCII, and `\X`/`\R`.


## The collector is now the default build (R9 follow-up)

**Decision: yes.** `make` builds `./ziman` with the Boehm-Demers-Weiser
collector; `make nogc` builds the old never-frees `./ziman-nogc` (for measuring,
or a platform the collector does not support). Reasons: the leak is not a corner
(the sample backend reached >1 GB in 100 list requests, ~31 GB in the review's
load test), the collector passes every suite under both builds, and the two
objections in the earlier entry are gone.

**The dependency objection.** The collector used to be cloned by a script at
build time. It is now **vendored**: `vendor/bdwgc` holds the ~30 source files
`extra/gc.c` includes, `include/` and the MIT-style license (1.8 MB, commit
`2547e33`, refreshed by `tools/vendor_bdwgc.sh`), and the Makefile compiles it
with the options Ziman needs (`ALL_INTERIOR_POINTERS`, `GC_THREADS`, parallel
marking). Building needs a C compiler and `ar`, nothing else.

**Measurements** (`tests/bench/server_load.sh`, sample backend, 100 requests per
phase, in-memory SQLite; 16 cores). The script's `post`/`list` phases measured
`/patients` when these runs were taken and measure `/api/me/allergies` now (see
its repair below), so the shape holds but the absolute numbers are not
reproducible verbatim:

| build | RSS after health / post / list | list p50 / p99 |
| --- | --- | --- |
| nogc | 19 / 38 / **1,275 MB** (still growing) | 70 ms / 114 ms |
| collector, defaults | 20 / 20 / 24 MB | 132 ms / 177 ms |
| collector, tuned (as shipped) | 42 / 42 / **42 MB** (flat) | 102 ms / 158 ms |

At the same request count with 60 patients, tuned runs were 1.3x the no-collector
list latency (58 ms vs 44 ms p50, 88 vs 70 ms p99). **Pause times** show up as
the p99/max: about 90-160 ms worst case here, on requests that allocate hundreds
of megabytes-worth of garbage; the health and post phases stay under 7 ms max. An
allocation-heavy micro benchmark (`tests/bench/bytes_bench.zm`) runs 2.3x slower
(0.47 s vs 0.20 s), the price of `GC_MALLOC` per small object the interpreter
allocates (a scope, an argument array); the test suite as a whole is within 5%.

**Tuning.** Tried: marker threads 1/2/4, `GC_FREE_SPACE_DIVISOR` 1/2/6, initial
heap 32/64/128 MB, incremental mode. Incremental mode is 3x slower on the
micro benchmark; the divisor barely matters; a 32 MB initial heap avoids the
early collections while the heap grows (fastest per RSS); marker count 2 keeps
parallel marking from using every core of a large machine (default is one per
core, which also made the process show 17 threads). Shipped: 32 MB initial heap,
2 markers, both set in `main` **before the first allocation** (the first
`GC_MALLOC` initialises the collector with defaults, which is how the marker
setting was silently ignored at first), with `GC_MARKERS` and the collector's
other environment variables still taking precedence.

**Consequences.** Threads: the collector's marker threads appear with the first
spawned thread, so the lifecycle test that bounds the server's thread count takes
its baseline after a warm-up request. `make test` runs against the collector
build and `make test-nogc` against the other. **Not done:** memory is not
returned to the OS; sockets, SQLite connections and TLS sessions are closed by
hand as before; and conservative scanning can retain garbage that looks like a
pointer.

**The collector, not the connection pool, is why throughput does not scale with
client threads (2026-09-28, F2/P2-1).** The developer review measured
`GET /api/me/allergies` at 496/654/506 req/s across 1/4/8 client threads on a
16-core box -- flat, not climbing with concurrency -- and named two suspects
without testing either: "GC allocation lock or the single connection-owner
thread per DB connection." Both are now checked. **Ruled out: the connection
pool.** `open_pool` collapses `sqlite::memory:` to one raw connection whatever
`pool-size` says (`lib/db/datasource.zm`'s own comment: "every in-memory
connection is its own private database"), which matches the "single
connection-owner thread" description exactly -- but giving the same benchmark
a real 4-connection pool (a file-backed SQLite database instead of `:memory:`)
did not help: 334/500/379 req/s across 1/4/8, the same flat shape. **Confirmed:
the collector.** `tests/bench/server_concurrency.sh` (new; registers one
account against the real backend, `serve.zm`, then runs `ab` at each
concurrency level) run against `./ziman` (the collector build) and
`./ziman-nogc` (never frees, leaks unboundedly, kept only for measuring) on
identical in-memory-SQLite config:

| build | c=1 | c=4 | c=8 | c=16 |
| --- | --- | --- | --- | --- |
| `ziman` (collector) | 347 | 584 | 429 | 361 |
| `ziman-nogc` | 500 | 1,319 | 1,711 | 2,552 |

The collector build is flat-to-declining past c=4 -- c=16 is *worse* than c=1 --
while the no-collector build keeps climbing through c=16 and is 7x faster
there. Single-threaded (c=1), the gap is small (347 vs 500, the ordinary cost
of `GC_MALLOC` per allocation already measured above); the gap opens
specifically as concurrent request-handling threads increase, which is the
signature of a lock every allocating thread contends on, not a per-allocation
constant cost -- consistent with a stop-the-world pause (every other thread
must halt while one collects) becoming more frequent and more disruptive the
more concurrently-allocating threads there are to halt. The 2-marker,
32 MB-initial-heap tuning above was chosen for RSS and single-request latency;
it was never tuned against concurrent throughput, which is a different
objective. **Not done:** finding a collector configuration (marker count,
incremental mode, a larger initial heap to push the first collection later,
`GC_MALLOC` call frequency in the interpreter's own hot paths) that keeps the
flat RSS win without the scaling collapse -- that is follow-up work, not this
measurement. This does not touch F2/P2-1's other half (raw single-threaded
dispatch speed, `strcmp` identifier lookup) at all; slot resolution or
compiling to closures would still be needed for that independently of
whatever the collector ends up costing under concurrency.

**A tuning pass on the follow-up above (2026-09-28): a bigger initial heap
helps a lot, nothing else tried does, and the scaling collapse is not fully
fixable by configuration alone.** All four levers are plain environment
variables the collector already reads (`GC_INITIAL_HEAP_SIZE`, `GC_MARKERS`,
`GC_ENABLE_INCREMENTAL`), so none of this needed a rebuild. `GET
/api/me/allergies`, `ab -k` (keep-alive, so a thread serves many requests
instead of one, matching how `server.zm` is actually used):

| configuration | c=1 | c=8 | c=16 |
| --- | --- | --- | --- |
| shipped (32 MB heap, 2 markers) | 389 | 609 | 473 |
| `GC_INITIAL_HEAP_SIZE=512M` | 509 | 897 | 633 |
| `GC_INITIAL_HEAP_SIZE=2G` | 483 | 964 | 661 |
| `GC_INITIAL_HEAP_SIZE=512M` `GC_MARKERS=8` | 520 | 931 | 638 |
| `GC_INITIAL_HEAP_SIZE=512M`, with `-k` | 596 | 1,176 | 831 |
| `GC_INITIAL_HEAP_SIZE=512M` `GC_ENABLE_INCREMENTAL=1`, with `-k` | 365 | 143 | 144 |
| `ziman-nogc`, with `-k` (ceiling reference) | 687 | 3,384 | 3,996 (c=32: 3,989) |

A bigger initial heap is a real, free win at every concurrency level (30-80%
over shipped) with no downside found in this test -- **except the one the
32 MB default exists for**: the collector became the default build
specifically to keep RSS flat and low (the measured "42 MB flat" result
above), and a 512 MB-or-larger initial heap spends that budget up front
whether or not the workload ever needs it, which is a real cost for a small
deployment even though it is invisible to a throughput benchmark. More marker
threads add nothing once the heap is bigger (confirms the existing 2-marker
choice, made to avoid a many-core machine's markers competing with request
threads, was not leaving throughput on the table). Incremental mode is not
just slower on a single-threaded micro-benchmark as already measured -- it is
*catastrophic* under this concurrent workload (about 8x slower at c=8, worse
than useless), almost certainly its write-barrier/dirty-page tracking
fighting the same many-threads-mutating-concurrently shape that makes
stop-the-world expensive here in the first place; reject it for this
workload, not just deprioritise it. **None of it removes the ceiling**: even
the best configuration found (bigger heap plus keep-alive) still peaks around
c=8 and *declines* by c=16, while `ziman-nogc` keeps climbing past c=32 --
the gap between them (3,996 vs 831 at c=16, 4.8x) is only a little smaller
than the untuned gap was. Closing it needs either less garbage generated per
request (fewer `GC_MALLOC` calls in the interpreter's hot paths -- ties back
to F2's other half, since a slot-resolved or closure-compiled interpreter
plausibly also allocates less per call) or the deeper memory-reclamation
redesign already on the roadmap ("Larger, needs its own design pass":
refcounting or per-thread/per-request arenas), not a configuration change.
**Not done, and a real decision for whoever picks this up:** whether the
bigger initial heap is worth shipping as the new default given the RSS
trade-off, possibly as a `datasource`/`server`-style configurable rather than
a hardcoded constant in `main.c`, so a deployment can choose its own point on
the throughput/RSS trade-off instead of the project picking one for everyone.


## Bounded channels and backpressure (R10 remainder)

`channel_new(capacity)` makes a bounded channel: `send` blocks while `capacity`
values are waiting, so a fast producer is held to the pace of a slow consumer
instead of growing a queue without limit (the unbounded queue was the review's
"a DoS primitive by design"). `channel_new()` is unchanged, since the connection
semaphore in `server.serve` and the pool's idle list rely on unbounded, pre-filled
channels, and a bounded default would break them. Also added: `try_send(ch, v) ->
bool` and `try_receive(ch) -> Option<T>` (never block; the `Option` is built by
the interpreter with the call's resolved type, the way `?` builds its propagated
`None`) and `channel_len(ch)` (how many are waiting). Capacity 0 (a rendezvous
channel) is rejected: it needs a sender and receiver to meet, which is a different
protocol from a queue, and nothing needs it yet. **Implementation:** the queue
is now a ring buffer (`head`, `count`, `capacity`) with two condition variables,
`not_empty` and `not_full`; the old array shifted every element on each
`receive`, which made draining a long unbounded queue quadratic. **Limits:** no
`close`/end-of-stream on a channel, no `select` over several, no timeout on
`send`/`receive` (poll with `try_*` and `clock.sleep_millis`), and a bounded
`send` with no receiver blocks forever, with no deadlock detection.


## Packed storage for `[int]`, `[float]` and `[bool]`

R23 packed `[byte]`; this extends the same design to the other scalar element
types. A `Value` is 80 bytes, so a million `[int]` cost 80 MB (and the collector
had to scan and copy it); packed it is 8 MB of pointer-free memory the collector
never scans. `ArrayObj` now has, besides `boxed_items`, one of `bytes`, `bools`,
`ints` or `floats` (at most one non-NULL, chosen by the element type) -- separate
fields rather than a tag, so every existing `arr->bytes != NULL` fast path
(`memcmp`, `memchr`, `net.write`, `sb_append_byte`) stays exactly right for
`[byte]` and never misfires on a bool array that shares the byte width.
`array_get` returns the right `Value` for each kind, `array_obj_view` (slices)
offsets whichever buffer is set, and `array_packed_edit` does `push`/`insert`/
`set`/`remove` on any packed kind in one place. **Producers that pack:** array
literals of one scalar type, `concat_arrays`, a builder (the first appended
element's type picks the buffer, so `ab_new<int>()` is a raw `int64` buffer and
`ab_to_array` one `memcpy`), `push` onto an empty boxed array, and the natives
that return `[int]` (`clock.now_raw`, the regex offsets). Anything else that ends
up an all-scalar boxed array is still correct. Empty arrays stay boxed (there is
no element to type them by) until they grow. `==` is `memcmp` for `[int]`/
`[bool]` and per-element for `[float]` (`NaN != NaN`, `-0.0 == 0.0`, unchanged).
**Measured** (`tests/bench/scalar_arrays_bench.zm`: three million-element arrays
built, summed, one sorted, compared): **52.9 s and 2.7 GB before, 7.9 s and 80 MB
after** (the ratio is inflated by the collector working through the boxed
arrays; either way `[int]` in a hot path was the most expensive thing in the
language). Not done: packing `string` elements (a `{ptr,len}` view is 16 bytes,
so a packed string array is possible but touches every string native), and
`decimal`/`bigint` arrays.


## Reporting several errors per run (roadmap open item 8)

A model (or a person) fixing a program used to need one compile per error. Now a
run reports every error it can find, up to 25, in source order, ending with
`ziman: N errors` (JSON mode prints just the objects, each now with a `file`
field, like warnings).

**Mechanism: recovery points, not error values.** `ziman_error` used to print and
`exit(1)`. Now, while a *recovery point* is active, it prints and counts the
diagnostic and `longjmp`s to the innermost one; with none active (running the
program, or code that does not recover) it behaves as before, so every one of the
hundreds of existing `ziman_error` call sites is unchanged. Recovery points are
installed only where carrying on is sound: **the parser** around each top-level
declaration (after an error, skip to the next line that starts a declaration with a
keyword in column 1 and continue); **derive** around each `#[derive]`; **the type
checker** around each function and around each *statement* (so a bad line in the
middle of a function does not hide the rest of it). A file with syntax errors is
not type-checked, since the missing declarations would only produce echoes. Errors
before the function bodies (a bad struct, a global `val`) and in the loader stay
first-error-wins.

**Cascades are suppressed, not merely counted.** When a `val`/`var` statement fails,
its name is still defined but *poisoned*; a later statement that reads or assigns
it is skipped silently (a quiet jump to the recovery point). Without this, one
undefined function would be reported again at every line that uses its result.
**Extended by (2026-10-12, F95) "`let x = 1` is parsed as the `val` it meant" below**: this
mechanism only ever covered a statement the parser recognised *as* a declaration, which is why
`let x = 1` -- not a declaration to the parser at all -- cascaded anyway.
The checker's own state (the value-block context, the generic parameter list, lint
state) is saved and restored around each recovery, because the jump leaves it
pointing into frames that no longer exist; loop variables around a `setjmp` are
`volatile`.

**Files.** `ziman_set_error_file` is set to each function's source file while it is
checked; text output shows ` in <file>` only for a file other than the entry file,
so a single-file error reads exactly as before, and JSON always carries `file`
(`null` for runtime errors, which do not know which module is running).

**Limits.** Recovery is per statement, so a later error in the *same* statement is
not seen; an error inside a lambda body or a value-block arm aborts the statement
that contains it; and an error in a module's top-level declarations (structs,
enums, global vals) or in `use` resolution is still fatal on its own. Runtime
errors are unchanged.
