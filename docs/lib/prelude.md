# `prelude`

The functions and types every Ziman program has without a `use`, split across
this directory by the type each name is about: array.zm, map.zm, set.zm, string.zm, option.zm,
result.zm, channel.zm, iter.zm and mutable.zm.

This file holds what belongs to no collection at all: the universal verbs `print`, `str`,
`hash` and `compare`, and parsing a number out of a string.

**`abs`, `min` and `max` were here until 2026-10-22 and are in the `math` module now**, so they
are `math.abs(x)` and `math.min(a, b)` behind a `use math` rather than bare vocabulary. The
prelude is the set of names a program gets without asking for them, and arithmetic beyond the
operators is not in it: `math.floor` and `math.round` were already a `use` away and are no more
specialised than `math.abs`. It cost 76 call sites across four trees, the backend repo included,
which is the honest measure of how much bare vocabulary is worth -- and the reason to spend it
is that `print` and `str` really do belong nowhere else, where these three had a module waiting.

Part of Ziman's prelude: every file in lib/prelude/ is merged bare into every program by the
loader, so everything here is callable with no `use` and `use prelude` does not resolve. In this
directory `pub` means what it means anywhere else as of 2026-10-22: **a declaration without it
cannot be called from outside the prelude.** `internal` reaches the bundled library's own
package, so lib/ may call one and a program may not, and a bare declaration belongs to the file
that declares it. It also still chooses what `ziman --api builtins` advertises.

That is a change. Until then `pub` said nothing about reachability here -- the prelude is merged
bare into every program, so a call into it resolves as an ordinary same-module call and there was
no boundary to check at -- and F172 relied on the gap, leaving names hidden from completion *and*
callable. Seven were on the far side of it, and every caller outside the prelude moved to the
public spelling first.

**Every public declaration here is written `pub` as of 2026-10-22, including the ones where it
changes nothing.** It is inert on a `native fn` or a `builtin fn`: those are advertised through
`fn_is_global_vocabulary` whatever their visibility says, so 143 of them carried no modifier at
all while 125 of their neighbours did -- one keyword, two meanings, depending on the kind of
declaration it sat on. Writing it everywhere costs nothing (`--api builtins` is byte-identical
across the change, which is how the sweep was checked) and buys the reading that **a bare
declaration in this directory is deliberately not public**. There is exactly one, `array_order_of`
in array.zm, which F172 decided should stay invisible, plus the six `internal` natives.

What `pub` does *not* do here is gate reachability, and that is a property of the prelude rather
than of `native`: these files are merged bare into every program, so there is no import boundary
for a non-public declaration to be refused at. In a *module* `pub` is load-bearing in the
ordinary way -- `metrics.zm`'s own `native fn declare_raw` is private, and
`metrics.declare_raw(...)` is refused with "add 'pub' to export it". Native functions are registered by prelude_register() in src/prelude.c, and each file's
header is generated from its source by the Makefile's prelude rule.

**A declaration lives in the file for the type its first argument has.** So `concat`'s string
form is in string.zm and its array forms in array.zm, and `get_or`'s `Option` form is in
option.zm with its two map forms in map.zm -- one overload set, read where each member's type
lives.

**`ziman --api builtins` prints an overload set as one block**, so the listing answers "what
does this name accept" in one place even when the sources do not.

