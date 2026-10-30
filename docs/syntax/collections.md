# Arrays, maps, sets and loops

Part of the [v0 syntax reference](README.md).

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

- **`arr[i] = x` is sugar for the rebinding, not a mutation** -- it is checked as
  `arr = set(arr, i, x)`, so it needs `var` and reports the ordinary `val` error without it.
  `push`/`insert`/`set` are themselves pure, never mutating their `arr` argument, regardless of
  `val`/`var` (a value has no notion of mutability in Ziman, only a *binding* does). Because the
  statement rebinds a name, the left-hand side has to *be* a name: `p.cells[0] = x` and
  `rows[0][1] = x` are errors that name the copy to write instead.
- **A `MutableArray` takes `b[i]` and `b[i] = x` too, and there they are the mutation.** Nothing is
  rebound and nothing is copied, so `val b = ...` is right. Reading and writing element `i` are the
  questions every sequence answers, so they are not spelled per type -- the same reason `len(b)`
  and `contains(b, x)` take either world. `set(b, i, x)` is deliberately *not* accepted: a copying
  call would read as though it left the container alone.
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
  `mutable_array_filled`, `b[i]`, `b[i] = value`, `push`, `insert`, `remove`,
  `narrow`, `swap`, `reverse`, `filter`, `sort`/`_desc`/`_with`, plus the
  bare `len`, `contains`, `clear` and `snapshot`) is the right tool for building/editing a
  large array piece by piece. `insert` takes `0..len` inclusive, so inserting at the length appends;
  **`narrow(b, start, end)` keeps that window in place** and is the in-place counterpart of `slice`:
  a query answers a value and leaves its source alone, a mutation changes the container and answers
  nothing. It is builder-only, like `clear` and `snapshot`, an immutable array having nothing to
  narrow. It replaced `mutable_array_truncate(b, n)`, which was `narrow(b, 0, n)`, and adds the half
  that one could not do -- dropping a **prefix**, which was `remove(b, 0)` in a quadratic loop
  against one pack-aware memmove. `clear(b)` is `narrow(b, 0, 0)` and stays the word for emptying.
  Unlike truncate it does not clamp: a range past the length is a runtime error, where
  `truncate(b, 99)` on a length-2 builder was a silent no-op that hid a wrong computation.
  `remove` answers what it removed, which is why there is no `pop` -- `remove(b, len(b) - 1)` is
  one. It is the array's own word: `remove(b, i)` on a builder answers the **element**, where
  `remove(a, i)` on an array answers the new array, the difference being the two worlds rather than
  two names. Discarding the builder's result is therefore fine, while discarding the array's is an
  error, that one having copied and changed nothing. Both are O(n), where `push` is
  amortised O(1) -- a loop of `insert(b, 0, v)` is quadratic, and that is why there
  is no `push_front`: the name would read as `push`'s twin and promise a cost it cannot give. Build
  tail-first and `reverse(xs)` once, which copies nothing. `filter(xs, keep)` on a builder keeps
  what its predicate accepts, in place and **allocating nothing** -- the only filter in the language
  that doesn't. `sort_with(xs, cmp)` copies no elements either (a merge sort over positions,
  then the permutation applied by following its cycles), while `sort`/`sort_desc` on a builder make
  two copies -- and that is the cheaper option rather than a compromise, because for a packed
  `[int]`, `[float]` or `[byte]` buffer those two compare *natively*, with no interpreter call per
  comparison, which is worth far more than two memcpys. A comparator of your own always went
  through the interpreter, so `sort_with` had no such trade to make. It
  reads back in place: `len(xs)`, `xs[i]` and `for x in xs` cost nothing, where
  `snapshot` copies the whole buffer every call (`--lint` flags that in a loop, and also flags
  `f(.., snapshot(b), ..)` when `f` has an overload reading the builder directly -- `contains`,
  `len` and the rest). See "Builtins" for exact signatures.

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
print(get(stock, "apple"))               // Some(4); None if absent -- always an Option<V>
seen = add(seen, 3)                      // a set adds with 'add'; 'insert' is positional only
print(keys(stock))                       // [apple, fig]; keys_set(stock) is {apple, fig}
print(values(stock))                     // [4, 9]; to_array(s) is the array of a set
for name, qty in stock { ... }           // key and value; `for name in stock` binds keys only
for x in seen { ... }                    // a set has one binder
```

- **Helpers** (return new collections, infer types from arguments): maps --
  `get_or(m, k, default)`, `merge(a, b)` (`b` wins on shared keys); a small map is written as a
  literal, `Map<string, int> { "a": 1 }`, and one built from two arrays computed at run time is a
  `MutableMap` and `set` in a loop (`map_from_arrays(keys, values)` was this list's third entry
  until F211, with no caller anywhere in the repo); sets -- `union`/`intersect`/`difference(a, b)`
  (`a`'s order, `b`'s extras appended for union), `is_subset(a, b)`, `to_set(items)`,
  `add(s, items...)`.
- **The names `--api builtins` could not see are down to one.** Twenty when the Map/Set
  pass began; Groups A-C dissolved eleven by giving them bare names, and the last eight
  were settled with it: `option_unwrap_or` became `get_or` over an `Option` (the
  one-entry case of the question it already asks of a `Map`), `mutable_update` and
  `mutable_map_update` became `update` over a cell and over a map entry,
  `mutable_map_update_all` became `update_all`, and `bytes_concat`, `split_once`,
  `inverse` and `map_err` (then called `result_map_err`) only ever needed `pub`. `array_order_of` stays
  unadvertised on purpose. *(`bytes_concat` went further in F213 and was deleted: its body was
  `return concat(parts)`, and `concat` has taken `variadic [T]` all along, so advertising it had
  published a second spelling of a builtin.)*
- **Group C, settled 2026-10-20**: `merge(a, b)` pours one map into another in either world
  (`set_all` is gone); `entries(m)` joins `keys` and `values` across both Map worlds;
  `map(c, f)` now covers a `Map` (`fn(K, V) -> R`, key-aware like `filter`) and a `Set`,
  retiring `map_map_values` and `set_map`; `set(m, k, v)` and `remove(m, k)` gain their
  builder forms; and **`remove` answers the value it took out while `discard` answers
  nothing** — both kept, because the value is wanted about half the time and building an
  `Option` to throw away is what `discard` exists to avoid.
- **`add(s, items...)` takes a `Set` or a `MutableSet`**, variadic in both, and replaces
  four names: `insert` on a `Set`, `set_insert_all`, `mutable_set_add` and
  `mutable_set_add_all`. Unlike `remove`, the two worlds agree on what they answer — a
  `Set` gives a new `Set` whether you add one element or five, a `MutableSet` gives
  whether anything was new — so the variadic form subsumes the singular rather than
  conflicting with it. **`insert` is positional only now**: it puts a value at an index in
  an array, and no longer doubles as set membership.
- **`remove(c, keys)` is variadic for a `Map`, a `MutableMap`, a `Set` and a `MutableSet`** — an
  array, several values or one value spell the same call in every world, and `#[at_least_one]`
  makes none a compile error. The immutable halves answer a new `Map` or `Set`, the builders
  whether anything was removed. **An array's `remove` stays singular and takes an *index*.**
