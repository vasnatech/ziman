# `collections`

Sorting, searching and higher-order helpers for arrays, in ordinary
Ziman. `Map<K, V>` and `Set<T>` are built-in types, not part of this module (see
docs/SYNTAX.md "Maps and sets"); `Option<T>` lives in lib/prelude.zm and is always
available bare.

Every function returns a new array and leaves its argument untouched (arrays are
immutable). Type arguments are written out at the call, since Ziman does not infer them:
`collections.map<int, string>(numbers, label)`. A function argument is a named function or
a lambda, and a lambda may use the enclosing function's immutable locals.

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
    val by_length = unwrap(collections.sort_by_keys<string, int>(words,
        collections.map<string, int>(words, fn(w) { len(w) })))
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
```

## Sorting

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
pub fn sort_by_keys<T, K: Ord>(items: [T], keys: [K]) -> Result<[T], string>
```

`items` ordered by `keys[i]` for each `items[i]` -- the key array is how a caller sorts by
a field or a computed value without a comparator function. An Err if the two arrays differ
in length.

### `sort_by_keys_desc`

```
pub fn sort_by_keys_desc<T, K: Ord>(items: [T], keys: [K]) -> Result<[T], string>
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

## Searching

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

## Transforming and testing

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