That sentence was here, in all ten of these headers, as the reason splitting a set across files
was safe -- and **it was false for a day**. The listing is built in file order, which grouped
every set only while no set was ever split; checked on 2026-10-22, `filter` came out in two
blocks 56 lines apart, `concat` in two, `get_or` in two, and `snapshot` and `clear` in **four**
each. The listing now groups by name (a set appears where its first advertised member does, so
the file order's topical reading survives), which made the claim true rather than deleting it.
A pure reordering: 307 lines before and after, identical as sets.

`docs/lib/prelude.md` is **not** grouped -- it is organised as one `##` section per file, so a
split set still has a `###` heading in each. The listing is the one place to read a whole set.

**There were eleven parts until 2026-10-22; collections.zm was the eleventh and is gone.** It
held the names that were about no one type, under the older rule that an overload set is never
split -- and the filing rule reversed that, so its names left one at a time: `concat`, `get_or`,
`update`, `set`, `remove_all`, `snapshot`, `clear`, then `filter` and `remove`, then `map`,
`flat_map`, `len`, `contains` and `mutable`. The last five went together, 28 declarations, and
emptied the file. Deleting it took the edits adding a part takes, reversed: the file, the
Makefile's `PRELUDE_PARTS`, and two lines in src/prelude.c -- an `#include` and a row in its
`PARTS` table.

What that file's header carried and this one now does: the parameter-name scheme below, the rule
that a set's members sit together within whichever file they land in, and the one piece of work
it left behind, **which is now done**: map.zm and set.zm were still grouped by world -- `get`,
`keys`, `values` and `merge` split in one, `add`, `union`, `intersect`, `difference` and
`is_subset` in the other, each with a builder block and an immutable block between them. Every
set's members sit together in every file now, immutable first.

It was "a larger piece than it looks" for the reason recorded: fourteen of those declarations
had no doc comment, so moving them was the small half and writing what they do was the rest.
map.zm's `values` and set.zm's whole thirteen-declaration tail published blank entries in
`docs/lib/prelude.md`; none does now. **The rule is that a set's members are adjacent, and no
file breaks it** -- which was worth re-running a detector after any move rather than trusting
that nothing drifted, and is checked by `tools/check_set_adjacency.py` as of 2026-10-23 so that
nobody has to remember to. It was added after a rename split `index_of` across 107 lines of
string.zm with the build, the suite and every other guard green: `mutable_string_index_of`
became a member of `index_of` and stayed where it sat, which is a thing only this rule notices.
A reader caught it instead, which is the gap a guard is for.

**Parameter names are part of the call surface**, because anything not dispatched by a builtin
id takes named arguments -- so one role gets one name across every declaration that has it,
whichever type it happens to be. The receiver of a sequence is `items` (both `[T]` and
`MutableArray<T>`) -- unambiguously so since 2026-10-22, when the `items` *function* became
`to_array` and one word stopped naming a function, a receiver and a variadic payload at once --
of a map `m`, of a set `s`, of text `s`, of a `Result` `result`, of a
channel `channel` and of a cell `cell` -- those last three are whole words, and the two that
were `c` until the names were swept are why: one letter meant two types, and a letter is always
one type away from that. `result` was `r` until 2026-10-22 for the same reason. The prefix era named them for their world -- `a`/`b`, `m`/`mb`, `s`/`sb` --
which is the thing F205 recorded as the wrong half of a rename, and it was still half-applied
until the names were swept.

**A role name beats the type name when it says more**, and those are kept deliberately rather
than flattened: `sorted` on `lower_bound`/`upper_bound`/`binary_search` states the precondition
at the call site, `parts` on `concat` and `pieces` on `append` name what is being joined,
`entries`/`item` on `mutable` name the source being converted, `other` is the second operand of
a set operation, `keep` on `filter` says what true means where `test` on `any`/`all`/`none`
merely asks, `separator` is `join`'s and `split`'s, `haystack`/`needle` are a search's, and
`i`/`j` on `swap` are a symmetric pair that `index` would not improve.

That reverses the older rule, which kept a split set whole in collections.zm -- **and that file
is gone**, retired on 2026-10-22 once the last of its names had been filed by type. **Every set
splits, however many files that means**: `len` and `contains` across five each, `snapshot` and
`clear` across four, `filter`, `remove`, `mutable`, `set` and `map` across three, `concat`,
`get_or`, `update` and `flat_map` across two or one. Nothing is exempt and nothing is left over.
`remove_all` was in this list until 2026-10-22, when `remove` went variadic and absorbed it --
a set can leave the list by being merged into another, not only by being filed. See docs/DESIGN.md's "Overloading available to a declaration",
and prelude.zm's header for what collections.zm's own header carried.

**result.zm was added the same day, and is the rule read forwards rather than backwards.**
`Result<T, E>` had shared option.zm with `Option<T>` "because they convert into each other and
because `unwrap` is one overload set over both" -- the same reasoning the rule replaced, so
`unwrap` has one member in each file now and each conversion sits with its source. Retiring one
part and adding another are the same edits, and there are four, not the three this said until
2026-10-22: the file, the Makefile's `PRELUDE_PARTS`, and *two* lines in src/prelude.c -- an
`#include` of the generated header as well as the row in its `PARTS` table. Leaving the
`#include` out stops the build with `PRELUDE_<NAME>_SOURCE undeclared`, which is how the
miscount was found: by adding a part and hitting it.

*This module is never imported: everything it declares is available bare in every program.*

## Example

```
// Everything here is available without a `use`.
fn main() {
    // Parsing returns a Result, so bad text is a value you handle.
    print(parse_int("42"))
    print(parse_int("4x2"))

    // Strings are UTF-8 and positions are bytes.
    print(split("a,b,,c", ","))
    print(split_once("key=a=b", "="))
    print(replace("aaa", "a", "b"))
    print(join(prefix: "[", suffix: "]", separator: ", ", pieces: ["x", "y", "z"]))

    // Option is an ordinary enum.
    val found = split_once("no separator here", "=")
    print(get_or<[string]>(found, ["", ""]))

    // Builders assemble a result without copying it on every step.
    val sb = mutable_string_new()
    for i in 0..5 {
        append(sb, str(i), ",")
    }
    print(snapshot(sb))
    val ab = mutable_array_new<int>()
    for i in 0..5 {
        push<int>(ab, i * i)
    }
    print(snapshot(ab))
}
```

Output:

```
Ok(42)
Err(cannot parse '4x2' as int)
[a, b, , c]
Some([key, a=b])
bbb
[x, y, z]
[, ]
0,1,2,3,4,
[0, 1, 4, 9, 16]
```

## `prelude.zm`

### `parse_int`

```
pub native fn parse_int(s: string) -> Result<int, string>
```

The number in `s`, or an Err naming the text when it is not one or does not fit:
`parse_int("9223372036854775808")` is an Err. `parse_int` takes an optional leading `-` and decimal
digits only: no blanks around the number, no `+`, no `0x`, no `_` (`trim` first to allow blanks).

### `parse_float`

```
pub native fn parse_float(s: string) -> Result<float, string>
```

A float from decimal or exponent text (`3.5`, `1e3`), or an Err: like `parse_int`, no blanks and no `+`, and
no hex (`0x10`). `nan`, `inf` and `-inf` (what `str` prints) are the non-finite floats it reads.
Anything `str` writes reads back, including a subnormal such as `5e-324`. Text naming a value
smaller than any subnormal (`1e-400`) rounds to `0.0`, the same rounding that makes `0.1` inexact;
text naming one too large (`1e400`) is an Err rather than silently becoming infinity, which is why
`inf` is accepted by name but `1e400` is not.

### `parse_bigint`

```
pub native fn parse_bigint(s: string) -> Result<bigint, string>
```

A bigint from digits with an optional sign, or an Err. Any length.

### `parse_decimal`

```
pub native fn parse_decimal(s: string) -> Result<decimal, string>
```

A decimal from digits with an optional fraction, keeping the scale as written (`"1.50"` stays
`1.50`), or an Err.

### `print`

```
pub builtin fn print<T>(x: T)
```

**`print` takes a type parameter, not a top type.** `print` really does accept anything --
a lambda, a `Channel`, a `Set` -- but an ordinary `<T>` already says that and already works:
`fn log<T>(label: string, value: T) { print(value) }` compiles and prints all three. A top type
would be a new concept earning a single declaration, and a type whose values you could accept
and then do almost nothing with, so `print` uses the generics the language already has.

### `str`

```
pub builtin fn str(x: int) -> string
```

**Six scalars, and not `string`** -- `str("x")` is refused with "it's already a string", so
`str` is not an "accepts anything" builtin however much it looks like one. The set of
conversions is closed, which is why it is six overloads rather than one signature over a
type parameter: `str` on a lambda or a `Set` has no answer to give.

```
pub builtin fn str(x: float) -> string
```

```
pub builtin fn str(x: bool) -> string
```

```
pub builtin fn str(x: bigint) -> string
```

```
pub builtin fn str(x: decimal) -> string
```

```
pub builtin fn str(x: byte) -> string
```

### `hash`

```
pub builtin fn hash<T: Eq + Hash>(x: T) -> int
```

### `compare`

```
pub builtin fn compare<T: Ord>(a: T, b: T) -> int
```

Negative, zero or positive; only the sign is meaningful.

## `array.zm`

### `bytes_to_string`

```
pub native fn bytes_to_string(bytes: [byte]) -> Result<string, string>
```

The text of `b`, or an Err when `b` is not valid UTF-8 (naming the byte offset of the first
invalid sequence) or holds a 0 byte (a string cannot) -- the one real boundary where untrusted
bytes (a socket, a file, an HTTP body) become a `string`, so it's the one place this is
checked; every other string function keeps working on whatever bytes a `string` already holds,
same as always. See docs/DESIGN.md's UTF-8 correctness entry.

### `slice`

```
pub native fn slice<T>(items: [T], start: int, end: int) -> [T]
```

The elements from `start` up to, not including, `end` (`0 <= start <= end <= len(x)`, a runtime
error otherwise), as an array from either world.

**From an array it does not copy.** Arrays are immutable, so the result shares `a`'s storage,
like `substring`: free to build, and it keeps the whole of `a` alive.

**From a MutableArray it copies the window, and must.** A view would share a buffer that can
still be pushed to, written through or truncated underneath it. So the answer is the same type
either way and only the cost differs -- and taking the builder directly is the point, because
`slice(snapshot(b), s, e)` copies the *whole* builder first (`snapshot` being O(n), the slice of
it then free), so a three-element window of a ten-thousand-element builder copied ten thousand
elements.

**`native fn`, not `builtin fn`, since F214.** The two members are the resolver's to choose
between, so `B_slice` and its arm are gone and each declaration finds its own C symbol through
parameter one: `array_slice` and `mutable_array_slice`. Nothing about the call changes -- `slice`
already took named and type arguments, being listed in `BUILTINS_WITH_NAMED_ARGS`, and its
diagnostics were already derived from these two signatures rather than hand-written, which is
what made it the cheapest of the remaining polymorphic builtins to convert.

```
pub native fn slice<T>(items: MutableArray<T>, start: int, end: int) -> [T]
```

### `concat`

```
pub native fn concat<T>(parts: variadic [T]) -> [T]
```

**Every part, in order, joined into one new array** -- `concat`'s array world. The string world
is in string.zm and carries the three reasons no call can pick among these four declarations,
which is what `#[compiler_resolved]` stands in for.

**One argument that is an array of arrays spreads**, its elements being the parts, the same rule
the string form carries: `concat([[1], [2]])` is `[1, 2]`, as is `concat([1], [2])`. What cannot
be said is concatenating a single array as one part, which is `slice(items, 0, len(items))`.

```
pub native fn concat<T>(items: MutableArray<T>, parts: variadic [T])
```

**Appending to a builder answers nothing**, the container having been written. The parts may
come from either world **and may be mixed** -- `concat(b, [5], mutable([6]), [7])` -- which
`mutable_array_push_all` allowed for its single source and this keeps for all of them. That
mixing is reason 3 in string.zm: a variadic parameter has one type, so these two declarations
each state half of the tail a call may pass and the rule sends the whole of it to one C
function. They do not tell each other apart either -- `concat(b)` passes an empty tail, which
fits both -- so both carry the attribute.

```
pub native fn concat<T>(items: MutableArray<T>, parts: variadic MutableArray<T>)
```

### `mutable_array_new`

```
pub native fn mutable_array_new<T>(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> MutableArray<T>
```

An empty builder; `capacity` and `grow_factor` are as in `mutable_string_new`.

### `mutable`

```
pub builtin fn mutable<T>(items: [T]) -> MutableArray<T>
```

**The MutableArray holding `items`' elements** -- `mutable`'s array member, and `snapshot`
backwards: `snapshot` answers the immutable type a builder is building, `mutable` the builder a
collection is built with. Its Map and Set members are in map.zm and set.zm, and the member for
everything else is in mutable.zm, which carries the account of how the four are told apart.

**`mutable([])` takes its element type from the context**, `[]` being the one literal that
carries none of its own: `val xs: MutableArray<int> = mutable([])`, a parameter's declared type,
a `return`, or a struct literal field all supply it. With no context there is nothing to infer
from and the error says so, naming `mutable_array_new<int>()`, which needs none.

**A literal argument is built straight into the builder.** `mutable([1, 2])` makes the
MutableArray from the literal's elements without collecting them into an array first -- the
checker hands them over as the call's own arguments, which is what `$map_lit` has always done --
so three allocations and about 154 bytes per construction do not happen. Nothing observable
changes; `mutable(xs)` on a variable still copies, because there the copy is the point.

### `mutable_array_filled`

```
pub native fn mutable_array_filled<T>(length: int, value: T) -> MutableArray<T>
```

**`length` copies of `value`, every slot addressable.** `mutable_array_new`'s `capacity` is not a
length: it reserves room and leaves `len` at 0, so `b[0] = v` after it is a runtime
out-of-bounds, and the only way to get slots you can index was to push them one by one. This is
the "n slots" constructor -- `[0] * n` or `new int[n]` elsewhere.

Both parameters are required on purpose. A `fill: Option<T>` on `mutable_array_new` would make
that function's first parameter mean *capacity* without a fill and *length* with one, and an
argument whose meaning depends on whether a later one was passed is exactly what this library
keeps removing. Required also means it calls positionally -- `mutable_array_filled(5, 0)`, with
`T` read from `value` -- where an `Option` parameter would have forced a named call.

There is no immutable twin: `snapshot(mutable_array_filled(n, v))` is the way to an `[T]` of
repeats, and `array_filled` would have to be a second name rather than an overload, differing
from this only in what it returns.

### `len`

```
pub native fn len<T>(items: [T]) -> int
```

**Its length**, O(1) in both worlds. `len`'s nine members are one C function registered under
the bare name -- the declarations say only which types may be asked the question, and this is
the one place a bare native registration is right. The other seven are in string.zm, map.zm,
set.zm and mutable.zm, two per file but for the cell's one; `--api builtins` prints all nine as
one block.

**Nine declarations over one implementation is not a layer**, and the note that used to stand
over all of them said it better than this can: SYNTAX.md's "asking a collection its size never
needs a per-type name or a conversion". There is no ranking, no implicit conversion and no
return-type-directed choice -- each member is picked by what argument one is, which is the
dispatch the interpreter already had.

```
pub native fn len<T>(items: MutableArray<T>) -> int
```

### `contains`

```
pub native fn contains<T: Eq>(items: [T], x: T) -> bool
```

**Whether `x` is an element**, O(n) in both worlds -- the question `len`'s nine are asked
alongside, each in the form its type gives it: an element of an array or a set, a *key* of a
map, a substring of text.

```
pub native fn contains<T: Eq>(items: MutableArray<T>, x: T) -> bool
```

### `snapshot`

```
pub native fn snapshot<T>(items: MutableArray<T>) -> [T]
```

**Each builder to the immutable value it is building.** O(n), and the handle stays usable
afterwards, which is what makes `snapshot` a reader rather than a finisher: a builder can be
snapshotted, appended to and snapshotted again.

**Four shapes, not five, and the other three are elsewhere** -- a MutableString's in string.zm,
a MutableMap's in map.zm, a MutableSet's in set.zm, one declaration per file for the type it
takes. What stays here is the refusal the whole set shares, because `#[when_unmatched]` is read
off whichever member writes it: `len` and `contains` take a `Mutable<T>` cell and these two
deliberately do not. `snapshot` of a cell is `mutable_get` -- the same crossing out of the
handle, but O(1) rather than the O(n) copy this name promises -- and a cell has no `clear`,
being always initialised, where emptying it would need exactly the zero value its type may not
have. mutable.zm says the same thing from the cell's side.

**In a loop, `snapshot` is the quadratic cost a builder exists to avoid** (`--lint` flags that
shape). A loop that needs what it has already appended reads `b[i]` or `for x in b` instead; see
the banner above.

### `clear`

```
pub native fn clear<T>(items: MutableArray<T>)
```

Empties a builder in place, keeping the handle. `narrow(items, 0, 0)` is the same thing said
the long way, and this is the word for it.

### `map`

```
pub native fn map<T, U>(items: [T], f: fn(T) -> U) -> [U]
```

**`f` applied to every element, as a new array.** `map`'s array member; the Map member is in
map.zm and the Set member in set.zm, each answering the type it was given.

**Native rather than written in Ziman**, unlike the other three members: the result's length is
known from the input's, so this allocates the array once where a Ziman body would grow a builder
and snapshot it.

*This block used to describe "the array half of the dispatcher's prefix layer" -- four `array_*`
natives that the bare names resolved onto by string concatenation, deliberately unadvertised.
Every part of that is gone: F179 deleted the concatenating loop (the C symbol is derived from
parameter one's type now, and the resolver picks the declaration), F178-F180 made these
declarations the overload members themselves rather than a layer beneath them, and they are
`pub` and listed in `--api builtins`. The comment survived all three and was still claiming the
layer was "uniformly internal" in F216.*

```
pub fn map<T, U>(items: MutableArray<T>, f: fn(T) -> U) -> [U]
```

The MutableArray member: the array body with the source read in place, so the copy `map` over a
snapshot would have made does not happen. Written in Ziman rather than shared with the native
above because it allocates a builder for the result where that one allocates the array
directly -- one allocation more on the way out, against a whole copy of the input saved on the
way in. The capacity is the input's length, so the builder never grows.

### `flat_map`

```
pub native fn flat_map<T, U>(items: [T], f: fn(T) -> [U]) -> [U]
```

**`flat_map` is `map` and then `concat`**, in one pass: `f` answers an array per element and they
are joined. Both its declarations are array-world, so the filing rule puts it here -- and it is
beside `map`'s array members, which is what F216 moved it to collections.zm to achieve when
`map` could not leave. `map` has left now, so the two read together *and* the rule is satisfied;
that exception is retired rather than overridden. The difference between them is one word of the
signature, `-> U` against `-> [U]`.

```
pub fn flat_map<T, U>(items: MutableArray<T>, f: fn(T) -> [U]) -> [U]
```

The MutableArray member, as `map`'s is: the builder is walked in place and the results collected
into a fresh one. `concat(out, f(item))` appends each answered array rather than nesting it,
which is what makes this flat.

### `narrow`

```
pub native fn narrow<T>(items: MutableArray<T>, start: int, end: int)
```

**Keeps only the elements from `start` up to `end`, in place, answering nothing.** The in-place
counterpart of `slice`: that one is a *query*, so it answers a value and leaves its source alone,
and this one is a mutation, so it changes the container and answers nothing -- the same split that
keeps `remove` from being spelled like `slice` either way.

Builder-only, like `clear` and `snapshot`. An immutable array has nothing to narrow: asking for a
window of one is `slice(a, start, end)`, which makes a new array for free by sharing storage.

It replaced `mutable_array_truncate(xs, length)`, which was this with `start` at 0 and is the
reason for the half truncate could not do: dropping a **prefix**. That was `remove(xs, 0)` in a
loop, O(n) a call and so quadratic, against one pack-aware memmove here. `clear(xs)` is
`narrow(xs, 0, 0)` and stays the word for emptying.

Unlike truncate it does **not** clamp: `end` past the length is a runtime error, as it is for
`slice` and for an index. Truncate's clamp made `truncate(b, 99)` on a length-2 builder a silent
no-op, which hid a wrong computation rather than reporting it.
`native fn` since F216, and the name that paid for the fix the whole family needed. Its only
parameter is `MutableArray<T>`, so an array argument binds no `T`, and type-argument inference
used to fail with "write it explicitly" -- advice no `T` can satisfy. F216 taught that failure
to blame the argument instead (`shape_cannot_match` in typecheck.c), so the message is now
"argument 1 of 'narrow' expects MutableArray<T>, found [int]" with the line below after it.
Every single-builder-parameter name is convertible on the back of that.

### `swap`

```
pub native fn swap<T>(items: [T], i: int, j: int) -> [T]
```

**Exchanges the elements at `i` and `j`, in both worlds.** The same question of each, answered
the way each can: a new array from the array, nothing from the builder, which is written in
place. Two `native fn` declarations since F216, one per world, where a single builtin id had
tested `args[0]` at run time. No prefixed *declaration* stands under it -- the
`mutable_array_swap` the vocabulary once had went when `swap` covered both worlds -- but the C
symbols keep the prefix, as every converted name's do.

**What this costs, recorded because it is the largest of the conversions and was measured
twice.** `reverse` on a builder is a loop whose body is *only* this call, so the frame a native
call pushes is the whole per-iteration cost: 4,000,000 swaps through `reverse` run about **7-11%
slower** than the builtin id did. That is the price of the id table not growing without bound,
and the id table is what does not scale. Two things are *not* the answer to it: rewriting
`reverse` over `b[i]` is **63% worse** (three index operations are three handle lookups, where
this does the exchange inside one), and keeping one name on an id to save 10% of one prelude
loop is how the table got to seventy entries.

The array form closes a gap rather than renaming one. Its only previous spelling was
`set(set(a, i, a[j]), j, a[i])`, which copies the whole array **twice** to move two slots,
because `set` always copies. This copies once.

```
pub native fn swap<T>(items: MutableArray<T>, i: int, j: int)
```

### `reverse`

```
pub native fn reverse<T>(items: [T]) -> [T]
```

**`items` back to front, as a new array; the one given is untouched**, arrays being values.
Native rather than written in Ziman so the result is one allocation of the known length, where
a MutableArray filled back-to-front would grow its buffer on the way.

It lived among the sequence questions further down, under a `// See `map`.` pointing at
collections.zm's note on the four array-side natives. The pair reads better together: these two
are the clearest case of the rule this file is arranged by, because the costs are what differ
and nothing else is. An array has to allocate -- there is no other way to answer with a value --
and a builder rearranges the buffer it already owns, so `reverse(snapshot(xs))` makes **two**
copies to do what `reverse(xs)` does with none.

```
pub fn reverse<T>(items: MutableArray<T>)
```

Reverses the buffer in place -- no copy at all, where `reverse(snapshot(xs))` makes two.

### `filter`

```
pub native fn filter<T>(items: [T], keep: fn(T) -> bool) -> [T]
```

**The elements `keep` answers true for, as a new array**, in their existing order. `filter`'s
array member; the Map and MutableMap members are in map.zm, the Set and MutableSet members in
set.zm, and `ziman --api builtins` prints all six as one block.

`native fn` for the reason `map`'s array member is one: the walk is a C loop over the elements
with one call back into Ziman per element, where the Ziman version would also pay for a builder
it then snapshots.

It sat in collections.zm with the other four until 2026-10-22, which the filing rule had always
said was wrong -- a declaration lives in the file for its first argument's type -- and the pair
below is why it matters: `reverse`'s two members above and these two are the same lesson twice,
so they read better in one file than the whole of `filter` did in another.

```
pub native fn filter<T>(items: MutableArray<T>, keep: fn(T) -> bool)
```

**Keeps only the elements `keep` answers true for, in place.** The one filter in the language
that allocates nothing: it walks once with a write cursor trailing the read cursor, where
`filter(snapshot(xs), keep)` makes two copies -- the same two `reverse(snapshot(xs))` makes.

`keep` must not change the builder. A reallocation inside it cannot corrupt anything, but the
rest of the walk would then be visiting the buffer mid-compaction, the same class of thing as
appending during a `for`.

### `iter`

```
pub native fn iter<T>(items: [T]) -> Iter<T>
```

The elements of `items` as an Iter, for code that takes a lazy source but has an array. Named
`iter` rather than `iter_of_array` since F170: the argument already says what the source is, the
way `to_set` is named for its result, and `iter_from` beside it is the other constructor.

It takes an array and **not** a MutableArray, which is what separates it from `fold`, `find` and
the rest of the either-world family: those walk their sequence once inside the call and keep
nothing, while the `Iter` this answers outlives the call, so a handle's contents could move
underneath it. `iter(snapshot(b))` is the builder route, and the copy it makes is the point
rather than a cost to be removed.

### `to_set`

```
pub native fn to_set<T: Eq + Hash>(items: [T]) -> Set<T>
```

**The distinct elements of a sequence, as a Set.** Named for its result rather than its source
-- `set_from_array` until 2026-10-18 -- for two reasons. The argument already says what the
source is, which matters now that there are two of them; and the deduplication is the part a
reader has to be told about under the old name, while `to_set` implies it, a set having no
duplicates by definition. `to_` rather than `as_` because this allocates and copies: `as_` reads
as a view that costs nothing, which this is not.

Either world, and the handle is read element by element rather than snapshotted first, so
accepting it saves a copy instead of hiding one. `to_mutable_set` is the same question with the
answer in the other world, and it has to be a second name rather than an overload: the two take
the *same* argument types and differ only in what they return, which first-argument dispatch
cannot distinguish.

```
pub native fn to_set<T: Eq + Hash>(items: MutableArray<T>) -> Set<T>
```

### `to_mutable_set`

```
pub native fn to_mutable_set<T: Eq + Hash>(items: [T]) -> MutableSet<T>
```

**The same question with the answer in the other world**: the distinct elements as a *builder*,
ready to be added to. It is the second name the paragraph above explains -- not an overload of
`to_set`, because the two take the same argument types and differ only in what they return, and
first-argument dispatch cannot tell those apart.

It earns a name rather than being spelled `mutable(to_set(xs))`, which is **two** passes and two
allocations where this is one. That is the rule for a fused conversion in this library: a name
when composing costs an extra full copy, and not otherwise -- which is why there is no
`mutable_array_from_set` (see docs/DESIGN.md).

Four declarations across the two names, each taking either world in its one argument that has
the choice -- the handle is always a handle. That is what `either_world()` in the type checker
is for, and these two names are its clearest case.

```
pub native fn to_mutable_set<T: Eq + Hash>(items: MutableArray<T>) -> MutableSet<T>
```

### `index_of`

```
pub native fn index_of(haystack: [byte], needle: [byte], from: int) -> int
```

The position of the first occurrence of `needle` in `haystack` at or after `from`, or -1; an
empty `needle` is found at `from`. For finding a delimiter (a CRLF CRLF, a boundary) in bytes:
every HTTP header end, chunk-size line, trailer and multipart boundary in `lib/http` is this.

**The bytes half of `index_of`, bare since F213**, where it was `bytes_index_of` -- the last
function whose name stated what type argument one had, which the argument states. Same question
of a `string` and of a `[byte]`, same answer, and the two differ only in what they scan.

Giving it the bare name needed the one thing F205 left undone: `native_lookup(fn->name)` finds a
native registered under the declared name *before* the prefix fallback runs, so two natives
sharing a name both resolved to whichever was registered bare. The string half was registered as
`index_of` and would have answered for both. Both are prefixed now -- `string_index_of` and
`array_index_of` -- so neither resolves by the bare name and the fallback picks from parameter
one, which is where they differ. That fallback knew `[T]`, Map, Set and the four builders; F213
taught it `string`.

### `sort`

```
pub fn sort<T: Ord>(items: [T]) -> [T]
```

`items` in ascending order of their own `compare`, which orders any number, string or byte,
and any struct or enum with a `<type>_compare` function (`#[derive(Ord)]` writes one). Stable:
equal elements keep their input order. Packed numbers and strings sort natively; anything else
goes through a generic stable merge sort.

**Those two paths are three hundred times apart, so it is worth knowing which one a call takes.**
Measured at 200,000 elements (F216; a 2026-era desktop, so read the ratios and not the seconds):


```
sort([int]) / sort_desc([int])     0.01 s   packed, compared natively
sort([string])                     0.06 s   packed, compared natively
sort([Row]) with #[derive(Ord)]    3.2  s   a Ziman compare per comparison
```

The generic path is O(n log n) and correct -- it scales as it should, 0.69/1.44/3.18 s over
50k/100k/200k -- it simply calls `Row_compare` about 3.5 million times at this size, and each of
those is an interpreter call. `sort_with` is the same story for the same reason (0.69 s on
`[int]` with a comparator of your own, 70x the native path).

**If one field decides the order, `sort_by` is the form to reach for** -- 0.04 s on the same
200,000 rows, because the key is computed once per element and the keys then sort natively.
**It is not a faster `sort`, though: it is a different ordering.** `sort` by a derived `Ord`
compares every field in declaration order, so it is total; `sort_by` compares one key and leaves
ties in input order. Three rows, two sharing a key, make the difference visible:


```
sort(rs)            [b, a, c]   k, then name
sort_by(rs, r.k)    [b, c, a]   k only, so the tie keeps its input order
```

```
pub fn sort<T: Ord>(items: MutableArray<T>)
```

Sorts the buffer by the element type's own `compare`. **The one twin here that does not save a
copy -- it makes two**, the snapshot and the sorted array, and that is the cheaper option rather
than a compromise: for a packed `[int]`, `[float]` or `[byte]` buffer the array form above
compares *natively*, with no interpreter call per comparison, which is worth far more than the
two memcpys it costs. `sort_with` has no such trade available and so is genuinely in place.

The write-back is `items[i] = v` per element rather than `clear` and push, so the buffer keeps
its capacity and the length never changes. Spelled out in each of the four rather than shared,
because a helper here would be *public* -- everything in the prelude is -- and "overwrite the
buffer with this array" is not a function the design asked for.

### `sort_desc`

```
pub fn sort_desc<T: Ord>(items: [T]) -> [T]
```

`items` in descending order of their own `compare`. Also stable, so this is not `sort`'s
result reversed -- that would flip ties.

```
pub fn sort_desc<T: Ord>(items: MutableArray<T>)
```

Descending, and the same trade as `sort`: two copies bought with a native comparison.

### `sort_with`

```
pub fn sort_with<T>(items: [T], cmp: fn(T, T) -> int) -> [T]
```

`items` ordered by a comparator function (negative, zero or positive, like `compare`), for an
order that is neither the type's own nor a single key -- a two-field ordering, say. Stable.
`sort_by` covers the single-key case, and computes each key once rather than per
comparison.

```
pub native fn sort_with<T>(items: MutableArray<T>, cmp: fn(T, T) -> int)
```

Sorts the buffer by `cmp`, **copying no elements at all**: a merge sort over positions, then the
permutation applied by following its cycles. It allocates two int64 order buffers and an n-byte
map -- 17 bytes per element, against the 80 a boxed element costs, twice, through a snapshot. A
comparator of your own always went through the interpreter per comparison, so unlike `sort` and
`sort_desc` there is no native comparison here to trade those copies for. Stable: equal elements
keep their input order.

### `inverse`

```
pub fn inverse<T>(cmp: fn(T, T) -> int) -> fn(T, T) -> int
```

**A comparator with its order turned round**, which is to `sort_with` what `sort_desc` is to
`sort`: `sort_with(xs, inverse(by_length))` rather than a second lambda written backwards, where
the only difference is which argument comes first and getting it wrong is silent. It composes,
so `inverse(inverse(cmp))` is `cmp`, and it is world-agnostic -- a comparator knows nothing
about arrays -- so the one function serves `sort_with` in both.

Ordinary Ziman: it returns a closure over `cmp`, which needs nothing the language lacks.

### `sort_by`

```
pub fn sort_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> [T]
```

`items` in ascending order of `key(item)`, the key computed **once per element** and the
elements then permuted into the keys' order -- the decorate-sort-permute shape, with the
decorate step no longer the caller's job. `key` returning an int, float, byte or string also
keeps the native sort: `map` answers a real `[K]`, so `array_order_of`'s scalar fast
path still fires. Stable: equal keys keep their input order.

**That fast path is the whole point, and it is large**: 200,000 structs sorted by an `int` field
take 0.04 s here, against 3.2 s for `sort` over the same rows with a derived `Ord` and 1.07 s for
`sort_with` comparing the same field -- the key is computed 200,000 times instead of being
recomputed inside 3.5 million comparisons, and `[int]` keys then sort in C.

**It is a different ordering, not a cheaper one**, which is the part worth reading twice before
swapping one for the other. A derived `Ord` compares every field in declaration order and is
therefore total; this compares one key and, being stable, leaves ties in their input order. On
`[Row { k: 1, name: "c" }, Row { k: 1, name: "a" }, Row { k: 0, name: "b" }]`, `sort` answers
`[b, a, c]` and `sort_by(rs, r.k)` answers `[b, c, a]`. Pick by the order you want; the cost is
the tie-breaker only when both orders are correct for the job.

```
pub fn sort_by<T, K: Ord>(items: MutableArray<T>, key: fn(T) -> K)
```

The same by a computed key. `key` is called once per element, on the snapshot, so a key
function that reads the buffer it is sorting sees it unchanged throughout.

```
pub fn sort_by<T, K1: Ord, K2: Ord>(items: [T], key1: fn(T) -> K1, key2: fn(T) -> K2) -> [T]
```

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord>(
    items: [T],
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3
) -> [T]
```

Three keys, same shape.

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord, K4: Ord>(
    items: [T],
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3,
    key4: fn(T) -> K4
) -> [T]
```

Four keys, same shape.

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord, K4: Ord, K5: Ord>(
    items: [T],
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3,
    key4: fn(T) -> K4,
    key5: fn(T) -> K5
) -> [T]
```

Five keys, same shape.

```
pub fn sort_by<T, K1: Ord, K2: Ord>(items: MutableArray<T>, key1: fn(T) -> K1, key2: fn(T) -> K2)
```

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord>(
    items: MutableArray<T>,
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3
)
```

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord, K4: Ord>(
    items: MutableArray<T>,
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3,
    key4: fn(T) -> K4
)
```

```
pub fn sort_by<T, K1: Ord, K2: Ord, K3: Ord, K4: Ord, K5: Ord>(
    items: MutableArray<T>,
    key1: fn(T) -> K1,
    key2: fn(T) -> K2,
    key3: fn(T) -> K3,
    key4: fn(T) -> K4,
    key5: fn(T) -> K5
)
```

### `sort_by_desc`

```
pub fn sort_by_desc<T, K: Ord>(items: [T], key: fn(T) -> K) -> [T]
```

`items` in descending order of `key(item)`. Also stable, so this is not `sort_by`'s
result reversed -- that would flip ties.

```
pub fn sort_by_desc<T, K: Ord>(items: MutableArray<T>, key: fn(T) -> K)
```

Descending by a computed key, on the snapshot, as `sort_by` is.

### `push`

```
pub native fn push<T>(items: [T], values: variadic T) -> [T]
```

A new array with the values on the end; the original is unchanged, these being immutable values.
Variadic because appending several is one question, not several: a fold of one-value pushes
reuses the buffer's tip when it has room, so `push(a, x, y, z)` costs about what the three
values cost rather than three copies of `a`.

**These two declarations are the whole of `push`'s type checking since F207.** The set is mixed
on purpose: the array form stays `builtin fn`, dispatched by its id (`B_push`), while the
MutableArray form is a `native fn`, so the resolver records it on `call_fn` and the interpreter
derives `mutable_array_push` from parameter one. One name, two mechanisms, no arm. `add` has had
exactly this shape since F199 (`builtin fn` for a `Set`, native for a `MutableSet`), which is
why it needed no new mechanism.

```
pub native fn push<T>(items: MutableArray<T>, values: variadic T)
```

The MutableArray's `push` is the array's, for the reason `len` and `contains` are: appending is
the same question of both, so it is the same word. The difference is in what the call answers --
a new array there, nothing here, the container having been written -- which is the difference
between the two worlds and not a difference between the two names.

`native fn` rather than `builtin fn`, which is what makes this half dispatchable without the
arm that used to rewrite the callee: `is_native` is `!is_builtin`, so only a native reaches
interpreter.c's symbol fallback, and that fallback derives the C symbol from **parameter one
only** (F205). Parameter one is where these two declarations differ, so it lands on
`mutable_array_push` and the array form is never a candidate for it.

### `insert`

```
pub native fn insert<T>(items: [T], index: int, x: T) -> [T]
```

**`insert` and `set` are not interchangeable, and each has its own error telling the reader
which to use.** `insert` puts something where it was not, at an index; `set` replaces what is at
a position, an array index or a map key. So an array has both, a map has `set` and no `insert`,
and a set has neither -- F199 settled that `insert` is positional only and gave both set worlds
`add`, and F206 made the checker say so, the set form having gone on being accepted undeclared
for a pass after the decision.

```
pub native fn insert<T>(items: MutableArray<T>, index: int, value: T)
```

An index in a MutableArray, for the same reason `push` is shared. `index` may be `len(b)`,
which appends -- the same rule the immutable form follows, so adding at the end needs no second
function. Neither set world has an `insert` form: membership is `add` in both, and an index
means nothing to a set.

**O(n), where `push` on a builder is amortized O(1)**, so a loop of `insert(b, 0, v)` is
quadratic. That is why there is no `push_front`: the name would read as `push`'s twin and
promise a cost it cannot give. Build tail-first and `reverse` once.

`native fn` for the reason the MutableArray `push` above is one, and the pair converted
together in F207: the symbol fallback reads parameter one, which is the only place these two
declarations differ, so it finds `mutable_array_insert`.

### `set`

```
pub builtin fn set<T>(items: [T], index: int, x: T) -> [T]
```

**A new array with `index` replaced** -- `set`'s array member, the other two being a Map's and a
MutableMap's in map.zm, which replace the value under a key.

`set` has no MutableArray form on purpose: an element write there is `b[i] = value`, the same
sugar an array's element write gets, and a copying three-argument call would read as though it
left the container alone. `set(b, i, v)` says so rather than failing as an unknown overload.

### `remove`

```
pub native fn remove<T>(items: [T], index: int) -> [T]
```

**A new array without `index`** -- `remove`'s array member, and the third of the element verbs
the banner above introduces. An index out of bounds is a runtime error. The Map, Set, MutableMap
and MutableSet members are in map.zm and set.zm; `--api builtins` prints all six as one block.

**`native fn` since 2026-10-22, and `remove`'s arm went with the conversion.** One builtin id had
served the three immutable members: its branch tested `args[0].type->kind` for a Map or a Set and
otherwise fell through to the array case. Parameter one is what the symbol fallback reads, so
each declaration finds a symbol of its own -- `array_remove`, `map_remove`, `set_remove`.

**The arm's one irreplaceable message lives here**, because `#[when_unmatched]` is read off
whichever member writes it: a `MutableString` has no element removal. That line is now what
*any* unmatched `remove` gets, including one that passed an int, which is the trade `clear` and
`snapshot` made before it (F190's criterion: the content survives, the aim does not). The arm's
other three messages were no loss -- two arity checks and `argument 2 of 'remove' expects int`
are what the resolver says anyway, and the list of accepted types it hand-wrote is derived from
the six signatures now, so it cannot fall behind them again. It had, once, by naming two
functions that no longer existed.

`x` became `item` on the Set member during that conversion, while the rename was still free: a
builtin takes no named argument, so nothing could have been calling it `x:`.

```
pub native fn remove<T>(items: MutableArray<T>, index: int) -> T
```

**O(n)**: everything after `index` moves back. There is no `pop` because this is one --
`remove(b, len(b) - 1)`, which moves nothing.

**The MutableArray's `remove` answers the element, where the array's answers the array.** Taking
element i out is the same question of both, so it is the same word; what differs is the answer,
and that is the difference between the two worlds rather than between two names -- the same
reasoning that made `push` and `insert` shared. Discarding the element is how `pop` is spelled
(`remove(b, len(b) - 1)`), which is why that is not reported as a wasted result the way a
discarded `remove(a, i)` is: an array's `remove` copies and changes nothing, a builder's writes.
That is what `#[effectful]` states, and the MutableMap and MutableSet members carry it for the
same reason.

**The six do not all answer the same kind of thing**, which is why one name over them needed
saying out loud: the three immutable members each answer a new collection, while the three
builder members answer three different things -- the element here, the `Option<V>` that was
stored in map.zm, whether the item was there in set.zm. This paragraph used to read *"There is
no `MutableMap` or `MutableSet` form"*, which was true the day it was written (2026-10-18, when
those two were still prefixed names) and false from the Map/Set pairing pass onward, since that
pass gave them this exact bare name. Found 2026-10-22 by moving the comment.

`native` rather than `builtin`, so the resolver owns it and `mutable_array_remove` needs no
declaration of its own: the symbol fallback reads parameter one and finds the registered
`mutable_array_remove`. It was a `builtin fn` until F208 and the builtin id never ran --
`check_call`'s arm rewrote the callee first, and `call_remove` only ever handled an array, so
the id path would have read an `ArrayObj` out of a handle had anything reached it.

### `is_sorted`

```
pub fn is_sorted<T: Ord>(items: [T]) -> bool
```

True when no element is greater than the one after it (an empty or one-element array is).

```
pub fn is_sorted<T: Ord>(items: MutableArray<T>) -> bool
```

The precondition is the caller's in either world, and `sort(b)` leaves the handle itself in
order, so a builder is if anything the more natural subject than an array.

### `is_sorted_desc`

```
pub fn is_sorted_desc<T: Ord>(items: [T]) -> bool
```

True when no element is *less* than the one after it -- what `sort_desc` leaves behind, as
`is_sorted` is what `sort` leaves behind. An empty or one-element array is both, and so is an
array of equal elements: neither question is strict, so `[2, 2, 2]` answers true to each.

**It does not unlock a descending `binary_search`.** The three searches below all assume
ascending order and there are no descending twins of them, so a descending sequence is searched
by `inverse`-ing nothing and reversing nothing: either keep it ascending and read it backwards,
or search with `lower_bound` on the ascending copy. This answers a question about an order; it
does not install one.

```
pub fn is_sorted_desc<T: Ord>(items: MutableArray<T>) -> bool
```

The builder twin, the array body with one word of the signature changed -- `len()` and `[i]` are
all it reads, which is what makes that literally true here rather than merely similar.

### `lower_bound`

```
pub fn lower_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element that is not less than
`target` -- where `target` would be inserted to keep the order, before any equal
elements. `len(sorted)` when every element is smaller. O(log n).

```
pub fn lower_bound<T: Ord>(sorted: MutableArray<T>, target: T) -> int
```

Reads O(log n) elements, so `lower_bound(snapshot(b), x)` copied the whole buffer to answer a
question that touches almost none of it. Its array body verbatim -- it reaches the sequence
through `len()` and `[i]` only, and nothing else was array-specific.

### `upper_bound`

```
pub fn upper_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element greater than `target` --
just after any equal elements. `upper_bound - lower_bound` is how many equal `target`.

```
pub fn upper_bound<T: Ord>(sorted: MutableArray<T>, target: T) -> int
```

As `lower_bound`, for the other end of a run of equal elements.

### `binary_search`

```
pub fn binary_search<T: Ord>(sorted: [T], target: T) -> Option<int>
```

The position of the first element equal to `target` in an already sorted array, or None.
O(log n).

```
pub fn binary_search<T: Ord>(sorted: MutableArray<T>, target: T) -> Option<int>
```

Calls the *builder* `lower_bound` rather than its array twin: taking `snapshot` to reuse the
array version would copy the buffer to save writing four lines.

### `find`

```
pub fn find<T>(items: [T], test: fn(T) -> bool) -> Option<T>
```

The first element for which `test` is true, or None.

```
pub fn find<T>(items: MutableArray<T>, test: fn(T) -> bool) -> Option<T>
```

Stops at the first match, so on a long builder it may touch almost none of the buffer that
`find(snapshot(b), test)` would have copied in full.

### `first`

```
pub fn first<T>(items: [T]) -> Option<T>
```

The first element, or None for an empty array.

```
pub fn first<T>(items: MutableArray<T>) -> Option<T>
```

One element out of a builder of any size, where `first(snapshot(b))` copied all of it.

### `last`

```
pub fn last<T>(items: [T]) -> Option<T>
```

The last element, or None for an empty array.

```
pub fn last<T>(items: MutableArray<T>) -> Option<T>
```

As `first`, from the other end.

### `min_by`

```
pub fn min_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is smallest (the first such, on a tie), or None for an empty array.

```
pub fn min_by<T, K: Ord>(items: MutableArray<T>, key: fn(T) -> K) -> Option<T>
```

Allocates nothing, so the snapshot is the entire cost this saves. It reaches elements with
`items[i]` inside `for i in 1..len(items)` rather than iterating, which works on a builder only
because F149 gave it `b[i]`.

### `max_by`

```
pub fn max_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is largest (the first such, on a tie), or None for an empty array.

```
pub fn max_by<T, K: Ord>(items: MutableArray<T>, key: fn(T) -> K) -> Option<T>
```

As `min_by`, the other way round.

### `fold`

```
pub fn fold<T, A>(items: [T], initial: A, f: fn(A, T) -> A) -> A
```

Left to right: `f(f(initial, items[0]), items[1])` and so on.

```
pub fn fold<T, A>(items: MutableArray<T>, initial: A, f: fn(A, T) -> A) -> A
```

No copy: `fold(snapshot(b), ...)` allocates and fills a whole array before adding anything up.

### `count`

```
pub fn count<T>(items: [T], test: fn(T) -> bool) -> int
```

How many elements `test` is true for.

```
pub fn count<T>(items: MutableArray<T>, test: fn(T) -> bool) -> int
```

Walks the whole buffer and keeps nothing.

### `any`

```
pub fn any<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for at least one element (false for an empty array).

```
pub fn any<T>(items: MutableArray<T>, test: fn(T) -> bool) -> bool
```

Short-circuits on the first true, so the saved snapshot is often the whole cost of the call.

### `all`

```
pub fn all<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for every element (true for an empty array).

```
pub fn all<T>(items: MutableArray<T>, test: fn(T) -> bool) -> bool
```

Short-circuits on the first false, as `any` does on the first true.

### `none`

```
pub fn none<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for **no** element (true for an empty array), completing the trio with
`any` and `all`.

**It is `any`'s complement, not `all`'s**, and the difference is worth having straight because
the three names invite the wrong pairing. `none(p)` is `!any(p)`: nothing satisfies `p`. The
complement of `all(p)` is a different question -- "something fails `p`" -- which is
`any(items, fn(x) { !p(x) })` and has no name here, because negating the test says it as
clearly. So the trio is not a lattice of three; it is `any`, its negation, and the universal.

An empty sequence answers true to both `all` and `none` and false to `any`, which is the usual
vacuous-truth reading and the one every other `all` in this library already has.

```
pub fn none<T>(items: MutableArray<T>, test: fn(T) -> bool) -> bool
```

Short-circuits on the first true, as `any` does -- the same walk, the opposite answer.

### `for_each`

```
pub fn for_each<T>(items: [T], f: fn(T))
```

Calls `f` on every element, in order, for its effect.

```
pub fn for_each<T>(items: MutableArray<T>, f: fn(T))
```

No copy, and nothing to return either -- the one of the eleven that was always a statement.

### `distinct`

```
pub fn distinct<T: Eq + Hash>(items: [T]) -> [T]
```

The distinct elements, each at its first position: `distinct([3, 1, 3, 2, 1])` is `[3, 1, 2]`.

```
pub fn distinct<T: Eq + Hash>(items: MutableArray<T>) -> [T]
```

Allocates its Set either way, so the snapshot this saves is a smaller share of the cost than it
is for `fold` -- still real on a large input, just less of it.

### `group_by`

```
pub fn group_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, [T]>
```

The elements grouped by `key`: a map from each key to the elements that have it, in input
order, with the keys in the order they first appear.

```
pub fn group_by<T, K: Eq + Hash>(items: MutableArray<T>, key: fn(T) -> K) -> Map<K, [T]>
```

Allocates its Map and its per-key builders either way -- the same trade as `distinct`.

### `sum`

```
pub fn sum(items: [int]) -> int
```

The total of `items`. A left fold with `+`, which is what anyone writes by hand, but `+` on
`int` traps on overflow in this language -- so this either answers the exact total or stops with
an error, and never the silently wrong number a wrapping sum would give.

**Every numeric element type, one declaration per type per world.** `int`, `float`, `bigint`,
`decimal` and `byte` -- ten declarations, because `sum` cannot be generic over them: `T: Ord`
would admit `string`, there is no `Num` bound in this language, and a generic body could not
write the zero it starts from. One declaration per type is what states the set exactly.

**It was `int` only until the float caveat was written down rather than acted on.** The stated
reason was that a float total depends on summation order for precision, so the right answer is
a decision (naive, pairwise, Kahan) rather than a default. That is true and it is not a reason
to withhold the name: a caller who writes the `fold` by hand gets naive left-to-right too, just
with more code. So `sum` over floats is **naive, left to right**, said out loud here, and a
caller who needs pairwise or Kahan still writes the `fold` -- now by choosing it rather than by
having no alternative. `bigint` and `decimal` are exact and have no such caveat.

**`sum` over bytes answers `int`.** Partly because 256 bytes can exceed what a byte holds, so a
byte total would wrap or trap on nearly any real input; and partly because this language refuses
`byte + byte` outright ("use 'as' to cast explicitly"), so the accumulation has to widen anyway.
It is the one member whose answer is not its element type.

**What widening the set cost: `sum([])` no longer resolves.** A bare empty literal carries no
element type, and five members now accept it -- where one member meant there was nothing to
choose. `sum` has no type parameter to write it on either, so the repair is to annotate the
array (`val none: [int] = []` then `sum(none)`), which is what the ambiguity error asks for by
name. Two fixtures said `sum([])` and now say that instead. It is the honest price of the name
covering five types rather than one, and the only call that changed.

```
pub fn sum(items: MutableArray<int>) -> int
```

The builder twin, reading the handle in place.

```
pub fn sum(items: [float]) -> float
```

Naive, left to right -- see the note on the `int` form above for why that is stated rather than
avoided, and what to write when the summation order matters.

```
pub fn sum(items: MutableArray<float>) -> float
```

```
pub fn sum(items: [bigint]) -> bigint
```

Exact and unbounded, so neither the overflow note nor the precision note applies.

```
pub fn sum(items: MutableArray<bigint>) -> bigint
```

```
pub fn sum(items: [decimal]) -> decimal
```

Exact in base ten, so a total of money or of measured quantities is the figure you expect --
which is the whole reason `decimal` exists beside `float`.

```
pub fn sum(items: MutableArray<decimal>) -> decimal
```

```
pub fn sum(items: [byte]) -> int
```

**Answers `int`, not `byte`** -- the one member whose answer is not its element type. See the
note on the `int` form for both reasons.

```
pub fn sum(items: MutableArray<byte>) -> int
```

### `frequencies`

```
pub fn frequencies<T: Eq + Hash>(items: [T]) -> Map<T, int>
```

How many times each distinct element occurs: `frequencies<string>(["a", "b", "a"])` is
`{a: 2, b: 1}`, keys in the order they first appear.

This comment was sitting above `array_join` instead, with no blank line between the two blocks,
so the generator published it as `array_join`'s documentation and shipped this function with
none at all (D21).

```
pub fn frequencies<T: Eq + Hash>(items: MutableArray<T>) -> Map<T, int>
```

Allocates its Map either way, keys in first-seen order, as in the array form.

### `index_by`

```
pub fn index_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, T>
```

The elements keyed by `key`: a map from each key to the element that has it. When two elements share a
key the later one wins, like `set` on a map; use `group_by` to keep them all. Keys are in the order they
first appear.

```
pub fn index_by<T, K: Eq + Hash>(items: MutableArray<T>, key: fn(T) -> K) -> Map<K, T>
```

Allocates its Map either way; last key wins, as in the array form.

## `channel.zm`

### `channel_new`

```
pub builtin fn channel_new<T>() -> Channel<T>
```

Takes its element type from the context, like `ok`/`err`/`Some`: a `val` with a declared
`Channel<T>`, a struct field, a `with` override, a returned value, or a parameter's type.

### `send`

```
pub builtin fn send<T>(channel: Channel<T>, value: T)
```

Blocks until there is room; refused on a closed channel.

### `try_send`

```
pub builtin fn try_send<T>(channel: Channel<T>, value: T) -> bool
```

False when the channel is full or closed, rather than blocking.

### `receive`

```
pub builtin fn receive<T>(channel: Channel<T>) -> T
```

Blocks until a value arrives. A closed and drained channel is a runtime error here --
`receive_option` is the shape that answers "closed" as a value.

### `try_receive`

```
pub builtin fn try_receive<T>(channel: Channel<T>) -> Option<T>
```

None when the channel is empty, rather than blocking.

### `receive_option`

```
pub builtin fn receive_option<T>(channel: Channel<T>) -> Option<T>
```

None when the channel is closed and drained.

### `receive_timeout`

```
pub builtin fn receive_timeout<T>(channel: Channel<T>, millis: int) -> Option<T>
```

None when `millis` elapses first.

### `channel_len`

```
pub builtin fn channel_len<T>(channel: Channel<T>) -> int
```

### `channel_close`

```
pub builtin fn channel_close<T>(channel: Channel<T>)
```

### `select2`

```
pub builtin fn select2<A, B>(first: Channel<A>, second: Channel<B>) -> Selected<A, B>
```

Whichever of the two has a value first, and which one it was. `first` and `second` are the
`Selected` variants' names, not an order of preference -- neither channel is favoured.

### `guard_new`

```
pub native fn guard_new<T>(channel: Channel<T>, message: T) -> Guard
```

A guard that sends `message` on `channel` when it is collected.

## `iter.zm`

### `iter_from`

```
pub native fn iter_from<T>(next: fn() -> Option<T>, close: fn()) -> Iter<T>
```

An Iter written in Ziman. `next` returns the next item as `Some(item)`, or `None` at the end; `close` is
called once when the source is finished, left early by a `for` loop (`break`, `return`, `try`) or closed
with `iter_close`. A closure cannot change a variable it captured, so the state lives in whatever the
closures call: a native handle such as a socket or a database cursor, or a Channel. Unlike the built-in
sources it is NOT closed by the garbage collector (that would run your code from a finalizer), so
finish it or close it.

### `iter_next`

```
pub native fn iter_next<T>(it: Iter<T>) -> Option<T>
```

The next item of `it`, or None when it is exhausted. For a `while` loop that wants the items one
by one; `for` does this for you.

### `iter_close`

```
pub native fn iter_close<T>(it: Iter<T>)
```

Releases what `it` holds (a file, say) now instead of when it is collected. A `for` loop does this
itself when it is left early; call it when you take items by hand and stop before the end. Harmless
on one that is finished or already closed.

## `map.zm`

### `mutable_map_new`

```
pub native fn mutable_map_new<K: Eq + Hash, V>() -> MutableMap<K, V>
```

An empty map builder.

### `len`

```
pub native fn len<K: Eq + Hash, V>(m: Map<K, V>) -> int
```

**How many entries**, O(1) in both worlds. `len`'s other seven members are in array.zm,
string.zm, set.zm and mutable.zm, and array.zm's carry the account of why nine declarations
share one C function.

```
pub native fn len<K: Eq + Hash, V>(m: MutableMap<K, V>) -> int
```

### `contains`

```
pub native fn contains<K: Eq + Hash, V>(m: Map<K, V>, key: K) -> bool
```

**Whether `key` is present** -- a *key*, which is the form the shared question takes for a map,
where an array is asked about an element and text about a substring. For "is this value in
here", walk `values`.

```
pub native fn contains<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K) -> bool
```

### `mutable`

```
pub builtin fn mutable<K: Eq + Hash, V>(entries: Map<K, V>) -> MutableMap<K, V>
```

**The MutableMap holding `entries`** -- `mutable`'s map member, and `snapshot` backwards. Its
array and Set members are in array.zm and set.zm, and the member for everything else is in
mutable.zm, which carries the account of how the four are told apart. `Map<K, V> {}` writes its
own types, so this never had the empty-literal gap `mutable([])` has.

**The one `builtin fn` left in this file, and the only one of its ten that could not convert.**
Tried on 2026-10-22 and the declaration checker refused it, which is §4's recorded blocker met
head-on rather than inherited:


```
function 'mutable' is already defined (line 94) -- two declarations may share a name only
when every call can tell them apart
```

Line 94 is mutable.zm's `mutable<T>(item: T) -> Mutable<T>`, and a `Map` satisfies a bare `T` as
well as it satisfies this, so the pair is indistinguishable. The exemption that lets the four
coexist today is that they are all `builtin fn`; the moment one stops being one, it is measured
against the others. **And the arm does not help**, because unlike `concat`'s it does not choose a
declaration -- it rewrites the callee to `$mutable_map_from`, so these four declarations are
documentation of a dispatch that happens under other names. Converting this member would mean
either giving `mutable` `#[compiler_resolved]` and a rule that records `call_fn` (the `concat`
shape, for a set whose own attribute comment says a second user is a reason to doubt the second)
or making the bare-`T` member narrower, which is a language question rather than a conversion.