- **`remove_all` was that name until 2026-10-22**, and the paragraph here had already worked out
  both the merge and its one obstacle: *"a variadic `remove` would subsume the singular for `Map`,
  `Set` and `MutableSet`, but not for `MutableMap`, whose singular answers the removed `Option<V>`
  where the plural answers `bool` — and an array's `remove` takes an *index*, which must not become
  variadic."* All of that held. The merge happened anyway, by spending the `Option<V>`:
  `remove(mm, k)` answers `bool` now, so taking a value out of a `MutableMap` in one step is no
  longer expressible. Nothing in lib/, tests/ or the sample backend ever read it — the only two
  readers were `remove_all` and `remove_keys`, each using it as a presence test. The old name
  points at the new one, and `remove`'s immutable `Set` member is a capability the plural never
  had.
- **`union`, `intersect`, `difference` and `is_subset` take either world**, the awkward row:
  each has three declarations, and **argument two** chooses between two of them, because a
  `MutableSet`'s operand may come from either world. The immutable halves took only a
  `Set` as their operand, an asymmetry that predated the pairing and was left alone —
  **and is closed for `is_subset` as of 2026-10-22 and for nothing else.** `is_subset` has four
  declarations now, one per receiver/operand pair. `union` and `intersect` would each need a second
  *variadic* declaration to match and `difference` a second operand form; neither was asked for, so
  the asymmetry stands for those three and is now an exception rather than a rule.
