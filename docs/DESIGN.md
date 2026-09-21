# Ziman — Design

Ziman ("language" in Kurdish) is a small, statically-typed language designed
to be cheap for an LLM to generate correctly and cheap for a human to read
without running it. It is not modeled on C — C is only this project's
*implementation* host (the interpreter is written in C; see README for why).
Ziman's own syntax and semantics are designed from scratch: it is not
designed to be tree-native/structural either — it's a normal parsed text
language, but every syntax and semantics decision is chosen to remove a
class of error that shows up often in LLM-generated and human-written code
alike, borrowing individual ideas (explicit mutability, no coercion) from
several languages rather than inheriting any one of them wholesale.

## Principles

1. **No hidden control flow.** What a line does must be derivable from that
   line plus its declared types and imports — never from action-at-a-distance
   (no operator overloading, no implicit conversions, no macros that rewrite
   meaning invisibly).
2. **One way to do a thing.** No syntactic synonyms. One loop construct per
   shape of loop, one function-definition form, one string-formatting
   mechanism. Every accreted "second way" in an existing language is a
   documented source of both human bugs and LLM training-distribution
   ambiguity — Ziman doesn't add a second way without removing the first.
3. **Explicit over implicit.** Mutability, type coercion, and effects are
   opt-in and visible at the declaration site, not inferred from usage.
4. **No semantic whitespace.** Blocks are delimited by `{}`, never by
   indentation. Indentation-as-syntax (Python) is a disproportionate source
   of LLM diff/generation errors — a model that drops or adds one space
   silently changes program structure. Statement boundaries are newlines,
   which is unambiguous (see SYNTAX.md) and not the same failure mode.