### `map`

```
pub fn map<K: Eq + Hash, V, R>(m: Map<K, V>, f: fn(K, V) -> R) -> Map<K, R>
```

`m` with `f` applied to every value, keys and their order unchanged:
`map(stock, fn(k, v) { str(v) })`. **`f` takes the key as well as the value**, which is what the
example here got wrong from the day it was written: it read
`map<string, int, string>(stock, fn(n) { str(n) })`, and a one-parameter lambda against
`fn(K, V) -> R` is refused -- *"this lambda takes 1 parameter(s) but a function taking 2 is
expected"* -- with or without the type arguments. A prelude doc comment is compiled by nothing,
which is how it stood. The key is usually unused, as here; name it and ignore it.

`map`'s array members are in array.zm with `flat_map`, and its Set member in set.zm.

### `get`

```
pub native fn get<K: Eq + Hash, V>(m: Map<K, V>, key: K) -> Option<V>
```

**The value of `key`, or None.** Both worlds answer the same way, and the `Option` is the whole
of the difference from `get_or`: this one reports absence, that one substitutes for it.

These two sat 200 lines apart until 2026-10-22, the file having been grouped by world -- every
immutable member under one banner and every builder member elsewhere. That banner called them
"the builtins over an immutable `Map` and `Set`", of which three things were wrong: they are
`native fn` since the map.zm conversions, there is no `Set` member here at all, and a reader
looking up `get` found one half of it.