- **`union` and `intersect` are variadic as of 2026-10-22**, in `merge`'s shape: the immutable
  member has no distinguished receiver at all (`union(a, b, c)` and `union(sets)` are the same
  call, so folding a list needs no loop), and each in-place member takes any number of operands.
  Both are symmetric, which is what makes a receiver pointless for them. The operands of one call
  must come from **one** world — a tail is `Set`s or `MutableSet`s, since a parameter cannot be
  both — so mixing them in a single call is a type error naming the argument.
  **`union` of none is the empty set and `intersect` of none is a runtime error**, which is the
  mathematics rather than an inconsistency: union's identity is the empty set and exists,
  intersection's is the universal set and does not. (`#[at_least_one]` rules out `union()` with no
  argument at all; an empty array *spread* is one argument and still reaches the function.)
  `difference` stays two-operand because it is not symmetric, and `is_subset` because it is a
  predicate rather than a fold — a variadic form of either would answer a second question, not the
  same one over more arguments.
- **`get(m, k)`, `get_or(m, k, d)` and `to_array(s)` take either world too**, items 3 and 4
  of the same pass: a `Map` or a `MutableMap` for the first two, a `Set` or a `MutableSet`
  for `to_array`. `map_get`, `map_get_or`, `set_items`, `mutable_map_get`, `mutable_map_get_or`
  and `mutable_set_items` no longer exist in any spelling. **`to_array` was itself `items` until
  2026-10-22**, renamed because a conversion is named for what it answers, not for what its
  argument holds — the rule `to_option`/`to_result` settled and `to_set` had been following all
  along. The old name was also the prelude's word for a sequence *parameter*, so one word had
  named a function, a receiver and a payload.
- **`to_mutable_array(s)` is the fourth corner**, added the same day: `to_set` and
  `to_mutable_set` go array-to-set over both array worlds, `to_array` and `to_mutable_array` come
  back over both set worlds. It is not `snapshot` — that answers the type a builder is *building*,
  where these answer a different type, which is why `to_array` takes no `MutableArray`.
- **`keys(m)`, `keys_set(m)` and `values(m)` take a `Map` or a `MutableMap`**, the second row of
  the Map/Set pairing pass. All answer immutable values from either world -- `keys` a `[K]`,
  `keys_set` a `Set<K>`, `values` a `[V]` -- because a result like this is nearly always consumed
  by a `for` or returned, so handing back a handle would allocate a thread-bound one for nothing.
  `map_keys`, `map_values`, `mutable_map_keys` and `mutable_map_values` no longer exist in any
  spelling.
- **`keys` answers an array and `keys_set` the `Set`** — swapped on 2026-10-24, having been the
  other way round since 2026-10-22. Three reasons, in order of weight. `keys` and `values` are the
  canonical pair and disagreed about their answer type. Callers overwhelmingly want to walk the
  keys: six places wrote `to_array(keys(m))`, and of the four using the `Set` directly, three were
  `len(keys(m))` (which is `len(m)`) and one a `contains` a `Map` answers itself. And the array is
  the *cheap* one — building the `Set` hashes every key into a trie that is then walked straight
  back out, about 10x on a 100,000-entry map.
- **Two names because a return type cannot tell two declarations apart.** `keys(m) -> [K]` and
  `keys(m) -> Set<K>` differ only in what they answer, and a name may be shared only when every
  *call* can distinguish the members -- `math.floor`/`math.floor_float` and
  `fs.read_text`/`read_bytes` again. Neither is `to_array(m)`, which would claim to convert the
  map rather than answer one of the three different questions a map can be asked.
