# `collections`

The array library, now a thin layer over the bare `array_*` helpers in
lib/prelude.zm (F120). Each function here forwards to its `array_` counterpart and exists only
so that the call sites written before the promotion keep working; `array_sort_with(xs, cmp)` is
the spelling to write. See docs/ROADMAP.md on what remains of this module's refactor.

`use collections` -- or `use collections.Name` to bring one item in bare.

## Example

```
use collections

// Sorting, searching, and the array helpers. Type arguments are written out at the call.
fn main() {
    val words = ["pear", "fig", "apple", "kiwi"]

    print(collections.sort<string>(words))
    print(collections.sort_desc<int>([3, 1, 2]))

    // Sort by a computed key without writing a comparator.
    val by_length = collections.sort_by_keys<string, int>(
        words,
        collections.map<string, int>(words, fn(w) { len(w) })
    )
    print(by_length)

    print(collections.map<int, int>([1, 2, 3], fn(n) { n * n }))
    print(collections.filter<int>([1, 2, 3, 4], fn(n) { n % 2 == 0 }))
    print(collections.fold<int, int>([1, 2, 3, 4], 0, fn(sum, n) { sum + n }))

    // Search a sorted array in O(log n).
    val sorted = collections.sort<int>([40, 10, 30, 20])
    print(collections.binary_search<int>(sorted, 30))
    print(collections.lower_bound<int>(sorted, 25))

    print(collections.distinct<int>([3, 1, 3, 2, 1]))
    print(collections.group_by<string, int>(words, fn(w) { len(w) }))

    // Counting, indexing, and a map's entries as an array.
    print(collections.frequencies<string>(["a", "b", "a", "c", "a"]))
    print(collections.index_by<string, int>(words, fn(w) { len(w) }))
    val stock = Map<string, int> { "apple": 4, "fig": 9 }
    print(map_entries<string, int>(stock)[1].key)
    print(map_map_values<string, int, string>(stock, fn(n) { format("{} units", n) }))
}
```

Output:

```
[apple, fig, kiwi, pear]
[3, 2, 1]
[fig, pear, kiwi, apple]
[1, 4, 9]
[2, 4]
10
Some(2)
2
[3, 1, 2]
{4: [pear, kiwi], 3: [fig], 5: [apple]}
{a: 3, b: 1, c: 1}
{4: kiwi, 3: fig, 5: apple}
fig
{apple: 4 units, fig: 9 units}
```

### `sort_indices`

```
pub fn sort_indices<K: Ord>(keys: [K]) -> [int]
```

The positions of `keys` in ascending order: the array of indices that, read in order,
visits `keys` smallest first. Stable: equal keys keep their input order. `sort` and
`sort_by_keys` are built on this.

### `sort_indices_desc`

```
pub fn sort_indices_desc<K: Ord>(keys: [K]) -> [int]
```

The positions of `keys` in descending order. Also stable: equal keys keep their input
order (this is not the ascending result reversed, which would flip ties).

### `sort`

```
pub fn sort<T: Ord>(items: [T]) -> [T]
```

`items` in ascending order of their own `compare`. `compare` orders any number, string or
byte, and any struct or enum with a `<type>_compare` function (`#[derive(Ord)]` writes one).

### `sort_desc`

```
pub fn sort_desc<T: Ord>(items: [T]) -> [T]
```

`items` in descending order of their own `compare` (stable).

### `sort_by_keys`

```
pub fn sort_by_keys<T, K: Ord>(items: [T], keys: [K]) -> [T]
```