```
pub native fn get<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K) -> Option<V>
```

The same of a builder.

### `discard`

```
pub native fn discard<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K)
```

Removes `key` and answers nothing. **Its reason for existing changed on 2026-10-22 without its
usefulness changing**, which is the kind of thing worth catching in writing rather than
rediscovering.

It existed because `remove` answered an `Option<V>`: a heap allocation per removal, where a
tracing collector prices a short-lived allocation by how much is alive rather than by its size.
Measured then on a 100,000-entry map, removing and re-inserting every key: 351-363 ns/op through
`remove` against 110-112 here. `remove` answers a `bool` now, so that gap should have closed --
and it did not. Re-measured the same day, one spelling per process: `remove` 41-51 ms per 100,000
ops against 12-13 ms here, the same 3.5x. The allocation moved rather than went, because `remove`
is variadic now and a variadic parameter builds a one-element array per call to carry its
argument.

So this is the single-key fast path, and it will stay one until `remove` gets the one-element
shortcut `push` has in interpreter.c (D18). Reach for `remove` when several keys go at once;
reach for this in a loop.

### `remove`

```
pub native fn remove<K: Eq + Hash, V>(m: Map<K, V>, keys: variadic K) -> Map<K, V>
```

**A new Map without any of `keys`** -- `remove`'s Map member. A key that is absent is not an
error; the answer is a map equal to the argument. `native fn` since 2026-10-22; array.zm's member
carries the account of that conversion and the `#[when_unmatched]` line the whole set shares.

