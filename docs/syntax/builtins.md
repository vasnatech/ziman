# Builtins

Part of the [v0 syntax reference](README.md).

## Builtins

These need no `use` — they are the prelude's declarations plus the handful the interpreter
dispatches by name. **A file may declare a name a builtin has**, and a bare call then reaches its
own (see "A program's own function shadows a builtin" under named arguments); `prelude.name(...)`
reaches the builtin past it, and is the only way to.

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
- **`format_dynamic` was here and was deleted on 2026-10-22**, having no caller in this library,
  in the sample backend or in any example — only its own fixture. It was `format` for a template
  that is a runtime *value*, answering a `Result` because the checks `format` makes at compile
  time had to become data. **Nothing replaces it**: a template is a literal again, and a template
  that is genuinely a value has to be scanned by hand. The old name says so rather than reading as
  undefined. Noted plainly because it is the one deletion here that removed a capability rather
  than a second spelling of one — `map_from_arrays` had a map literal and `bytes_concat` had
  `concat`.
- `len(arr) -> int` / `len(s: string) -> int` — element count, or **character count**: on text
  `len` counts code points, so `len("héllo")` is 5. Also answers for a `Map`, a `Set`, and any of
  the four builders (`MutableArray`, `MutableString`, `MutableMap`, `MutableSet`), and for a
  `Mutable<T>` cell, whose length is always 1. So asking a collection its size never needs a
  per-type name or a conversion. It answers for a *handle*, not for a struct that holds one, which
  has no length of its own.
- `byte_len(s: string) -> int` — the byte count, for either string world. **Not the one to bound an
  index with** — that is `len`, because every index function counts characters. This is for the
  places that genuinely mean storage: a length prefix on a wire, a `Content-Length`, base64's
  padding modulus, a byte budget on a document. Using it as a scan bound is invisible on ASCII input
  and wrong on everything else. `byte_len(s) == len(to_bytes(s))`.
- **`len` on text counted bytes until 2026-10-23, and `char_count` counted characters**; they
  swapped, `byte_len` took the byte question and `char_count` was deleted. The old `len` was the
  silent-wrongness direction — "length" meaning bytes surprises a reader exactly when the text stops
  being ASCII, which is the case nobody tests — and the new one is the obvious-slowness direction,
  since counting code points is O(n) where a byte length is a field read. 482 call sites in this
  library and 92 in the sample backend moved to `byte_len`; none of them changed behaviour, because
  code written against byte semantics means bytes. They were found by deleting the declarations and
  compiling, not by searching, since a `len` on text stays an `int` either way.