`items` ordered by `keys[i]` for each `items[i]` -- the key array is how a caller sorts by
a field or a computed value without a comparator function. The two arrays differing in length
is a bug in the caller's own logic (both are always the program's own arrays, never data from
outside it), not a recoverable condition -- a fatal error naming both lengths, the same policy
every programmer-error case in the library follows now (see docs/DESIGN.md's "Programmer
errors are fatal" entry), not a `Result` a caller could forget to check.

### `sort_by_keys_desc`

```
pub fn sort_by_keys_desc<T, K: Ord>(items: [T], keys: [K]) -> [T]
```

The descending form of `sort_by_keys`.

### `sort_with`

```
pub fn sort_with<T>(items: [T], cmp: fn(T, T) -> int) -> [T]
```

`items` ordered by a comparator function (negative, zero or positive, like `compare`), for
an order that is neither the type's own nor a single key -- a two-field ordering, say.
Stable. `sort_by_keys` covers the single-key case.

### `is_sorted`

```
pub fn is_sorted<T: Ord>(items: [T]) -> bool
```

True when no element is greater than the one after it (an empty or one-element array is).

### `lower_bound`

```
pub fn lower_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element that is not less than
`target` -- where `target` would be inserted to keep the order, before any equal
elements. `len(sorted)` when every element is smaller. O(log n).

### `upper_bound`

```
pub fn upper_bound<T: Ord>(sorted: [T], target: T) -> int
```

In an already sorted array: the position of the first element greater than `target` --
just after any equal elements. `upper_bound - lower_bound` is how many equal `target`.

### `binary_search`

```
pub fn binary_search<T: Ord>(sorted: [T], target: T) -> Option<int>
```

The position of the first element equal to `target` in an already sorted array, or None.
O(log n).

### `find`

```
pub fn find<T>(items: [T], test: fn(T) -> bool) -> Option<T>
```

The first element for which `test` is true, or None.

### `first`

```
pub fn first<T>(items: [T]) -> Option<T>
```

The first element, or None for an empty array.

### `last`

```
pub fn last<T>(items: [T]) -> Option<T>
```

The last element, or None for an empty array.

### `min_by`

```
pub fn min_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is smallest (the first such, on a tie), or None for an empty array.

### `max_by`

```
pub fn max_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>
```

The element whose `key` is largest (the first such, on a tie), or None for an empty array.

### `reverse`

```
pub fn reverse<T>(items: [T]) -> [T]
```

The same elements, last to first. (`sort_desc` is not `reverse(sort(..))`: it keeps equal
elements in their input order.)

### `map`

```
pub fn map<T, U>(items: [T], f: fn(T) -> U) -> [U]
```

`f` applied to every element, in order.

### `filter`

```
pub fn filter<T>(items: [T], keep: fn(T) -> bool) -> [T]
```

The elements for which `keep` is true, in order.

### `flat_map`

```
pub fn flat_map<T, U>(items: [T], f: fn(T) -> [U]) -> [U]
```

The arrays `f` returns for each element, joined into one: `flat_map` of a function
returning `[]` drops an element, returning several expands it.

### `fold`

```
pub fn fold<T, A>(items: [T], initial: A, f: fn(A, T) -> A) -> A
```

Left to right: `f(f(initial, items[0]), items[1])` and so on.

### `count`

```
pub fn count<T>(items: [T], test: fn(T) -> bool) -> int
```

How many elements `test` is true for.

### `any`

```
pub fn any<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for at least one element (false for an empty array).

### `all`

```
pub fn all<T>(items: [T], test: fn(T) -> bool) -> bool
```

True when `test` holds for every element (true for an empty array).

### `for_each`

```
pub fn for_each<T>(items: [T], f: fn(T))
```

Calls `f` on every element, in order, for its effect.

### `distinct`

```
pub fn distinct<T: Eq + Hash>(items: [T]) -> [T]
```

The distinct elements, each at its first position: `distinct([3, 1, 3, 2, 1])` is `[3, 1, 2]`.

### `group_by`

```
pub fn group_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, [T]>
```

The elements grouped by `key`: a map from each key to the elements that have it, in input
order, with the keys in the order they first appear.

### `join`

```
pub fn join(items: [string], sep: string) -> string
```

How many times each distinct element occurs: `frequencies<string>(["a", "b", "a"])` is `{a: 2, b: 1}`, keys
in the order they first appear.
The pieces of `items` with `sep` between them. Written because there was no concise way to do
this at all: a cold model reaching for `parts.join("-")` was pointed at the builtin `join`, which
is a *path* join over strings and cannot take a list, and then had to write a loop with `format`
(docs/reviews/tasks.md F35). `sep` goes between, not after, so a single element joins to itself
and an empty array to the empty string.

### `sum`

```
pub fn sum(items: [int]) -> int
```

The total of `items`. A left fold with `+`, which is what anyone writes by hand, but `+` on
`int` traps on overflow in this language -- so this either answers the exact total or stops with
an error, and never the silently wrong number a wrapping sum would give.

Integers only, deliberately: a float total depends on summation order for precision, so the
right answer there is a decision (naive, pairwise, Kahan) rather than a default, and `fold`
already expresses whichever one a caller wants.

### `frequencies`

```
pub fn frequencies<T: Eq + Hash>(items: [T]) -> Map<T, int>
```

### `index_by`

```
pub fn index_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, T>
```

The elements keyed by `key`: a map from each key to the element that has it. When two elements share a
key the later one wins, like `set` on a map; use `group_by` to keep them all. Keys are in the order they
first appear.