**Variadic since 2026-10-22, which is where `remove_all` went.** The plural was a separate name
taking the plural of what the singular took, and a variadic parameter is how this language says
that -- `add` had already been both for a set since F199, so `remove` was the asymmetric one.
Nothing is ambiguous: argument one fixes `K`, so a lone `[K]` argument spreads and a lone `K`
does not, each having only one reading. set.zm's `add` carries the measurement.

```
pub native fn remove<K: Eq + Hash, V>(m: MutableMap<K, V>, keys: variadic K) -> bool
```

Drops each of `keys`, answering whether any of them was actually there; a key that is absent is
not an error.

**It answered `Option<V>` -- the value it took out -- until 2026-10-22, and now answers `bool`.**
That is the one capability this merge spent, and it was spent deliberately: a variadic form has
no single value to answer, so keeping the `Option` would have meant keeping `remove_all` for this
one member and the name would have survived for a single declaration. Nothing in lib/, tests/ or
the sample backend ever read the value -- the only two readers were `remove_all` and
`remove_keys`, each using it as a presence test with the binding discarded. Taking a value out of
a MutableMap in one step is no longer expressible; `get` then `remove` is two.

**It is not cheaper, which is worth stating because it looks as though it should be.** The
`Option` was an allocation per removal -- `discard`'s whole reason for existing -- and dropping
it bought nothing, because a variadic parameter allocates a one-element array per call to carry
the argument. One trade for another. Measured one spelling per process (the repeated lesson from
option.zm: measuring several in one process reads the later ones against a heap the earlier ones
already grew), 100,000-entry map, removing and re-inserting every key:


```
old remove  Option, singular     38-45 ms
new remove  bool, variadic       41-51 ms
discard     void, singular       12-13 ms
```

So `discard` keeps a real reason to exist, and it is a *different* reason than the one its own
comment gives: not that it avoids building an `Option`, but that it avoids the variadic collapse.
`push` has a one-element fast path in interpreter.c for exactly this cost (D18), picked by
comparing the cached native pointer; `remove` has none yet, and adding one is what would make
`discard` genuinely redundant.

`#[effectful]`, because "was any of it there?" is an answer "delete these if present" has no use
for. It was needed for a sharper reason while this answered a value: without it the
dropped-result gate read "answers something" as "has no effect" and refused `remove(mb, k)` in
statement position.

### `remove_keys`

```
pub native fn remove_keys<K: Eq + Hash, V>(m: Map<K, V>, keys: Set<K>) -> Map<K, V>
```

**`m` without any key that `keys` holds**, and **a second name for what looks like one
question** -- which is a cost taken deliberately, so the reasoning is here rather than left to
be rediscovered and "fixed".

`remove(m, keys: Set<K>)` cannot be a declaration. Tried on 2026-10-22 against `remove_all`,
which is the name the variadic parameter then belonged to:


```
function 'remove_all' is already defined -- two declarations may share a name only when
every call can tell them apart
```

The sibling above takes `variadic K`, and **a `Set<K>` argument satisfies that too** -- as one
element whose type happens to be a set, which `Map<Set<int>, V>` makes a real map to want. So no
call can tell "remove these keys" from "remove this one key, which is a set", and the rule is
right to refuse rather than pick. The same shape as `concat`'s second wall and `mutable`'s.

**The wall moved to `remove` when `remove_all` was deleted the same day** and is unchanged in
substance: the variadic parameter is what it was ever about, and `remove` holds it now. This is
why `remove_keys` outlived the name it was a sibling of.

**What was already possible**, and is still the shorter way when the set is at hand:
`remove(m, to_array(drop))` -- `items` answers the elements as an array and the variadic's
lone-array spread takes it, in both worlds. This exists for the call that reads better naming
the set, and for the one that would otherwise pay for an intermediate array.

```
pub fn remove_keys<K: Eq + Hash, V>(m: MutableMap<K, V>, keys: Set<K>) -> bool
```

The builder half: the entries are dropped in place and the answer is whether any of them was
there. `#[effectful]` for the reason `remove`'s builder form carries it -- the call writes
the map, and "was any of it there?" is an answer "make sure these are gone" has no use for.

### `get_or`

```
pub native fn get_or<K: Eq + Hash, V>(m: Map<K, V>, key: K, value: V) -> V
```

**The value under `key`, or `value` when there is none** -- and no `Option` allocated either
way, which is why it is not `get` plus `unwrap_Some`.

```
pub native fn get_or<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K, value: V) -> V
```

The same of a builder. Ordinary Ziman rather than a builtin, since it is exactly that switch:
the point is that a counting loop (`set(counts, k, get_or(counts, k, 0) + 1)`) reads as one line
instead of four. The `Option` form of this name is in option.zm, being the one-entry case of the
same question.

### `set`

```
pub native fn set<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K, value: V)
```

Adds `key`, or replaces its value while keeping its position.

```
pub native fn set<K: Eq + Hash, V>(m: Map<K, V>, key: K, value: V) -> Map<K, V>
```

A new Map with `key` added, or with its value replaced and its position kept -- the immutable
half of the builder form above, which writes in place and answers nothing. `set`'s third member
takes an array and an index and is in array.zm; neither set world has one, membership being
`add` in both, and no builder has one either, an element write being `b[i] = value`.

### `update`

```
pub fn update<K: Eq + Hash, V>(m: MutableMap<K, V>, key: K, f: fn(Option<V>) -> V) -> V
```

`f` applied to whatever is at `key` -- `None` when nothing is -- stored back, and returned. One
lookup where `get` then `set` is two, which is the whole reason this exists where
`mutable_set`-on-a-cell did not need an equivalent. `update`'s cell form is in mutable.zm.

`#[effectful]` for the reason the cell form carries it: the call writes the entry, and the new
value is an answer `update(counts, k, bump)` on its own line has no use for.

```
pub fn update<K: Eq + Hash, V>(m: Map<K, V>, key: K, f: fn(Option<V>) -> V) -> Map<K, V>
```

**A new Map with `key`'s value replaced by `f` applied to what was there** -- `None` when
nothing was -- the immutable half of the builder form above. The key keeps its position when it
was present and is appended when it was not, `set` deciding that either way.

**It pays two lookups where the builder form pays one**, which is worth stating because that
saving is the builder form's whole reason for existing: this body *is* `get` then `set`, the
line a caller writes today, and the name is what it buys. A native could do it in one walk and
would be the version that earns its place; this is deliberately the plain one.

**No `#[effectful]`**, unlike the other two members of the name: this copies and changes
nothing, so a discarded result is a mistake and the dropped-result rule should keep refusing it.
That is the whole difference between the immutable and builder halves of a name, read off the
return type -- and the reason the attribute exists is the three places where that reading is
wrong.

`update_all` has no immutable member and needs none: `map(m, fn(k, v) { ... })` already answers
a new Map with every value replaced, so the asymmetry is only on the single-entry form.

### `filter`

```
pub native fn filter<K: Eq + Hash, V>(m: Map<K, V>, keep: fn(K, V) -> bool) -> Map<K, V>
```

The entries `keep` answers true for, as a new Map; keys keep their order. **`keep` takes the key
as well as the value.** `filter`'s other four members are in set.zm and array.zm, one pair per
world, and `ziman --api builtins` prints all six as one block.

**`native fn` since 2026-10-22**, where this and the `Set` form shared one `builtin fn` id whose
branch switched on `args[0].type->kind` at run time. The symbol fallback reads parameter one, so
each found a C function of its own -- `map_filter` and `set_filter` -- and neither has to ask
what it was given. That conversion needed one new thing: a native walking a collection's entries
while calling back into Ziman was the first of its shape, and the guard the branch held over its
whole loop had to be exported for a native to hold it (`native.h`'s `native_gc_unsafe_enter`,
which is candid about what could and could not be reproduced).

```
pub fn filter<K: Eq + Hash, V>(m: MutableMap<K, V>, keep: fn(K, V) -> bool)
```

Drops every entry `keep` answers false for, in place -- the builder half, which writes the map
and answers nothing where the form above answers a new one. Safe to write as a `for` plus a
`remove` for the same reason `update_all` is: iterating a MutableMap walks a snapshot of the
entries, so the writes cannot disturb the walk.

### `update_all`

```
pub fn update_all<K: Eq + Hash, V>(m: MutableMap<K, V>, f: fn(K, V) -> V)
```

`f` applied to every value, keys and their order unchanged. Safe to write as a `for` plus a
`set` because iterating a MutableMap walks a snapshot of the entries, so the writes cannot
disturb the walk.

### `keys`

```
pub native fn keys<K: Eq + Hash, V>(m: Map<K, V>) -> Set<K>
```

**The keys, in insertion order.** A `Set`, not an array: a map's keys are unique by
construction, so the type that says so is the honest one.

```
pub fn keys<K: Eq + Hash, V>(m: MutableMap<K, V>) -> Set<K>
```

The same of a builder, and the reason this returns a value rather than a handle: a result like
this is nearly always consumed by a `for` or returned, so handing back a handle would allocate a
thread-bound object per loop for nothing.

### `keys_array`

```
pub native fn keys_array<K: Eq + Hash, V>(m: Map<K, V>) -> [K]
```

**The keys as an array, in insertion order** -- the same elements `keys` answers, in the type
`values` answers.

**It has to be its own name.** `keys(m) -> Set<K>` and `keys(m) -> [K]` differ only in what they
answer, and two declarations may share a name only when every *call* can tell them apart, which a
return type cannot do. Tried on 2026-10-22 and refused with "function 'f' is already defined ...
two declarations may share a name only when every call can tell them apart". `math.floor` and
`math.floor_float` are the established answer to exactly this -- one question, two answer types,
a suffix naming the answer because the resolver cannot -- and this follows them. It does *not*
follow the `to_array` rule, and that is deliberate: `to_array(m)` would claim to convert the map,
where this answers one of three different questions a map can be asked.

**It earns a name on the fused-conversion rule rather than on taste**, DESIGN.md's "a fused
conversion earns a name only when composition costs an extra full copy". `to_array(keys(m))` is
not one extra copy, it is an entire `Set<K>`: every key hashed into a trie, which is then walked
back out into the array that was wanted all along. This walks the map's own entries straight into
the array. Measured on a 100,000-entry map, 20 calls per run, one spelling per process:


```
to_array(keys(m))   156-183 ms per call
keys_array(m)         16-18 ms per call
```

About 10x, which is the set construction and nothing else. Both numbers were written into this
comment as a guess before they were run, and both were wrong by more than an order of magnitude;
the ones above are measured.

The immutable member shares one C function with `to_array`'s Set member, because a Set is a
MapObj whose values are placeholders and so the two walks are identical.

```
pub fn keys_array<K: Eq + Hash, V>(m: MutableMap<K, V>) -> [K]
```

The same of a builder. Ordinary Ziman, like the builder halves of `keys` and `values` beside it
rather than like `to_mutable_array`: the builder worlds in this file are written in Ziman and
the consistency is worth more than the pass it would save.

### `values`

```
pub native fn values<K: Eq + Hash, V>(m: Map<K, V>) -> [V]
```

**The values as an array, in the keys' insertion order.** An array rather than a `Set`, which is
the asymmetry with `keys` worth noticing: values are not unique, and collapsing duplicates would
lose the correspondence with `keys(m)` that makes the pair useful together.

This declaration had no doc comment at all until 2026-10-22 and published blank in
`docs/lib/prelude.md` -- it sat third under a banner that described the group, which is the cost
the collections.zm sweep measured and the reason a set's members now sit together.

```
pub fn values<K: Eq + Hash, V>(m: MutableMap<K, V>) -> [V]
```

The same of a builder.

### `merge`

```
pub native fn merge<K: Eq + Hash, V>(maps: variadic Map<K, V>) -> Map<K, V>
```

**Every map in order, joined into one new Map**; a later map wins where an earlier one has the
same key. Keys keep the order they were first seen in.

**Variadic, and this replaced a two-parameter form rather than joining it.** `merge(a, b)` was
`merge(m: Map<K, V>, other: Map<K, V>)` until 2026-10-22, and the two could not coexist: a
variadic `Map<K, V>` accepts exactly two maps as well, so no call could tell them apart and the
declaration checker refuses the pair. Replacing is the right direction anyway -- the binary form
*is* the two-element case -- and every `merge(a, b)` call site is unchanged, because a variadic
parameter takes them written out. What did change is the parameter name, from `m`/`other` to
`maps`, which only a named call could notice.

**One argument that is an array of maps spreads**, its elements being the maps, the same rule
`concat` and `push` follow: `merge(candidates)` for a `[Map<K, V>]` merges them all. Two or more
arguments are never a spread.

`#[at_least_one]`, because `merge()` has no map to take `K` and `V` from and would answer
nothing useful -- the same reason `add` and `remove` carry it. `merge(a)` is allowed and
answers `a`.

```
pub native fn merge<K: Eq + Hash, V>(m: MutableMap<K, V>, entries: variadic Map<K, V>)
```

**Every map in `entries`, in order, written into `m`**; a later one wins, and `m`'s own entry
wins over nothing -- an existing key keeps its position and takes the new value. Answers
nothing, the container having been written, where the immutable member above answers a new Map.

**Variadic since 2026-10-22**, as the immutable member became on the same day. The parts may
come from either world, but **not mixed in one call**: one declaration takes a tail of `Map`s
and the other a tail of `MutableMap`s, a variadic parameter having exactly one type. That is the
third of `concat`'s walls, and `concat` only clears it because an arm chooses for it and one C
function reads either world per element -- there is no arm here, so `merge(mb, m1, b2)` is
refused and the repair is two calls. Worth knowing rather than discovering.

**`#[at_least_one]` is load-bearing, not just sensible.** Without it the two declarations are
indistinguishable: `merge(mb)` passes an empty tail, which fits both, and the declaration
checker refuses the pair outright. With it the minimum arity is 2 -- `ovl_min_arity` counts the
attribute as part of the arity for exactly this reason -- and at two arguments position one is a
`Map` against a `MutableMap`, which is disjoint. It is also what `merge(mb)` deserves, being a
call that would do nothing.

One C function serves both, as before: the symbol fallback reads parameter one and both have a
`MutableMap` there, so `mutable_map_merge` tells the worlds apart by each element's own runtime
kind -- which is also why it would already handle a mixed tail if a rule ever chose one.

```
pub native fn merge<K: Eq + Hash, V>(m: MutableMap<K, V>, entries: variadic MutableMap<K, V>)
```