5. **Errors are data, not just prose.** The compiler's diagnostics are
   structured (machine-parseable) and render as clear text for humans from
   the same underlying record. This lets an LLM (or a human's editor)
   consume a compile error programmatically without scraping a string.
   (Planned for v0.2 — v0.1 just needs errors to carry line/column and a
   stable message shape.)
6. **Small orthogonal core.** Prefer a handful of composable constructs over
   many special-cased ones. Anything not in the core is a library function,
   not new syntax.

## Concrete decisions for v0

- **File extension:** `.zm`
- **Typing:** static, with local type inference for `let` bindings.
  Function signatures always declare parameter and return types explicitly
  — a function's type is a contract that shouldn't require reading its body
  (helps both a human skimming a diff and a model generating a call site).
- **Mutability:** `val` declares an immutable binding, `var` declares a
  mutable one (Kotlin/Scala-style) — two distinct keywords rather than a
  keyword+modifier, so `grep -n "var "` alone finds every mutable binding
  in a file. Mutability is exactly the kind of thing that should be visible
  at the declaration, not inferred from whether an assignment shows up
  later in the function. Trade-off worth naming: `val`/`var` differ by one
  character, which is a one-keystroke typo that silently flips mutability —
  accepted for v0 for the terseness and precedent; a future formatter/linter
  should visually distinguish them (e.g. color, lint warning on the
  near-miss) rather than the language changing the keywords.
- **No implicit coercion.** `int` and `float` don't silently mix; use an
  explicit `as` cast. This is one of the most common silent-bug classes in
  C-family languages and a common LLM mistake (assuming numeric types
  unify the way Python's do).
- **Statement termination:** newline-terminated, no semicolons. A statement
  continues onto the next line only while a paren/bracket/brace is still
  open or the line ends in a binary operator — a purely lexical rule (like
  Go's automatic semicolon insertion), not an indentation rule. This is not
  the same failure mode as Python's block-structure-by-whitespace: dropping
  a newline can't silently change which block a statement belongs to,
  because blocks are still braced.
- **Naming convention:** `snake_case` for values and functions, `PascalCase`
  for types — and since struct types shipped, this is **enforced by the
  parser**, not just documented. That upgrade wasn't a style preference on
  its own: a struct literal (`Point { x: 1 }`) is recognized purely by its
  identifier starting uppercase, in every position a primary expression
  can appear, including inside an `if`/`while` condition. For that to be
  sound (never misparse a variable as an attempted struct literal, or vice
  versa), a value/function name can never start uppercase and a struct
  name always must — enforced in both directions, not left to convention.
- **No `return`-less last-expression returns in v0.** Every function that
  returns a value uses an explicit `return`. (Reconsider post-v0: implicit
  last-expression return is common in AI-generated functional-style code
  and reads fine, but it's a second way to return a value — worth revisiting
  once the core is stable, not before.)
- **Arrays (`[T]`) are value types, not reference types.** Assigning or
  passing an array always yields an independent value — never an alias two
  names can mutate through. This is the same principle #1 reasoning as
  everywhere else: a reference-semantics collection lets a function
  silently mutate the caller's data through a parameter, which isn't
  derivable from the call site without reading the function's body, and
  Ziman has no effects-in-signatures (yet) to make that visible. The
  corollary: `push`/`insert`/`set` never mutate their `arr` argument, they
  always return a new array — one mutation pattern (`arr = builtin(arr,
  ...)`), no exceptions, no indexed-assignment statement. `val` on an
  array is therefore a *stronger* guarantee than `val`/`final` on a
  collection in most mainstream languages (Kotlin, Java): the whole value
  is frozen, not just which object the name points to, because there's no
  "same object, mutated in place" concept for arrays at all.
  Implementation note, not a language-semantics one: this doesn't cost the
  performance you'd expect from "always copies." An ArrayObj is immutable
  once built (see `src/value.h`), so a plain Value struct copy (shared
  pointer) is always safe — exactly like Ziman's strings, which get away
  with the same trick for the same reason. The O(n) cost of a "copy" only
  actually happens inside `push`/`insert`/`set` themselves, building the
  one new buffer they return.
- **Structs are plain, nominal, value-typed data — no classes, no
  inheritance, no methods.** Considered and explicitly rejected: Java-style
  classes with implementation inheritance. Virtual dispatch through a
  class hierarchy is exactly the kind of thing principle #1 rules out
  elsewhere — `obj.foo()`'s behavior depends on `obj`'s runtime type and
  where in the hierarchy an override lives, not on anything visible at the
  call site. Go and Rust independently reached the same conclusion.
  Instead: a `struct` is only ever data, and functions operate on it the
  same way they operate on any other value (`distance_squared(a, b)`, not
  `a.distance_squared(b)`). Revisit trait/interface-style polymorphism
  only once a concrete program needs it and free functions plus nominal
  typing genuinely can't express it — not before.
  Construction (`Point { x: 1, y: 2 }`) requires every field, named,
  never positional — a positional `Point(1, 2)` is exactly the "swapped
  x/y" bug class for any struct with two same-typed fields, silently.
  Mutation follows the identical value-semantics story as arrays: no field
  assignment, `with p { x: 99 }` produces a new struct with named fields
  replaced and the rest copied from `p` — considered and rejected a `..`
  spread-style operator for the "rest of the fields" case (Rust/JS-style)
  in favor of a plain `with` keyword, since the goal here is easy for an
  LLM to both read and reliably generate, and a word costs nothing extra
  to recognize where a new punctuation token is one more symbol to learn.
  A `new` keyword before construction (`new Point { ... }`) was also
  considered and rejected, in favor of the casing-based disambiguation
  documented above — the terser form leans on the single most universally
  trained-in casing convention across mainstream languages, so a model
  doesn't need a Ziman-specific keyword to recognize "this constructs a
  value" at all.
- **Modules: one file per module, always-qualified cross-module access,
  `pub` required to export.** The driving goal is a representative slice
  of a real backend (see docs/ROADMAP.md's North Star), which needs
  files/networking/a database organized as real modules rather than
  bolted onto the interpreter one builtin at a time. Every cross-module
  call or type reference is qualified (`user.greet(u)`, never a bare
  `greet(u)` that happens to resolve to some import) — the same
  "derivable from the line itself" reasoning as principle #1 everywhere
  else, just applied across files instead of within one. `pub` mirrors
  `val`/`var`: an important property (is this part of the module's public
  contract) is explicit at the declaration, not inferred from whether
  something outside happens to use it. Implementation-wise, cross-module
  references and a module's own top-level names are rewritten to
  `<module_path>::<name>` by a loader pass before the existing single-
  file type checker and interpreter ever run — chosen specifically so
  neither needed to learn about multiple modules at all, only the loader
  did. Full design (path resolution, `native fn` for the one place C is
  allowed to show through, why there's no manifest file) is in
  docs/ROADMAP.md, since it was designed and recorded there before
  implementation, not the other way around.
- **A native function's C implementation deliberately doesn't mirror its
  declared Ziman signature — inspired by JNI, adapted to need less.**
  JNI's native methods get a C signature that's genuinely different from
  the declared Java one (two extra parameters prepended — `JNIEnv*` and
  the receiver — and opaque handle types like `jstring` in place of the
  Java type), because C's primitives and Java's object model are
  incompatible representations that need an explicit marshaling layer to
  bridge. Ziman doesn't have that problem: the tree-walking interpreter
  already represents every value — scalar, array, or struct — as one
  uniform tagged-union `Value` (`src/value.h`), so there's no impedance to
  bridge. The result is simpler than JNI needs to be: every native
  function is a plain C function of one fixed shape, `Value fn(Value
  *args, int argc, int line, int col)` (`line`/`col` standing in for
  JNI's `JNIEnv*` — not a large callback table, just enough to raise a
  `ziman_error` that reads like any other runtime error), registered
  under the exact fully-qualified name the loader already produces
  (`io::read_file`, not a separately-tracked module path + name pair).
  JNI also offers a real lesson on *how* to bind: prefer an explicit
  registration table (`RegisterNatives`) over deriving the C symbol name
  by convention (name-mangling) — the mangled-name approach is JNI's
  legacy path, explicit registration is what the ecosystem moved to
  (Android's own NDK guidance recommends it), and it's what Ziman's
  `native_register()` does from the start. What this does *not* solve:
  `print`/`str`/`len`/`push`/`insert`/`set`/`format` still can't become
  ordinary `native fn` declarations, because they need a top/"any" type,
  a small union, or generics that don't exist yet — a C-ABI trick doesn't
  change what the Ziman-level type checker can express. See
  docs/ROADMAP.md.
- **`Result<T, E>` for recoverable errors — not exceptions, not general
  sum types, a second hardcoded parametric type instead (same trick as
  `[T]`).** Exceptions were ruled out on the same grounds classes and
  inheritance were: an exception unwinding several stack frames is
  exactly the "not derivable from the line itself" pattern principle #1
  exists to prevent — `foo()` might throw something you don't know about
  without reading its implementation. A fully general `Result<T, E>`
  with real sum types, a `match` expression, and exhaustiveness checking
  would be the "proper" Rust-shaped answer, but it's also a big, novel
  type-system investment for a problem that a much narrower feature
  solves just as well: `Result<T, E>` as one more opaque builtin type
  (like arrays), manipulated only through four functions
  (`ok`/`err`/`is_ok`/`unwrap`, plus `unwrap_err`) — at the time, no
  `enum`, no pattern matching, no user-definable sum types at all. (User-
  declared `enum`s shipped later, once it became clear every *future*
  parametric type — `Option<T>`, `Map<K, V>` — would otherwise mean
  hand-writing another one-off C special case forever; see the `enum`
  entry below. `match`/pattern matching is still deferred.) `T` and `E`
  are
  fully arbitrary Ziman types (not fixed to `string`, which was the
  original, narrower proposal) since hardcoding a second type parameter
  costs barely more than hardcoding one, and it buys something a fixed
  error type couldn't: a native function like `io.read_file` can keep
  its own error type as plain `string`, while application code layers a
  richer error type on top at the boundary, converting rather than
  forcing one universal error shape everywhere.
  `<T, E>`, not `[T, E]`: considered and rejected reusing arrays'
  bracket convention, in favor of keeping `[]` scoped to sequences (the
  array type, the array literal, and indexing already share it) and
  using `<>` — the near-universal cross-language convention for
  "parameterize a builtin type" — for everything else. This is also
  specifically Rust's own convention, not just "common": `Vec<T>` uses
  angle brackets; `[T; N]` (square brackets) is reserved for a
  fixed-size array. No real parsing ambiguity with the comparison
  operators either, since a type only ever appears after `:`/`->`/`as`,
  never in expression grammar, where `<`/`>` mean something else.
  `ok(x)`/`err(e)` reuse the exact mechanism empty array literals (`[]`)
  already needed — an "expected type" available only in specific
  contexts — extended to a second context (a function's own declared
  return type, not just a `val`/`var` annotation), since `return
  ok(...)`/`return err(...)` is by far `Result`'s most common shape, not
  an edge case the way returning an empty array is for arrays.
- **User-declared `enum` (tagged unions) — added once "hardcode one more
  type" stopped scaling.** `[T]` then `Result<T, E>` both hardcoded a
  single parametric shape directly into the compiler; a third
  (`Option<T>`) and a fourth (`Map<K, V>`) would mean the same one-off C
  work forever, in C, never in Ziman itself — the opposite of the
  project's own goal. Fixing that needs two things together: real type
  parameters (not yet built, deferred to its own design pass — see
  ROADMAP.md's "Generics" entry) and a way to express "one of several
  shapes, optionally holding data" at all, which a plain `struct` can't
  (it's a pure product type). `enum` is the second half, shipped first,
  as a *concrete* (non-generic) tagged union — `enum Option<T> { Some(T)
  None }` still isn't expressible until generics land on top of this.
  Considered Java's enum model as an alternative (a closed, homogeneous
  set of pre-built singleton values, e.g. `enum Color { RED, GREEN,
  BLUE }`, or `Planet(mass, radius)` constants sharing one field set) and
  rejected it: it can't express `Option`/`Result`'s actual shape at all
  (Java itself doesn't model `Optional<T>` as an enum for this reason —
  it's a plain class), since `Some(T)`/`None` need *different* payload
  shapes decided at runtime, not a fixed compile-time enumeration of
  identical-shaped values. Ziman's `enum` is Rust/Swift-shaped instead: a
  variant carries zero or one payload value, and a payload-less variant
  (`Point` in `enum Shape { Circle(float) Point }`) is the degenerate
  case of that same mechanism — exactly what Java's plain-constant case
  reduces to, so nothing was lost by not adopting Java's model
  separately. Never more than one payload value per variant, and never
  an inline tuple- or struct-shaped payload: Ziman has no tuple type, and
  `Result<T, E>`'s `Ok`/`Err` already established "one value per case,
  wrap a `struct` for more" as the shape to match, so `enum` doesn't get
  a second, inconsistent product-type mechanism of its own.
  Construction and consumption are both always qualified by the enum's
  own name (`Shape.Circle(5.0)`, `Shape.is_Circle(s)`,
  `Shape.unwrap_Circle(s)`) — considered and rejected bare construction
  (Rust's usual look for `Some`/`Ok`) because those read as bare only
  because they're prelude-special-cased; an ordinary user enum in Rust
  needs qualifying too, and two unrelated Ziman enums could easily share
  a variant name (`None`, `Active`, `Error`), which bare construction
  would make silently ambiguous. `is_<Variant>`/`unwrap_<Variant>` are
  compiler-generated per enum, per variant — the same predicate-plus-
  unwrap shape `Result`'s `is_ok`/`unwrap`/`unwrap_err` already
  established, just generalized from a hardcoded 2-variant type to
  however many variants a user's enum has, and namespaced under the
  enum's own name so two enums' generated functions never collide. No
  `match`/pattern matching yet — deliberately deferred, same instinct as
  `Result` shipping without one.
- **`Channel<T>` and `spawn` — real OS-thread concurrency, built on a
  single-writer / actor pattern, not shared memory plus locks.** Surfaced
  by trying to write a layered (controller/service/repository) example
  backend: every "write" had to explicitly return a new value and the
  caller had to rebind it, which works for one request's own call chain
  but doesn't solve *multiple concurrent requests* needing to reach one
  shared, consistently-updated store. Considered and rejected Java/
  Spring's actual model — OS threads sharing memory, synchronized with
  locks — for two reasons: a mutex held by one thread while another
  silently blocks is exactly the "not derivable from the line itself"
  problem principle #1 already rules out, and implementing it honestly
  would mean reworking the whole runtime's memory model, since `Value`
  sharing-by-pointer is only safe today because nothing else can be
  touching the same memory concurrently. The actor model instead confines
  the unsafe part to one visible place: one thread owns a piece of
  mutable state and is the only thing that ever touches it; everyone else
  sends it a message and waits for a reply. `Channel<T>` is a third
  hardcoded parametric type (after `[T]`/`Result<T, E>`), but unlike
  those, hardcoding it isn't a shortcut around generics — its entire
  value (safe blocking `send`/`receive` across real threads) is native by
  necessity; no amount of user-definable generics would ever let pure
  Ziman express real thread synchronization, so there's no future "let
  users define their own Channel-like things" case being foreclosed the
  way there was for `Option<T>`/`Map<K, V>`. Unified, not split into
  `Sender<T>`/`Receiver<T>` the way Rust's `mpsc` is — that split exists
  to support Rust's ownership/move system, which Ziman doesn't have, so
  it would just be extra type surface with no payoff here. Unbounded
  queue in v0 (no backpressure) — a real production concern, not a
  correctness one, deferred until something concretely needs it.
  `spawn name(args)` is a new *statement*, not an ordinary call, because
  Ziman has no first-class functions: naming the target function directly
  and resolving it statically (exactly like an ordinary call's callee
  already is) sidesteps that gap entirely rather than requiring it. The
  spawned function must return `void` — there's no thread-handle/join
  mechanism in v0, so with no first-class functions and no join, a
  result can only come back through a `Channel` the function is
  explicitly given; requiring `void` makes that a compile-time rule.
  Considered building first-class functions (or closures) first, since
  `spawn`'s natural long-run shape would take a closure; decided against
  it because nothing *concrete* forces the question yet (no `net`/`http`
  module exists either, which is the more likely real forcing case, via
  needing a routing table of path → handler). A plain reference to an
  already-declared function (closer to a C function pointer than a JS
  closure — no captured environment, since a top-level function has
  nothing to capture) would likely be the cheaper, sufficient answer when
  that need actually arrives, rather than full closures.
  Sharing a `Value` across threads needed no new machinery at all: this
  interpreter never frees an `ArrayObj`/`StructObj`/`EnumObj`/`ResultObj`
  once built (same "AST nodes are heap-allocated and never freed" policy
  ast.h already documents for the AST itself — the interpreter is a
  short-lived process, so the OS reclaims everything at exit), and
  nothing mutates one after construction, so a value crossing a channel
  is just a pointer copy — no use-after-free, no data race. Only the
  channel's own queue needed a
  real mutex/condvar.
- **Generics — explicit type arguments everywhere, no inference, no
  bounds/constraints, staged struct → enum → function.** The
  "hardcode one more parametric type into the compiler" trick (`[T]`,
  `Result<T, E>`, `Channel<T>`) stopped scaling once `enum` shipped and
  made the real demand concrete (`Option<T>`, and any other
  Result/Option-shaped type a user wants, without another one-off C
  special case each time — see the `enum` entry above and ROADMAP.md's
  "Generics" entry). Every instantiation spells out its type arguments —
  `Box<int> { value: 5 }`, never `Box { value: 5 }` inferred from the
  field — the same "explicit over implicit" instinct behind `val`/`var`
  requiring a declared type anywhere it can't be trivially inferred from
  the right-hand side. No bounds or `where`-style constraints on a type
  parameter, matching "no trait system yet" (see the classes-vs-traits
  discussion above): a generic declaration's body can do nothing with an
  abstract `T` beyond storing and passing it around. Represented as a new
  `TY_TYPEVAR` `TypeKind` — "the Nth/named formal parameter of the
  enclosing generic declaration" — substituted on demand wherever a
  concrete instantiation's fields/variants/parameters are actually looked
  at (struct-literal field checking, field access, `with`, `==`
  comparability), never eagerly monomorphized: `Value` is already
  runtime-type-erased (`ArrayObj`/`StructObj` already just hold generic
  `Value*` regardless of static field types, same fact the `Channel<T>`
  entry above leans on for cross-thread sharing), so there's no runtime
  representation to generate per instantiation — only the type checker's
  substitution logic is new work. `Name<Arg, ...>` in *type* position
  (`struct Box<T>`'s own field declarations, a `val`/param annotation) is
  unambiguous with a comparison the same way struct-literal `{` already
  is: a type name is always PascalCase, and casing is enforced everywhere,
  so an uppercase identifier followed by `<` can never be the left operand
  of a real `x < y` comparison. A generic *function* call site
  (`identity<int>(5)`) is the one place this isn't free: a lowercase
  callee followed by `<` is genuinely ambiguous with `x < y`, and needs a
  one-token lookahead heuristic (peek at what follows `<`; commit to
  parsing type arguments only if it looks like a type-starter) — deferred
  to the generic-functions stage, since generic structs don't need it.
  Same-module only for now, like `enum` — no `qualifier.Name<...>`
  support yet. Staged as generic structs, then generic enums
  (`enum Option<T> { Some(T) None }`, unlocking the actual `Option<T>`
  motivation), then generic functions, each a smaller, independently
  useful slice rather than one large change.

## Deliberately deferred (not v0)

- Effects visible in function signatures (which functions do I/O or mutate
  outside their own frame) — real value, real design cost, tracked in
  ROADMAP.md.
- Structured/machine-parseable compiler diagnostics as a first-class output
  mode (`--json` errors).
- Modules / imports beyond a single file.
- A package manager or build system beyond a Makefile.

## Non-goals

- Ziman is not trying to be a tree-native/structural-editing language (see
  the earlier discussion in this project's originating conversation for
  why that's a much larger, more novel effort). Source of truth is text.
- Not chasing raw performance in v0 — it's a tree-walking interpreter.
  Compilation to bytecode or native code is a later milestone if the
  language design proves out.
