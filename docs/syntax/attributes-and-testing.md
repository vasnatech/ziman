# Attributes, derive and testing

Part of the [v0 syntax reference](README.md).

## Attributes and derive

`#[...]`, above a declaration, attaches compile-time metadata to it. **Nine are interpreted and
anything else is refused**: `derive` (below), `thread_bound` and `thread_key` on a
`struct`/`enum`, and `test`, `deprecated`, `when_unmatched`, `at_least_one`, `effectful` and
`compiler_resolved` on a function. Which of the nine applies where is a second question, answered
after the declaration is known -- `#[test]` on a struct is refused by name.

This paragraph said "other names parse but have no effect yet" until 2026-10-30, and that was the
last attribute position where it was true. Two things were silent: `#[foo_bar]`, which fell off the
end of the chain, and `#[drive(Json)]` -- a misspelling of the most-used attribute in the language
-- which reached the branch that reads a derive target list, was parsed, and was thrown away. An
attribute the compiler accepts while nothing reads it is indistinguishable from one that works,
which is `#[deprecated]`'s own history. A project's own attribute goes on a **field**, where its
generator declares it (see [docs/design/user-generators.md](../design/user-generators.md) §5a).

`#[at_least_one]` says the function's trailing `variadic` parameter needs a value — see
"Variadic parameters". It takes no argument, since a function has at most one variadic parameter
and it is always last, and it is refused on a function that has none rather than quietly meaning
nothing.

`#[effectful]` says a function that answers a value **also writes its argument**, so discarding
the value in statement position is an ordinary call rather than a mistake. It matters only for the
prelude's overload sets, where the rule is otherwise read off the return type: the `[T]` member of
a pair computes a new value, the builder member rearranges a buffer and answers nothing, so
"answers something" means "has no effect" and a discarded result is reported. The three `remove`
declarations on a builder are the exception — `remove(b, i)` answers the element, `remove(mb, k)`
the `Option<V>` it took out, `remove(sb, x)` whether the item was there, and throwing any of them
away is how `pop` and "delete if present" are spelled. Like `#[at_least_one]` it takes no argument
and is refused where it could mean nothing, here on a function that answers nothing at all.

`#[compiler_resolved]` says the declarations sharing this name **cannot be told apart by their
arguments**, and that a rule inside the compiler chooses among them. It exempts them from the rule
that two declarations may share a name only when every call can tell them apart — the rule under
"Overloading", which is otherwise a redefinition error.

One name in the prelude carries it, `concat`, and it is not a facility for programs. `concat()`
with no arguments is accepted by its string form and its array form alike; `concat(someStrings)`
fits both and they differ only in what they return, which is never a basis for choosing; and
`concat(b, [5], mutable([6]), [7])` passes a variadic tail mixing an array with a builder, which no
single variadic parameter can describe. The compiler decides all three, as it did when the name was
a builtin.

Two things keep it from being a way to ship a set no call can resolve. It is **refused where it
would mean nothing**, like `#[at_least_one]` and `#[effectful]`: a declaration that every call can
already tell from its siblings — or that has none — is an error, since the attribute would only be
disabling a working check. And for any name the compiler has no rule for, the error moves to the
call rather than disappearing: the resolver finds two declarations accepting the arguments and
reports an ambiguity, listing them. So the attribute takes no argument, and marking your own
functions with it gains you nothing.

`#[when_unmatched("...")]` on any member of an **overload set** adds one line to the error raised
when a call matches none of them. The error already names the types that position accepts, worked
out from the declarations; this carries what the declarations cannot — advice about what the
author probably meant, such as *"an immutable collection is emptied by writing an empty one"*. Any
one member may write it and the whole set shares it, because the advice is about the name. The
message is required: `#[when_unmatched]` with no text is a syntax error rather than a no-op.