- **What the swap broke, and what it did not.** `len` and `contains` answer for a `Set` and an
  array alike, so almost every call site kept working untouched. The one shape that cannot is an
  explicit `==` against a `Set` literal (`keys(m) == Set<int> {}`), which is exactly the case where
  the caller meant a set -- `keys_set` there.
- **`filter(c, keep)` takes any of the six collections**, which is the first row of the Map/Set
  pairing pass: an array or a `MutableArray` with `keep: fn(T) -> bool`, a `Map` or a
  `MutableMap` with `keep: fn(K, V) -> bool`, a `Set` or a `MutableSet` with `fn(T) -> bool`. The
  immutable three answer a new collection; the builders rearrange in place and answer nothing --
  the same split every paired name in this language uses. `map_filter`, `set_filter`,
  `mutable_map_filter` and `mutable_set_filter` no longer exist in any spelling.
  A type argument a lambda's result would have to supply is inferred only when the lambda is fully
  annotated (every parameter typed and `->` written), which is the same information a named function
  gives; an unannotated lambda takes its types *from* the expected type, so it has none to contribute
  and the call needs them written: `map<K, V, R>(m, f)` applies `f: fn(K, V) -> R` to every entry (keys/order unchanged);
  `map<T, U>(s, f)` over a Set merges elements mapping to the same value; `entries<K, V>(m)` is an
  array of `MapEntry<K, V> { key, value }` in map order. Arrays into maps: `array_frequencies<T>`
  counts into `Map<T, int>`, `array_index_by<T, K>(items, key)` builds `Map<K, T>` (later element wins;
  `group_by` keeps them all), `join(items: [string], sep)` puts `sep` *between* the pieces (the
  builtin `join` is a path join over strings, not this), and `sum(items: [int])` totals them --
  integers only, since `+` traps on overflow so the answer is exact or an error, where a float
  total depends on summation order and `fold` expresses whichever order you want.
- **No derive converts one**: `#[derive(Json)]`, `Row` and `Properties` all refuse a field whose
  type is a thread-bound handle, however deeply nested -- inside an `Option`, an array, a type
  argument. Nothing written out could decode back to the same mutable object, so hand over the
  finished value and derive that: `snapshot(x)` for a collection, `get(c)` for a cell.
- **Printing one**: a handle prints as the value it is building -- `[1, 2, 3]` for a
  `MutableArray`, its text for a `MutableString`, `{a: 1}` for a `MutableMap`, `Mutable(5)` for a
  cell -- not as the one-field struct it is. A handle *inside* another value prints a summary
  instead (`MutableArray(3 items)`, `MutableMap(2 entries)`), which is what makes a cycle
  unfollowable: a struct holding a `MutableArray` of itself prints its fields and stops at the
  handle. An immutable value never needs this, because a value is a tree.
- **Iterating a mutable collection**: `for x in xs` and `for i, x in xs` over a `MutableArray`
  walk the **live buffer**, re-reading the length each pass, so a body that appends sees its own
  additions and one that clears ends the loop. `for k in m` / `for k, v in m` over a `MutableMap`
  and `for x in s` over a `MutableSet` walk a **snapshot of the entries** in insertion order, so
  the body may change the handle freely without affecting what the loop visits. The difference is
  not a preference: a hashed handle has no cursor that could visit it in insertion order -- that
  order lives in a per-entry sequence number the entries have to be sorted by -- and a node's slots
  shift on insert, so it is a snapshot by construction. A `MutableSet` takes one binder, like a
  `Set`.
