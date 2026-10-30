# Modules

Part of the [v0 syntax reference](README.md).

## Modules

One file is one module, identified by its filesystem path — no separate
`module Foo` declaration to keep in sync with the filename.

```ziman ignore: the first half of a two-file example; `main.zm` below imports it
// models/user.zm
pub struct User {
    id: int
    name: string
}

pub fn greet(u: User) -> string {
    return format("hello, {}", u.name)
}

fn validate_name(name: string) -> bool {   // no `pub` -- private to this module
    return len(name) > 0
}
```

```ziman ignore: the second half of a two-file example, importing the `models/user` above
// main.zm
use models/user

fn main() {
    val u = user.User { id: 1, name: "Ada" }
    print(user.greet(u))
}
```

- **`use path/to/mod [as alias]`** — slash-separated, maps directly onto the filesystem
  (`models/user` → `models/user.zm`), resolved relative to the entry file's own directory
  first, then falling back to a bundled *core module* (below) if no project file exists; a
  project file of the same name always wins. A folder may hold its own module under its own
  name (`controller/controller.zm` for `use controller`, the way bundled `http` is
  `lib/http/http.zm`); the shorter file wins when both exist. Slash, not dot, since a module
  path never shares a token with `.field` access. The referenced name is the path's last
  segment, or the `as` alias — either way casing-enforced lowercase. Two `use` lines that would be
  referenced by the same name are a compile error naming both paths and suggesting an alias (`use
  entity/patient as entity_patient`), as is importing one module twice; before that check existed the
  first `use` silently won and the second did nothing. Nothing is auto-renamed: a name you did not
  write is what the rule below about bare names exists to prevent.