### `entries`

```
pub fn entries<K: Eq + Hash, V>(m: Map<K, V>) -> [MapEntry<K, V>]
```

The entries of `m` as an array, in the map's insertion order: for collecting them, sorting them (by value,
say, with `sort_by_desc`) or taking the first few. Written with the type arguments:
`entries<string, int>(stock)`. To only walk a map, `for k, v in m` needs no array.

```
pub fn entries<K: Eq + Hash, V>(m: MutableMap<K, V>) -> [MapEntry<K, V>]
```

The entries of a MutableMap as an array, in insertion order -- the builder half of `entries`,
answering an immutable value for the same reason `keys` and `values` do.

### `snapshot`

```
pub native fn snapshot<K: Eq + Hash, V>(m: MutableMap<K, V>) -> Map<K, V>
```

The entries added so far as an ordinary `Map`, in insertion order; O(n), and the builder stays
usable afterwards. `snapshot`'s other three members take a MutableArray, a MutableString and a
MutableSet, each in the file for its type, and the refusal the whole set shares is written on
the MutableArray form in array.zm.

### `clear`

```
pub native fn clear<K: Eq + Hash, V>(m: MutableMap<K, V>)
```

Empties the builder in place, keeping the handle.

## `mutable.zm`

### `mutable_new`

```
pub native fn mutable_new<T>(value: T) -> Mutable<T>
```

A cell holding `value`.

### `len`

```
pub fn len<T>(cell: Mutable<T>) -> int
```

A cell holds exactly one value, so its length is always 1 -- `len`'s ninth member, and the only
one of the nine that is not about a collection. The other eight are in array.zm, string.zm,
map.zm and set.zm.

**Ordinary Ziman rather than a native, since 2026-10-22.** The other eight ask their argument
something; this one does not, and a constant is not worth a C function. It made `len` a mixed
set -- eight natives and one Ziman body, the shape `push` had -- which costs this member the
frame-free shortcut the natives get (`impl == ziman_len` in interpreter.c, worth +23% on `len`
when it was added) and that is the right trade for a call that can only ever answer 1.

### `contains`

```
pub native fn contains<T: Eq>(cell: Mutable<T>, x: T) -> bool
```

Whether the value in the cell equals `x`: the one-value case of the question the other eight
members ask of a collection.

### `mutable`

```
pub builtin fn mutable<T>(item: T) -> Mutable<T>
```

**`mutable(x)` makes the mutable counterpart of whatever it is given, and this is the member for
everything that is not one of the three collections.** For an array, a Map or a Set it answers
that collection's builder (those three members are in array.zm, map.zm and set.zm); for anything
else -- a scalar, a string, a struct, an enum, even another handle -- there is no builder to
answer, and the mutable counterpart of a single value is a cell.

**The dispatch is by shape, and the collections win.** An array matches both `[T]` and the bare
`T` here, and the array member is the one that applies; the same for a `Map` and a `Set`. The
arm in `check_call` decides that, **not declaration order** -- which is worth saying now that
the four live in four files and a reader cannot see them in one order at all. Checked by moving
this member above the other three and re-running: the same answers. The one thing the rule costs
is that a *cell holding* one of those three cannot be spelled this way -- `mutable([1, 2])` is a
`MutableArray<int>`, never a `Mutable<[int]>`. Write that one as `mutable_new([1, 2])`, which is
what the cell's own constructor is for and is unambiguous about which of the two you meant.

Two consequences worth knowing rather than discovering. A string answers `Mutable<string>`, not
a MutableString: a MutableString is *filled* rather than converted (`mutable_string_new` then
`append`), so it is not this conversion -- and the corpus agrees, since of 133
`mutable_string_new` call sites exactly one seeds the buffer from a whole existing string. And a
builder answers a cell holding that same handle, **not a clone of it**; there is no way to clone
a builder at all, which is deliberate rather than pending. `mutable_array_from`,
`mutable_map_from` and `mutable_set_from` used to be that way and were removed once `mutable`
covered the conversion they were mostly used for: keeping both would have put two spellings on
one question, and the clone half had no caller outside its own fixtures.

### `get`

```
pub native fn get<T>(cell: Mutable<T>) -> T
```

The value in the cell.
**The value in the cell**, O(1). `get`'s other two members take a Map and a MutableMap and
answer an `Option<V>`, because a key may be absent; a cell always holds a value, so this one
answers it directly and the arity tells the three apart.

**`mutable_get` until 2026-10-22.** The prefix said which world the name belonged to, which is
what the bare names have been replacing since F178 -- and `get` was already the word for
reaching into a map. The old spelling answers with this one rather than "no such function".

### `set`

```
pub native fn set<T>(cell: Mutable<T>, value: T)
```

Replaces the value in the cell.
**Replaces the value in the cell**, answering nothing. `set`'s other three members take a Map, a
MutableMap and an array, all with a key or an index between the container and the value; a cell
has one slot and needs none, so two arguments against three is what tells them apart.

**`mutable_set` until 2026-10-22**, and this one needed the type checker's help: `set` keeps an
arm for the three per-type refusals it carries (a Set, a MutableArray, a MutableSet each get
their own message), and that arm claimed the name before any declaration was reached. It
declines for a cell now, as it already did for a Map and a MutableMap.

### `update`

```
pub fn update<T>(cell: Mutable<T>, f: fn(T) -> T) -> T
```

`f` applied to the value in the cell, stored back, and returned -- the counter idiom,
`update(n, fn(x) { x + 1 })`. Ordinary Ziman over `mutable_get`/`mutable_set`, since there is no
lookup for a native to save, where the MutableMap form of this name is a native precisely
because there is one. That form takes a key and is in map.zm, with the entry it updates.

**`#[effectful]` because the counter idiom above is a statement.** `update` writes the cell and
answers the new value, so the return-type rule -- which reads "answers something" as "has no
effect" across these sets -- refused `update(n, fn(x) { x + 1 })` on its own line with *"the int
returned by 'update' is discarded, and 'update' has no other effect"*, of the one function in
the file whose whole job is that effect. Missing since the attribute existed (F208) and found by
auditing every prelude declaration that writes an argument and answers a value, 2026-10-22.

## `option.zm`

### `get_or`

```
pub fn get_or<T>(o: Option<T>, value: T) -> T
```

**The value inside `o`, or `value` when `o` is None** -- the one-entry case of the question the
map forms ask, which is why it shares their name rather than being an `option_unwrap_or`.

`value` is the fallback, not something stored: `get_or(o, 0)` reads "get, or nought". The name
is `value` across all three forms of this set, where `set(m, key, value)` uses the same word for
the thing being written -- the type tells them apart, and the alternative was a fourth spelling
of "the other one".

### `is_Some`

```
pub builtin fn is_Some<T>(o: Option<T>) -> bool
```

### `is_None`

```
pub builtin fn is_None<T>(o: Option<T>) -> bool
```

### `unwrap_Some`

```
pub builtin fn unwrap_Some<T>(o: Option<T>) -> T
```

### `unwrap`

```
pub builtin fn unwrap<T>(o: Option<T>) -> T
```

An `Option`'s Some payload, which is a runtime error on a `None`. `unwrap` is the one name
shared by the two types -- its `Result` member is in result.zm -- and **this** member is why it
is not spelled `unwrap_Ok`: a name carrying one type's variant would not fit the other.
result.zm's header has the full note on why `unwrap_err` is lower case where `unwrap_Some` is
not.

### `filter_present`

```
pub fn filter_present<T>(items: variadic Option<T>) -> [T]
```

The values that are actually there, in order, with the `None`s dropped. What builds a list out
of fields that may each be absent, without an `if` per field:


```
xml.element(xml.name("patient"), [], filter_present<xml.Node>(
    xml.opt_element("firstName", p.first_name),
    xml.opt_element("dateOfBirth", p.date_of_birth)
))
```

Named `filter_present` rather than `array_present` since F170. The `array_` prefix said which
*world* the argument came from, which is what the prefix meant everywhere else; here the
argument is a written-out list of maybes and there is no other world it could have come from,
so the prefix was answering a question nobody asked. `filter_` says what it does instead, and
puts it beside `filter` where a reader looking for "keep some of these" will be.

Variadic rather than taking an array, because the call site is nearly always a written-out
list of maybes rather than one already in hand; an array you do have goes in as
`filter_present<T>(items: existing)`, the spelling `concat_arrays` took before `concat` absorbed it.

`filter` cannot do this: it keeps elements, so it would hand back `[Option<T>]` still
wrapped, and `flat_map` only does it by turning each value into a one- or zero-element
array, which allocates per element and reads like a puzzle.
Plain value `push`, not a MutableArray, and the reason is that `T` is generic: which spelling
wins depends on whether `T` packs, so the one chosen here is the one whose worst case is
mildest rather than the one that wins anywhere in particular.

Every item present, which is the heaviest case and the one where a capacity hint is exactly
right rather than an over-estimate:


```
              n       push   builder   builder+capacity
[int]      10,000    480 ms    343 ms         352 ms
[int]     200,000   1040 ms    760 ms         712 ms
[string]   10,000    617 ms   1278 ms        1250 ms
[string]  200,000   1396 ms   3028 ms        2669 ms
```

A packed buffer (`int`, `float`, `byte`, `bool`) is unscanned and a builder is about 1.4x
ahead of value `push` there. A boxed buffer is scanned for pointers at every collection, and
a builder ends up holding two large scanned regions at once -- the grown buffer and
`snapshot`'s copy of it -- where `push` grows one array in place and never copies, so `push`
is about 2x ahead instead. Worst case: `push` 1.4x, either builder 2.2x. Below a hundred
elements, which is what the variadic form is nearly always called with, all three are inside
the run-to-run noise.

The capacity hint makes no material difference either way since the runtime stopped
allocating a boxed array it was about to discard. Measure one spelling per process if these
are ever re-run: taking all three in one gave the last of them a heap the earlier two had
already grown, which read as a 4x spread that does not exist.

### `to_result`

```
pub fn to_result<T, E>(o: Option<T>, make_err: fn() -> E) -> Result<T, E>
```

The reverse of `to_option` in result.zm: Some(value) becomes Ok(value), and None becomes
Err(make_err()) -- a function, not a plain value, so building the error (an allocation, a
formatted message) is skipped entirely on the common Some path.

**`make_err` was `err_supplier` until 2026-10-22**, the only `_supplier` in the prelude and a
Java interface name where every neighbouring function parameter is a short imperative saying
what the function does in this call: `keep` on `filter`, `test` on `any`/`all`/`none`, `cmp` on
`sort_with`, `key` on `sort_by`, `next` and `close` on `iter_from`. `make_err` answers the
question a reader actually has here -- what goes in the Err -- where `_supplier` named the
shape of the argument, which its type already gives.

**Named `option_to_result` until 2026-10-22.** The `option_` prefix said argument one is an
`Option` and argument one says that, the same rule that took the prefix off `map_err` and
`to_option` the same day; the old name points here. Named for its target like `to_set`, which
had been doing this since long before.

## `result.zm`

### `ok`

```
pub builtin fn ok<T, E>(value: T) -> Result<T, E>
```

`ok` and `err` take their type from the context, like `channel_new` and `Some`: the error type of
an `ok(...)` is nowhere in its argument, so a `val`/`var` annotation, a struct field, a `with`
override, a returned value or a parameter's type has to say which `Result` it is.

That context-dependence is why these two are among the handful of builtins that cannot become
declarations: a native takes already-evaluated arguments against a fixed signature and has
nowhere to read an expected type from, so `ok(5)` would leave `E` unbound. The arm exists to say
so when no context supplied one.

### `err`

```
pub builtin fn err<T, E>(error: E) -> Result<T, E>
```

The failure, and the mirror of `ok` in what it leaves unsaid: `ok(5)` has no `E` in its argument
and `err("bad")` has no `T`, so both read the Result they are becoming from their context.

It had no comment of its own until 2026-10-22 -- it shared `ok`'s block above, which is enough
for a reader of the source and not enough for `docs/lib/prelude.md`, where every declaration
gets its own `###` heading and this one published a blank entry under it. The same shape `max`
had next to `min` in prelude.zm. A shared comment is a blank entry in the manual; the detector
for that is worth re-running after any move.

### `is_ok`

```
pub builtin fn is_ok<T, E>(result: Result<T, E>) -> bool
```

Whether `result` is an `Ok`. `is_ok(result)` and `if Ok(v) = result` ask the same question; the
second also binds the value, so this is for the cases that only need the answer.

**`result` is the Result receiver everywhere in this file**, spelled out rather than the `r` it
was until 2026-10-22. The prelude has both habits -- `m` for a map, `s` for a set or a string --
and a whole word is the one that has survived contact: `channel` and `cell` were both `c` until
the names were swept, and one letter meaning two types is exactly the failure a letter invites.
A word cannot collide with the next type that wants the same initial.

### `unwrap`

```
pub builtin fn unwrap<T, E>(result: Result<T, E>) -> T
```

A `Result`'s Ok payload, which is a runtime error on an `Err`. `unwrap`'s other member takes an
`Option<T>` and is in option.zm; that Option form is why this is not spelled `unwrap_Ok`.

### `unwrap_err`

```
pub builtin fn unwrap_err<T, E>(result: Result<T, E>) -> E
```

`result`'s error, which is a runtime error when it is an `Ok` -- the mirror of `unwrap`, and lower
case for the reason the note at the top of this file gives.

### `filter_ok`

```
pub fn filter_ok<T, E>(items: variadic Result<T, E>) -> [T]
```

**The Ok values, in order, with the failures dropped**, and `filter_err` is the same over the
errors. Added in F171 shaped after option.zm's `filter_present` exactly: variadic, so a
written-out list of fallible steps needs no array built first, and an array already in hand goes
in as `filter_ok<T, E>(items: existing)`.

Both type arguments are written even though each answers only one of them, since
Ziman infers neither and `Result<T, E>` names both: `filter_ok<int, string>(...)` answers
`[int]`, `filter_err<int, string>(...)` answers `[string]` over the very same input.

**The parameter is `items`, not `results`**, where the single `Result` next door is `result`.
These two are filters and `items` is the prelude's word for a sequence receiver: `filter` takes
`items`, so does `filter_present`, and all four read the same way at a call site. `results`
would say only what the signature already says, where `items` says which role the argument
plays -- the distinction the naming rule turns on.

**Neither one propagates**, which is the whole point and also the risk: `filter_ok` *silently
discards* every failure, so it is for the cases where a failure is genuinely not news -- one
bad `Accept` range among ten, a malformed line in a feed that keeps going. When a failure
should stop the work, `try` and `if Err(e) =` are still the answer and always were. Calling
both on the same input is how to keep the successes and still report what was lost.

### `filter_err`

```
pub fn filter_err<T, E>(items: variadic Result<T, E>) -> [E]
```

The errors, in order, with the successes dropped -- `filter_ok`'s other half.

### `map`

```
pub fn map<T, E, U>(result: Result<T, E>, f: fn(T) -> U) -> Result<U, E>
```

**The Ok value transformed, with an Err passing through untouched** -- the `Result` member of
`map`, joining the four that already answer the same question for a `[T]`, a `MutableArray<T>`,
a `Map<K, V>` and a `Set<T>`. `map_err` below is the mirror over the error.

It lives here because a declaration lives in the file for the type its first argument has, so
this is the fifth member of a set split across five files; `--api builtins` prints all five as
one block.

**The transform is `f`, the name its four siblings use.** Parameter names are part of the call
surface here, because anything not dispatched by a builtin id takes named arguments, so one role
gets one name across every declaration that has it: `map(items: xs, f: g)` and
`map(result: r, f: g)` are both legal, and a member that spelled the transform differently would
make the set's call surface depend on which type you happened to be holding. The prelude says
`f` fourteen times, against `key` twelve, `test` ten, `keep` six and `cmp` three.