- `mutable_new(v) -> Mutable<T>` / `get(c) -> T` / `set(c, v)` / `update(c, f: fn(T) -> T) -> T`
  — one cell holding one value: the root the four mutable collections specialise, and what lets a
  new mutable shape be written in Ziman instead of added to the language (a ring buffer is a struct
  over a `MutableArray` plus two `Mutable<int>`s). An update returns the new value, so a counter is
  `update(n, fn(x) { x + 1 })` — and because that writes the cell, the answer may be thrown away:
  `update` carries `#[effectful]`, which is what makes the counter idiom a statement.

  **Three of those four names are shared.** `get`, `set` and `update` each take a cell *and* a map,
  told apart by the argument: `get(c)` against `get(m, key)`, `set(c, v)` against
  `set(m, key, value)` and `set(a, index, x)`, `update(c, f)` against `update(m, key, f)`. They
  were `mutable_get`, `mutable_set` and `mutable_update` until the prefix came off (`update` with
  F201, the other two on 2026-10-22); the old spellings answer with the new one rather than "no
  such function". Only the constructor keeps a prefix, because `mutable_new(v)` is not a question
  anything else answers.

  Always initialised — there is no empty `mutable_new<T>()`, because `T` may have no zero — and `T`
  is anything, including another handle. A cell answers `len(c)`, which is always 1, and
  `contains(c, v)`, but **not** `snapshot` or `clear`: crossing out is `get`, an O(1) read rather
  than an O(n) copy, and emptying would need exactly the zero its type may not have. So a cell has
  no `from`/`snapshot` pair at all — `mutable_new` takes a value and `get` returns one, which *is*
  the crossing between the two worlds.
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
  Takes **either world**: from an array it shares storage and costs nothing, from a `MutableArray`
  it copies the window, which it must, a view being unable to share a buffer still being written.
  Taking the builder directly is the point -- `slice(snapshot(b), s, e)` copies the whole builder
  first, so a small window of a large one copied all of it.
  Does not copy (shares the original's storage, like `substring`); `push`/`insert`/`set` on
  either build a new array, so neither changes under the other.
- `concat` — **one name over three forms**, chosen by the first argument: `concat(pieces…)
  -> string`, `concat<T>(parts: variadic [T]) -> [T]` in a single allocation, and
  `concat<T>(b: MutableArray<T>, parts…)` appending in place from either world. **One argument that
  is an array spreads** — `concat(parts)` on a `[string]` is the string `"xyz"`, on a `[[int]]` it
  is the flattened `[int]`, and on any other array it is a copy of that one part. Two or more
  arguments are always pieces or parts. It was `concat` plus `concat_arrays` plus
  `mutable_array_push_all` until 2026-10-18; being a builtin it takes no named arguments, so the
  old `concat(parts: arrays)` spelling is now the positional `concat(arrays)`.
- `char_at(s, i) -> string` — the one-*character* string at `i` (`0 <= i < len(s)`, else runtime
  error), however many bytes that character takes: `char_at("héllo", 1)` is `é`.
- `split_once(s, sep) -> Option<[string]>` — `Some([before, after])` at the *first* `sep`,
  `None` if absent -- for `key=value` text whose value may itself contain the separator.
- `index_of(s, needle, from) -> int` — character index of the first `needle` at/after `from`
  (`0 <= from <= len(s)`, else runtime error), or `-1`; an empty `needle` matches at `from`.
- `last_index_of(s, needle) -> int` — character index of the last `needle` (`len(s)` if empty), or `-1`.
- `starts_with(s, prefix) -> bool` / `ends_with(s, suffix) -> bool`.
- `is_char_at(s, i, c) -> bool` — whether `c` appears at `i`; `false` (never an error) when `i`
  is out of range or `c` doesn't fit, replacing a guarded `p < len(s) && char_at(s, p) == "x"`.
- **Identifier case**: `snake_case`, `camel_case`, `pascal_case`, `kebab_case`,
  `upper_snake_case`, `upper_kebab_case`. `MiddleName` → `middle_name` → `middleName` →
  `MiddleName` → `middle-name` → `MIDDLE_NAME` → `MIDDLE-NAME`. A capital opens a word after a
  lower-case letter or a digit, or at the end of a run (`userID` → `user_id`, `IDCard` →
  `id_card`). **These convert a *name*** — a field, a type, a configuration key — and are
  **ASCII**: a letter outside it is passed through rather than guessed at.
  `camel_case` is deliberately **not** `snake_case`'s inverse — it drops underscores and raises
  what follows, so `userID` passes through untouched — because a `#[derive(Json)]` key is
  `camel_case` over a snake_case field name, and `json.snake_keys` is the inverse of exactly what
  it produces (a narrower rule, under which `userID` becomes `user_i_d`).
  Native, and shared with the compiler: `src/derive.c` calls the same `snake_case` for
  `Decl.snake_name`, where it used to have its own copy of the algorithm.
- `replace(s, old, new) -> string` — swaps every non-overlapping `old` (must not be empty).
- `replace(s, pairs: Map<string, string>) -> string` — **every key replaced by its value in one
  pass**, longest match first, and a replacement is never rescanned. That is the point: a
  sequential version is three lines a caller can already write, and those three lines are where the
  bugs are. `replace(text, Map { "\r\n": " ", "\n": " ", "\r": " " })` cannot turn a CRLF into two
  spaces whichever order the map is written in, where the chained form was correct only because
  `\r\n` came first; and `replace(v, Map { "\\": "\\\\", "\"": "\\\"" })` cannot escape an escape it
  just wrote. `Map { "a": "b", "b": "c" }` leaves `"a"` as `"b"`. An empty key is a runtime error,
  as for the three-argument form; an empty map answers `s`.
- `equals_ignore_case(a, b) -> bool` — ASCII case-insensitive equality, allocating nothing
  (unlike `to_lower(a) == to_lower(b)`). None of the functions above build intermediate strings.
- **Strings are stored as bytes and counted, indexed and cut in characters.** `len("héllo")` is 5,
  `char_at(s, 1)` is a whole `é`, `substring` cannot split a character, and the two searches answer
  character indices -- so `substring(s, 0, index_of(s, ",", 0))` means what it reads as. `byte_len`
  and `to_bytes` are the storage questions, under names that say so.
  **The unit moved on 2026-10-23**, and indexing UTF-8 by character would make every scanner
  quadratic, so a string carries a known-ASCII bit (`ZSTR_ASCII`, `src/value.h`) and each of these
  functions is two implementations: with no high byte anywhere a character index *is* a byte index
  and the code is the arithmetic it always was. Text with real multi-byte characters takes the walk,
  so random access into it is O(n) -- see `docs/design/journal/`. Invalid UTF-8 is a runtime error naming
  the byte offset, reachable only through `append_byte`, since `bytes_to_string` rejects it at the
  one real boundary and `substring` can no longer manufacture it: `len(s) -> int` (code points), `chars(s) -> [string]` (split into them:
  `chars("héllo")` is `[h,é,l,l,o]`, and `len(s)` is cheaper than `len(chars(s))`),
  `truncate(s, max_chars) -> string` (shortened without cutting a code point in half;
  `max_chars >= len(s)` returns `s` unchanged, negative is a runtime error). **It was
  `truncate_chars` until 2026-10-23**: the suffix said which unit the limit counts in, back when
  `len` counted bytes; characters are the default unit now unless a name says `byte_len`, and the
  parameter keeps `max_chars` so the unit stays at the call site.
- `trim(s) -> string` — leading/trailing whitespace removed.
- `lines(text) -> [string]` — the lines, without their terminators. Not the same as
  `split(text, "\n")`: a `\r` is removed from each line of CRLF text, and a final newline
  *terminates* the last line rather than starting an empty one, so `"a\nb\n"` and `"a\nb"` both
  have two lines. A blank line in the middle is a real line and stays; an empty string has none.
- `reverse(s) -> string` — `s` with its characters in the opposite order, reversing *code points*
  rather than bytes, so text outside ASCII comes back as text. **It was `reverse_string` until
  2026-10-23**, the last `_string` suffix in the prelude; `reverse` is one set of three now, this
  member plus an array and a builder in array.zm. `is_empty(s) -> bool` (`s == ""`);
  `is_blank(s) -> bool` (empty or only whitespace). `to_upper`/`to_lower(s) -> string` — ASCII
  folding under the invariant locale; `to_upper(s, l: Locale)` is the locale-aware form (see
  "Locale"), and `equals_ignore_case` and `compare` take one the same way.
- **The character classes**: `is_alpha(s)` / `is_numeric(s)` / `is_alpha_numeric(s) -> bool` —
  whether **every** character of `s` is an ASCII letter, an ASCII digit `0`-`9`, or either. False
  for `""`, deliberately: "every character is a digit" is vacuously true of none, and answering
  that way would make the guard useless. `is_digit(s)` is the single-character form (one
  character, and that character a digit) and `is_char(s)` asks only whether `s` is exactly one
  character — `len(s) == 1`, answered without counting the rest, and false for text that is not
  valid UTF-8. Each takes a `string` or a `MutableString` (read in place, no `snapshot`).
  **ASCII by decision**: six private helpers across `lib/` each re-implemented the digit test and
  all six meant ASCII, reading CSV cells, TOML keys and zone offsets where a non-ASCII digit is a
  syntax error rather than a digit; the Unicode questions are `regex`'s `\p{L}` and `\p{Nd}`,
  which have real general-category tables behind them. They ask nothing about sign, point or
  exponent — `is_numeric("-7")` is false, that being `parse_int`'s question.
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
- `abs`, `min` and `max` moved to the `math` module on 2026-10-22 and are **no longer bare
  vocabulary**: `use math`, then `math.abs(x)`, `math.min(a, b)`. `math.abs`'s `T` is
  `int`/`float`/`bigint`/`decimal`, not `byte`; `math.min`/`math.max` are variadic and
  `Ord`-bounded like `compare`/`sort` — two-or-more arguments or one array (the `variadic T`
  convention), at least one value required (`math.min()` is a compile error, an empty array a
  runtime one), every argument the same `T` with no implicit mixing. The prelude is the names a
  program gets without asking, and arithmetic beyond the operators is not among them.
- Rounding (`floor`/`ceil`/`round`, `math.round`/`_floor`/`_ceil`) and `math.pow` are in
  the `math` module too, and always were.
- `push(arr: [T], values: variadic T) -> [T]` — new array with the values appended;
  `arr = push(arr, x)` in a loop is amortised O(1) (the result keeps spare room the *newest*
  version's next `push` reuses; an older version's `push` still copies, so no version changes
  under another). Several values cost about what the values cost, not one copy each, because every
  result after the first is itself the newest version.
- `push(b: MutableArray<T>, values: variadic T)` / `insert(b: MutableArray<T>, i, x)` — the same
  two words for the same two questions of a builder, writing it in place and answering nothing.
  There is no `set` form: write the element with `b[i] = x`.
- `insert(arr, i, x) -> [T]` — `x` inserted at `i` (`[0, len(arr)]`, else runtime error).
- `remove(b: MutableArray<T>, i) -> T` — the element taken out of the builder, which is written in
  place. No `MutableMap`/`MutableSet` form: those answer an `Option<V>` and a `bool`, so they keep
  `mutable_map_remove`/`mutable_map_discard`/`mutable_set_remove`, and each refusal names them.
- `swap(arr, i, j) -> [T]` / `swap(b: MutableArray<T>, i, j)` — the two elements exchanged: a new
  array from an array, in place for a builder. One allocation, where the only previous spelling for
  an array, `set(set(a, i, a[j]), j, a[i])`, copied the whole thing twice to move two slots. It is
  the only name for either: `mutable_array_swap` was removed once this covered both worlds, so
  unlike `push` there is no prefixed form underneath.
- `set(arr, i, x) -> [T]` / `remove(arr, i) -> [T]` — element at `i` replaced/removed
  (`[0, len(arr))`, else runtime error). `insert`/`set`/`remove`/`concat` always copy.
  No "pop" -- indexing already covers peeking.
- **Array helpers, callable with no import** — the counterparts of the bare `map_*`/`set_*`
  helpers that `Map` and `Set` already have: `map<T, U>(items, f)`,
  `filter_present` below. All return a new array.
- **Eleven more lost the prefix and take either world**: `map`, `flat_map`, `fold`, `for_each`,
  `find`, `first`, `last`, `any`, `all`, `count`, `sum`. Each walks its sequence once and keeps
  nothing, so a `MutableArray` needs no `snapshot` -- which is what the route used to be, and it
  allocated and filled a whole array before the first element was looked at. The `array_`/
  `mutable_array_` names remain underneath as the halves these dispatch to. An **empty literal**
  dispatches to the array half unchecked (a handle is never a literal), so `first<int>([])` and
  `map<int, int>([], f)` work as they always did.
- **Six more take either world**: `distinct`, `group_by`, `index_by`, `frequencies`, `min_by`,
  `max_by`. Four of them allocate a Map or a Set whatever they are given, so the input copy saved
  is a smaller share of their cost than it is for `fold`; `min_by`/`max_by` allocate nothing and are
  the same case as the eleven above.
- **Four more take either world**: `is_sorted`, `lower_bound`, `upper_bound` and `binary_search`,
  the questions an *ordered* sequence answers. These are the purest case of the rule: their array
  bodies reach the sequence through `len()` and `[i]` only, so each builder twin is that body
  character for character. Three of them require the sequence to be in order already, and that
  precondition is the caller's in either world -- unchanged by the pairing, and `is_sorted` is
  there to ask it. A builder is if anything the more natural subject, since `sort(b)` rearranges
  the handle in place and the thing you then search is that same handle:
  `binary_search(snapshot(b), x)` copies the whole buffer to ask a question that reads O(log n)
  of it.

  What stays `array_`-prefixed is everything a builder cannot answer the same way:
  `array_order_of`, `permute`, and `iter`, whose `Iter` outlives the call. (`filter_present` lost
  its own `array_` prefix in F170 -- it takes a written-out list of maybes, so the prefix was
  naming a world the argument could not have come from.)
- **`array_join` was deleted rather than renamed.** It was one line delegating to `join`, which owns
  the bare name and does strictly more (`prefix`/`suffix`), so its callers now say
  `join(separator: s, pieces: a)` -- what the wrapper expanded to. The one cost: `join`'s `pieces`
  is a variadic, where an empty literal cannot be inferred, so `array_join([], s)` becomes a typed
  `val` plus the call.
- **Twenty-eight of them lost their prefix**, because neither world owned the bare name:
  `map`, `filter`, `reverse`, `sort`, `sort_desc`, `sort_with`, `sort_by`, `sort_by_desc`,
  `flat_map`, `fold`, `for_each`, `find`, `first`, `last`, `any`, `all`, `count`, `sum`,
  `distinct`, `group_by`, `index_by`, `frequencies`, `min_by`, `max_by`, `is_sorted`,
  `binary_search`, `lower_bound` and `upper_bound` each take an array **or** a `MutableArray`,
  answering a new array from the one and rearranging the other in place.
  **Each is two ordinary declarations sharing a name**, and the call resolves to one of them from
  its arguments -- so `array_map`, `mutable_array_sort` and the rest no longer exist in any
  spelling, and `ziman --api builtins` lists each name twice, once per world. `sort<int>([])` is
  spellable: a declaration takes type arguments and named arguments where the builtin these
  replaced took neither.
- `inverse<T>(cmp) -> fn(T, T) -> int` — a comparator with its order turned round, which is to
  `sort_with` what `sort_desc` is to `sort`: `sort_with(xs, inverse(by_length))` rather than a
  second lambda written backwards, where the only difference is argument order and getting it wrong
  is silent. It composes (`inverse(inverse(cmp))` is `cmp`) and knows nothing about arrays, so the
  one function serves both worlds.
- **`filter_present<T>(items: variadic Option<T>)`** is the odd one out: it takes *maybes* and
  returns the values that are there, in order, which is how a list is built out of fields that
  may each be absent without an `if` per field. It is variadic because the call site is nearly
  always a written-out list (`filter_present<Node>(opt_element(..), opt_element(..))`); an array
  already in hand goes in as `filter_present<T>(items: existing)`, the named-argument spelling a
  variadic takes. `filter` cannot express it -- it keeps elements, so the results would still be
  wrapped -- and `flat_map` only can by building a one- or zero-element array per value.
- **`filter_ok<T, E>(items: variadic Result<T, E>) -> [T]`** and
  **`filter_err<T, E>(items: variadic Result<T, E>) -> [E]`** are the same shape for a `Result`:
  the values that succeeded and the reasons the rest did not, the two halves of one input, each
  in order. Both type arguments are written even though each answers only one of them, because
  `Result<T, E>` names both and Ziman infers neither. Like `filter_present` they are variadic,
  with an array in hand going in as `filter_ok<T, E>(items: existing)` -- which is also the
  realistic call, since an array of `Result`s is what `map(xs, fallible)` answers.
  **Neither propagates:** `filter_ok` *silently discards* every failure, so it is for the cases
  where a failure is not news -- one bad `Accept` range among ten, a malformed line in a feed
  that keeps going. Where a failure should stop the work, `try` and `if Err(e) =` are still the
  answer. Calling both on the same input keeps the successes and still reports what was lost.

  The sorts are stable in both directions, so `sort_desc` is not `sort` reversed (that would flip
  ties).
  The rest of the array library needs no import either. Most of it is now spelled **without** the
  prefix -- `fold`, `find`, `first`, `last`, `min_by`, `max_by`, `count`, `any`, `all`,
  `for_each`, `distinct`, `sum`, `sort_by`/`_desc`, `is_sorted`, `lower_bound`, `upper_bound`,
  `binary_search`, `group_by`, `frequencies`, `index_by` -- each taking either world and
  dispatching to the `array_`/`mutable_array_` pair underneath; those prefixed names remain
  callable but are the implementations rather than the spelling to reach for. Only
  `array_order_of`, the permutation primitive, is the only one still said with the prefix. There was a
  `collections` module holding these until 2026-10-14; it is gone, and arrays are now the same
  shape as `Map` and `Set`, whose `map_*` and `set_*` helpers have always been bare.
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
"Arrays"). `+` stays numeric-only in v0 (see docs/design/journal/ on why string concatenation didn't
just overload it).