- **`use path/to/mod.Name [as alias]`** imports one function/struct/enum, bound to a bare
  local name (`use uuid.Uuid` then just `Uuid`) — one name per import, no `{A, B, C}` list.
  That last part is a decision, not an omission, and the reasons are worth having here because the
  obvious complaint about it is correct: the reference backend has 1,275 `use` lines and one file
  imports fifteen names from `time`. Three things outweigh that. `--fmt` already sorts runs of
  consecutive `use` lines, so nothing has to find the right slot in an alphabetised run by hand. One
  name per line is independently valid, so a mistake costs one line and the error points at it, where a
  malformed group breaks the whole import. And one fact per line stays greppable: `use service.is_email`
  is a searchable statement about one name, which a group is not. Against those, "one way to do a thing"
  (docs/design/journal/'s second principle) decides it — a group would be a second way to import three names. See
  docs/design/journal/'s "Grouped imports, declined" for the full argument.
  The bare name's casing must match `Name`'s own, checked at parse time (the parser already
  relies on casing alone to tell a struct literal from a call). Privacy-checked once, at the
  `use` line, exactly as a dotted reference would be. A compile-time error to import the same
  local name twice, or to collide with something the importing module declares itself.
- **Every cross-module reference is qualified or per-name imported — never a bare name that
  silently resolves to an import.** No wildcard import form; a per-name import always names
  the one thing it binds, so a file's `use` lines account for every bare name its body could
  have come from.
- **`prelude.` is the one qualifier with no `use` and no module behind it.** It names the implicit
  namespace — the prelude's declarations and the builtins — for the one case where a bare name
  cannot: a file that declares `format` or `len` itself has taken that name for every call in it,
  and `prelude.format(...)` is how the original is still reached. It resolves against the implicit
  namespace *only*, so `prelude.X` for an `X` of your own is a compile error, and a module you
  imported under the name `prelude` keeps the name.
- **Three levels of visibility, ascending: private (no modifier), `internal`, `pub`.** Private
  is callable/constructible only within its own module (cross-module reference is a compile
  error naming both the declaration and module) — so names only need to be unique *within*
  their module, not the whole program. `pub` exports to any module anywhere. `internal` sits
  between: visible to any module in the same *package*, invisible outside it — today "package"
  means one of exactly two things, the bundled standard library as a whole or your own project
  as a whole, not a per-directory boundary (see docs/design/journal/'s visibility entry for why a
  directory-based definition would have broken the actual motivating case). Violating it is
  worded distinctly from plain private: `'x' in module 'y' is internal to its own package`.
  `internal` is a *soft* keyword — recognized only immediately before `fn`/`native fn`/
  `struct`/`enum`/`val` (the same position `pub` occupies; they're mutually exclusive, `pub`
  checked first) — everywhere else, including a function's own name or an import, it's an
  ordinary identifier (the same technique `unit` uses as a type).
- **Overloading: several declarations may share a name when every call can tell them apart.**
  Write `fn describe(n: int)` and `fn describe(s: string)` side by side and a call picks one from
  its arguments. **Any argument position decides**, not just the first — `fn at(row: int, col:
  int)` and `fn at(row: int, label: string)` are a set, chosen by argument two — and so does
  arity, so `area(side)` and `area(w, h)` are a set too. A lambda argument's *arity* counts, since
  it can be read without checking the lambda.

  Three restrictions, each with a reason:
  - **No return-type dispatch.** Two declarations taking the same arguments and differing only in
    what they answer are a redefinition, reported where they are written. `to_set` and
    `to_mutable_set` are two names for this reason.
  - **No specificity ranking.** A parameter that is a bare type variable matches everything, so
    `fn g<T>(a: [T])` beside `fn g(a: [int])` is a redefinition, not a more specific overload.
  - **No implicit conversion** to make a member match.

  A call can still be ambiguous when an argument carries no type of its own — an empty literal, an
  unannotated lambda, a bare `None` — because resolution never type-checks such an argument before
  choosing (it would have to know the overload first). `take([])` between `take([int])` and
  `take([string])` is refused, and the message says which argument left the choice open. Bind it
  to a typed `val` first.

  **An overload set cannot be used as a function value**: `val f = describe` has no arguments to
  choose from. Wrap the one you mean in a function of your own.

- **In `lib/prelude/`, visibility means what it means anywhere else** — since 2026-10-22. A
  declaration you can call with no `use` is one marked `pub`, and there are 268 of them; the rest
  are the prelude's own machinery and calling one is a compile error naming which kind it is.
  `internal` reaches the bundled library's own package, so a module in lib/ may call
  `sort_positions` and a program may not. A declaration with no modifier belongs to the file that
  declares it: `array_order_of` is the only one, and `sort_by`/`sort_by_desc` in the same file are
  its only callers.

  **It did not work that way before, and the difference is worth knowing if you read older
  comments.** The prelude is merged bare into every program, so a call into it resolves as an
  ordinary same-module call and there was no import boundary for `pub` to be checked at — so `pub`
  chose only what `ziman --api builtins` advertises, and every declaration was callable whatever
  its modifier said. `internal` was the only modifier that subtracted anything, and it subtracted
  advertising rather than reach. Seven names lived in that gap; the thirty-odd calls they had
  outside the prelude moved to the public spellings (`add` for `mutable_set_add`, `sort_by` for
  `array_order_of`) before the rule changed.
- **`[pub] val NAME[: Type] = <expr>` at module top level declares a global value** — evaluated
  once before `main` runs, in file order, never per-call. No global `var` (Ziman's concurrency
  model is single-writer actors over `Channel<T>`; see docs/design/journal/'s "Global val" entry).
  Reachable unqualified from any function in the module (or via `use mod.NAME`), but never
  shadowable — reusing its name as a local `val`/`var`/parameter/loop-binder anywhere in the
  module is a compile error, not silent shadowing. No `mod.NAME` dotted bare access; use a
  per-name import for that.

  ```
  val max_retries = 3
  fn should_retry(attempt: int) -> bool { return attempt < max_retries }
  ```
- **No manifest file.** The dependency graph is the entry file's `use` statements, followed
  transitively; each module loads at most once (a diamond dependency shares one instance), and
  a circular `use` is a compile error rather than infinite recursion. The entry file is never
  itself a `use` target, so a program with no `use` at all behaves exactly as before modules
  existed.
- **Where `use foo` looks**: `foo.zm` beside the importing file first, then the directory of the
  nearest `ziman.toml` at or above it, then each directory in `ZIMAN_MODULE_PATH` (colon-separated,
  in order, first match winning), then the bundled core modules. Beside the file always wins, so
  nothing that resolved before either of the middle two resolves differently now. Both exist for the
  same reason -- letting tests live in a directory of their own -- and are two ways to point one
  search path at a directory, not two mechanisms: `ZIMAN_MODULE_PATH=src/main/ziman` makes
  `src/test/ziman/calc_test.zm` able to `use calc`, and a `ziman.toml` at a project's root lets
  anything under it write the project-relative path instead (`use src/main/ziman/calc`), which is
  the same path a file beside the subject would use. A module found either way resolves its own
  `use` lines the same way, so it may import too. `ZIMAN_MODULE_PATH` has the same shape and
  spelling as `ZIMAN_RESOURCE_PATH`, which does this for resources.
- **`ziman.toml`** marks a project's root and **nothing reads its contents** -- an empty file is the
  usual case. It is the name reserved for a manifest that does not exist yet, taken now so that
  manifest later lands in the file already marking the root rather than a second file appearing
  beside it. The search for it stops after eight levels, so a stray one in a home directory cannot
  quietly claim every file beneath it.
- **`native fn name[<TypeParams>](params) -> type`** declares a function with no body — its
  implementation is a C function registered under its exact fully-qualified name, looked up at
  call time; calling one with nothing registered is a runtime error. A generic native fn takes
  explicit type arguments like any other generic (no inference), and its C side never needs to
  know which concrete type a call instantiated it with (`Value` is already runtime-type-erased
  regardless of static types — see "Generics"). `fs.read_text(path) -> Result<string, string>`
  is the standard example (a missing/unreadable file is a `Result`, not a crash).
- **`builtin fn name[<TypeParams>](params) -> type`** declares a function with no body whose
  implementation *and type rule* are both the interpreter's: one of the names in the "Builtins"
  section below. It exists so the signature can be printed by `ziman --api builtins`, completed by
  an editor and found by a reader -- **nothing checks or calls through it.** Every builtin arm in
  the type checker returns before the ordinary function lookup is reached, so a call is typed by
  that arm and dispatched by id exactly as it was before any declaration existed; the declaration
  is data *about* the builtin rather than the builtin. Two rules follow from that: the name must be
  one the interpreter actually dispatches (a typo is a compile error naming
  `ziman --api builtins`), and it is the one declaration exempt from "a builtin cannot be
  redefined", since it is the declaration *of* one. Only `lib/prelude/` declares these.
  See docs/design/journal/'s "`native` and builtin are two different mechanisms" for why the two are not
  interchangeable: a `native fn`'s type is checked by the ordinary call path, a builtin's by a
  hand-written arm that can see the whole call expression.
- **A bundled *core module*** is ordinary `.zm` source embedded in the `ziman` binary, resolved
  by `use` when no project file by that name exists (docs/CORELIB.md lists every one and every
  `pub` signature): `fs`, `os`, `clock`, `random`, `uuid` (`Uuid`, RFC 9562 v4 and
  time-ordered v7, satisfies `Eq`/`Hash`/`Ord`), `time`, `number`, `crypto`, `json` (`Json` is a recursive enum,
  `JArray([Json])`/`JObject(Map<string, Json>)`; satisfies `Eq`/`Hash` but not `Ord` — no
  `json_compare`, so `compare`/`sort` on `Json` is a type error), `properties`, `log`, `net`,
  `http`/`server`/`client`, and the database layer `db`/`sql`/`sqlite`/`pg`/`mysql`. Each is an
  ordinary qualified module (`use fs`, always `fs.read_text`) except the prelude
  (`lib/prelude/`), which merges bare into every program with no `use` at all (`Option<T>`,
  the builders, `parse_int`/`concat`/`split`/...; see docs/design/journal/'s "Prelude" entry).

### Naming conventions for module functions

Two usage modes (`use net` then `net.connect(...)`, or a per-name import binding a bare
name), each with a rule the bundled modules follow and new ones should too:

1. **Called qualified: a short verb, no module prefix** (`net.connect`, `fs.read_text`,
   `server.route`, `client.get`) — the qualifier already says where it's from. Don't import
   such a name bare (`use server.response` collides with an application name).
2. **Imported bare, or found by naming convention: carry the noun** — self-describing and
   unique across modules (`byte_from_hex`, `net_error_message`, and everything `#[derive]`
   dispatches on: `<snake_type>_to_json`/`_compare`/`_to_sql`). The seven `time` types are the
   one exception, and it is a measured one -- see "the seven `time` types spell their converters
   short" under `#[derive(Json)]`.
3. **A module defining types names operations after the type** (`uuid_new`,
   `add_days`) — the type is the noun, so rule 2 applies and the name is safe bare
   or qualified. Generic combinators on built-in types (`map`/`filter`/`sort`)
   are rule 1.
4. **Conversions are `<from>_to_<to>`/`<to>_from_<from>`**, parsing is `parse`/`<type>_parse`,
   and a fallible call returns a `Result`, never a `try_` prefix.

The thing to avoid: a short generic verb also meant for bare import — the collision the two
modes are otherwise designed around.

### `{ .. }` blocks as values

A `{ .. }` block is an expression: its statements run, and its **last expression is its value**.
What this buys over a helper function is a computation of more than one step that stays where the
value it produces is named -- most usefully at module level, where a `val` can be built with a loop:

```ziman ignore: `names` and `compile` are the reader's own
val patterns = {
    val out = mutable_array_new<string>()
    for name in names {
        push(out, compile(name))
    }
    snapshot(out)
}
```

That shape is the reason it exists. A `MutableArray` cannot be the type of a module-level `val`
(every thread would share it), but a *builder inside a block* is not the val's type -- only
`snapshot(out)` is, so the val is an ordinary immutable `[string]` and the handle never escapes the
block it was made in.

- **Where:** anywhere an expression may appear. As an operand it needs parentheses when anything
  follows (`({ .. }) * 2`), the same rule an `if` used as a value has.
- **Not a statement.** A block at the start of a statement is a compile error naming what to write
  instead, because its value would have nowhere to go. Ziman has no bare scope block: a `val` lasts
  to the end of the block it is in, and a `defer` runs when that block exits.
- **It must end with an expression that has a value**, and that expression may itself be an
  `if`/`switch` used as a value, so a block whose tail is a choice works.
- **It cannot `return`, `break`, `continue` or `try`**, for the reason an `if` used inside an
  expression cannot: the interpreter can leave a statement, not the middle of an expression. Bind
  the part that leaves to a `val` before the block.
- `{}`, `{"a": 1}` and `{1, 2}` are still the map and set literal mistake, and still say so --
  those are written with their type arguments (`Map<string, int> { "a": 1 }`).

### `if` and `switch` as values

An `if` or `switch` can produce a value, which removes the "declare a `var`,
then assign it in every branch" pattern:

```ziman ignore: `n`, `e` and `text` are the reader's own; the point is where such an expression may go
val label = if n > 0 { "positive" } else if n < 0 { "negative" } else { "zero" }

val status = switch e {
    NotFound(m) { 404 }
    Constraint(m) { 409 }
    Connection(m) { 500 }
    Sql(m) { 500 }
}

val n = switch parse_int(text) {
    Ok(v) { v }
    Err(msg) { return err(msg) }      // an arm may leave instead of producing a value
}
```

- **Where:** the entire right-hand side of a `val`/`var`, an assignment, or `return` -- an arm
  there may `return`/`break`/`continue`/`try` to leave. Anywhere else an expression can appear
  (a call argument, an operand, an array element, a struct field) it's also allowed, but no arm
  may leave (the interpreter can leave a statement, not mid-expression) -- a `break` inside a
  loop the arm itself contains is still fine. As an operand it needs parentheses when anything
  follows on the line (`(if a { 1 } else { 2 }) + 3`), else the following tokens read as part of
  the last arm's value. At the start of a statement, `if`/`switch` are still plain statements.
- **An arm's value is its last expression** (may be `foo()?`); a trailing `if`/`switch` is
  itself the value, so `else if` chains and nested choices work. An arm may instead leave
  (`return`/`break`/`continue`); at least one arm must produce a value and every arm must do
  one or the other, or it's a compile error. `if` needs an `else`; `switch` stays exhaustive.
- **All arms share one type**, checked against a declared/expected type when there is one (so a
  bare value auto-wraps to `Some`/`Ok` per arm) or decided by the first arm otherwise -- no
  auto-wrap *between* arms. `switch` payload names scope to their own arm only.