`map_err` below keeps `convert`, which is not the same inconsistency: it is its own name rather
than a member of this set, so nothing constrains it, and `convert` says more about a function
whose whole job is turning one module's error type into another's.

Ordinary Ziman rather than a native, like three of the four other members: a `switch` over two
variants is the whole implementation and there is nothing a native would make faster.

### `map_err`

```
pub fn map_err<T, E, F>(result: Result<T, E>, convert: fn(E) -> F) -> Result<T, F>
```

The error replaced by `convert(error)`, with an Ok passing through untouched. The piece that was
missing wherever one module's error type has to become another's -- `fs` answers an `FsError` and a
caller whose own errors are strings wants `try map_err(fs.read_text(p), fs.fs_error_message)`
rather than four lines of switch at every call.

**Named `result_map_err` until 2026-10-22.** The `result_` prefix said which world the first
argument came from and the argument already says it, which is the rule every other prefix drop
followed; the old name points here, as those do. `to_option` below went the same way, and the
note there has the rule the two of them settled.

### `to_option`

```
pub fn to_option<T, E>(result: Result<T, E>) -> Option<T>
```

The Ok value, or None on Err -- the error itself is dropped, so use this only where the
caller truly doesn't care why something failed, not as a shortcut past handling it. `to_result`
in option.zm is the reverse, and the pair reads as one word each way.

**Named `result_to_option` until 2026-10-22**, when the prefix went for the reason every other
prefix went: it said argument one is a `Result`, and argument one says that. The old name points
here.

**A conversion is named for where it goes; argument one says where it came from.** That is the
rule this rename settled, and it is worth stating because the opposite was argued first: that
`result_` named a conversion's *source*, so a bare `to_option` would not say what it converts
from. `to_set` refutes it and predates all of this -- prefix-free in array.zm, named for its
target, taking `[T]` or `MutableArray<T>`. The source was never in the name.

## `set.zm`

### `mutable_set_new`

```
pub native fn mutable_set_new<T: Eq + Hash>() -> MutableSet<T>
```

An empty set builder.

### `len`

```
pub native fn len<T: Eq + Hash>(s: Set<T>) -> int
```

**How many elements**, O(1) in both worlds. `len`'s other seven members are in array.zm,
string.zm, map.zm and mutable.zm, and array.zm's carry the account of why nine declarations
share one C function.

```
pub native fn len<T: Eq + Hash>(s: MutableSet<T>) -> int
```

### `contains`

```
pub native fn contains<T: Eq + Hash>(s: Set<T>, x: T) -> bool
```

**Whether `x` is a member** -- O(1) here, where the array form of the same question is O(n),
which is most of why a Set is worth converting to.

```
pub native fn contains<T: Eq + Hash>(s: MutableSet<T>, x: T) -> bool
```

### `mutable`

```
pub builtin fn mutable<T: Eq + Hash>(items: Set<T>) -> MutableSet<T>
```

**The MutableSet holding `items`** -- `mutable`'s set member, and `snapshot` backwards. Its array
and Map members are in array.zm and map.zm, and the member for everything else is in mutable.zm,
which carries the account of how the four are told apart. `Set<T> {}` writes its own type, so
this never had the empty-literal gap `mutable([])` has.

### `map`

```
pub fn map<T: Eq + Hash, U: Eq + Hash>(s: Set<T>, f: fn(T) -> U) -> Set<U>
```

The set of `f(x)` for every `x` in `s`, in the order first produced. Two elements that map to the
same value become one: `map(Set<int> { 1, 2, 3 }, fn(n) { n % 2 })` is `{1, 0}`. `map`'s array
members are in array.zm with `flat_map`, and its Map member in map.zm.

### `add`

```
pub native fn add<T: Eq + Hash>(s: Set<T>, items: variadic T) -> Set<T>
```

Adds each of `items`, answering whether any of them was new.

**A `MutableSet<[int]>` is not a special case, and this comment used to claim the wrong reason
for that.** It said the singular `mutable_set_add` being non-variadic was what kept one element
and several apart -- which stopped being true when that name was deleted (2026-10-22), and had
never been the reason. The receiver pins `T`, so each shape reads one way only: a lone `[int]`
is one element, because a spread would have to be `[[int]]`, and a lone `[[int]]` is a spread,
because an element would have to be `[int]`. Measured on all three shapes, including the
written-out `add(s, [1, 2], [3, 4])`.

`min`/`max` are the genuinely ambiguous case and the contrast worth keeping: nothing there pins
`T`, so `min([[3, 9], [1, 2]])` has both readings available and the spread wins by preference.
A variadic whose first parameter fixes the element type has no such gap.

`#[effectful]`, because "did anything change?" is an answer you are usually entitled to ignore:
`add(s, x)` on its own is "make sure x is in here", and that is the ordinary call. Without the
attribute the dropped-result gate reads a non-void prelude overload as pure and refuses it, as
it did from F199 until F212.

**The immutable member is a `native fn` since 2026-10-22, and one question disappeared with the
conversion.** Its branch read `variadic_tail_is_array_spread` off the expression node to tell
`add(s, a, b)` from `add(s, items)`, because a builtin receives raw arguments and had to ask the
checker which shape arrived. `collapse_variadic_args` runs before a native is entered, so both
shapes are one array by then and the native never asks -- the same simplification `min`/`max`
and the set operations below got the same day.

**A new `Set` with every one of `items` added**, the immutable half: an element already present
changes nothing and keeps its position. It answers the set rather than "was anything new",
because a value has nothing to report -- which is the difference between the two halves of this
name, and why only the builder half is `#[effectful]`.

```
pub native fn add<T: Eq + Hash>(s: MutableSet<T>, items: variadic T) -> bool
```

The same written into the builder, answering whether any of them was new.

**`native fn` since 2026-10-22, which retired `mutable_set_add`.** This was an ordinary Ziman
loop over an `internal native fn mutable_set_add(s, item)`, so the prelude held a second
spelling of its own vocabulary for the one purpose of being looped over -- and every caller
inside lib/ used the second spelling because `internal` let them. Five of those became `add`,
and the loop moved into C where the one-element primitive already lived. The symbol is still
`mutable_set_add`: the fallback reads parameter one, so the C name keeps what the Ziman name
gave up, which is the whole job of that fallback.

### `remove`

```
pub native fn remove<T: Eq + Hash>(s: Set<T>, items: variadic T) -> Set<T>
```

**A new Set without any of `items`** -- `remove`'s Set member. An element that is absent is not
an error; the answer is a set equal to the argument. `native fn` since 2026-10-22, and the
parameter was `x` until that conversion made the rename free. array.zm's member carries the
account of it and the `#[when_unmatched]` line the whole set shares.

**Variadic since 2026-10-22, and this member gained rather than merged**: `remove_all` never had
an immutable Set form, so "a set without these" took a fold or a `filter` before. `add` beside it
has been variadic in both worlds since F199, which is the asymmetry this closed.

```
pub native fn remove<T: Eq + Hash>(s: MutableSet<T>, items: variadic T) -> bool
```

Drops each of `items`, answering whether any of them was there; nothing happens for one that is
absent. **`add`'s exact opposite in shape as well as in meaning since 2026-10-22**, when
`remove_all` was deleted and its variadic parameter moved here -- the two had been a plural name
against a singular one, which was the only thing keeping `add` and `remove` from reading alike.

`#[effectful]` for the reason `add` carries it: "did anything change?" is an answer
"make sure these are gone" has no use for.

### `filter`

```
pub native fn filter<T: Eq + Hash>(s: Set<T>, keep: fn(T) -> bool) -> Set<T>
```

The elements `keep` answers true for, as a new Set, in their existing order. A Set is a MapObj
whose values are placeholders, so `keep` takes one argument where `filter`'s Map form takes two.
`native fn` since 2026-10-22; map.zm's member carries the account of that conversion, the two
having shared one builtin id.

```
pub fn filter<T: Eq + Hash>(s: MutableSet<T>, keep: fn(T) -> bool)
```

Drops every element `keep` answers false for, in place -- the builder half, which writes the set
and answers nothing where the form above answers a new one.

### `to_array`

```
pub native fn to_array<T: Eq + Hash>(s: Set<T>) -> [T]
```

**The elements as an array, in insertion order.** `to_set` is the other direction, and the two
names are deliberately a pair: a conversion is named for what it answers, which is the rule
`to_option`/`to_result` settled and `to_set`/`to_mutable_set` had been following all along.

**It was `items` until 2026-10-22.** The old name said what the *argument* holds, which the
argument's own type already says, and it collided with itself twice over: `items` is the
prelude's word for a sequence *parameter* (every array receiver, and the variadic tail of `add`
and `remove`), so one word named a function, a receiver and a payload. The old name points here.

**Not the same question as `snapshot`, and this is the distinction worth getting right.**
`snapshot(b)` answers *the type the builder is building* -- a MutableArray's snapshot is `[T]`, a
MutableSet's is `Set<T>`. `to_array(s)` answers a *different* type from the one it was given.
That is why `to_array` takes the two **set** worlds and not a `MutableArray`: `to_array(builder)`
would be a second spelling of `snapshot`, and the retired `ab_to_array` points at `snapshot` for
exactly that reason -- a name whose closeness to this one is worth knowing about.

```
pub native fn to_array<T: Eq + Hash>(s: MutableSet<T>) -> [T]
```

The same of a builder, which had no comment of its own until 2026-10-22 and so published a bare
signature in `docs/lib/prelude.md`.

**`native fn` later the same day**, where it had been a Ziman loop pushing every element into a
MutableArray and snapshotting it -- three passes and two buffers, against one of each here,
because `maptable_entries` already hands the elements back contiguous and there is nothing left
to push into. Measured on a 100,000-element set, 20 calls per run: **6.0 ms per call before,
1.1 ms after**, about 5.7x. Insertion order is preserved, checked at 2,000 elements against the
order they went in, which is past the point the table grows.

It shares its tail with the immutable member and with `keys_array`: a MutableSet is a
MutableMapBuf where a Set is a MapObj, and that is the only difference between the three walks.

**The pair reads immutable-first now.** It was the one set in this file ordered the other way
round, against the rule prelude.zm's header states, and the comment that existed sat on this
member and pointed at the undocumented one above it ("as `items` gives for a Set").

### `to_mutable_array`

```
pub native fn to_mutable_array<T: Eq + Hash>(s: Set<T>) -> MutableArray<T>
```

**The elements as a builder**, the cross-world half of the pair above -- `to_mutable_set` is to
`to_set` what this is to `to_array`, and the four names make one table:


```
to_set           [T] or MutableArray<T>  ->  Set<T>
to_mutable_set   [T] or MutableArray<T>  ->  MutableSet<T>
to_array         Set<T> or MutableSet<T> ->  [T]
to_mutable_array Set<T> or MutableSet<T> ->  MutableArray<T>
```

Added 2026-10-22 with the rename, because the row was missing: `mutable(to_array(s))` was the
only way to get there and allocates twice, an immutable array to throw away and then the
builder. Both members are native for the reason `to_mutable_set` is -- one pass over the
elements straight into the buffer.

```
pub native fn to_mutable_array<T: Eq + Hash>(s: MutableSet<T>) -> MutableArray<T>
```

### `union`

```
pub native fn union<T: Eq + Hash>(sets: variadic Set<T>) -> Set<T>
```

**Every element of every set given**, each in the order it was first seen.

**Variadic since 2026-10-22**, in `merge`'s shape rather than a receiver with one operand:
`union(a, b, c)` and `union(sets)` are the same call, so folding a list of sets needs no loop at
the call site. Symmetric, which is why the immutable member has no distinguished receiver at all
-- `difference` below keeps one because it is not symmetric.

**`union` of none is the empty set.** An empty array spread is one argument, so
`#[at_least_one]` does not rule it out, and a variadic with an identity reads perfectly well
empty -- the same rule that makes `concat()` the empty string. `intersect` is the contrast.

```
pub native fn union<T: Eq + Hash>(s: MutableSet<T>, others: variadic Set<T>)
```

The same written into `s`, answering nothing, with any number of operands. Two declarations for
one question: the tail is `Set`s here and `MutableSet`s below, and a parameter cannot be both --
the wall `concat` and `merge` document. `#[at_least_one]` is load-bearing on this pair rather
than decorative: it is part of the arity, and without it `union(s)` would match both with
nothing to choose between them.

```
pub native fn union<T: Eq + Hash>(s: MutableSet<T>, others: variadic MutableSet<T>)
```

### `intersect`

```
pub native fn intersect<T: Eq + Hash>(sets: variadic Set<T>) -> Set<T>
```

**Only the elements in every set given**, in the first one's order. Variadic since 2026-10-22,
beside `union` and for the same reason: an intersection of three sets is one question, and
writing it as nested pairs says less.

Folded left, so each later set only removes and the work falls to the smallest set seen so far.
Order the arguments smallest-first when it is known and cheap to know; the answer is the same
either way.

**`intersect` of none is a runtime error, where `union` of none is the empty set.** That
asymmetry is the mathematics: union's identity is the empty set and exists, intersection's is the
universal set and does not. `min`/`max` of an empty array raise for the same reason, and
`concat()` succeeds because its identity is the empty string.

```
pub native fn intersect<T: Eq + Hash>(s: MutableSet<T>, others: variadic Set<T>)
```

The same written into `s`, answering nothing, with any number of operands.

```
pub native fn intersect<T: Eq + Hash>(s: MutableSet<T>, others: variadic MutableSet<T>)
```

### `difference`

```
pub native fn difference<T: Eq + Hash>(s: Set<T>, other: Set<T>) -> Set<T>
```

**`s` without anything `other` holds**, in `s`'s order. Not symmetric: `difference(a, b)` and
`difference(b, a)` are different questions, which is why the receiver is named `s` and the
argument `other` rather than giving them matching names.

**Still two-operand where `union` and `intersect` went variadic on 2026-10-22**, and the reason
is that asymmetry. A variadic `difference` would have to mean "`s` without anything in any of the
rest", which is a useful fold and a different question from the one the name answers now; it was
not asked for and is not assumed. `is_subset` below is left alone for a related reason: it is a
predicate rather than a fold, so "a subset of all of these" is a second question too.

```
pub native fn difference<T: Eq + Hash>(s: MutableSet<T>, other: Set<T>)
```

The same written into `s`, answering nothing.

```
pub native fn difference<T: Eq + Hash>(s: MutableSet<T>, other: MutableSet<T>)
```

### `is_subset`

```
pub native fn is_subset<T: Eq + Hash>(s: Set<T>, other: Set<T>) -> bool
```

**Whether every element of `s` is in `other`.** True of an empty `s` against anything, which is
the convention every set type uses and the one case worth stating. Unlike the three above this
answers a value in both worlds -- there is nothing to write -- so its two in-place members
differ from the immutable one only in what `s` is, not in what the call means.

```
pub native fn is_subset<T: Eq + Hash>(s: Set<T>, other: MutableSet<T>) -> bool
```

The same question of a builder operand. **This is the one place the four set operations' old
asymmetry is closed**, added 2026-10-22: a builder receiver has always taken an operand from
either world, where an immutable receiver took only a `Set`, and that gap was documented as
predating the pairing and "left alone rather than quietly widened". It is widened here and only
here, deliberately -- `union` and `intersect` would each need a second variadic declaration and
`difference` a second operand form, none of which was asked for.

Four declarations now, one per receiver/operand pair, all distinguishable because each pair of
argument types is distinct. Two symbols: the fallback reads parameter one, so the two immutable
receivers share `set_is_subset` and tell the operand apart by its runtime kind, exactly as the
two builder receivers share `mutable_set_is_subset`.