- **The mutable collections**: `MutableMap<K, V>` (`mutable_map_new`/`_set`/`_get`/`_get_or`/
  `_remove`/`_update`/`_update_all`/`_filter`/`_remove_all`/`_keys`/`_values`) and `MutableSet<T>`
  (`mutable_set_new`/`_add`/`_add_all`/`_remove`/`_remove_all`/`_filter`/`_items`), the
  `MutableArray` counterparts. **The mutators report what they did**: `add` and `remove` answer a
  `bool` — whether anything was added or removed — so a caller can dedupe without a `contains`
  first. `remove` on a MutableMap answered the value that was there, as an `Option<V>`, until
  2026-10-22; it answers `bool` like the rest now, and the `_all` forms that used to sit beside
  these were absorbed into the same names when `remove` went variadic. *(What keeps a
  `MutableSet<[int]>` from being a special case is that the checker compares the argument against
  the **element type**: argument one fixes it, so a lone `[int]` is one element and a lone `[[int]]`
  is a spread, each having only one reading. Each of these carries `#[at_least_one]`, so the no-op
  call is a compile error.)* `_keys` and `_values` return the immutable `Set<K>`
  and `[V]`, not handles -- a result like that is nearly always consumed by a `for` or returned,
  so a handle would allocate a thread-bound object per loop for nothing. Each mutates its own private trie in place (O(n) total instead of copying per
  entry) -- use one in a loop adding many entries. Type arguments are written out
  (`set<string, int>(m, "a", 1)`). **The four questions every collection answers have
  no per-type names**: `len(m)`, `contains(m, k)`, `snapshot(m)` and `clear(m)` work on all four
  mutable types, where there used to be `mapb_len`, `setb_contains`, `mapb_to_map` and the rest.
  `snapshot` copies into an immutable `Map`/`Set` (O(n), the handle stays usable, and later changes
  do not touch the copy). One thread at a time -- every `Mutable*` type belongs to the thread that
  made it.
- **Taking a second collection**: `concat(xs, parts…)`, `merge(m, values)` and
  `union`/`intersect`/`difference`/`is_subset` on a MutableSet all change (or read) the handle in
  the first argument, and the rest take either world -- `concat` takes any number of them, mixed
  freely. The three self-aliasing cases get three different answers, each deliberate:
  `concat(xs, xs)` **doubles** the array, `merge(m, m)` is a no-op, and
  `difference(s, s)` **empties** the set. Union and intersect with yourself are no-ops
  and `_is_subset(s, s)` is true. With several aliasing parts the appends are *sequential*, each
  reading the target as it now is: `concat(c, c, c)` on a one-element builder answers four.
- **Crossing into the mutable world is `mutable(x)`, and back is `snapshot(b)`.** It beats `new`
  plus a loop for the two hashed shapes, because it copies the trie's nodes with the leaves shared
  rather than hashing and placing every entry again, and it sizes an array's buffer to the source
  in one allocation. **`to_set(xs)` and `to_mutable_set(xs)`** are the same question with the answer
  in different worlds, and each takes **either world** as its source -- an array or a MutableArray,
  read element by element rather than snapshotted, so accepting a handle saves a copy instead of
  hiding one. They have to be two names rather than overloads: they take the same argument types and
  differ only in what they return, which first-argument dispatch cannot distinguish. Both are named
  for the result because both **deduplicate** -- a reader of `to_set` expects that of a set, where
  `set_from_array`/`mutable_set_from_array` (their names until 2026-10-18) had to document it -- and
  `to_` rather than `as_` because they allocate and copy, where `as_` reads as a free view.
  There were three more -- `mutable_array_from`, `mutable_map_from`, `mutable_set_from`, each also
  either-world -- and they were removed once `mutable` covered the conversion they were mostly used
  for: keeping both put two spellings on one question. Their either-world half was a **clone**, and
  that went with them, so **a builder cannot be cloned**; it had no caller outside its own
  fixtures.
- **`mutable(x)` answers the mutable counterpart of whatever it is given.** For the four worlds
  that have a builder that is the builder, which reads as `snapshot` backwards:
  `mutable([1, 2])` is a `MutableArray<int>`, `mutable(s)` a `MutableSet`, `mutable(m)` a
  `MutableMap`, and `mutable("hi")` a `MutableString` holding `"hi"`. For **anything else** -- a
  scalar, a struct, an enum, even another handle -- there is no builder to answer and the mutable
  counterpart of a single value is a cell, so it is `Mutable<T>`.
  **Text joined on 2026-10-23**, reversing a recorded decision that it belonged in the cell half
  "because a MutableString is *filled* rather than converted". A MutableArray is filled too and
  converts anyway; what settled it is that the old answer compiled and then failed at whatever
  touched the cell, pointing at a line that was not the mistake. A *builder* argument still
  answers a cell holding that same handle -- there is no way to clone a builder.
  A **literal** argument is built straight into the builder -- `mutable([1, 2])` never collects the
  elements into an array first, saving three allocations per construction -- while `mutable(xs)` on
  a variable copies, the copy being the point there. `mutable([])` takes its element type from the
  context (`val xs: MutableArray<int> = mutable([])`, a parameter type, a `return`, a struct literal
  field), `[]` being the one literal that carries none of its own; with no context the error names
  the places that supply one and `mutable_array_new<int>()`, which needs none.