`#![...]`, with the `!`, attaches to the *whole file* instead of to the declaration after it (the
same inner/outer split Rust draws), so it must appear before the first declaration -- writing one
later is an error that says so. Three names are interpreted. `#![experimental]` marks a module
whose surface is more likely to have a gap than the rest of the library. It changes nothing at
compile time or runtime by design; its job is to be the single place that claim lives, so that
docs/CORELIB.md and docs/lib/*.md can generate the "Experimental" callout from the source rather
than each repeating it by hand, and `make experimental-check` can hold the README's own table to
it. `pg`, `mysql`, `toml` and `yaml` carry it.

`#![deprecated]`, or `#![deprecated("use newmod instead")]`, says the whole module should not be
used in new code. It is reported **at each `use` of it** -- the one place the dependency is
written, and so the one line that dropping it means editing -- rather than at every call into the
module, which would bury the answer under repetition. Both import forms count, since each is a
`use` that has to go:

```
$ ziman app.zm
app.zm:8:1: warning: module 'oldmod' is deprecated: use newmod instead
app.zm:9:1: warning: module 'oldmod' is deprecated: use newmod instead
```

`#![generator(Target)]` says the module **is** the generator for `#[derive(Target)]`, and that
its `pub fn generate` is what the compiler calls with the reified declaration. The target is
required: a generator that does not say what it generates could only be found by its module's
name, which is the string concatenation the attribute replaces -- and a claimed target is
checked, so a module declaring none is refused where it is written rather than at the first
`#[derive(Target)]` in another file. One module, one target.

The bundled generators carry it -- `lib/core/derive_ord.zm` is `#![generator(Ord)]` -- and
**nothing dispatches on it yet**: `#[derive(X)]` is still a fixed set of names in the compiler,
and closing that is the work `docs/design/user-generators.md` describes.

`#![derive(Repository(Entity))]` says the module is a repository over that entity, and generates the CRUD
functions whose body the entity decides: `insert`, `update`, `find`, `find_or_error`, `exists`,
`delete`, `find_all`, `count_all` and `delete_all`. The entity is required, and the only other
thing the attribute takes is `label:` -- everything else a generated function needs is already
written beside the struct, and repeating one here would be a second place for it to be wrong. The table and column list come from
`#[derive(Entity("table"))]`, and the parameters of the keyed functions come from its `#[pk]`
fields, so `delete(conn, id: Uuid)` takes that name and that type because the entity's key is
`#[pk] id: Uuid`; a compound key makes it `delete(conn, patient_id: Uuid, broadcast_id: Uuid)`.

**A name the module defines itself wins, and nothing is generated for it.** That is what makes the
attribute addable to a repository already written by hand: one whose `delete` is scoped by an owner
as well as an id keeps it, where a generated keyed version replacing it would silently remove a
guarantee. The trade is that the file does not say which of its functions are generated --
`--emit-derived` prints them and names each one it skipped, and `--api` on the module names the
attribute, since reading one file cannot reach the entity in another.

`#![derive(Repository(Medication, label: "Medication"))]` adds the entity's name **as a person says it**,
and nothing else reads it: the `DbError.NotFound` a derived query raises when it answers one row
and finds none. It is an argument because it is not a fact about the struct --
`MedicationRefillLog` is "Medication refill log" to a reader and `VitalSign` is "Vital sign", and
no rule gets both right from the type name. Without it the entity's own name is used. The message
is `"<label> not found: <id>"` when the query's predicate addresses the entity's key and that key
renders as text (a `Uuid`, a number, a string), and `"<label> not found"` when it does not.

`#![derive(Repository(Notification, crud: false))]` names the module's entity and generates **none** of the
nine. A derived query (below) needs only the naming half of this attribute, and a module whose
table should not have a generated `delete_all(conn)` -- an audit log, a users table -- can now say
so and still derive its queries. It is a per-module statement, not a list of which generated names
are wanted: the entity's own decisions stay beside the struct.

An entity with no `#[pk]` field is an error: every one of the keyed functions addresses one row, and
without a key nothing does -- unless `crud: false`, where none of them is generated and so none
needs a key. So is a `#[pk]` field of an `Option` type, and a second
`#![derive(Repository(..))]` in the same file -- one module, one entity.

`#[derive(Repository)]`, the declaration-level form, goes on a **bodyless `pub fn` whose name is its
query**. The keyed CRUD above is every query an entity decides on its own; this is for the rest,
and the name is the whole specification:

```ziman
#![derive(Repository(VitalSign))]

use datasource
use db.DbError
use uuid.Uuid

#[derive(Row, Entity("vital_signs"))]
pub struct VitalSign {
    #[pk]
    id: Uuid
    patient_id: Uuid
    type: string
    reading: int
}

// Every vital sign of one patient, highest reading first.
#[derive(Repository)]
pub fn find_by_patient_id_order_by_reading_desc(conn: datasource.Connection, patient_id: Uuid) -> Result<[VitalSign], DbError>

// One row, scoped by its owner: another patient's row is DbError.NotFound, not forbidden.
#[derive(Repository)]
pub fn find_by_id_and_patient_id(conn: datasource.Connection, id: Uuid, patient_id: Uuid) -> Result<VitalSign, DbError>

#[derive(Repository)]
pub fn count_by_patient_id(conn: datasource.Connection, patient_id: Uuid) -> Result<int, DbError>

#[derive(Repository)]
pub fn delete_by_patient_id(conn: datasource.Connection, patient_id: Uuid) -> Result<int, DbError>
```

The grammar is

```
name       := verb [ "_by_" predicate ] [ "_order_by_" ordering ]
verb       := "find" | "count" | "exists" | "delete"
predicate  := term { "_and_" term }
term       := column [ "_" op ]
op         := "between" | "in" | "not" | "is_null" | "is_not_null" | "ignore_case"
ordering   := column [ "_" dir ] { "_and_" column [ "_" dir ] }
dir        := "asc" | "desc"
```

where every `column` is a field of the module's entity. **There is no `_all`**: how many rows a
query answers is its return type's business and nothing else says it, so `find_by_patient_id`
returning `[VitalSign]` is every match and returning `VitalSign` is one. No `_by_` means no
predicate at all -- `find(conn) -> Result<[VitalSign], DbError>` is every row.

| verb | return type | what it does |
|---|---|---|
| `find` | `Result<E, DbError>` | one row; `NotFound` when there is none |
| `find` | `Result<Option<E>, DbError>` | one row; absence is the answer |
| `find` | `Result<[E], DbError>` | every matching row |
| `count` | `Result<int, DbError>` | how many match |
| `exists` | `Result<bool, DbError>` | whether any match |
| `delete` | `Result<unit, DbError>` | the row this addresses; `NotFound` when none matched |
| `delete` | `Result<int, DbError>` | every matching row, answering how many |

**Parameters bind by name, not by position**, and this is the point of the whole thing. After the
connection, the parameters are the predicate's terms, named for their columns and in that order; a
parameter named for the wrong one is a compile error naming both. The same written against a
positional binder -- `find_by_id_and_patient_id(patient_id, id)`, two `Uuid`s the wrong way round
-- compiles, boots, and answers another patient's rows forever. The parameter's *type* is checked
against the field's too.

An operator changes what the term asks for: `in` takes one parameter of `[T]`, `between` takes
`<column>_from` and `<column>_to`, `is_null` and `is_not_null` take none, and `not` and
`ignore_case` take what the plain term would. A column declared `Option<T>` asks for a `T`, not an
`Option<T>`: `col = NULL` matches no row in any engine, so the None side of a nullable column is
what `_is_null` is for.

The two forms that can answer `NotFound` -- `find` returning `Result<E, DbError>` and `delete`
returning `Result<unit, DbError>` -- word it from the module attribute's `label:`, as above. A
repository whose error text is part of its interface wants that argument; without it the message
names the struct.

Everything else about the function is checked before any SQL exists, and reported at the line the
function was written on: a term naming no column (listing the ones the entity has), a name with
two readings (showing both, rather than preferring one), a verb and return type that do not pair,
and `#[derive(Repository)]` in a module with no `#![derive(Repository(..))]`. A name the grammar cannot read --
`find_recent_by_patient_id`, `find_page_by_patient_id`, anything whose words are about the
application rather than about the table -- is written out by hand, where a hand-written body wins
as it does over the keyed CRUD. `--emit-derived` prints what each one became: one private
`Translated` val per function, translated once when the module loads, plus the body that binds to
it.

A `use` from inside a bundled module is silent, the same same-source rule the declaration-level
attribute follows. On the program's own entry file the attribute warns nowhere, because nothing
imports an entry file -- there is no line for a warning to point at.

`#[deprecated]`, or `#[deprecated("use X instead")]`, marks a function, `struct`, `enum` or
module-level `val` that still works but should not be used in new code. Using it *from another
module* -- calling it, naming it as a value, writing a struct literal, reaching an enum variant,
reading a val -- is a **warning on every compile**, quoting the message when one was given:

```ziman ignore: a signature with no body, showing what the attribute is written on
#[deprecated("use open_session_with_timeouts, which also bounds the request head")]
pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session
```

```
$ ziman app.zm
app.zm:12:18: warning: 'server::open_session_with_timeout' is deprecated: use open_session_with_timeouts, which also bounds the request head
```

**A warning never changes the exit code**, so a deprecation cannot break a build that was passing;
it is reported because a warning nobody can see without running a second command moves nobody off
the old API. `ziman --lint` reports the same warnings *and* exits 1, since there finding things is
the whole request -- that is where the style findings (a `var` never reassigned, a declaration
nothing reads, a redundant `as` alias) live, and those stay behind the flag.

Use from *within* the declaring module is deliberately silent: a library keeping a superseded
function working for its existing callers would otherwise warn on every internal call it makes to
itself, and whoever wrote the deprecated thing is the one person who does not need telling. A
**type annotation naming a deprecated type is also silent**: a function migrating away from one
still has to name it to accept one, so warning there would be loudest in exactly the code doing the
migration. All four declaration kinds carry the attribute; there is no kind it is rejected on.

`#[thread_bound]` marks a struct that belongs to the thread that made it (as `MutableString`/
`MutableArray`/`MutableMap`/`MutableSet`/`Iter` are). A type containing one is refused as a
`Channel` element, `spawn` argument, or module-level `val` type -- including a lambda that captured
one, whose type records that fact, so sending or spawning the lambda is refused where it is written
rather than failing on the receiving thread. Each builder and iterator also remembers its own thread
and a use from another is a runtime error, which stays as a backstop for anything the type checker
cannot see. Hand over the finished value -- `snapshot(x)` for any of the four, an iterator's items
as an array -- not the handle itself. For a struct written in Ziman this attribute gives only the
compile-time refusals (no native check).

**A thread-bound type is neither comparable nor hashable**, and neither is anything holding one:
`==` on two handles is a type error, `hash` of one is, and a struct with a handle field is refused
as a `Map` key or `Set` element. The contents live behind an opaque index into a native table, so
two handles say nothing about what is in them -- comparing the snapshots (`snapshot(a) ==
snapshot(b)`) is the thing that was meant. Until 2026-10-13 this was accidentally *allowed*,
because a handle is a struct whose one field is an `int`: two empty `MutableArray`s compared
unequal, `hash` of one was stable only within a run, and such a struct keyed a `Map` on a pointer
value.

**A type named `Mutable*` must be thread-bound**, and the compiler checks it: `Mutable` followed by
an uppercase letter (or on its own) is a claim of family membership, and a plain value wearing the
name would make "every `Mutable*` type belongs to the thread that made it" untrue. A `struct`
satisfies it with the attribute *or* by holding something that has it, so
`struct MutableRingBuffer { buf: MutableArray<int>, head: int }` needs no annotation -- the error
fires only on a `Mutable*` name with nothing mutable inside. An `enum` carries no `#[thread_bound]`
of its own, so its only way to satisfy the rule is a thread-bound variant payload. `MutablesRegistry`
is not caught, which is why the check reads the character after the prefix. The converse does not
hold: a thread-bound type need not be named `Mutable*`, or `Iter` would have to be renamed.

```ziman
use json
use json.Json

#[derive(Json)]
pub struct Point { x: int  label: Option<string> }

fn main() {
    val p = Point { x: 42, label: Option<string>.Some("hi") }
    print(json.format(point_to_json(p)))             // {"x":42,"label":"hi"}
    print(unwrap(point_from_json(point_to_json(p))).x)  // 42
}
```

- **`#[derive(Json)]`** generates `<snake_case>_to_json(v) -> Json` and `<snake_case>_from_json(j)
  -> Result<Type, string>` (both directions always, via a real multi-word `PascalCase` ->
  `snake_case` conversion: `ValidationError` -> `validation_error`). On a **struct** it also generates
  `<snake_case>_from_json_all(j) -> Result<Type, [string]>`, which reports *every* bad field instead of
  stopping at the first -- a document with four wrong types gives four messages. Use it where a client
  should see all of its mistakes at once; `_from_json` is unchanged and remains the cheaper call. A nested
  struct contributes one message (its own first), since it is decoded by its single-error function, and an
  enum gets no companion: its decode either matches a variant or does not, so there is nothing to collect.
- **Per-field conversion, in priority order**: (1) the field type's own `_to_json`/`_from_json`
  (hand-written or derived -- the seven `time` types spell theirs `to_json`/`to_<type>`, see the
  import bullet below); (2) a `_to_string`/`_from_string` pair (every `time`/`uuid` type has
  one, wrapped thin -- really tier 1 one step removed); (3) a plain scalar, or `decimal` (written
  as the nearest float, read from its shortest text -- `#[derive(Row)]` stores it as TEXT,
  keeping scale, instead); (4) `Option<U>`/`[U]`, recursing into `U`. Anything else (`bigint`,
  `byte`, `Result`, `Channel`, a generic struct/enum) is a compile error, not a wrong answer.
- **Field names map `snake_case` -> `camelCase`** for the JSON key (Jackson's default), fixed,
  not configurable. **`Option<U>` fields tolerate absence**: a missing key or explicit `null`
  both decode to `None`; every other field is required. **Deserialization is lenient**: an
  unrecognized extra key is silently ignored.
- **A decode error names the field**: `{"email": 5}` against `{ email: string }` reports
  `"email: expected a string"`, composing one level per nesting (`"address: zipCode: expected a
  string"`); an array/set/map-value element has no field name, so decodes with the bare message.
- **An enum derives an externally tagged shape**: `{"type": "<Variant>"}` (unit) or `{"type":
  "<Variant>", "value": ...}` (payload, following the same per-field priority). `Option<T>` keeps
  its own `null`/unwrapped handling even as a variant's payload type, never tagged this way.
- **A derive imports what its generated code calls**, only where the module doesn't already bind
  the name: `#[derive(Json)]` brings `json.Json`/`get`/`require`/
  `require_string`; `#[derive(Row)]` brings `db.SqlValue`/`DbError`/`Row`/`row_get`/
  `sql_as_*`; `#[derive(Properties)]` imports `properties`. It can't supply a field type's own
  conversion, except `Uuid` and every `time` type get theirs imported automatically; any other
  field type's conversion must already be imported, or the error says to derive or write it.
  Imported names are visible module-wide.
- **The seven `time` types spell their converters short**: `to_json(d)`/`to_sql(d)` one way, and
  `to_localdate(j)`/`to_localtime`/`to_localdatetime`/`to_duration`/`to_zoneoffset`/`to_zoneid`/
  `to_instant` the other, each taking either a `Json` or a `SqlValue`. `to_json` and `to_sql` are
  told apart by their argument, so the seven share one name each; their inverses all take a `Json`
  or a `SqlValue`, so each needs the type in its name. **This is not extended to derived types** --
  every `#[derive(Json)]` type would join one `to_json` overload set, and the resolver caps a set at
  32 candidates, so `<snake_case>_to_json` stays the rule for everything generated. `Uuid` keeps
  `uuid_to_json`, its name having nothing to shorten.

## Testing

`#[test]` marks a function for `ziman test file.zm [...]` (a separate CLI mode from `ziman
file.zm`) to run. It takes no parameters and returns nothing or `Result<unit, E>` for any `E`
(so it can `try` freely); the file needs no `main`.

`ziman test` takes `--coverage`, `--coverage-lcov=F` and `--gc-stress`, on either side of
`test` (`ziman test --gc-stress f.zm` and `ziman --gc-stress test f.zm` are the same run); any
other option is refused rather than read as a file name.

`ziman test` loads and type-checks each file, finds every directly-declared `#[test]` function,
and runs each inside its own per-thread runtime-error boundary (the mechanism `spawn` uses --
see "Concurrency") so one failing test doesn't stop the run. A runtime error or an `Err` result
both count as failure, message shown; anything else passes. Prints `PASS <file>::<name>` or
`FAIL <file>::<name>  <message>`, then a `N passed, M failed` summary; exit 0 only if all pass.

**Parametrized: `#[test(cases: NAME)]`** runs a one-parameter function once per array element:

```ziman
use test

struct AddCase { a: int  b: int  expected: int }
val add_cases = [AddCase { a: 1, b: 1, expected: 2 }, AddCase { a: 2, b: 2, expected: 4 }]

#[test(cases: add_cases)]
fn test_add(c: AddCase) { test.assert_eq(c.expected, c.a + c.b) }
```

`NAME` is looked up in the same file only, as a `val` or zero-parameter function of type `[T]`
(`T` the test's parameter type). A `val`'s array is used as-is; a function is called once,
inside its own boundary (a broken builder is one reported failure, not a crash). Each case
reports independently as `<file>::name[i]` -- unlike a plain `for` loop over cases inside an
ordinary `#[test]`, where the first failure ends the whole test.

**The `test` module** (`lib/core/test.zm`) fails every assertion through `unwrap` on a
manufactured `Err` -- no new interpreter primitive needed:

- `fail(message)` -- unconditional. `assert(cond)`/`assert_false(cond)`.
- `assert_eq<T: Eq>(expected, actual)`/`assert_ne<T: Eq>(unexpected, actual)` -- structural
  equality, message shows both values.
- `assert_near(expected, actual, tolerance)` -- for `float`, where `assert_eq(0.3, 0.1 + 0.2)`
  fails and is right to. A NaN is not near anything, itself included; a negative tolerance is
  rejected rather than silently never passing.
- `assert_lt`/`assert_le`/`assert_gt`/`assert_ge`, each `<T: Ord>(actual, bound)`.
- `assert_contains`/`assert_not_contains` over `(string, needle)`, `([T], T)`, `(Set<T>, T)` and
  `(Map<K, V>, K)` -- the shapes the prelude's `contains` takes, showing both values. Prefer them
  to `assert(contains(..))`, which can only say `assertion failed`.
- `assert_len(expected, items)`, `assert_empty`/`assert_not_empty` -- the same four shapes,
  showing the collection rather than only its length. The prelude's `is_empty` is `string`-only,
  so for a collection these are the only short spelling.
- `assert_all_match`/`assert_any_match`/`assert_none_match`, each `<T>([T], fn(T) -> bool)` -- as
  far as the `all`/`any`/`none` builtins reach. The `all` and `none` forms name the first element
  that breaks the rule and its index, which the builtins cannot.
- `assert_entry<K: Eq + Hash, V: Eq>(m, key, value)` -- tells a missing key from a different
  value, where `assert_eq(value, unwrap(get(m, key)))` dies inside `unwrap` on the first.
- `assert_starts_with`/`assert_ends_with`; `assert_blank`/`assert_not_blank` ("blank" being
  `trim` leaving nothing); `assert_matches`/`assert_not_matches(s, pattern)`, which **search**
  like `regex.is_match` rather than requiring the whole string as Java's `String.matches` does --
  anchor with `^...$` for that. A pattern that will not compile fails with the compiler's message.

These four shapes are the immutable ones. A test holding a `MutableArray`/`MutableMap`/
`MutableSet`/`MutableString` passes it through `snapshot` first; the type error names the shapes
that are accepted, so the fix is visible where it happens.
- `assert_some<T>(opt) -> T`/`assert_none<T>(opt)`; `assert_ok<T, E>(r) -> T`/`assert_err<T,
  E>(r) -> E` -- the `_some`/`_ok` forms return the unwrapped value for chaining.

Failure messages use `format`'s `{}` (not `str`, which rejects a bare generic `T` -- see
"Builtins"), so `assert_eq`'s only requirement is the `T: Eq` bound `==` already needs.

**A failing assertion is reported at the line that asserted**, not at the `unwrap` inside
test.zm that raised it: `ziman test` asks where the innermost run of bundled-`test` frames was
entered from, and names that line (and its file, when the assertion is in a helper in another
file). An ordinary runtime error inside a test -- an index out of range, a `None` unwrapped --
keeps its own location, which is already the one the reader wants.