```
pub native fn is_subset<T: Eq + Hash>(s: MutableSet<T>, other: Set<T>) -> bool
```

```
pub native fn is_subset<T: Eq + Hash>(s: MutableSet<T>, other: MutableSet<T>) -> bool
```

### `snapshot`

```
pub native fn snapshot<T: Eq + Hash>(s: MutableSet<T>) -> Set<T>
```

The elements added so far as an ordinary `Set`, in insertion order; O(n), and the builder stays
usable afterwards. `items` answers the same elements as an array -- this one answers the type
the builder is building, which is what makes it `snapshot` and not a second `items`.

### `clear`

```
pub native fn clear<T: Eq + Hash>(s: MutableSet<T>)
```

Empties the builder in place, keeping the handle.

## `string.zm`

### `to_bytes`

```
pub native fn to_bytes(s: string) -> [byte]
```

**The UTF-8 bytes of `s`.**

**`string_to_bytes` until 2026-10-22.** The `string_` prefix said argument one is a `string`,
which argument one says -- the rule every other prefix drop followed -- and a conversion is named
for what it answers, which is the rule `to_option`/`to_result` settled and `to_set`/`to_array`
had been following all along. Both rules point the same way here. The old name points home.

The symbol is unchanged and no C moved: the fallback derives `string_to_bytes` from `to_bytes` on
a `string`, which is exactly what was registered. Checked for the trap that caught
`last_index_of` the same day -- a bare registration would be found before the fallback is
consulted -- and nothing registers `to_bytes`.

**`bytes_to_string` keeps its name**, so the pair is deliberately asymmetric. Reversing it would
make a third `to_string` beside `str(x)` for scalars and `snapshot(sb)` for a builder, and
`snapshot`'s own comment already defends that boundary. It also answers a `Result` where this
cannot fail, so the two were never mirror images: every string has bytes, and not every byte
sequence is a string.

### `len`

```
pub native fn len(s: string) -> int
```

The bytes of `s` from `start` up to, not including, `end` (`0 <= start <= end <= len(s)`, a
runtime error otherwise).
**Its length in bytes**, not characters -- `char_count` is the character question. `len`'s other
seven members are in array.zm, map.zm, set.zm and mutable.zm; `--api builtins` prints all nine
as one block, and array.zm's members carry the account of why nine declarations share one C
function.

```
pub native fn len(s: MutableString) -> int
```

### `contains`

```
pub native fn contains(s: string, needle: string) -> bool
```

**Whether `needle` appears in it** -- the form the shared question takes for text, where an
array is asked about an element and a map about a key.

```
pub native fn contains(s: MutableString, needle: string) -> bool
```

### `substring`

```
pub native fn substring(s: string, start: int, end: int) -> string
```

### `concat`

```
pub native fn concat(pieces: variadic string) -> string
```

The pieces of `s` between each `sep`, empty pieces included: `split("a,b,,c", ",")` is
`[a, b, , c]`. `sep` must not be empty.
**Every piece, in order, joined with no separator** (`concat("a", "b", "c")` is `"abc"`; zero
pieces is `""`). For a separator or a prefix/suffix use `join`; for many pieces built up over a
loop, a `MutableString` -- a fresh `concat` on every iteration copies everything so far, the
quadratic cost `--lint` already flags for `x = concat(x, ..)`.

**One argument that is an array spreads** -- its elements are the pieces -- which is the rule
that keeps `concat(parts)` answering `"xyz"` for a `[string]`. The cost is that concatenating a
single array *as one piece* cannot be said: `concat(["a", "b"])` is `"ab"`, never a copy. Two or
more arguments are always pieces, so only the one-argument case carries the rule.

**The array and MutableArray forms of this name are in array.zm**, beside the types they take.
`ziman --api builtins` still lists all four together; what is split is where you read them.

**Four `native fn` declarations that the signatures alone cannot tell apart**, which is why all
four carry `#[compiler_resolved]` and why this entry is long. Three calls choose nothing:

1. `concat()` with no arguments answers `""`. The string form and the array form both accept
   zero arguments, so the rule answers the string by fiat -- the same fiat the builtin used.
2. `concat(parts)` for a `[string]` must answer `"xyz"`. The array form accepts that same
   argument as one part and would answer `[string]`. The two differ **only in what they
   return**, and F148 ruled out choosing an overload by return type -- it is the same reason
   `to_set` and `to_mutable_set` have to be two names.
3. `concat(b, [5], mutable([6]), [7])` mixes an array and a builder in one variadic tail. A
   declaration's variadic parameter has **one** type, so the two builder declarations each state
   half of what the call may pass, and the rule routes the whole tail to one C function that
   reads either world per element. A single declaration would need a union type, which
   docs/DESIGN.md refuses outright ("a language that refuses general bounds should not acquire
   general unions for them"). `concat(b)` with an empty tail likewise fits both.

For three sessions this was recorded here as the conversion that *could not* be done, and the
three walls above are why -- but they are all walls in front of **picking a declaration from the
arguments**, and none of them is a reason for the name to keep a builtin id. `#[compiler_resolved]`
says that out loud: the rule in check_call still chooses, exactly as the arm did, and what it
records is `call_fn` instead of an id, so the declarations are the signature of record, named
arguments and `--api builtins` come from them, and the ~48-comparison id walk is gone. The
attribute is also the only thing keeping these four past the check that refuses an
indistinguishable pair as a redefinition; see its comment in src/ast.h. Converting `mutable` is
blocked for its own reasons, recorded in docs/design/full-overloading.md §4.

### `split`

```
pub native fn split(s: string, separator: string) -> [string]
```

### `trim`

```
pub native fn trim(s: string) -> string
```

`s` without leading and trailing blanks (space, tab, CR, LF).

### `is_empty`

```
pub native fn is_empty(s: string) -> bool
```

Whether `s` has no characters: `s == ""`.

### `is_blank`

```
pub native fn is_blank(s: string) -> bool
```

Whether `s` is empty or only blanks (whatever `trim` removes): `trim(s) == ""`, without making the trimmed string.

### `char_at`

```
pub native fn char_at(s: string, i: int) -> string
```

The one-byte string at position `i` (`0 <= i < len(s)`, a runtime error otherwise), so
`char_at(s, i) == "x"` replaces `substring(s, i, i + 1) == "x"`. It is a byte, not a character.

### `last_index_of`

```
pub native fn last_index_of(s: string, needle: string) -> int
```

The byte position of the last `needle` in `s`, or -1. An empty `needle` is found at `len(s)`.

```
pub native fn last_index_of(s: MutableString, needle: string) -> int
```

The same of a builder: no `from`, because the immutable member above has none either.

**`mutable_string_last_index_of` until 2026-10-22**, the third of F205's prefix leftovers and a
free rename -- the symbol the fallback derives from `last_index_of` on a MutableString is the one
already registered. It sits here rather than in the MutableString block below because a set's
members sit together, which is the rule that block predates.

### `is_char_at`

```
pub native fn is_char_at(s: string, i: int, c: string) -> bool
```

True when `c` appears in `s` starting at position `i`. False, never an error, when `i` is out
of range or `c` does not fit, so `is_char_at(s, p, "x")` replaces the guarded
`p < len(s) && char_at(s, p) == "x"`. `c` is normally one character but any text works.

### `chars`

```
pub native fn chars(s: string) -> [string]
```

`s` split into its individual code points, each its own one-character string:
`chars("héllo")` is `[h, é, l, l, o]`, not six one-byte pieces. A runtime error naming the
byte offset if `s` is not valid UTF-8 (only reachable by chaining a byte-level function like
`substring` into this one -- `bytes_to_string` already rejects invalid UTF-8 at the boundary
where it would otherwise enter a program).

### `char_count`

```
pub native fn char_count(s: string) -> int
```

The number of code points in `s`: `char_count("héllo")` is 5, where `len("héllo")` (bytes) is
6. Cheaper than `len(chars(s))` when only the count is wanted. Same UTF-8 requirement as `chars`.

### `truncate_chars`

```
pub native fn truncate_chars(s: string, max_chars: int) -> string
```

`s` shortened to at most `max_chars` code points, without ever cutting one in half -- the safe
way to truncate text for display (`substring` counts bytes and can produce invalid UTF-8).
`max_chars >= char_count(s)` returns `s` unchanged; `max_chars` must not be negative. Same
UTF-8 requirement as `chars`.

### `equals_ignore_case`

```
pub native fn equals_ignore_case(a: string, b: string) -> bool
```

True when `a` and `b` are equal when the ASCII letters A-Z are read as a-z (other bytes,
non-ASCII included, must match exactly). Builds no intermediate strings, unlike comparing
`to_lower(a)` and `to_lower(b)`.

### `index_of`

```
pub native fn index_of(s: string, needle: string, from: int) -> int
```

The byte position of the first `needle` in `s` at or after `from` (`0 <= from <= len(s)`, a
runtime error otherwise), or -1. An empty `needle` is found at `from`. The `[byte]` half of the
set is in array.zm, which carries why the two share the name and what it cost to arrange.

```
pub native fn index_of(s: MutableString, needle: string, from: int) -> int
```

The same of a builder, searched in place so a string being built can be inspected without
`snapshot`, which copies all of it per call. `0 <= from <= len(sb)`, a runtime error otherwise.

**`mutable_string_index_of` until 2026-10-22**, and the rename is what its own comment had been
asking for: it took a `from` "because the immutable `index_of(s, needle, from)` does -- mirroring
the twin matters more than saving an argument", which argues for sharing the twin's *name* rather
than for matching its shape under a different one. Three members now, one per world -- `[byte]`
in array.zm, `string` above, `MutableString` here -- and the symbol is unchanged.

### `starts_with`

```
pub native fn starts_with(s: string, prefix: string) -> bool
```

True when `s` begins with `prefix`.

### `ends_with`

```
pub native fn ends_with(s: string, suffix: string) -> bool
```

True when `s` ends with `suffix`.

### `replace`

```
pub native fn replace(s: string, old: string, new: string) -> string
```

`s` with every non-overlapping `old` replaced by `new`. `old` must not be empty.

### `to_upper`

```
pub native fn to_upper(s: string) -> string
```

`s` with the ASCII letters a-z made upper case; other bytes are unchanged.

### `to_lower`

```
pub native fn to_lower(s: string) -> string
```

`s` with the ASCII letters A-Z made lower case; other bytes are unchanged.

### `join`

```
pub native fn join(
    prefix: Option<string>,
    suffix: Option<string>,
    separator: Option<string>,
    pieces: variadic string
) -> string
```

The strings `pieces` joined into one in a single pass, given one by one
(`join(pieces: "a", "b", "c")`) or as an array (`join(pieces: arr)`). `separator` goes between
the pieces, and `prefix` and `suffix` around the whole; each is optional in a named call:
`join(prefix: "[", suffix: "]", separator: ", ", pieces: arr)` is `[a, b, c]`. Written as a
native so that joining N strings is O(N), where adding them with `concat` one at a time is not.

### `split_once`

```
pub fn split_once(s: string, separator: string) -> Option<[string]>
```

Splits `s` at the FIRST `sep`: Some([before, after]), or None when `sep` is not in `s`. The
right tool for `key=value`, `host:port` and similar, where the value may itself contain the
separator (`split` would cut it apart). `sep` must not be empty.

### `mutable_string_new`

```
pub native fn mutable_string_new(capacity: Option<int> = 20, grow_factor: Option<float> = 2.0) -> MutableString
```

An empty builder. `capacity` is the bytes to reserve up front and `grow_factor` the multiple
its capacity grows by when an append would overflow it; both are optional, so `mutable_string_new()` is
usually all that is written.

### `append`

```
pub native fn append(s: MutableString, pieces: variadic string)
```

Appends each of `pieces`, given one by one or as an array. The builder changes in place and
nothing is returned.

### `append_byte`

```
pub native fn append_byte(s: MutableString, pieces: variadic byte) -> bool
```

Appends raw bytes, for text assembled from decoded bytes (a percent-decoder, say):
`append_byte(sb, 195b, 169b)`. A string cannot hold a 0 byte, so if any byte is `0b` NOTHING
is appended and the result is false (true otherwise). The bytes need not be valid UTF-8 on
their own.

**Deliberately not `#[effectful]`**, which the 2026-10-22 audit of that attribute stopped at.
It writes its argument and answers a value, which is the shape of the three `remove`s and of
`add` -- but their `bool` says "did anything change?", an answer a caller is entitled to ignore,
and this one says **whether anything was appended at all**. Discarding that loses a failure
silently. So the criterion is not "mutates and answers" but "mutates, and the answer is one the
ordinary call has no use for"; if `append_byte` ever joins an overload set, the dropped-result
gate refusing a discarded result will be right.

### `mutable_string_get`

```
pub native fn mutable_string_get(s: MutableString, index: int) -> string
```

Reading and writing one byte of the buffer: what lets a string being built be inspected without
`snapshot`, which copies all of it per call. Byte-indexed, like every string operation in the
language -- `len` is bytes and `char_at(s, i)` is the one-byte string at `i`, so there is no
UTF-8 question left to settle here.

**Searching it used to be here too**, and moved on 2026-10-22 when
`mutable_string_index_of`/`mutable_string_last_index_of` became members of `index_of` and
`last_index_of`: a set's members sit together, so they went up to join their immutable twins.
What stays is what this world alone has.

### `set`

```
pub native fn set(s: MutableString, index: int, value: string)
```

Replaces one byte: one element in, one element out. A `value` of any other length is a runtime
error rather than a splice, because splicing would change the length and `set` nowhere else in
the language does that.

**`mutable_string_set` until 2026-10-22.** Its own comment had said it was "mirroring the
immutable `set(arr, i, x)`" while keeping a prefix that said argument one is a MutableString --
which argument one says. `append` and `append_byte` dropped theirs in F205 and these were the
leftovers of that pass.

The symbol is unchanged: the fallback derives `mutable_string_set` from `set` on a MutableString,
so the C registration is exactly what it was. What the rename did cost is one line in the `set`
arm, the last arm-handled name in the prelude: it refused every shape it did not recognise, so a
MutableString had to be added to the list it declines to the resolver instead.

### `snapshot`

```
pub native fn snapshot(s: MutableString) -> string
```

What has been appended so far as an ordinary `string`; O(n), and the builder stays usable
afterwards. The one crossing out of a MutableString, and the reason there is no
`mutable_string_to_string`: `snapshot` is the word every builder uses, its other three members
taking a MutableArray, a MutableMap and a MutableSet in their own files.

**In a loop this is the quadratic cost the builder exists to avoid.** `append` is what a loop
writes; `snapshot` is what runs once at the end (`--lint` flags the other shape).

### `clear`

```
pub native fn clear(s: MutableString)
```

Empties the builder in place, keeping the handle and its capacity.

### `lines`

```
pub fn lines(s: string) -> [string]
```

The lines of `text`, without their terminators. Written because a cold model reaching for
`text.lines()` was sent to `split(text, "\n")`, which is close but not the same thing: `split`
leaves a `\r` on every line of CRLF text and leaves a trailing empty string after a final
newline, so "a\nb\n" splits into three pieces where it has two lines. Both of those are the
kind of difference that shows up as an off-by-one in someone's line count rather than as an
error (docs/reviews/tasks.md F35).

A final newline is a terminator, not a separator -- "a\nb\n" and "a\nb" both have two lines --
which is how every line-oriented tool reads a text file. An empty string has no lines.

### `reverse_string`

```
pub fn reverse_string(s: string) -> string
```

`s` with its characters in the opposite order. Reverses *code points*, not bytes, so reversing
text outside ASCII gives text rather than mojibake -- the byte-level version of this is the
classic way to corrupt UTF-8, and `chars` already answers in code points.