- **`mutable_array_new`'s `capacity` is not a length, and `mutable_array_filled(n, v)` is the one
  that is.** Capacity reserves room and leaves `len` at 0, so `b[0] = v` straight after
  `mutable_array_new(capacity: 5)` is a runtime out-of-bounds; before `mutable_array_filled` the
  only way to get slots you could index was to push them one at a time. `mutable_array_filled(5, 0)`
  answers five addressable slots, with `T` read from the value, and a negative length is a runtime
  error. Both parameters are required rather than a `fill: Option<T>` on `mutable_array_new`, whose
  first parameter would then have meant *capacity* without a fill and *length* with one. There is no
  immutable twin: an `[T]` of repeats is `snapshot(mutable_array_filled(n, v))`, and `array_filled`
  could only be a second name, differing from this one in nothing but its result.
  **The collections win the dispatch**: an array matches both `[T]` and the bare `T`, and the array
  form applies, so `mutable([1, 2])` is never a `Mutable<[int]>`. Write that one `mutable_new([1,
  2])`, which is unambiguous about which you meant. Two consequences worth knowing rather than
  discovering: a string answers `Mutable<string>` and not a `MutableString` (which is *filled*
  rather than converted -- `mutable_string_new` then `append`, and of 133
  `mutable_string_new` call sites in this tree exactly one seeds the buffer from a whole existing
  string), and a builder answers a cell holding that same handle rather than a clone of it, there
  being no way to clone a builder at all.
- **`MutableString`** is the text one: `mutable_string_new`/`_append`/`_append_byte` build it, and
  `_get`/`_set`/`_index_of`/`_last_index_of` read and search it in place, so a string being built
  can be inspected without `snapshot` copying all of it. Character-indexed like every other string
  operation, so `_get` answers the one-character string at an index as `char_at` does, and `_set`
  replaces exactly one character -- two characters is a runtime error, because changing the length is
  what `_append` is for. `_set` does move bytes, since one character of UTF-8 is one to four of them:
  writing `e` over `é` shortens the buffer and `é` over `e` lengthens it, while the character count
  `len` answers stays put. All four counted bytes until 2026-10-23, which made
  `char_at(sb, i) == char_at(s, i)` false for the same text in the two worlds. `_index_of` takes a `from` and `_last_index_of` does
  not, mirroring their immutable twins exactly.
- **Values, like arrays** -- `set`/`insert`/`remove` return a new collection, leaving the
  argument untouched. A persistent hash trie internally, so an update copies only a few nodes:
  building an n-entry map is O(n log n).
- **Insertion order**: `keys`/`values`/`to_array`/`for`/`print` all go in first-inserted
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
  `Map`, `Set` or array is found again by `get`/`contains`, and storing it twice keeps one
  entry -- otherwise a key could never be retrieved, and a `Map<float, _>` would grow without bound
  while `get` returned `None`. `compare(nan, nan)` is `0` and agrees with key lookup. This is the
  same line Java draws between `==` and `Double.equals`. `-0.0` and `0.0` are one key and `==` also
  equates them, so nothing differs there.
- **Names**: `Map`/`Set` are reserved words. `map_get`, `keys` (`Set<K>`), `values`
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
  `for` picks up where the first stopped). `iter(items)` makes one from an array;
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
  `Range` type to store/pass/print. Deliberate; see docs/design/journal/'s "Rationale consolidated
  from docs/syntax/" entry.
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
