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
   (Implemented as `ziman --json`: one JSON object per error, rendered
   from the same record as the text form -- see the "`--check` and
   `--json`" entry below.)
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
  project's own goal. Fixing that needed two things together: real type
  parameters (deferred to its own design pass at the time — see the
  "Generics" entry below, now landed through generic enums) and a way to
  express "one of several shapes, optionally holding data" at all, which
  a plain `struct` can't (it's a pure product type). `enum` is the second
  half, shipped first, as a *concrete* (non-generic) tagged union —
  `enum Option<T> { Some(T) None }` wasn't expressible until generics
  landed on top of this (it is now — see the "Generics" entry below).
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
  Ziman had no first-class functions when `spawn` was designed (references to
  named functions arrived later, ROADMAP R13, and `spawn` now accepts generic
  targets too): naming the target function directly
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
- **Generics — explicit type arguments everywhere, no inference, staged
  in five parts.** The "hardcode one more parametric type into the
  compiler" trick (`[T]`, `Result<T, E>`, `Channel<T>`) stopped scaling
  once `enum` shipped and made the real demand concrete (`Option<T>`,
  and any other Result/Option-shaped type a user wants, without another
  one-off C special case each time — see the `enum` entry above and
  ROADMAP.md's "Generics" entry). Every instantiation spells out its type
  arguments — `Box<int> { value: 5 }`, never `Box { value: 5 }` inferred
  from the field — the same "explicit over implicit" instinct behind
  `val`/`var` requiring a declared type anywhere it can't be trivially
  inferred from the right-hand side. **No general bounds/`where`-style
  constraints** — no trait system yet (see the classes-vs-traits
  discussion above): a generic declaration's body still can't do anything
  with an abstract `T` beyond storing and passing it around, *except* two
  hardcoded bounds: `Eq` (`<T: Eq>`, meaning "supports `==`"), added
  specifically because `Map<K, V>`/`Set<T>` (see ROADMAP.md's
  "Collections module" entry) need to compare an abstract key type —
  without it, `key == k` inside a generic `map_get` would be exactly the
  comparison an unbounded `TY_TYPEVAR` correctly can't prove safe — and
  `Hash` (`<T: Hash>`, meaning "supports the `hash` builtin"), added
  right alongside it so `Map`/`Set` can actually be a real hash table
  (bucket index = `hash(key) % bucket_count`) instead of a linear `Eq`-
  only scan, which would only have moved the "just hardcode one more
  parametric type" problem this whole generics effort exists to end into
  a performance problem instead. Combinable on one parameter with `+`
  (`<K: Eq + Hash>`) since `Map`/`Set` need both on the same key type at
  once. `Hash` is scalar-only by decision (`int`/`float`/`bool`/`string`)
  — no derived/structural hash for a struct or enum, matching the
  existing "no derived equality for an arbitrary struct beyond field-by-
  field `==`" pattern, so a general per-type hash story was explicitly
  deferred rather than attempted alongside this. Both are narrow,
  special-cased escape hatches, not the start of a general trait system,
  and both closed, compiler-known concepts, not user-definable
  interfaces. Represented as a new
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
  Cross-module works too (`mod.Box<int> { ... }`,
  `mod.Option<int>.Some(5)`, `mod.identity<int>(5)`) — this also closed a
  pre-existing, generics-independent gap where cross-module enum access
  didn't exist at all yet (the qualified-reference parser only ever
  handled one dot; `qualifier.EnumName.Variant` needed a second). Staged
  as five independently useful slices rather than one large change (see
  ROADMAP.md's "Generics" entry for the current state of each): generic
  structs, bounded type parameters (`Eq`), generic enums (unlocking
  `Option<T>`), cross-module generics (unlocking a shared `collections`
  module), then generic functions — now all implemented. The function
  stage is also where the `Eq` bound finally gets *exercised* inside a
  body (`fn are_equal<T: Eq>(a: T, b: T) -> bool { return a == b }`), not
  just enforced at instantiation, which surfaced two real gaps in the
  earlier stages: a bounded type parameter used as a type argument to
  *another* bounded generic type in a declaration's own signature
  (`fn map_get<K: Eq, V>(m: Map<K, V>, ...)`) needed the same "which
  declaration's type parameters are in scope" context during
  *declaration validation* that a function body needs for `==` — fixed by
  generalizing what had been a function-body-only tracking variable into
  one covering every generic declaration's own well-formedness check, not
  just a function's body; and a function's own type parameter referenced
  from *inside its body* in any position other than a param/return type
  (`Option<V>.Some(...)`, a `val` annotation, a cast) was never
  reclassified from a provisional struct/enum reference into a type
  variable, since only a struct/enum *declaration*'s field/variant types
  ever went through that step before — fixed with a small parser-side
  walker (mirroring loader.c's self_qualify_expr/self_qualify_stmt
  shape) that reclassifies every type-bearing spot in a generic
  function's parsed body, run once right after the body itself parses.
- **`bigint`/`decimal` — arbitrary-precision integer and decimal, a
  third and fourth numeric type alongside `int`/`float`.** Added once
  `int`'s fixed 64 bits stopped being enough for a real, concrete need
  (exact arbitrary-size integer arithmetic, e.g. `100!`; exact
  money/measurement math) that `float` can't cover either (binary
  floating point can't represent most integers past 2^53, or most
  decimal fractions at all, exactly). `bigint` is modeled on Java's
  `BigInteger`: sign-magnitude, immutable, every operation builds a new
  value rather than mutating (see src/bignum.c) — the same "never
  mutate, always build fresh" pattern `ArrayObj`/`StructObj` already
  use, so `bigint` slots into the existing object model with no new
  memory-management concept. Backed by base-1e9 limbs (not binary), a
  deliberate simplicity-over-speed choice matching this project's own
  "not chasing performance" non-goal below — string conversion is
  nearly free, and the arithmetic itself (schoolbook multiply, binary-
  search-per-limb long division rather than Knuth's faster but far
  fiddlier Algorithm D) stays easy to verify by hand rather than fast at
  scale. `decimal` is modeled on Java's `BigDecimal`, built directly on
  `bigint`: an unscaled `bigint` magnitude plus a scale (see
  src/decimal.c), needing no new low-level arithmetic of its own —
  `+`/`-`/compare align scales first (exact, never rounded), `*` needs
  no alignment (multiplies magnitudes, adds scales, always exact), `/`
  rounds half-up-away-from-zero to `max(a.scale, b.scale)` since
  decimal division doesn't always terminate, and `%` stays exact (an
  integer-truncated quotient needs no rounding). `==` compares numeric
  value ignoring trailing-zero scale differences (`1.0d == 1.00d`),
  deliberately diverging from Java's well-known `BigDecimal.equals()`
  gotcha (scale participates in `equals()` but not `compareTo()` there)
  — and `hash()` normalizes away trailing zeros first so it stays
  consistent with that same equality.
  A `Value` stores both as a heap pointer (`BigNum *`/`Decimal *`), not
  embedded inline the way `int`/`float`/`bool` are, specifically so the
  `Value` union — and therefore every `Value` in the interpreter, not
  just these two types' — doesn't grow just because two more types
  exist. A literal needs an explicit suffix (`n` for `bigint`, `d` for
  `decimal`) rather than being inferred from context or from a bare
  literal: a bare literal always means `int`/`float` (unchanged, no
  breaking change to any existing program), and a suffix means the
  literal's meaning is self-evident right at the point of use, the same
  "explicit over implicit" reasoning that also shaped `Eq`/`Hash`'s
  bound syntax and generics' explicit type arguments — no "look
  elsewhere to know what this means" the way a purely context-inferred
  literal would require. Neither ever implicitly mixes with any other
  numeric type in an operator (`bigint + int` is a type error, `use
  'as' to cast explicitly`, same rule `int`/`float` already enforce
  with each other) — casting between any two of the four is lossy
  wherever the target can't represent the source exactly (saturating
  toward `int`, truncating the fractional part toward `bigint`, and
  going through `float` in either direction since `float` itself can't
  represent most decimals exactly), and that lossiness is never silent
  or implicit. `decimal_round`, not an overload of `round`, is
  `decimal`'s equivalent of `floor`/`ceil`/`round` — Ziman has no
  argument-based overloading anywhere, so a different signature needs a
  different name, not the same name doing something else by arity.
- **The prelude — a fixed, small native module merged bare into every
  program, no `use` required and never qualified.** `bigint`/`decimal`
  arriving meant new type-specific functions with no obvious home
  (`parse_bigint`, `parse_decimal`, `decimal_round`) alongside older
  ones that had been hardcoded `typecheck.c`/`interpreter.c` dispatch
  since before either type existed (`parse_int`, `parse_float`,
  `floor`/`ceil`/`round`, `concat`, `substring`, `split`, `trim`,
  `to_upper`, `to_lower`) — both kept growing that dispatch table every
  time a type gained a new operation. But native functions (built for
  `io`) are qualified by design (`io.read_file`, `use io`), and these
  functions are core, always-there operations exactly like `int`/
  `float`/`string` themselves — requiring `use numbers`/`use strings`
  for `parse_int`/`concat` would be a real regression, and moving only
  *some* of a shared-name builtin's type branches to a qualified module
  while others stayed core would be actively confusing. The fix isn't
  new loading machinery: the prelude's source is parsed exactly like any
  other module and merged into the flat `Module` the same way the entry
  module's own functions already are (see the module system entry
  below) — just unconditionally, and never self-qualified, so `parse_int`
  stays `parse_int`, not `prelude::parse_int`. Once merged, a call to it
  is now genuinely indistinguishable from a call to any other function
  in the program, and everything downstream — type-checking a call,
  calling a native function at runtime (`native_lookup`, built for `io`)
  — already worked generically for *any* native fn, so none of that
  needed to change either. This moved 14 functions out of hardcoded
  dispatch with zero new type-checking or calling logic, and (verified)
  zero breaking change to any existing program: every one of them
  already had exactly one fixed signature, no cross-type polymorphism
  under the name.
  **Not every builtin can make this move, and it's a structural
  boundary, not an oversight**: `print`/`str`/`format`/`len`/`push`/
  `insert`/`set`/`contains`/`abs`/`min`/`max`/`hash` each need to work
  under *one* unqualified name across *multiple* types (`abs(5)`,
  `abs(5.0)`, `abs(5n)`, `abs(5.0d)` all share the name `abs`) — a
  native fn is exactly one fixed signature per declared name, and Ziman
  has no argument-based overloading anywhere, so a truly polymorphic-by-
  name builtin has nowhere to go without either generic native fn
  signatures (not designed) or overloading (not wanted). The operators
  themselves were never a candidate either way, for the same "no
  overloading" reason — `+`/`-`/`==`/etc. stay hardcoded in
  `eval_binary`/`check_binary` regardless.
  **Broadened later (see the "Prelude" entry near the end of this list)
  to carry ordinary Ziman declarations too, not just native fn
  signatures** — this entry's own framing ("a fixed, small native
  module") describes why it was built, not everything it holds today.
- **Bundled core modules — ordinary, qualified `.zm` modules embedded in
  the binary, distinct from the prelude.** The `collections` module
  (`Box`/`HashMap`/`HashSet` at the time — `Box` was later removed
  outright, see below) was, at first, something every
  project had to vendor a copy of — real friction, surfaced by actually
  dogfooding it in a second project (`health-record-backend-ziman`),
  which had a working, useful module it could only get by copy-pasting a
  file across repos. The prelude's fix doesn't apply here: `collections`
  needs to *stay* qualified (`collections.HashMap`, `use collections`)
  the way any module does — merging it bare like the prelude would mean
  `HashMap` and `HashSet` becoming reserved global
  names no project could ever declare its own version of, for a module
  that's much bigger and much more likely to want an app-specific
  variant someday. So this is a second, narrower mechanism
  (`src/corelib.h`/`corelib.c`): `loader.c`'s `load_module` tries a
  project-relative file first, exactly as it always has, and only
  falls back to an embedded module's source if no such file exists — a
  local file of the same name always wins, so a project can still
  shadow a core module with its own. The embedding itself piggybacks on
  the same "parse a source string, no new machinery" trick the prelude
  established: the canonical `.zm` source lives at `lib/core/*.zm`, the
  Makefile turns each into a generated `#define X_SOURCE "..."` header
  at build time (so the embedded copy can never drift from the source
  file a human actually reads and edits), and `corelib.c` just looks the
  requested `use` path up in a small table. Confirmed as a genuine
  drop-in for vendoring, not just a demo of one: every test fixture that
  used to carry its own copy of `collections.zm` had that copy deleted
  outright and needed zero other changes to keep passing. `io.zm` (the
  `io` native module's signature file, previously vendored the same way)
  joined the same day, closing the identical gap the module system's own
  original design write-up had flagged for it — adding a second core
  module cost one line in the Makefile's module list, one line in
  `corelib.c`'s table, and turned the Makefile's header-generation rule
  from one hardcoded case into a real pattern rule.
- **Per-name imports (`use path.Name [as alias]`) — fixes a real
  ergonomics gap generics exposed, without touching the "always
  qualified" rule.** Generic collection code turned out to be the worst
  case for two of Ziman's rules compounding: `use collections` then
  `collections.hashmap_set<int, string>(map, 30, "alice")` repeats a
  module prefix *and* explicit type arguments on nearly every line. Two
  candidate fixes were on the table — a per-name import for the prefix,
  and Java-diamond-style ("`new HashMap<>()`") type-argument inference
  for the repetition — and only the first one shipped, because they land
  on opposite sides of what Ziman is actually for: an LLM already
  generates `from x import y` / `use x::y` shaped code correctly by
  habit, since it's common across languages, and this doesn't add any
  new implicit reasoning — it just moves "where did this bare name come
  from" from every call site to one `use` line, arguably *more* visible
  than a repeated prefix, not less. Diamond-style inference would have
  reopened "no inference, ever" (see the Generics entry above) in its
  worst form: *conditional* inference that only fires for the exact
  `val x: T = call()` shape and silently doesn't anywhere else (a
  function argument, a struct field, a nested expression) — worse than
  either full inference or none, since identical-looking code sometimes
  needs type arguments and sometimes doesn't, for reasons invisible at
  the call site itself. Left this one alone.
  Mechanically, a per-name import is resolved and privacy-checked once,
  at the `use` line (`resolve_named_imports` in loader.c), producing a
  `local_name -> fully-qualified-name` table consulted by the same
  self-qualify walk that already rewrites a module's own bare references
  to their qualified form — an imported name is just a second table that
  walk falls back to, not new tree-walking machinery. It introduces no
  new *declarations*: the imported function/struct/enum still lives
  under its one real qualified name in its own module's function/struct/
  enum list, so nothing about typechecking a call, or the existing
  duplicate-name/builtin-redefinition checks, needed to change either.
  The casing-consistency rule on an `as` alias (`use collections.HashMap
  as box2` is a compile error) is enforced at parse time, before the
  loader even knows what `HashMap` is — it doesn't need to: an
  uppercase-vs-lowercase mismatch would already be wrong regardless of
  the target's kind, since Ziman's grammar depends on casing alone to
  parse a struct literal versus a function call.
- **`byte` — a fifth scalar type, but deliberately not a sixth numeric
  one.** Motivated by wanting to represent raw octets (a UUID's bytes,
  `random_bytes`' output, see the "random/uuid" entry below) — `[int]`
  would work but says nothing about the 0-255 range invariant, and every
  numeric type Ziman has added so far (`bigint`, `decimal`) required a
  full systematic sweep (lexer suffix, parser, `Value` union, typecheck,
  interpreter) for something whose only concrete use case right now is
  "hold one element of a byte array" — a real cost, so the type earns
  its keep by being unsigned (0-255, not Java's signed -128..127, a
  well-known footgun any real octet-shaped data would immediately hit)
  and by deliberately **not** getting `+ - * / %`, unary `-`, `< > <=
  >=`, or `abs`/`min`/`max`. Ziman has no operator overloading, so
  giving `byte` arithmetic would force an answer to a question nothing
  actually needs answered yet: does `byte + byte` wrap mod 256 (silent
  truncation, which Ziman avoids elsewhere) or saturate (arbitrary for
  an operation nothing calls)? Leaving it unanswered by not building
  the operators at all is the same "don't decide until a requirement
  arises" posture as the Ord/Cmp/trait question. What *is* supported —
  `==`/`!=`, `hash`, `str`/`print`/`format`, and a saturating cast
  to/from `int` (`300 as byte` is `255`, matching
  `bignum_to_i64_saturating`'s "clamp, don't wrap" precedent for every
  other narrowing numeric cast) — is exactly what a raw-data type needs:
  construct it, compare it, display it, move it in and out of `int`.
  Also composes for free with everything generics already built: `byte`
  satisfies `Eq`+`Hash`, so `HashMap<byte, V>` and `[byte]`
  equality/`contains` work with no new code beyond adding `TY_BYTE` to
  the existing scalar-kind checks.
- **`random`/`uuid` — the two new core modules `byte` was built for.**
  `random` is as small as a native module gets: one function,
  `random_bytes(n: int) -> [byte]`, reading straight from
  `/dev/urandom` (not a platform syscall like `getrandom()`/
  `arc4random_buf()` — plain `fopen`/`fread`, the same shape
  `native_io.c`'s `read_file` already uses, no per-platform `#ifdef`).
  At the time it was built, it was the only possible source of real
  non-determinism in Ziman — no clock/time builtin existed yet either,
  so there was nothing to seed a Ziman-side PRNG from even if one had
  been built instead (see the `time`/`clock` entry below for how that
  changed).
  `uuid` is pure Ziman on top of it — a `Uuid` struct wrapping
  `[byte]`, `uuid_new()` generating an RFC 4122 v4 UUID, `uuid_to_string`
  formatting the canonical dashed-hex form. The interesting part is
  setting the version/variant bits with **no bitwise operators at
  all** (Ziman has none — no `& | ^ << >>`): clearing or setting a
  nibble of an unsigned byte via integer div/mod by a power of two is
  exactly equivalent to masking it, so `(b % 16) + 64` (clear the top
  nibble, set it to 4) does precisely what `(b & 0x0F) | 0x40` would in
  a language with bitwise ops, and `(b % 64) + 128` does what `(b &
  0x3F) | 0x80` would. This only works because `byte`'s cast to/from
  `int` exists and its arithmetic doesn't need to (see the `byte` entry
  above) — the version/variant computation happens entirely in `int`,
  with `byte` only as the storage type going in and out.
  Confirms two things about earlier design choices, incidentally: `byte`
  needed no bitwise operators to be useful (a real forcing case for them
  still hasn't shown up), and a core module can depend on another core
  module (`uuid.zm`'s own `use random.random_bytes`) with zero new
  loader logic — the fallback in `load_module` triggers on the `use`
  path string alone (`"random"` matching `CORE_MODULES`), never
  consulting the *importing* module's own origin, so it doesn't matter
  that `uuid.zm`'s source came from the binary rather than a real file
  on disk.
  `Uuid` satisfies `Eq` (structural `==`/`!=` on `[byte]`, free from the
  existing struct/array equality rules) but not `Hash` (hashing has no
  derived form for a struct, see the collections module entry) — enough
  to use as a struct field compared with `==`, not yet enough to use as
  a `HashMap` key. Left unresolved rather than worked around, same
  "don't decide until a requirement arises" posture as `byte`'s own
  missing arithmetic.
- **`time`/`clock` — seven `java.time`-shaped types, one new native
  primitive, and a representation that changed twice during design.**
  The scope was deliberately narrowed once up front: `ZoneOffset` only,
  with `ZoneId` a thin wrapper that could only name `"UTC"`. Named
  political timezones were added later (see "Named time zones" below).
  `Instant`/`Duration`'s representation went through two designs before
  landing: first a single `bigint` nanoseconds-since-epoch/nanoseconds
  count (elegant — `bigint` already has full native `+ - * / < > <= >=
  == !=`, so comparison and arithmetic would have been free), then
  `{ seconds: int, nanos: int }` once nanosecond precision was
  specifically requested and it became clear plain `int` (int64)
  *seconds* has a completely different range story than plain `int`
  *nanoseconds* does: milliseconds-since-epoch in `int64` covers ~292
  *million* years, nanoseconds-since-epoch covers only ~292 (Java's own
  reason for splitting `Instant` into seconds+nanos instead of one
  wide integer), and **seconds alone has no practical range limit at
  all** (~292 *billion* years) — so `{seconds: int, nanos: int}` gives
  every advantage the `bigint` design had (no range anxiety) with a
  simpler, non-arbitrary-precision type, once nanosecond resolution is
  wanted. The one thing this representation costs, compared to the
  `bigint` version: `nanos` must be kept normalized (`[0, 999999999]`,
  sign lives entirely in `seconds`) by every constructor and arithmetic
  function through one shared `normalize` helper — the exact
  sign-handling subtlety that makes `java.time.Duration`/`Instant`
  fiddly to implement correctly from scratch, and the reason `duration_
  to_string` needs its own careful "true magnitude" formatting logic
  rather than a naive field-by-field print (`{seconds: -1, nanos:
  500000000}` is `-0.5`s, not `-1.5`s).
  The calendar math (`LocalDate`'s epoch-day conversion, which
  `Instant`'s UTC formatting also reuses) is Howard Hinnant's
  public-domain days-from-civil algorithm from libc++'s `<chrono>`,
  translated directly into Ziman rather than derived from scratch —
  it was already written against truncating (toward zero) integer
  division, exactly Ziman's own `/`/`%` semantics, so the translation
  needed no adaptation beyond syntax. Verified against known reference
  points (epoch day 0, a negative epoch day, the 1900/2000 century
  leap-year rule, `Jan 31 + 1 month` clamping) rather than trusted on
  inspection alone.
  `instant_now()` needed exactly one new native primitive — `clock`'s
  `now_raw() -> [int]`, a single `clock_gettime()` call returning
  `[seconds, nanos]` as a two-element array (the same "native returns a
  small array of scalars, pure Ziman gives it a shape" split
  `random_bytes -> [byte]` already established), read atomically in one
  call specifically to avoid the narrow-but-real bug of seconds and
  nanos corresponding to two different clock reads.
  As with `byte`/`Duration`/`Instant`, no `<`/`>` on any of the seven
  types — every one gets an explicit `_compare(a, b) -> int` instead,
  same "don't reopen Ord/Cmp without a forcing case" posture held
  throughout this project.
- **The database modules and their names.** `db` is the shared module, as `http` is for `server` and `client`:
  the value, row and error types (`SqlValue`, `Row`, `DbError`), the `sql_as_*` readers and the URL parser
  (`db.parse_url`, `db.url_param`; a bare `parse` says nothing in a shared module). `datasource` is the module
  a program calls (`datasource.open`, `datasource.query`, `datasource.rows`, pools, transactions), named for the
  Java term for where connections come from; it holds the `Connection` enum that wraps every driver and so
  cannot be shared with them. The point is the dependency direction: `sqlite`, `pg`, `mysql`, `uuid` and
  `time` use `db` and never `datasource`, which uses them. (These had been `sql` for the vocabulary, `url`
  for the parser and `db` for the caller before.)
  `db` also holds what `pg` and `mysql` had each written out: the `Request` enum and its request and page
  types, `run_request` (send a statement to the connection's thread and wait) and `open_rows` (open a cursor
  and read it as an `Iter`, with its page buffer, cursor number and guard), about 150 lines that were
  identical in the two drivers. Each driver's `rows` is now one line calling `db.open_rows`.
- **Lazy iteration: `Iter<T>` and `fs.lines`.** `for` walked arrays, maps, sets and ranges, all
  fully in memory. A file of any size, a database cursor, a large CSV need to be read as the loop
  asks. `Iter<T>` is a prelude struct holding a native handle (the address of an `IterObj`: a
  `next` function, a `release` function and its state; tracked in a `HandleSet` like sockets and
  connections, with a GC finalizer so an abandoned source is still closed). `for` over an `Iter`
  calls `next` per item and releases the source on every way out (`break`, `return`, and so a `?`);
  an exhausted source releases itself. The item type is the struct's type argument as the checker
  resolved it on the iterable expression, so `for line in lines` binds a `Result<string, string>`.
  Items of `fs.lines` are `Result`s rather than plain strings so a read error mid-file is a value
  the loop handles with `?` instead of an exception; a line holding a NUL byte (a `string` cannot)
  is an Err item and the iteration carries on. Opening a file when the process is out of descriptors
  runs the collector once and retries, so iterators dropped mid-file do not exhaust them.
  `iter_next` and `iter_close` are the manual forms. `iter_from<T>(next, close)` is a source written in
  Ziman: the interpreter calls the `next` closure through the same `native_call_closure` the array
  natives use, and `close` on release; the state lives in what the closures call (a channel, a native
  handle) because a closure cannot mutate a captured variable, and it is deliberately excluded from
  the finalizer (`no_finalizer`), which must not run Ziman code. `fs.chunks` and `fs.walk` are native
  (a depth-first stack of sorted directory listings; `lstat` decides, so a symlink to a directory is
  a path and cannot loop). `datasource.rows` is a SQLite cursor (`cursor_open_raw`/`_next_raw`/`_close_raw`
  over `sqlite3_step`, its own handle set and finalizer) wrapped by `iter_from` in `sqlite.zm`;
  `datasource.rows` on a pool borrows a connection and wraps the iterator so closing it also releases the
  connection. Measured on SQLite: a million-row SELECT through `datasource.rows` used 72 MB and 16 s, through
  `datasource.query` 1.7 GB and 50 s.
  **Postgres and MySQL cursors.** Both are protocol-level, run by the connection's owner thread, which now
  takes `OpenCursor`, `FetchCursor` and `EndCursor` requests next to `Run`; each cursor gets a number so a
  late message about a finished one (a garbage-collected guard) cannot end the one that replaced it. The
  client side is an `iter_from` whose `next` drains a page buffer (a `Channel<Option<Row>>`, since a
  closure cannot mutate a captured variable) and asks for another page when it is empty; the guard
  in the closure's state ends the cursor if the iterator is dropped, because the finalizer only sends a
  channel message. **Postgres:** `Parse`/`Bind`/`Describe`/`Execute` with a 500-row limit and a `Flush` instead
  of a `Sync`, since a `Sync` outside a transaction block destroys the unnamed portal; `PortalSuspended`
  means more, `CommandComplete` the end, and only then is `Sync` sent and `ReadyForQuery` read. Because an
  unsynced portal owns the protocol state, the connection answers any other statement with an Err until the
  cursor is done. **MySQL:** the statement is prepared and executed with `CURSOR_TYPE_READ_ONLY`, which
  answers with the column definitions and no rows; each `COM_STMT_FETCH` returns binary rows and an EOF
  whose `LAST_ROW_SENT` status ends the cursor. The server allows other statements between fetches, so
  they are allowed; only one cursor per connection is. **Errors** that arrive after some rows of a page
  are delivered as rows first and the error on the next fetch (`deferred`), so the loop sees every row the
  server sent and then the Err item. Postgres 300,000 rows: 51 MB against 609 MB; MySQL: 43 MB against
  706 MB. **A bug found on the way:** `guard_new` registered its finalizer with `GC_REGISTER_FINALIZER_NO_ORDER`,
  which does not keep what the guard references alive: the message it sends (and the channel) could
  already be reclaimed, and the owner thread received some other value, which the old code hid because
  anything unrecognised means "close" in every driver's `serve`. It now uses ordered finalization. The line reading is a shared `LineSource`
  (`src/iter.c`), used by `fs.lines` and by `ndjson.records`, which parses each line with the same
  native JSON parser as `parse_json` (an Err item names the line, as `parse_ndjson` does). Every lib
  function that reads a file was checked: `properties`, `toml`, `yaml` and `csv` need the whole
  document (placeholders and profiles span lines, a quoted CSV cell can contain a newline), so only
  `ndjson` streams. Not done: a lazy stream API on top
  (`map`/`filter` fusing over an `Iter`), other sources (a database cursor, `csv.records`,
  `net` lines), and a `for` over an `Iter` of a user-defined source (the sources are native).
- **More lazy sources: `net.lines`, `csv.records`, `client.stream`.** All three are written in Ziman on
  `iter_from`, with the position kept in a one-element `Channel` used as a mutable cell (receive the state,
  send the new one) because a closure cannot change a captured variable. `net.lines` keeps unread bytes and an
  end-of-input flag there and looks for `\n` with `bytes_index_of`; it never closes the socket. `csv.records`
  reads lines from `fs.lines` and joins them until a record's `"` count is even (a quoted cell opens and
  closes with one each and a doubled quote adds two), then hands that one record to the same native scanner
  `parse_csv` uses, so the two agree cell for cell; errors from the scanner are relative to the record, so
  their line numbers are shifted to the file's, and a record left open at the end of the file is re-scanned
  to get the scanner's own message (a stray quote in the middle of a cell looks like an unclosed one to a
  line count). `client.stream` splits the request-writing half out of `exchange` (`write_request`), reads
  only the head, and then reads the body block by block under a small state machine for the three framings
  (length, chunked with its size lines, trailers and per-chunk CRLF, until-close); it asks for `identity`
  because the whole-buffer decompressor cannot run a block at a time, and does no redirects, cookies or
  connection reuse.
- **Named time zones (`zoneid_of`, module `zone`).** Read from the operating
  system's compiled zone files (TZif, `$TZDIR` or `/usr/share/zoneinfo`)
  rather than embedding the IANA database: the data changes several times a
  year and is the operating system's to keep current, and a program that runs
  where the files are absent gets an Err, never a wrong offset. `zone` parses
  the version 2+ block (64-bit transitions), keeps the transition list and the
  local time types as parallel arrays of scalars (Ziman compares arrays of
  structs by reference-free `==` only for scalars, and `ZoneId` has to stay
  comparable), and reads the POSIX rule string in the file footer (`M`, `J` and
  day-number forms, offsets like `<+0330>-3:30`, an optional `/time`) for the
  years after the last listed transition, so `2100-07-01` in New York is EDT
  without a table. `localdatetime_to_instant_in` probes the offsets a day
  before and after and keeps the valid ones: two mean an overlap (the earlier
  instant wins), none a gap (moved forward by the gap, as `java.time`). Zone
  names are checked before they become a path (letters, digits, `_+-/`, no
  `..`, no leading slash). Tests use copies of seven zone files under
  `tests/fixtures/tzdata` selected with `TZDIR`, so the suite does not depend on
  the machine's tzdata. Not done: leap seconds (ignored), the historical
  `Zone.Rules` API (`transitions`, next/previous transition), and a bundled
  fallback for systems without zone files.
- **`docs/CORELIB.md` — a generated reference, not a hand-written one,
  once the core-module surface got big enough to need one.** All
  following a strict `typename_verb` naming convention specifically so
  an LLM that's seen three functions on a type can usually guess the
  fourth correctly — but "usually correct" isn't "don't need a lookup,"
  and there was no single place listing every signature in one pass.
  The obvious wrong move was hand-writing that list: this project has
  repeatedly caught (and had to fix) stale comments and docs elsewhere
  in this same session, so a hand-copied signature list was guaranteed
  to drift the first time a core module's function was renamed or a
  parameter added. Instead, `tools/gen_corelib_reference.sh` extracts
  every `pub struct`/`pub enum`/`pub fn`/`pub native fn` directly from
  each `lib/core/*.zm` file's own source (a small awk state machine —
  track "inside a struct/enum body," print until the closing `}`; for a
  function, print the signature line with its trailing ` {` stripped)
  and assembles them under one hand-maintained one-line description per
  module. Confirmed against source rather than trusted, both times it's
  been touched: the generated file's total `pub` line count matched
  `grep -c '^pub ' lib/core/*.zm`'s count exactly before either was
  considered done. Not run as part of `make`/`make all` (it's a doc,
  not a build input) — a separate `make corelib-reference` target, run
  by hand after touching a core module's public API, the same "opt-in,
  not part of the default build" treatment `test-bignum`/`test-decimal`
  already get.
- **`json` — a parser and serializer, written in pure Ziman, and the
  first real test of a genuinely recursive type.** Motivated directly
  by wanting realistic HTTP-shaped request handling
  (`health-record-backend-ziman`'s own still-open "no JSON, no real
  request/response types" gap) — but before writing a line of it, the
  actual open question was whether Ziman could even *represent*
  arbitrary JSON at all: a `Json` value is a classic recursive sum type
  (an array or object can contain more `Json` values), and this
  project's own ROADMAP.md carried an old note that "a self-referential
  struct can't be constructed." Verified directly rather than assumed:
  `enum Json { JNull JBool(bool) JNumber(float) JString(string)
  JArray([Json]) JObject([JField]) }` (with `JField { key: string,
  value: Json }`, declared *before* `Json` even exists in the file)
  compiles and runs correctly today, including genuinely nested values.
  The old limitation turned out to be narrower than it sounded: it's
  about a *direct* self-reference with no escape (`struct Node { next:
  Node }`, which really is infinite and unbuildable), not recursion
  through an array, which has an actual finite base case (`[]`) the
  same way a linked list's `None` tail does. `JNumber` holds a `float`
  (matching how virtually every JSON implementation represents
  numbers, JavaScript's own included) rather than `decimal`/`bigint` —
  nothing needs exact precision for a JSON *number* specifically, since
  an ID that needs it is always transmitted as a JSON *string* instead.
  `Json` does **not** support `==`/`!=`: array equality requires a
  scalar element type, and `[Json]`'s element (`Json`, an enum) isn't
  one — a real, known gap, left alone since nothing needs to compare
  two `Json` trees yet. The module deliberately stops at the general
  parsing engine (`parse_json`, `json_to_string`, and one convenience
  lookup, `json_object_get`) rather than reaching further into
  schema-specific request parsing (`parse_patient_create_request`-
  shaped functions) — turning arbitrary JSON into one already-known
  struct is application logic that belongs in
  `health-record-backend-ziman` itself, not language infrastructure.
- **Global val — a module-wide constant, motivated by a real repeated-
  rebuild cost `number.zm` hit first.** `hex_digit_char`/`hex_digit_value`
  each rebuilt their own 16- (and 6-) element lookup table from scratch
  on *every call*, since Ziman had no way to declare a value once at
  module scope — the only alternative to a local `val` inside the
  function body. Small in absolute terms, but a real, avoidable cost that
  would only get worse the more a core module leans on a fixed table (an
  ASCII lookup, a unit-conversion factor, a default `NumberFormat`) — and
  a real gap for the North Star goal of an always-on backend, the same
  category of concern as the memory-reclamation entry above, just at
  much smaller scale here. `[pub] val NAME[: Type] = <expr>` at module
  top level closes it: evaluated once, in file order, before `main`
  runs, reusing a local `val`'s exact grammar and declared-vs-inferred
  type checking (implemented as a synthetic `STMT_VAL` fed through
  `check_stmt`/`exec_stmt` — see typecheck.c's check_global_vals /
  interpreter.c's init_global_vals — rather than a parallel
  reimplementation of that logic). No global `var`: Ziman's concurrency
  story is single-writer actors passing messages over a `Channel<T>`
  (see the Concurrency entry above), and a mutable global would be
  exactly the shared, lock-free mutable state that model exists to avoid
  — adding one would quietly reopen a question this project already
  closed, for no motivating need (every real case so far wanted a fixed
  table, never mutable shared state).

  The interesting design question wasn't the grammar (a straight lift of
  local `val`'s own), it was **how a bare reference resolves** given
  Ziman's existing "always qualify or import by name, never a bare name
  from nowhere" rule for functions/structs/enums (see the "Bundled core
  modules"/"Per-name imports" entries above) — but a global val lives in
  the *same* namespace as an ordinary local variable, unlike a function
  or struct name, which is syntactically unambiguous (call syntax,
  `Name { }` construction) and can never collide with a bare identifier.
  Two designs were on the table. One: give each module its own Env,
  storing that module's globals under their *bare* names, chained as the
  parent of every one of that module's own function-call scopes —
  ordinary lexical shadowing (a same-named local `val` inside a function)
  would then work for free, via the exact scope-walking `env_lookup`
  already does for nested blocks, with no rewriting needed for the
  same-module case at all. Rejected because a *named import*
  (`use mod.NAME`) still needs the imported binding reachable from a
  totally different module's Env, which means either copying it across
  Env objects at program-init time (real, fiddly ordering work: the
  target module's own globals have to exist first) or falling back to a
  qualified-name lookup anyway for that one case — two different
  resolution mechanisms for what's supposed to be one feature. Chosen
  instead: exactly one flat global Env, holding *every* global val from
  *every* module under its fully-qualified name — the same "always
  qualified, one flat namespace" treatment a function/struct/enum
  already gets (see loader.c's self_qualify_module) — with *every* bare
  reference (same-module or imported) rewritten to that qualified form
  by self_qualify_expr's `EXPR_IDENT` case, the one case that previously
  did nothing. One env, one rewrite rule, no special-casing for which
  kind of reference it was.

  The cost of that choice: the rewrite is a blind text substitution with
  no scope tracking, so it would be unsound if some unrelated *local*
  `val`/`var`/parameter/loop-binder in that module happened to share a
  global val's name — self-qualify has no notion of lexical scope
  anywhere else in this file either (it doesn't need one for functions/
  structs/enums, which can't collide with a variable name by
  construction), and teaching it real scope tracking just for this one
  case felt like a lot of new machinery for a feature meant to replace a
  handful of rebuilt-every-call literal tables. Instead: forbid it
  outright. `loader.c`'s `check_no_val_shadowing` walks every function in
  a module (params, and every `val`/`var`/for-binder in its body,
  recursively through nested blocks) and rejects reusing a global val's
  name — this module's own, or an imported one's local name — as any of
  those, anywhere in the module, not merely in the one scope that would
  actually collide. Coarser than real scope tracking would need, but
  simple, and the same non-scope-aware "collides with an existing
  declaration" shape every other collision check in this file already
  has. Net effect: unlike a local `val`, a global one can never be
  shadowed — caught once, at load time, as a clear "can't also be used
  as a local name" error, not a silent semantic difference between two
  same-named bindings.

  Deliberately not built: `mod.NAME` *dotted* bare access, the way a
  qualified function call or struct literal already works. The parser's
  existing `qualifier.name` lookahead only ever commits to a qualified
  form when what follows resolves unambiguously (a call's `(`, a struct
  literal's `{`, an enum access's second `.`) — a bare trailing name with
  none of those already, today, backs out and falls through to ordinary
  `.field` access on a local variable (see parser.c's parse_primary), so
  giving it a *second* meaning would reopen exactly the kind of
  local-variable-vs-cross-module-reference ambiguity the qualified-
  call/struct-literal forms were carefully built to avoid. A per-name
  import (`use mod.NAME`) already does the job with the same one-line
  cost as it does for a function or struct, so there was no forcing case
  to take on that ambiguity for values specifically.
- **Prelude — broadened from "native functions only" to carry ordinary
  Ziman declarations too, moving `Option<T>` out of `collections` and
  into it.** Motivated by a plain observation: `Option<T>` is needed
  far more widely than `HashMap`/`HashSet` are (any function that might
  not have an answer wants it — a lookup, a parse, a search — while a
  hash-based container is a narrower, opt-in data-structure choice), so
  requiring `use collections` (or a per-name import of it) just to
  return "maybe a value" was friction with no real payoff: nobody was
  ever going to want an app-specific `Option` the way they might want an
  app-specific `HashMap`. The prelude already existed for exactly this
  shape of problem — "needed everywhere, `use` would be pure ceremony" —
  just not yet for anything beyond native fn signatures.
  Mechanically, the smallest change that could plausibly work: give the
  prelude's merge step in `loader.c`'s `load_module_graph` the same
  struct/enum/global-val flattening the entry module's own declarations
  already get, not just functions — and put `Option<T>`'s real source
  (moved verbatim, enum plus `option_unwrap_or`) into what
  `prelude_source()` returns. Two real gaps surfaced doing this, both
  fixed rather than designed around:
  1. `parse_type()` always builds a provisional `TY_STRUCT` for a bare
     `TYPE_IDENT` (see ast.h's `Type` comment), reclassified to `TY_ENUM`
     by `resolve_type_refs`'s fallback — but that fallback only ever
     checked *the current module's own* enum list, which is exactly
     right for the entry module referencing its own bare enum, and
     exactly wrong for `option_unwrap_or`'s own signature referencing
     `Option<T>` inside `prelude.zm` itself (never resolved at all,
     since prelude.zm never runs through `self_qualify_module`, the
     pass that would otherwise have already reclassified it). Fixed by
     running `resolve_cross_refs` on the parsed prelude module too,
     before merging it — the exact same fallback already handles it
     correctly once given the chance to run.
  2. The same fallback then needed to work from *any* module, not just
     the entry one and the prelude itself — `collections.zm`'s own
     `hashmap_get` returns `Option<V>`, a bare reference to a type that
     lives in neither `collections.zm`'s own enum list nor the entry
     module's. Fixed by giving `Loader` a `prelude_mod` field, set once
     up front (before any dependency loads), and adding it as a third,
     final fallback in `resolve_type_refs` — checked after a module's
     own enums, so a module could in principle still shadow the name
     with one of its own (not that any should).
  Both gaps existed because every previous bare-name reference case
  (a function call, a struct literal, an enum access) is resolved by
  the *expression* walk, which doesn't have this provisional-type
  ambiguity at all — this was the first time a bare name needed
  resolving from a *type position*, from outside both the referencing
  module and the entry module. Verified by running the full test suite
  after each fix, not assumed correct from the diff alone — the second
  gap in particular only surfaced because `collections.zm` itself
  (a real dependency, not the entry module) has a bare `Option`
  reference in a type position, exactly the case nothing before this
  had ever exercised.
  Deliberately *not* made `use`-able under the name `prelude`, despite
  `lib/prelude.zm` sitting in the same directory as every module
  that is: `lib/core/%.zm` doubles as `corelib.c`'s `use`-fallback lookup
  table, and adding `prelude` to it would let `use prelude` parse the
  file a *second* time under a redundant qualifier (`prelude.Option`
  alongside the already-bare `Option`) — solved by getting its header
  generated through the same Makefile rule as everything else in
  `lib/core/` (so its source is still real, readable `.zm`, never a
  hand-escaped C string) while never adding a matching line to
  `corelib.c`'s `CORE_MODULES` table.
  Nine existing test fixtures had to be updated as a direct, foreseen
  consequence: they each declared their own throwaway
  `enum Option<T> { Some(T) None }` at entry-module scope purely to
  exercise generic-enum machinery in isolation, and a bare, unqualified
  entry-level declaration collides with the prelude's own bare `Option`
  the same way redeclaring `parse_int` at the top level already did —
  renamed to `Maybe<T>` in each, since the fixtures never actually
  depended on the name `Option` itself, just needed *some* generic enum.
- **`Box<T>` removed from `collections`, outright — not moved
  anywhere, unlike `Option<T>` above.** A grep across every real
  consumer (`lib/core/*.zm`, `health-record-backend-ziman`) turned up
  zero actual uses: `HashMap`/`HashSet` are built on their own private
  `Entry<K, V>`, never `Box`, and nothing in the dogfooding project ever
  referenced it either. It was the first generic struct ever written —
  a minimal `struct Box<T> { value: T }` used to prove the mechanism
  before `HashMap`/`Option` gave generics a real motivating payoff — and
  it never earned a real use afterward. Worth noting as its own small
  data point on module naming: a single-value wrapper was arguably
  never "a collection" in the first place, so removing it makes
  `collections` a *more* accurate name for what's left (`HashMap`,
  `HashSet`), not a less accurate one — considered, and rejected,
  renaming the module for this. `struct Box<T> { value: T }` remains
  perfectly valid Ziman for anyone to declare themselves; docs/SYNTAX.md
  and this file both still use it as the canonical minimal illustration
  of generic-struct syntax, same as before — the only thing that
  changed is that it's no longer a `collections` export.
- **Collections module — HashMap/HashSet given real load-factor growth,
  plus size/keys/values accessors, after "the hashmap implementation
  isn't great" named the fixed 16-bucket count as a real problem, not
  an acceptable simplification.** The original design (see the earlier
  "Collections module" entry above) documented the fixed bucket count
  as a known limitation and left it there; revisited once it was named
  directly, since an unbounded chain length means `hashmap_get`/`has`
  degrade to a linear scan past roughly 16 entries — a hash map that
  stops being O(1) once it's actually holding a meaningful amount of
  data isn't doing its one job. Fixed with the standard technique
  (double the bucket count and rehash every entry once the load factor
  — `count / bucket_count`, computed as `count * 4 > bucket_count * 3`
  to avoid a float cast for something this simple — crosses 0.75,
  matching Java's `HashMap` default), implemented by adding a `count`
  field to both structs and having `hashmap_set`/`hashset_add` check
  `should_grow` after each insert, rehashing via `hashmap_grow`/
  `hashset_grow` when it fires. Both grow functions re-insert every
  existing entry through the ordinary `hashmap_set`/`hashset_add` path
  rather than a separate bulk-insert routine — deliberately, since the
  freshly doubled bucket count keeps the grown map well under the load
  factor for its own new size, so none of those re-insertions can
  themselves trigger a further grow, and reusing the existing function
  means the grow path can't drift out of sync with ordinary insertion
  behavior.
  **This does not make `set`/`add`/`remove` O(1), and the module's own
  header comment says so plainly rather than overclaiming**: Ziman's
  arrays are immutable value types (see the Arrays entry above), so
  `set(m.buckets, idx, new_bucket)` always copies the *entire* outer
  bucket array, growing or not — every mutating call is O(bucket_count)
  for that reason alone, a cost this change doesn't remove. What
  changes is the shape of that cost: without growth, a long chain means
  copying a chain of full `Entry<K, V>` structs on every write to that
  one bucket, and that chain length is unbounded; with growth, chains
  stay short (bounded by the load factor) and the dominant per-call
  cost becomes copying `bucket_count` array *references* instead — a
  real reduction in the constant factor, not a change in asymptotic
  order. Actually fixing the ceiling would need mutable or shared array
  storage at the language level (see the memory-reclamation entry
  above) — out of scope for one module to solve on its own, and not
  attempted here.
  `hashmap_size`/`hashmap_keys`/`hashmap_values` (and their `HashSet`
  equivalents, `hashset_size`/`hashset_items`) were added alongside
  growth for an unrelated but adjacent reason: there was previously no
  way to enumerate or size a `HashMap`/`HashSet` at all without reaching
  into `buckets` directly, which the module's own comment already said
  not to do. Purely additive — every existing function keeps its exact
  signature, so this doesn't affect anything already using the module
  (there is no such consumer yet in `health-record-backend-ziman`, per
  a grep before starting this change).
  No shrink-on-remove: `hashmap_remove`/`hashset_remove` decrement
  `count` but never shrink the bucket array back down. A real gap for a
  map that grows large and then drains, but a narrower, lower-priority
  one than unbounded growth was, and left as a deliberate scope cut
  rather than designed around.
- **`time` — a compiled `DateTimeFormat` for formatting/parsing, plus a
  real gap closed: there was no way to parse a date string back into a
  `LocalDate` at all.** Motivated by a concrete, already-flagged gap:
  `health-record-backend-ziman`'s `request.zm` had its own hand-rolled
  `parse_iso_date` (split on `"-"`, three `parse_int` calls, then
  `localdate_of`), with a comment reading *"a general ISO-date parser
  could live in ziman's own `time` core module someday."* Every type
  already had a fixed `_to_string`; none had the inverse.
  Two related but separately-scoped features, not one: (1) an ISO
  `_from_string` for every type, the exact inverse of its existing
  `_to_string`, and (2) a general `_format`/`_parse_pattern` pair (on
  the three calendar-shaped types plus `Instant`) for a caller-chosen,
  non-ISO pattern — closer in shape to `number.zm`'s own
  `NumberFormat`/`parse_pattern`. Built (2) first, then implemented (1)
  *on top of it* — `localdate_to_string`/`localdate_from_string` etc. are
  thin wrappers calling `localdate_format`/`localdate_parse_pattern`
  with one of five `pub val iso_*_format` presets — one formatting/
  parsing engine per type, not two independently-maintained
  implementations of the same ISO shape. Verified this refactor was
  byte-for-byte behavior-preserving by running the full existing test
  suite (which checks several `_to_string` outputs for exact string
  equality) before writing a single new test.
  `_format`/`_parse_pattern` take a `DateTimeFormat`, not a raw pattern
  string — deliberately, after an explicit request that a pattern not
  be re-parsed on every call. The first version of this feature *did*
  take a bare pattern string, re-scanned character by character on
  every single `_format`/`_parse_pattern` call; `DateTimeFormat` is a
  *compiled representation* of a pattern instead (an array of `parts` —
  each either a known field or a coalesced run of literal characters —
  plus a precomputed `width`), built once by `datetime_format_of` and
  meant to be reused for as many calls as needed, the same way Java's
  own `DateTimeFormatter.ofPattern` compiles once and is reused. This
  is exactly what the `iso_*_format` global vals are for: each is
  compiled exactly once, at program start (global vals are evaluated
  eagerly before `main` runs — see the "Global val" entry above), and
  then reused for the process's entire lifetime — a concrete, motivating
  use case for global vals beyond a fixed literal table, and the reason
  `DateTimeFormat` is a struct wrapping a *compiled* array, not a
  one-field wrapper around the pattern string itself (a trivial wrapper
  would have to re-scan the string on every call regardless of being
  wrapped, so it wouldn't actually solve the stated problem).
  The pattern language itself is deliberately tiny and fixed-width-only
  in both directions — a token's compiled width always equals its own
  rendered length (`yyyy` → 4; `MM` → 2; and so on) — unlike
  `java.time.DateTimeFormatter`, where a letter's *repeat count*
  changes its meaning entirely (`M` is a bare numeric month, `MM` is
  zero-padded, `MMM`/`MMMM` are short/full month *names* — a completely
  different kind of feature, needing a name table and locale awareness
  this module has no reason to build). This fixed-width property isn't
  just simpler, it's *used*: `instant_parse_pattern` slices an instant
  string into its calendar part and its zone-offset suffix at exactly
  `fmt.width` characters, precomputed once at compile time rather than
  summed on every call, with no separator to search for. No escaping
  mechanism either — every literal character a real pattern needs (`-`,
  `:`, `.`, `T`) already falls outside the six recognized letters
  (`y`/`M`/`d`/`H`/`m`/`s`/`S`), so nothing needs escaping; a
  differently-repeated run of one of those letters (three `S`s, say)
  just doesn't match a token and is folded into the surrounding literal
  text instead — a documented scope cut in the same spirit as
  `number.zm`'s pattern subset, not a validated-against trap.
  `Duration`/`ZoneOffset`/`ZoneId` deliberately don't get the general
  pattern pair, only the ISO `_to_string`/`_from_string` one: `Duration`'s
  `"PT{seconds}[.{nanos}]S"` is a total-elapsed-time count, not a
  calendar field breakdown, and `ZoneOffset`/`ZoneId`'s shapes are each
  just one or two fixed forms — neither fits a year/month/day/hour/
  minute/second pattern language, the same way `Duration` never got a
  `DateTimeFormatter`-equivalent in java.time itself either.
  `duration_from_string` was the fiddliest of the new `_from_string` functions to
  get right: `duration_to_string`'s negative-duration case doesn't
  print the true seconds/nanos, it prints a *magnitude* transform
  (`mag_seconds = -true_seconds - 1`, `mag_nanos = 1e9 - true_nanos`,
  so that `-0.5s` prints the honest `"PT-0.500000000S"` instead of a
  misleading `"PT-1.5...S"`) — parsing has to invert that arithmetic
  exactly, not just negate the parsed magnitude back, verified against
  the `-0.5s` worked example directly before trusting it.
  `instant_from_string`/`instant_parse_pattern` accept either offset shape
  `zoneoffset_from_string` does (`"Z"` or a real `"+HH:MM"`/`"-HH:MM"`), not
  only the `"Z"` `instant_to_string` itself always produces — a string
  from a genuine external API (not just this module's own output round-
  tripping) may carry a real offset, and accepting it costs nothing
  extra given `zoneoffset_from_string` already exists.
  Applied immediately: `health-record-backend-ziman`'s `request.zm` had
  its hand-rolled `parse_iso_date` deleted outright in favor of the new
  `time.localdate_from_string` — the exact motivating gap, closed.
- **Variadic parameters — the first new call-site grammar since generics,
  built for one concrete native function (`join`), not speculatively.**
  Motivated by a scan of `lib/core/*.zm` for O(N²) allocation patterns:
  several functions (`json_to_string`, `escape_json_string`,
  `parse_string`, `hashmap_keys`/`hashmap_values`/`hashset_items`) build
  a result by repeatedly calling `concat`/`push` in a loop, each call
  copying the *entire* accumulated result so far — O(N²) work to produce
  an O(N) result, an unavoidable consequence of Ziman's immutable
  arrays/strings once written that way. Considered making `concat`
  itself variadic first; rejected once it became clear that wouldn't
  actually help the motivating case: a variadic *call* still requires
  listing each argument individually in source, and `json_to_string`'s
  loop has a runtime-sized `[Json]` array, not a fixed, source-visible
  count. What was actually needed was a way to hand over an existing
  array and get single-pass, native buffer-building — `join(pieces:
  [string]) -> string` alone would have solved that with zero language
  changes. Variadic parameters were still worth building on top of that,
  for a narrower, genuinely ergonomic reason: they let `join("Hello, ",
  name, "!")` read naturally for the common case (a handful of pieces
  known at the call site) without forcing an array literal, while the
  array form still works directly for the runtime-sized case that
  started this — one function serves both, not two.
  **Syntax: `pieces: string...`, suffixed onto the type, not a new
  keyword or a prefix marker.** Considered against how other languages
  spell this (Java/Swift suffix the type: `String... args`/`args:
  String...`; Go suffixes the type but before the name: `nums ...int`;
  Python/JS prefix the name: `*args`/`...args`; C#/Kotlin use a keyword:
  `params string[] args`/`vararg args: String`) — no two of them agree,
  so there's no external convention to defer to, only Ziman's own
  already-fixed `name: type` order to disturb as little as possible.
  Reasoned explicitly from an AI-generation angle here, not just a
  human-readability one, per this project's own stated priority: a new
  keyword (`vararg`, `params`, `variadic`) invites a subtly-wrong
  synonym a model could plausibly generate instead (`varargs`,
  `repeated`); `...` has no synonym to drift toward and is either
  present or a syntax error, nothing in between — less surface area for
  a generator to get quietly wrong. Lexed as the existing `TOK_DOTDOT`
  (a for-loop range's `..`) immediately followed by a `TOK_DOT`, not a
  new token — the two can never be confused, since a range's `..` never
  appears in a parameter list.
  **A variadic parameter's declared type is the *element* type, not the
  array type** (`nums: int...`, never `nums: [int]...`) — inside the
  function it behaves as an ordinary `[int]` (bound that way in
  check_function/call_function), but the signature shows a reader what
  a *caller* writes, which is individual `int`s, not an array.
  **A single already-built array works as the whole variadic tail, not
  just individually-listed elements** — `sum(nums)` where `nums: [int]`
  passes it through directly rather than requiring `nums` to be
  unpacked back into individual arguments first. This was flagged
  initially as a likely source of Java-style varargs/array ambiguity,
  then reconsidered and accepted: Java's ambiguity is specifically about
  *overload resolution* choosing between competing method signatures
  when a single array argument could match more than one of them —
  Ziman has no overloading at all, so there is exactly one declared
  `sum`, and whether a trailing argument means "the whole array" or "one
  more element" is always decidable from that argument's own static
  type alone (`[int]` vs `int` can never be confused, given no implicit
  coercion exists either). No ambiguity survives in a language with
  neither overloading nor coercion, even though the same-shaped call
  genuinely is ambiguous in one that has both.
  **`join(pieces: string...) -> string`** is the concrete payoff,
  implemented natively (`src/prelude.c`'s `native_join`): two linear
  passes over the collected array (sum lengths, then copy into one
  `malloc`'d buffer) — genuinely O(N), the fix the motivating scan was
  looking for, not just a relocation of the same O(N²) pattern into a
  variadic-shaped wrapper. A pure-Ziman `join` built on top of the new
  variadic syntax would have reintroduced the exact problem one level
  down (accumulating via `concat` in a loop over the collected array),
  so being able to write it natively was the actual point, not the
  variadic syntax on its own.
  Mechanically, the collection from "however many arguments a call site
  wrote" down to "exactly one `Value` per declared parameter" happens
  once, in `interpreter.c`'s `collapse_variadic_args`, called at the top
  of `call_function` before either the native-dispatch or the ordinary
  env-binding path — so neither path, nor any future one, needs its own
  copy of this logic, and a variadic function's own implementation
  (native or not) never has any way to tell a spread call from an
  individually-listed one; it only ever sees one array.
  This is stage one of a larger, still-in-progress design conversation
  (named arguments at call sites, `Option<T>` parameter omission with
  defaults, `Result<T, E>` bare-value auto-wrapping) — deliberately
  built and landed on its own first, since it's fully self-contained and
  has no dependency on the rest.
- **Named arguments at call sites — stage two of the conversation
  above, reusing `name: value` rather than a second syntax for the same
  idea.** Motivated by a real, already-present bug class: a call like
  `localtime_of(hour, minute, second, nano)` has four same-typed `int`
  parameters, so a transposed pair of arguments is a silent, no-type-
  error mistake — at least as easy for an LLM to make as a human, maybe
  easier, since nothing about the types stops it. Reused the exact
  `name: value` shape a struct literal's fields already have
  (`Point { x: 1, y: 2 }`) instead of inventing a second way to write
  the same "this value goes with this name" idea — one shape for an AI
  to remember, not two. **A call is entirely positional or entirely
  named, never a mix** (`f(1, y: 2)` is a syntax error) — decided once,
  by whether the *first* argument is followed by `:`; every remaining
  argument in that call is then required to match. **The label must
  match the callee's declared parameter name exactly** — no separate
  external label the way Swift lets a call site and a declaration use
  different names for the same parameter; whatever the signature calls
  it is the only name a caller may use. **Named arguments may appear in
  any order** — that's the actual payoff for the motivating bug: even
  reordered, `localtime_of(nano: n, second: s, minute: m, hour: h)`
  still reaches the right parameters, because the label, not position,
  decides. **Every parameter must still be supplied exactly once** — a
  named call is not, on its own, a way to omit an argument (that's
  stage three, `Option<T>` parameter omission, layered on top of this
  one, since omission is only ever going to be legal in a named call);
  wrong count, an unknown name, or the same name given twice are all
  compile-time errors. **Restricted to real (`fn`/`native fn`)
  functions, not the hardcoded core builtins** (`print`, `len`, `push`,
  `hash`, ...) — each of those has exactly one fixed, already-special-
  cased signature with no `FunctionDecl`/`ParamList` behind it for a
  named call to resolve against, so a named call to one of them is
  rejected outright rather than silently accepted as if it were
  positional.
  Mechanically, a named call is canonicalized to positional order
  exactly once, in `typecheck.c`'s `canonicalize_named_args` — called
  right after the callee's `FunctionDecl` is resolved, before any of
  the existing (unmodified) fixed/variadic argument-count and type
  logic runs. It validates the name set against `fn->params`, reorders
  `Expr *` pointers into declared-parameter order in place, and clears
  the call's name array back to `NULL`. Past that point — including
  everywhere in `interpreter.c` — a canonicalized-named call is
  byte-for-byte indistinguishable from one that was always positional;
  no downstream code needed to change at all, the same "resolve once,
  simplify forever after" pattern already used for global val's
  self-qualifying rewrite and for `resolve_type_refs`'s prelude
  fallback.
  A variadic parameter's named value is still exactly one expression —
  there's no way to write several individually-labeled elements under
  one name in a named call — so it's treated as the existing
  single-trailing-argument case variadic calls already have: either the
  element type or the whole array type, decided the same way `sum(nums)`
  already is.
- **`Option<T>` parameter omission and defaults — stage three, and the
  first of the four stages that lets a call leave an argument out.**
  Legal only for an `Option<T>`-typed parameter, and only in a named
  call — a positional call stays exactly as strict as it always was
  (every parameter, in order, no exceptions), since there's no label to
  hang the omission on otherwise, and a required parameter never gets
  an "absent" state to fall back to no matter how it's called.
  **An omitted parameter with no declared default becomes `None`; a
  declared `= expr` default is used instead when there is one**
  (`priority: Option<int> = 3`) — explicitly *not* `= Option<int>.None`
  for the no-override case, since that's already what an omitted
  parameter becomes with no `=` written at all; spelling it out would
  just be a longer way to say nothing new.
  **The default is evaluated exactly once, before `main` runs — sugar
  for a hidden global val, reusing that timing rather than its actual
  machinery.** Once-vs-per-omitted-call is behaviorally identical here
  specifically because Ziman's values are immutable (there's no
  Python-style "mutable default" trap where a single shared instance
  could accumulate state across calls), so once is strictly cheaper
  with nothing given up; like a global val's own initializer, a default
  expression can't reference this function's other parameters, only
  whatever's already in scope at program start.
  **Wherever an `Option<T>` value is expected — a supplied argument or
  a default expression — a bare `T` is legal too, auto-wrapped to
  `Some(T)`.** Modeled directly on Rust's own `impl<T> From<T> for
  Option<T>` / `Into<Option<T>>`-bounded parameters: despite being at
  least as strict about implicit coercion as Ziman is everywhere else,
  Rust carves out exactly this one exception, because wrapping a
  single, statically-known type is never lossy or ambiguous the way
  numeric coercion (`int` silently becoming `float`, say) would be —
  the same reasoning applies here unchanged. An already-`Option<T>`
  value keeps working too, so nothing that used to type-check stops.
  Mechanically, this reuses two patterns already established by the
  first two stages rather than adding new ones. First, the
  "resolve once, rewrite the AST, the interpreter never finds out"
  pattern named arguments already use: a bare `T` argument or default
  is rewritten in place, during type-checking, into a synthetic
  `Option<T>.Some(...)` node (`wrap_bare_in_some`) that reuses ordinary
  enum construction (`EXPR_ENUM_ACCESS`) wholesale — `eval_enum_access`
  in `interpreter.c` needed zero changes to evaluate it exactly like a
  `Some(...)` a person wrote by hand, and the same trick builds the
  synthetic `None` an omitted-with-no-default parameter gets
  (`make_none_expr`). Second, a genuinely new problem needed a genuinely
  new (but still small) piece: a default has to be evaluated once, at
  program start, then handed to however many later calls omit that
  parameter, which means smuggling an already-computed runtime `Value`
  into the AST somehow, without giving `ast.h` (which `value.h` itself
  depends on) any way to know what a `Value` even is. Solved by giving
  each `Param` with a default a synthesized, globally-unique,
  unforgeable key (`default_key` — unforgeable because it contains `$`,
  a character no real Ziman identifier can ever contain), evaluating
  its default into `g_global_env` under that key exactly once, in a new
  `init_param_defaults` (interpreter.c, run right after
  `init_global_vals`, before `main`) — and then, when a call omits that
  parameter, building a synthetic `EXPR_IDENT` node referencing the same
  key (`make_default_ref_expr`) rather than inventing a new "embedded
  value" AST node kind. Ordinary bare-name lookup (`env_lookup`'s
  parent-chain walk, already used by every real global val reference)
  does the rest with no interpreter changes at all.
  One sequencing wrinkle the first version of this got wrong: a
  synthetic node is built already fully resolved (`resolved_type` set
  directly, at construction), specifically so nothing re-typechecks it
  later, since a default-reference node's key isn't even defined in
  `g_global_env` until *interpretation* starts — re-running `check_expr`
  on it during type-checking would look it up in the wrong environment
  entirely and fail. Fixed by having the shared argument-checking
  helper (`check_value_against_param_type`) treat a non-`TY_VOID`
  `resolved_type` at the time it *first* sees an argument slot as proof
  it's looking at one of these synthetic nodes already-resolved by
  construction (an ordinary freshly-parsed argument is always `TY_VOID`
  until `check_expr` fills it in — see `ast.h`'s `Expr.resolved_type`
  comment) — reusing an existing convention as an implicit "already
  checked" marker rather than adding an explicit flag for it.
  A generic function's `Option<T>` parameter can't have a default when
  its `T` is that function's own still-unbound type parameter (checked
  via the element type's `TY_TYPEVAR` kind) — a default is evaluated
  once, at program start, long before any call supplies a concrete type
  argument, so there's no concrete type yet to evaluate the default
  against. A variadic parameter can't have a default at all (rejected
  at parse time) — the two features don't combine into anything
  meaningful, and "omitted" already has an existing, unrelated meaning
  for a variadic parameter (zero trailing arguments, legal today,
  nothing to do with `Option<T>`).
  `Result<T, E>` bare-value auto-wrapping is the fourth and last stage
  of this conversation, still to come.
- **`join` grew `prefix`/`suffix`/`separator`, its first real caller
  outside the language's own test suite.** The original single-purpose
  `join(pieces: string...) -> string` (see "Variadic parameters" above)
  only ever concatenated its pieces directly -- no way to put a
  separator between them or wrap the result, short of a second
  `concat` pass around the result, which reintroduces exactly the extra
  allocation `join` existing natively was meant to avoid. Extending the
  signature to `join(prefix: Option<string>, suffix: Option<string>,
  separator: Option<string>, pieces: string...) -> string` folds all
  three into the same single native pass instead. Each is `Option<string>`
  specifically so the common case -- none of them -- costs nothing extra
  at the call site: `join(pieces: arr)` leaves all three out, each
  falling back to its own do-nothing default (`None`, meaning "don't
  apply this one"), the exact behavior plain `join` always had. This is
  possible with zero changes to the call resolution machinery: named
  arguments and Option<T> parameter omission (both above) already
  handle it, and native functions were never a special case for either
  -- `check_function`'s param-default validation and
  `canonicalize_named_args`'s omission-filling run identically whether
  `fn->is_native` or not, since neither one touches (or needs to touch)
  a function's body.
  `native_join` (`src/prelude.c`) unwraps each `Option<string>` argument
  itself, via a small `option_string_or_null` helper reading the
  argument's own `EnumObj.variant_name` at runtime -- by the time a
  native function ever sees one of these arguments, typecheck has
  already resolved it to a genuine `Option<string>` (auto-wrapping a
  bare string into `Some(...)` if that's what was written), so there is
  nothing left to validate, only to branch on. Still one allocation,
  two linear passes overall: the first sums `prefix`/`suffix`/each
  piece's length plus `separator`'s length times one fewer than the
  piece count, the second copies everything (prefix, then each piece
  with a separator between consecutive ones, then suffix) into that one
  buffer -- genuinely O(N) regardless of how many of the three optional
  parameters were actually given.
  One call site broke from the signature change and needed fixing:
  `join(pieces)`, a plain single positional argument valid under the
  old 1-parameter signature, is no longer legal on its own now that
  `pieces` is the fourth parameter -- becomes `join(pieces: pieces)`, a
  named call, which is also the form every other real call site outside
  this codebase's own tests will need to use for the simple case, now
  that `join` takes more than one parameter.
- **`StringBuilder` -- a general, write-once fix for the whole class of
  problem `join`'s own motivation (see "Variadic parameters" above) was
  only ever a partial answer to.** Prompted directly by
  `uuid_to_string`: still a concat-in-a-loop (~20 calls per UUID, each
  abandoning its own intermediate string -- see the memory-reclamation
  entry on why that never gets reclaimed), a real, repeated cost given
  `Uuid` is `Patient`'s own id in `health-record-backend-ziman`. Fixing
  that one call site natively, the way `join` itself was fixed, would
  have worked -- but would need repeating, by hand, in native C, for
  every future accumulate-via-concat/push function this codebase (or
  any Ziman program) ever writes, including ones that don't exist yet.
  `StringBuilder` is the fix that only needs writing once: a native,
  mutable, amortized-growth buffer, exposed as ordinary Ziman so every
  future accumulator becomes a plain loop appending to one shared
  builder, with no new native code required no matter how many more of
  these show up later.
  **A plain struct wrapping an opaque `handle: int`, not a new built-in
  type.** `Channel<T>` is Ziman's only other mutable, identity-based
  value, and it needed to become a genuine new `TypeKind` (its own
  runtime `Obj`, wired into `type_equals`, the interpreter's `Value`
  union, thread-safe queue semantics) specifically because it's
  *generic* -- `Channel<T>` for arbitrary `T`, with `send`/`receive`
  needing to type-check against that `T`. `StringBuilder` never holds
  anything but bytes destined to become one `string`, no type parameter
  involved, so none of that integration is needed: `handle` indexes into
  a native-side table of real, growable C buffers (`src/prelude.c`),
  invisible to Ziman code, which just sees an ordinary, if unusually
  cheap-to-copy, struct. Same accepted tradeoff `Uuid.bytes` already
  has: no per-field privacy exists in Ziman to actually stop a caller
  from reading or fabricating a `handle` -- documented as "don't,"
  not enforced.
  **API, decided across a short back-and-forth**: `sb_new(capacity:
  Option<int> = 20, grow_factor: Option<float> = 2.0) -> StringBuilder`
  -- both `Option<T>` specifically so the common case, `sb_new()`, tunes
  neither; `sb_append(sb: StringBuilder, pieces: string...)` -- void,
  not `-> StringBuilder`, since `sb` mutates in place (the same handle,
  never a new one) and Ziman has no `.method()` chaining syntax for
  returning it to enable anyway, mirroring `send`'s own void return on
  `Channel<T>`; `sb_to_string(sb: StringBuilder) -> string` -- a
  snapshot, safe to call more than once and to keep appending
  afterward, and genuinely necessary rather than relying on `str(sb)`,
  since Ziman has no per-type custom-formatting hook (`str()` on a
  struct just prints its literal fields, `StringBuilder { handle: N }`,
  which would be actively wrong here); `sb_len(sb: StringBuilder) ->
  int` for reading the accumulated length without materializing the
  string. The variadic `pieces: string...` on `sb_append` reuses the
  exact shape `join` itself already has, for the same ergonomic reason.
  Rejected "grow the buffer preemptively, HashMap-style, once it crosses
  a *load factor*" in favor of an ordinary growth *factor* (multiply
  capacity when actually full, standard C++ `std::vector`/Java
  `ArrayList` doubling behavior) -- a load factor is about avoiding long
  hash-collision chains, which a plain byte buffer has no equivalent of;
  there's nothing here to trigger early on.
  **Native implementation** (`src/prelude.c`): `g_sb_table`, an array of
  *pointers* to individually heap-allocated `StringBuilderBuf` objects
  (never an array of the structs themselves) -- so growing the table
  (`realloc`-ing the array of pointers) never invalidates a buffer some
  other code already holds a pointer to, only the pointers move, never
  the buffers. `g_sb_table_mutex` guards every read or write of the
  table itself (registering a new handle, or looking one up), since
  reading a pointer variable another thread might concurrently be
  reallocating is undefined behavior in C regardless of whether the
  bytes involved would turn out identical. Appending to an already-
  fetched `StringBuilderBuf` happens outside that lock, entirely
  unsynchronized between callers -- matching Java's own (non-thread-
  safe) `StringBuilder`: meant to be built up by one thread at a time,
  not shared, not a design gap specific to this implementation. (That
  was made a rule, not a convention, in 2026-09: a builder records the
  thread that made it, every native operation checks it, and the
  compiler refuses the obvious ways of sharing one -- `#[thread_bound]`
  in prelude.zm, see SYNTAX.md "Attributes and derive".)
  `option_int_or`/`option_float_or` unwrap `capacity`/`grow_factor`
  (already-resolved `Option<T>` values by the time a native function
  ever sees them, per `join`'s own precedent) -- deliberately checking
  `variant_name` first rather than ever reading a `None`'s payload
  field directly, since `eval_enum_access` happens to store a harmless
  `value_int(0)` there for every no-payload variant construction
  regardless of the variant's *declared* payload type -- reading that
  as a `float` for a `None` `Option<float>` would silently reinterpret
  the wrong union member. `uuid_to_string` was rewritten on top of this
  the same day, closing the loop on the function that started the
  conversation -- one shared buffer and one final snapshot copy in place
  of ~20 leaked intermediate strings per call.
  **One real, pre-existing bug found and fixed along the way**, in
  `loader.c`'s `resolve_type_refs`: a bare struct-type reference that
  isn't also a same-module enum falls through to check whether it's
  actually a *prelude* enum instead (the same disambiguation `Option<T>`
  needs from every other module) -- but while the prelude module is
  resolving *its own* cross-references, `ldr->prelude_mod` is still
  `NULL` (load_module_graph assigns it only after that call returns),
  so this fallback crashed instead of correctly concluding "not an enum
  after all, must be a real struct" the instant the prelude itself
  contained a genuine bare struct reference for the first time --
  `StringBuilder` was that first case (`Uuid` is a struct too, but lives
  in a separate module reached through `use`, never bare inside the
  prelude). Fixed with a `NULL` guard; a completely inert one while
  resolving any *other* module, where `prelude_mod` is always already
  set by the time this runs.
  **A second, independent gap found the same way**: `sb_new()` --
  literally empty parentheses -- didn't work at all before this,
  because omission-detection was keyed off seeing at least one `name:`-
  prefixed argument, and a call with *zero* arguments has none to key
  off regardless of whether it was meant as "positional, zero given" or
  "named, zero given, everything omitted." `sb_new` was the first
  function ever declared where literally every parameter is `Option<T>`
  and the common call omits all of them, exposing that this case had no
  legal syntax at all. Fixed by treating a syntactically-positional,
  genuinely empty call (`argc == 0`) as an all-omitted named call
  instead, in `canonicalize_named_args` -- sound because there is
  nothing an empty argument list could mean *other* than "give me
  everything the declaration already allows me to leave out," and inert
  for a function that takes no parameters at all (the fill loop never
  runs) or has any non-`Option<T>` parameter (still correctly rejected,
  now with the same specific "missing required argument" message a
  partially-omitted named call already gives, an improvement over the
  old generic count-mismatch wording for this case). A *partial*
  positional call (`f(1)` when `f` needs two) is untouched by this and
  stays exactly as strict as it always has been.
- **Generic native fns — a narrow, pre-existing gap closed to unblock
  `ArrayBuilder` (below), not a project of its own.** `native fn
  foo<T>(...)` simply didn't parse before this: `parse_native_function`
  never grew the `<TypeParams>` handling `parse_function` (an ordinary
  `fn`) already had, an arbitrary asymmetry rather than a deliberate
  design choice -- confirmed directly (`native fn identity_native<T>(x:
  T) -> T` failed with a plain syntax error) before assuming it needed
  new machinery at all. Fixed by mirroring `parse_function`'s existing
  `<T>` handling in `parse_native_function` verbatim, minus the
  body-specific `reclassify_type_params_in_stmt` step (a native fn has
  no body to walk). Once the parser accepted the syntax, everything else
  already worked with zero further changes: `check_call`'s generic-
  instantiation logic (explicit type arguments required at every call,
  substitution, per-argument type checking) never special-cased
  `is_native` anywhere -- only `check_function`'s *body*-checking skips
  native fns, which was never in the way here -- verified directly with
  a throwaway `identity_native<T>(x: T) -> T`: missing type arguments,
  a wrong type-argument count, and a wrong argument type each produced
  the exact same errors a generic ordinary `fn` already gives, and a
  fully well-typed call correctly reached native dispatch, only failing
  on "no registered implementation" (expected, since nothing was ever
  registered for a throwaway test function). A generic native fn's own
  C implementation never needs to know which concrete type a given call
  instantiated it with to do its job, for the same reason ordinary
  generic structs/enums need no monomorphization: `Value` is already
  runtime-type-erased regardless of Ziman's static types.
- **`ArrayBuilder<T>` -- StringBuilder's generic counterpart, fixing the
  identical problem one level up.** `push` copies the *entire* existing
  array on every call, for exactly the reason `concat` copies the
  entire existing string: Ziman's arrays are immutable value types.
  `hashmap_keys`/`hashmap_values`/`hashset_items` (`lib/core/
  collections.zm`) were still doing this -- the array-shaped half of the
  original corelib memory-leak scan that motivated `join`/`StringBuilder`
  in the first place, never itself fixed. Same shape as `StringBuilder`
  throughout: `struct ArrayBuilder<T> { handle: int }`, not a new
  `TypeKind` (still no thread-safe queue semantics needed, `Channel<T>`'s
  own reason for existing as one); `ab_new<T>(capacity: Option<int> =
  20, grow_factor: Option<float> = 2.0)`, `ab_append<T>(ab:
  ArrayBuilder<T>, pieces: T...)` (void, mutates in place),
  `ab_to_array<T>(ab: ArrayBuilder<T>) -> [T]` (a repeatable snapshot),
  `ab_len<T>(ab: ArrayBuilder<T>) -> int`.
  Unlike `StringBuilder`, this genuinely needed `T` as a real type
  parameter -- a plain buffer only ever holds bytes destined to become
  one `string`, but a builder for arbitrary array contents has to be
  generic over what it holds, both so `ab_append`'s own element type is
  checked at all and so two different `ArrayBuilder<K>`/`ArrayBuilder<V>`
  values (as `hashmap_keys`/`hashmap_values` both need, from the same
  `HashMap<K, V>`) don't collide as the same type. That's what the
  "Generic native fns" entry above exists to unblock: this is its first
  real use.
  **The native buffer itself needed no per-`T` dispatch at all** --
  `src/prelude.c`'s `ArrayBuilderBuf` is just `Value *items` (identical
  table/handle machinery to `StringBuilderBuf`, just `Value`-sized slots
  instead of byte-sized ones), since an ordinary Ziman `ArrayObj` is
  already exactly this: a generic, type-erased buffer of `Value`s. A
  `Value` copies by plain assignment regardless of what type it holds,
  so `native_ab_append`/`native_ab_to_array` never need to know `T`'s
  identity to do their own job correctly.
  **One thing they *can't* get right without help: the concrete `T` on
  their own *result*'s `.type` field.** `ab_new<T>() -> ArrayBuilder<T>`
  and `ab_to_array<T>(ab) -> [T]` both return a value whose static type
  depends on `T`, but `NativeFn`'s signature (`Value *args, int argc,
  int line, int col`) carries no type-argument information at all --
  there's no argument of type `T` a native `ab_new<T>()` could even
  read `T` back off of. Solved without touching that signature (and
  without touching any of the ~20 existing, non-generic native
  functions that would otherwise all need an unused parameter added):
  `eval_expr`'s `EXPR_CALL` case now overwrites a generic native call's
  result `.type` with `e->resolved_type` immediately after the native
  function returns, the same way `channel_new()`'s own result already
  gets its type from `e->resolved_type` rather than anything a builtin
  could know in isolation -- typecheck already computed the fully
  substituted, concrete return type for this exact call, so trusting
  that over whatever placeholder the native function built internally
  is both correct and free. Scoped to `fn->is_native && fn->type_params.count
  > 0` specifically, so it's a genuine no-op for every native function
  that existed before this (a non-generic native's own hand-built
  `.type` and `e->resolved_type` already agree by construction).
  Verified this actually mattered, not just in theory: `for x in arr`
  (`STMT_FOR` in `interpreter.c`) reads `iterable.type.elem` at runtime
  to bind the loop variable's type, so an `ArrayBuilder<int>`'s
  `ab_to_array<int>(ab)` result needed a *correctly* `int`-tagged
  `.type.elem`, not just a plausible-looking placeholder, for iterating
  the resulting array to work at all -- confirmed directly by summing
  over one before trusting the fix.
  `hashmap_keys`/`hashmap_values`/`hashset_items` were rewritten on top
  of this the same day, closing the loop on the other half of the
  original memory-leak scan (`uuid_to_string`, the string half, was
  already closed by `StringBuilder` itself).
- **Two real interpreter bugs, both found by using `StringBuilder`/
  `ArrayBuilder` for real across every corelib module, not by
  inspection.** Neither was specific to any one call site -- both are
  now permanent regression fixtures, not just fixed inline.
  1. *`init_global_vals` ran before `init_param_defaults`, and a global
     val's own initializer calling a function that relies on one of its
     own omitted, defaulted parameters crashed (a `NULL` `env_lookup`,
     dereferenced).* Surfaced by `time.zm`'s `pub val iso_date_format =
     datetime_format_of("yyyy-MM-dd")`, once `datetime_format_of` itself
     started calling `ab_new<T>()` with both parameters omitted --
     `ab_new`'s default-value keys aren't defined in `g_global_env`
     until `init_param_defaults` runs, and that hadn't happened yet.
     Fixed by swapping the order in `interpret()` (moving `g_global_env
     = env_new(NULL)` out of `init_global_vals` and into `interpret`
     itself, once, since both functions now add to the same env rather
     than one of them creating it). The reverse dependency (a parameter
     default's own expression referencing an actual global val) is
     unaddressed either way and hasn't been hit by anything real in this
     codebase -- every existing default is a self-contained literal.
     A genuine cycle between the two would need evaluating both lazily,
     on first reference, rather than two fixed eager passes; not
     attempted, since nothing has needed it yet.
  2. *A variadic parameter whose element type is itself an array type
     was misinterpreted at runtime.* `collapse_variadic_args`'s own
     disambiguation for the one genuinely ambiguous shape (exactly one
     trailing argument) used to be `args[fixed_count].type.kind ==
     TY_ARRAY` -- correct only because, until now, nothing had ever
     combined a variadic parameter with an element type that's itself an
     array. `ArrayBuilder<[T]>` (exactly what `collections.zm`'s
     `new_buckets_of_size` builds: `ab_append<[T]>(buckets,
     empty_bucket)`, one `[T]` element per call) broke this immediately
     -- a single element of type `[T]` also satisfies "is an array", so
     the heuristic silently treated one legitimate element as if it were
     the *entire* tail, discarding whatever else had been appended in
     the same call. The real, unambiguous answer was already being
     computed correctly, just one layer up and then thrown away:
     `check_call` (typecheck.c) already decides this definitively, from
     the trailing argument's own static type, with no ambiguity possible
     (an element type can never equal its own array-wrapped form).
     Fixed by recording that decision directly on the call expression
     (`Expr.as.call.variadic_tail_is_array_spread`, ast.h) instead of
     re-deriving a weaker approximation of it at runtime -- the same
     "resolve once during type-checking, the interpreter just trusts it"
     pattern used throughout this session's other features. Threaded
     through `call_function`/`collapse_variadic_args` as an explicit
     parameter rather than inferred; `spawn`/`main`'s own call sites,
     which can never reach a variadic call in the first place (spawn's
     own typecheck path requires an exact positional argument count, no
     collapsing at all), just pass `0`.
- **A full corelib sweep for the `concat`/`push`-in-a-loop pattern,
  after `StringBuilder`/`ArrayBuilder` existed to fix it generally.**
  Went through every bundled module looking for genuine, unbounded
  accumulation, not just any `concat`/`push` call -- most are already
  fine (a small, fixed number of calls, never inside a loop whose
  iteration count scales with real input, the same reasoning `uuid_new`
  itself was left alone under). Converted where the loop count is
  genuinely unbounded in practice:
  - `json.zm`: `parse_string` (`StringBuilder`, char-by-char),
    `parse_array`/`parse_object` (`ArrayBuilder`, element/field-by-
    field), `escape_json_string` and `json_to_string`
    (`StringBuilder`) -- these three were the exact functions the
    *original* corelib memory-leak scan named as the string-shaped
    offenders, alongside `hashmap_keys`/`hashmap_values`/`hashset_items`
    (already fixed) as the array-shaped ones; never actually applied
    back to `json.zm` itself until now.
  - `number.zm`: `bigint_magnitude_to_hex` (arbitrary-precision bigints
    can have arbitrarily many hex digits), `insert_thousands`
    (arbitrary-length grouped numbers), `adjust_decimal_places`. Two
    of these originally *prepended* onto a string in a loop
    (`concat(new_char, existing)`) -- neither builder supports
    prepending, only appending, so both were restructured to produce
    their output in the correct left-to-right order directly:
    `bigint_magnitude_to_hex` collects digits into an
    `ArrayBuilder<string>` in the (reversed) order repeated division
    naturally produces them, then reads that array back out highest-
    index-first into a `StringBuilder`; `insert_thousands` changed
    which *direction* it iterates the digit string (right-to-left,
    prepending → left-to-right, appending), inserting a separator
    before digit `i` exactly when `(n - i) % 3 == 0` -- the same
    grouping boundary, restated so it can be computed while moving
    forward instead of backward. `strip_all` needed no builder at
    all: removing every occurrence of a substring is exactly what
    `join(pieces: split(s, needle))` already means, a native O(N) pass
    in one line, replacing what used to be `concat` in a loop over
    `split`'s own pieces.
  - `time.zm`: `datetime_format_of`'s `parts` array (`ArrayBuilder`,
    an arbitrarily long/verbose pattern), `format_fields` (`StringBuilder`,
    runs once per *formatted value*, not once per pattern like
    `datetime_format_of` -- the hotter of the two). `pad_left` and
    `datetime_format_of`'s own `literal_buf` (the handful of literal
    characters between two tokens in a pattern) were deliberately left
    as plain `concat` -- both are bounded to a small, fixed size
    regardless of any real input, nothing to gain from a builder.
  - `collections.zm`: `new_buckets_of_size` (`ArrayBuilder`, called on
    every grow, `bucket_count` doubles each time and has no bound).
    `hashmap_set`/`hashmap_remove`/`hashset_remove`'s own `push` loops
    were deliberately left alone -- each is bounded by one bucket's
    chain length, kept short by design (the same load-factor growth
    `new_buckets_of_size` is part of), not a genuine unbounded
    accumulation the way `bucket_count` itself is.
  - `clock.zm`/`io.zm`/`random.zm`: single native-fn declarations, no
    Ziman-level logic to sweep at all.
  Two real bugs (above) were found and fixed purely as a byproduct of
  doing this sweep for real rather than only reasoning about it --
  neither would have been caught by inspection alone, since both only
  manifest at runtime, under specific combinations (a global val's
  initializer reaching a defaulted parameter; a variadic element type
  that's itself an array) nothing had previously exercised.
- **Using function values: `datasource.with_transaction` and `server.serve` (roadmap
  R13, 2026-09-24).** Stage 1 deliberately has no capture, so a callback cannot
  reach a value the caller has. The answer is an explicit **context parameter**
  threaded through a generic function, and it turned out to be enough for both of
  the things that had been out of reach. `datasource.with_transaction<R>(conn, work)`
  and `with_transaction_ctx<C, R>(conn, ctx, work)` do the whole acquire /
  begin / `defer rollback` / commit / release dance -- the exact sequence that
  had to be written out (and could be got wrong: a `?` leaving a transaction
  open) at every call site -- and roll back on an `Err` from `work` or a failed
  commit. `server.serve<C>(listener, ctx, handler, options)` owns the accept loop,
  thread per connection, the connection limit (a channel as a counting
  semaphore) and graceful shutdown, and calls `handler(ctx, request)`; the
  sample backend's `server.zm` lost its request loop and its semaphore and now
  passes `routes.route` and the pool. **One language change fell out of it:**
  `serve` has to `spawn` a per-connection function that is generic over the
  context type, and `spawn` took only non-generic functions -- it now accepts
  explicit type arguments (`spawn serve_connection<C>(...)`), checked and
  substituted like any generic call. A generic `spawn` needs them written out for
  the same reason every generic call does (no inference). **What this shows
  about stage 2:** the context parameter is workable but it is the price of no
  capture -- every callback-taking API needs a `_ctx` variant or a generic
  context, and a handler that needs two things has to bundle them in a struct.
  That is the argument for capturing lambdas; it is not yet strong enough to
  make them a priority, and the alternative recorded in the previous entry
  (capture by value, no `var`) is unchanged.
- **Function values, stage 1: references to named functions, no closures
  (external review, roadmap R13, 2026-09-23).** Ziman had no function values, so
  a library could not take "your handler" (`http` could not be a framework),
  nothing could be `map`ped or `filter`ed, and every callback-shaped API had to
  be designed around the gap (`sort_by_keys`, a route chain written out by
  hand). The review argued the stated reason -- "libraries can't take opaque
  callback-shaped holes" -- is already delivered by having no overloading, no
  methods and no inheritance, and that the real cost was the training-data
  mismatch (a model's strongest habit is a lambda). **What shipped is the
  smaller half:** a function type `fn(A, B) -> R` and the name of a top-level
  function as a value of it -- *no anonymous functions and no capture*. That
  keeps the one good reason for the old restriction intact: a value that carries
  no environment cannot reintroduce shared mutable state or a hidden `var`
  capture, so the actor model is unchanged, and it is safe to send across a
  channel. **Mechanism.** `TY_FUNC` (parameter types in `type_args`, return type
  in `elem`); a `Value` holds a `FunctionDecl *`. The loader cannot see local
  variables, so for a bare identifier or `module.name` it only records a
  *candidate* fully-qualified name on the expression (`fn_ref_name`); the type
  checker resolves it, and only when no local of that name exists, so a local
  always wins. A call whose callee was written as a plain name first checks for
  a local of function type (`through_value`). Equality, hashing, ordering and
  every derive reject function types with no new code (they only accept what
  they list). **Restrictions, deliberate:** generic functions and functions with
  a variadic parameter can't be used as values (no single type for the
  former; unknown argument count for the latter); only a *name* can be called,
  not an arbitrary expression; named arguments and omitted `Option` parameters
  don't apply through a value. **Standard library:** `collections.map`, `filter`,
  `fold`, `any`, `all`, `find`, `for_each` and `sort_with`. **What it does not
  do yet, and why:** -- *(update, same day: both things this paragraph said were
  not built now are; see the next entry.)* **Stage 2, if it is needed:**
  capturing lambdas, capturing by value as an immutable snapshot at creation
  (the rule `defer` already uses for its arguments) and forbidding capture of a
  `var`, so the actor model still holds. Recorded rather than started.
- **`Hash` is structural and automatic (external review, roadmap R12,
  2026-09-23).** `Hash` was limited to the seven scalars, so a `HashMap` could
  not be keyed by a `Uuid`, an enum or a struct -- a real hole for a backend
  whose entities are id-keyed. The fix mirrors what `==` already does rather
  than `Ord`'s naming convention, because equality *is* structural and an order
  is not: `==` on a struct or enum already compares field by field, so the hash
  is the same recursion, and `a == b` implies `hash(a) == hash(b)` with no
  function for anyone to write or forget (Rust makes this opt-in with `derive`;
  Ziman's `==` is automatic, so an opt-in would be boilerplate a model forgets).
  **Rule:** a type is hashable when it supports `==` and every leaf is a
  scalar; arrays are hashable only for scalar elements (the same limit array
  `==` has), a struct hashes its fields in order, an enum its variant name and
  then its payload, `Result` its variant and payload, and a type parameter is
  hashable when bounded by `Hash`. The checker's test recurses exactly like the
  equality test (with the same depth guard), and the runtime builtin recurses
  over the same shapes. No `collections` change was needed -- `HashMap` and
  `HashSet` already required `Eq + Hash`, so `HashMap<Uuid, Patient>` and
  composite keys just work. **Details:** `-0.0` is normalised to `0.0` before
  hashing because they compare equal; a `NaN` key is never found (NaN != NaN),
  which is documented rather than forbidden since bare `float` keys were
  already allowed. **Cost:** hashing a struct or array is proportional to its
  size on every lookup.
- **`Ord`, `compare` and a generic `sort` (external review, roadmap R11,
  2026-09-23).** Sorting was not merely missing from the standard library but
  *inexpressible*: with no function values there is no comparator, and with
  only `Eq` and `Hash` as bounds there was no way to say "T can be ordered".
  The nine hand-written `_compare` functions across `time` were the symptom --
  the language needed ordering nine times and worked around it nine times.
  **Design.** A third hardcoded bound, `Ord`, satisfied by the scalars that
  order naturally (`int`, `float`, `bigint`, `decimal`, `byte`, and `string`
  byte-wise -- `bool`, arrays, `Option` and `Result` deliberately not) and by a
  struct or enum that has a `<snake_type>_compare(a: T, b: T) -> int` function.
  That is a *naming convention*, the same kind derives already dispatch on
  (`<type>_to_json`), chosen over a new declaration form because it needs no
  syntax and the `time` types already followed it; the checker verifies the
  function exists and has the right signature, both at a bound and at a
  `compare` call, and the runtime `compare` builtin finds and calls it. Both
  spellings are accepted (`local_date_compare` and `localdate_compare`),
  because the bundled `time` types were written before this convention was
  named. A builtin `compare<T: Ord>(a, b) -> int` does the work -- and its
  contract, like Java's `Comparator` and C's `strcmp`, is negative/zero/positive:
  only the sign means anything, so a hand-written `_compare` may return any
  `int` and nothing here depends on it being `-1`/`0`/`1`, and `<`, `>`,
  `<=`, `>=` gained `string` and `byte` (the common gap) but stay
  non-overloadable, so a user type never redefines an operator (principle #2):
  it goes through `compare`. A `float` order is made total by putting NaN after
  everything and equal to itself, since an order with NaN "equal to everything"
  is not transitive. **Sorting** lives in `collections` (a prelude function
  named `sort` would collide with every program that defines its own): a
  stable bottom-up merge sort computes the *permutation* (`sort_indices`), and
  `sort` and `sort_by_keys` both gather by it. `sort_by_keys` is the answer to
  "no comparator": the caller builds the key array and the sort orders the
  items by it, which covers any field or computed ordering with no function
  values. `#[derive(Ord)]` writes `_compare` for structs (lexicographic by
  field) and enums (variant position, then payload). **Cost and limits:** a
  pure-Ziman merge sort sorts 20,000 ints in about 0.3 s; a native fast path
  for scalar arrays is an easy later win. A type has exactly one order (its
  `_compare`); anything else goes through `sort_by_keys`. **Added right after:**
  the order extends recursively to arrays (lexicographic), `Option<T>` (`None`
  first, the same choice Rust makes) and `Result<T, E>` (every `Ok` before
  every `Err`), each `Ord` when its element/payload types are -- so nested
  shapes sort and `#[derive(Ord)]` accepts fields of those types.
  `sort_desc`/`sort_by_keys_desc` merge with the comparison flipped rather than
  reversing the ascending result, so ties keep their input order. A generic type can't
  derive `Ord` yet. Bounded type parameters are still checked only at the use
  site, as with `Eq`/`Hash`.
- **Server hygiene: exit status, bounded concurrency, graceful shutdown
  (external review, roadmap R10, 2026-09-23).** The showcase server had four
  problems a real deployment hits at once: the configuration-error path printed
  a message and then crashed on an `unwrap` anyway (Ziman had no way to say
  "stop here" with a status); it spawned one thread per connection without
  limit, a denial-of-service primitive on its own; Ctrl-C killed it mid-request;
  and a startup failure looked like a stack of runtime noise. **Language
  additions, all in `os`:** `exit(code)`, `write_stderr(text)`,
  `install_shutdown_handler()` and `shutdown_requested()` -- SIGINT/SIGTERM set
  an atomic flag, nothing more. `net.accept` now waits in 250 ms `poll` slices
  and returns an error once the flag is set. That is deliberate: a signal is
  delivered to an arbitrary thread, so relying on `EINTR` in the thread that
  happens to be blocked in `accept` would be a race; polling a flag is not.
  **No new concurrency primitive:** the connection limit is a channel used as a
  counting semaphore -- it starts with one token per allowed connection, the
  accept loop takes one before spawning, and the connection thread gives it back
  with a `defer` on every way out -- so at most that many threads ever exist,
  and a client past the limit is accepted by the kernel and waits. Shutdown
  stops accepting, then takes every token back, which is exactly "all connections
  finished", and only then closes the pool. `server.max-connections` (default
  64) is validated at startup. **Limits, stated:** shutdown waits for in-flight
  connections, so an idle keep-alive client delays it by up to
  `server.idle-timeout-ms`; waiting for a slot cannot itself be interrupted by a
  signal (it ends when a connection finishes); and this is a bounded thread per
  connection, not a worker pool -- the threads are still created and destroyed
  per connection. Still open from the review's list: `send` never blocks and
  channel queues are unbounded (no backpressure primitive yet), and the pending
  queue behind a full server is the kernel's, not ours.
- **Memory reclamation: a tracing collector, opt-in as `make gc`
  (external review, roadmap R9, 2026-09-23).** The default build never frees
  a heap value, and the review measured the sample backend's server reaching
  ~31 GB RSS after ~1,500 requests. Neither planned fix was built. Refcounting
  needs an increment or decrement at every place a `Value` is copied (hundreds
  of sites), atomic counts across threads, and breaks on the one mutable type
  -- a `Channel<T>` can hold a struct that holds the channel (a real cycle).
  Per-thread arenas free nothing in a thread-per-*connection* server until the
  client disconnects. A tracing collector fits how Ziman values already work
  (immutable, shared by pointer, shared across threads): no ownership design,
  cycles are fine, and the leak stops being what makes cross-thread sharing
  safe. **Mechanism:** `src/zgc.h` is force-included into every source file of a
  `make gc` build and redirects `malloc`/`calloc`/`realloc`/`strdup` to the
  Boehm-Demers-Weiser collector and turns `free` into a no-op; `GC_INIT()` runs
  at start-up and `gc.h` redirects `pthread_create` so every spawned thread is
  registered. Conservative scanning of thread stacks and registers means the
  interpreter needed no rooting discipline. The text bytes of a string are
  allocated "atomic" (never scanned for pointers). libgc is built from a pinned
  source tree by `tools/build_bdwgc.sh` (git + a C compiler, no cmake or
  autotools) with `ALL_INTERIOR_POINTERS` and `GC_THREADS`. **Two things bit
  during the prototype and are worth remembering.** (1) Without
  `ALL_INTERIOR_POINTERS`, an interior pointer more than a page into a large
  block is not recognised, so a live string slice or token into a big source
  buffer was collected -- parse errors in bundled modules, but only when a
  collection happened to run mid-parse. (2) The real retention root was ours:
  `StringBuilder` and `ArrayBuilder` kept every builder ever created in a
  global handle table (each `json_to_string` node makes one), so nothing a
  builder held was ever unreachable. The handle is now the builder's address
  (a plain int in the Ziman value, which the collector still sees), with a
  magic word so a forged or stale handle is a clear error rather than a wild
  read. **Results** (sample backend, sequential curl, the review's workload of
  500 `GET /health`, 500 `POST /patients`, 500 `GET /patients`): peak RSS
  ~31 GB before, ~0.47 GB with the collector -- flat at ~17 MB through the
  first 1,000 requests, then a ~465 MB plateau during the list phase (each list
  of hundreds of patients builds large transient structures; it was not still
  growing). A single list request used to add ~12.5 MB of RSS each; with the
  collector, three in a row moved 30.4 -> 30.5 -> 30.7 MB. All 421 fixtures,
  the Postgres/TLS suites and the backend's Curly scenarios pass under both
  builds. **Not done, deliberately:** it is not the default build (libgc is a
  build-time dependency fetched by the script, and a decision for the project);
  pause times are unmeasured (parallel marking used many cores in the most
  garbage-heavy phase -- `GC_MARKERS` tuning is untried); sockets, SQLite
  connections and TLS sessions are still closed by hand and simply leak if
  forgotten, as before; memory is not returned to the OS; a `Channel`'s mutex is
  never destroyed; and conservative scanning can retain garbage that looks like
  a pointer. If the dependency is unwelcome, the same shape can be written in
  the interpreter (an allocation registry, conservative stack scanning,
  cooperative safepoints for stop-the-world) -- the prototype says the approach
  works; it does not say which implementation to keep.
- **Strings are `{ptr, len}` views, so slicing is free (2026-09-23).**
  A `string` was a NUL-terminated `char *`, so every `substring`, `trim`,
  `char_at` and `split` piece allocated and copied -- and, since nothing is
  freed, leaked -- and `len` was a `strlen`. The parsers (`json`, `http`,
  `pg`, `properties`) cut strings constantly, so this was a large share of
  the garbage behind the memory problem (R9) and most of their time. A
  string value is now `{ptr, len, term}`: a whole buffer or a slice of one,
  and the two are the same representation, not two forms with two code
  paths in every builtin. `substring`, `trim`, `char_at`, `split` and
  `split_once` return slices; `concat`, `replace`, `join`, `to_upper` and
  friends still build fresh buffers. **`term`** records that `ptr[len]` is a
  NUL -- true for a fresh buffer and for a slice that runs to the end of a
  terminated one -- which is what lets a native hand the bytes to a C API
  (`fopen`, SQLite, `getenv`) with no copy; otherwise `value_cstr` makes a
  terminated copy. Costs: a `Value` grew from 72 to 80 bytes (the union's
  string member is 16 bytes), a string is limited to 4 GiB, every native
  that read a string had to move to `ptr`/`len` (about 50 sites -- the
  compiler found each, because the old `as.s` member was removed), and a
  small slice keeps its whole parent alive, which becomes a retention
  question once memory is reclaimed. Measured on a synthetic JSON-parse +
  split/trim workload: 4.6 s -> 1.5 s, peak RSS 348 MB -> 327 MB -- the time
  win is large, the memory win small, because most of what a JSON parse
  allocates is `Json` nodes and parse-result structs, not string bytes.
  Not changed: strings still may not contain a 0 byte (`bytes_to_string`
  still rejects one); the representation could now allow it, but that is a
  language decision, not a side effect.
- **`--check` and `--json`: the compiler as a tool an agent can call
  (external review, roadmap R8, 2026-09-23).** The CLI took exactly one
  argument, and principle #5 ("errors are data") had been promised since
  v0 and never built -- yet it is the item most aligned with the AI-first
  thesis: an agent iterating against the compiler should not have to
  scrape prose. `ziman --check file.zm` loads and type-checks without
  running (an agent's between-edits call; running a server just to see
  whether it compiles is not acceptable). `ziman --json` prints every error
  as one JSON object on stderr, `{"severity":"error","phase":"type",
  "line":3,"col":18,"context":null,"message":"..."}`, where `phase` is
  `syntax`/`module`/`type`/`runtime` and `context` names the derive an error
  came from (R6) or is null; the text form is rendered from the same
  arguments, so the two cannot drift. `--check --json` prints
  `{"ok":true}` on stdout when clean. Program output stays on stdout and
  diagnostics on stderr, so `--json` composes with a running program. Also
  added: `--emit-derived` (print the source each `#[derive]` generates --
  the debuggability argument for generating readable source in the first
  place), `--version` (`git describe`, baked in at build time), `--help`.
  Errors still exit 1; a bad command line exits 2. **Deliberately not
  done:** several errors per run (the checker still stops at the first --
  a real gap for an agent, since fixing one error to discover the next
  costs a round trip), and a `file` field (no error carries one yet; the
  loader would have to thread module paths through every error site). Both
  are recorded as open in the roadmap.
- **A derive imports the names it generates code against (external
  review, roadmap R7, 2026-09-23).** One `#[derive(Json)]` needed five
  hand-written `use json.*` lines, and forgetting one produced the old
  invisible-line error with no mention of `use`. The docs presented "Nothing
  is auto-imported" as a principle; it was an implementation consequence.
  The generator knows exactly which names it emitted, so it now adds them
  (a small per-target table: Json, Row, and a whole-module `use properties`
  for Properties), skipping any name the module already binds -- an
  explicit `use`, or the module's own declaration of that name -- so
  nothing that compiled before changes meaning and the injected names stay
  traceable to the `#[derive]` line. Not covered on purpose: a *field
  type's* own conversion (`uuid_to_json`, `time`'s functions, ...). Those
  depend on which type the field is and whether its module provides one, so
  they stay explicit, but the error now says what to do ("give that type its
  own #[derive(Json)], or write it by hand and `use` it"). Result: the
  sample backend dropped 45 derive-only `use` lines across nine files with
  no other change.
- **`#[derive]` errors point at the declaration and say which derive
  produced them (external review, roadmap R6, 2026-09-23).** A derive
  generates real Ziman source and re-parses it, and the generated tokens
  carried positions inside that invisible text, so a missing import in the
  user's file reported `type error at 1:5: unknown struct or enum type
  'Json'` -- a line that exists nowhere the user can open. For a language
  whose compile errors are the interface an agent iterates against, a wrong
  line is close to worse than no line. Fix, in two parts. Every generated
  token is restamped with the line/column of the declaration it was
  generated for, so any error anywhere in the expansion points at the
  user's own `struct`/`enum`. And each generated function remembers where
  it came from (`FunctionDecl.derived_from`, e.g. "derive(Json) for
  'Point'"), while the loader's qualify/resolve passes and the type checker
  set a small global error context around each function, so the message
  reads `type error at 8:1 (in derive(Json) for 'Point'): ...`. No
  file name is printed because no error carries one yet (every error is a
  bare line:col). What it deliberately does not do: pinpoint a *field*
  inside the struct (the whole expansion maps to the declaration), or
  improve the wording of the error itself -- the missing-`use` message is
  R7.
- **Diagnostics for the habits a model brings from other languages
  (external review, roadmap R5, 2026-09-23).** The compiler's error
  messages are the interface an agent iterates against, and the review
  found them uneven exactly on the highest-frequency mistakes: four good
  (`?` misuse, missing generic args, unsatisfied bounds, type mismatches),
  and four bad. Fixed three: `p.to_string()` said "no module imported
  under the name 'p'", sending the author to fix an import instead of
  telling them Ziman has no methods -- it now says so and gives the
  function form (`str(p)`, `len(p)`, `xs = push(xs, item)`, or generally
  `name(p, ...)`), while still mentioning `use p` for the genuine
  forgot-the-import case; `"a" + "b"` said the operands "must be the same
  numeric type ... (use 'as' to cast explicitly)", self-contradictory and
  suggesting a cast that cannot exist -- it now points at `concat`/`format`
  (and `push`/`concat_arrays` for arrays); `match e { ... }` said "expected
  an expression, found '{'" -- it now says there is no `match` and points
  at `switch`. `match` stays a legal identifier (a function named `match`
  still works); the special case only fires when it is followed by an
  identifier or literal, which is never a call. The derive-error case from
  the same table is R6.
- **`switch`: exhaustive consumption of an enum or a Result (external
  review, roadmap R4, 2026-09-23).** Enums were consumed with `if
  Shape.is_Circle(s) { ... Shape.unwrap_Circle(s) ... }` chains, so adding
  a variant compiled cleanly everywhere and crashed at runtime wherever a
  consumer forgot it -- verified against the sample backend's own
  `service_error_response`. That is the failure an LLM refactoring one
  file at a time is *most* likely to cause, and the one static check the
  language most directly could have but didn't, while `1 + 1.0` was a
  compile error. Full pattern matching (nested patterns, guards, `or`
  patterns, `match` as an expression) stays deferred; what shipped is the
  small intermediate the review suggested: a statement, one arm per
  variant, no default arm. Design points: arms use bare variant names and a
  `Variant(binder)` for the payload (casing tells a variant from a
  binder, the scrutinee's type says the enum); **no default arm on purpose**
  -- it would reintroduce the silent case; statement-only, so each arm is
  a block and returns work the ordinary way. **Implementation:** the
  type checker validates the arms (unknown, duplicate, missing, payload/
  binder mismatch, a non-enum scrutinee) and then rewrites the node in
  place into a block holding a hidden `val $swN = <scrutinee>` and an
  if/else chain over the existing `is_X`/`unwrap_X` calls (or
  `is_ok`/`unwrap`/`unwrap_err` for a Result), the same trick `defer` and
  the auto-wrap use for synthetic nodes. So generics, payload typing, the
  "does every path return" analysis and the interpreter all needed nothing
  new -- the interpreter never sees a `switch`. The last arm becomes the
  chain's `else`, which is what lets an all-arms-return `switch` satisfy
  the returns-on-every-path rule. Cost: five walkers (parser type-param
  reclassification, three loader passes, the checker) learned a new
  statement kind. The sample backend's three `is_X`/`unwrap_X` chains were
  converted as the first users.
- **`for i in a { a[i] }` is a compile error (external review, roadmap
  R3, 2026-09-23).** One header, `for x in expr`, binds the element for an
  array and the index for a range, and the only guard was a naming
  convention -- in a language that enforces identifier casing in the
  parser. A model carrying Go/JS/Python's meaning of a single binder wrote
  `for i in a { sum = sum + a[i] }` and got a plausible wrong answer (`2`
  instead of `4`), sometimes a crash. Options considered: a distinct
  keyword (`in` for ranges, `of` for arrays -- closes the whole class,
  matches JavaScript's for-in/for-of, but breaks ~55 sites), removing the
  single-binder form (needs a `_` discard Ziman lacks), or a narrow check.
  Chosen: the narrow check, because the grammar is not actually ambiguous
  (a range is only ever a literal `a..b` after `in`), so this is a
  model-prior problem, not a parsing one, and one keyword stays. The check
  is exact -- single binder, iterated expression a plain identifier, index
  expression the bare binder, same identifier -- tracked with a small stack
  of in-scope loop binders so a shadowing inner loop is not flagged. Known
  gap: it does not catch other misuses of the element as a position
  (`for i in a { print(i) }`). Revisit `of` if the misuse keeps appearing.
- **`int` overflow is a runtime error, not a silent wrap (external review,
  roadmap R2, 2026-09-23).** `9223372036854775807 + 1` used to print
  `-9223372036854775808`, contradicting the language's own rule that casts
  clamp ("nothing silently wraps") and, worse, being undefined behaviour in
  the C implementation (signed overflow, no `-fwrapv`). Chosen rule: trap
  on `+ - *`, unary `-`, `MIN / -1` and `abs(MIN)` with an explicit
  message; `MIN % -1` is `0` (mathematically right, and the C operation
  traps). Trap over saturate because a saturated wrong answer is still a
  silent wrong answer, and over documented wrap because the language has
  `bigint` for values that outgrow 64 bits. The build also gains `-fwrapv`
  (so the interpreter's own arithmetic is well defined) and `-O2`. Cost:
  one overflow-checked builtin per operation; the measured effect of the
  checks plus `-O2` on a recursive-`fib` benchmark was a ~18% speedup over
  the old `-O0` build, not the 2-3x the review guessed, since a tree-walking
  interpreter is dominated by allocation and dispatch.
- **`Result<T, E>` bare-value auto-wrap -- the fourth and final stage
  of the named-arguments/Option-omission/auto-wrap conversation, and
  the smallest of the four.** Agreed rule (the user's own, resolving
  the one ambiguity -- what happens when `T == E` -- I'd raised earlier
  in the conversation): a bare value of type `T` becomes `Ok(T)`; a
  bare value of type `E` becomes `Err(E)`; when `T == E`, a bare value
  always becomes `Ok` -- deliberately not clever about the degenerate
  same-type case, since there's no principled way for the sugar itself
  to guess which was meant; `err(value)` remains the explicit escape
  hatch for that one case. An already-`Result<T, E>` value passes
  through unchanged, symmetric with `Option<T>`.
  **No omission, and so no default-value syntax either** -- unlike
  `Option<T>`, `Result<T, E>` has no natural "absent" state an omitted
  argument could stand in for (an outcome has to be *something*), so
  this stage is purely about what a *supplied* value looks like. This
  kept the change entirely inside `check_value_against_param_type` (the
  same shared helper Option<T>'s own auto-wrap already uses) -- no
  changes needed to `canonicalize_named_args`'s omission-filling at all,
  since a `Result<T, E>` parameter already correctly fell through to
  "missing required argument" there (only `is_option_type` params were
  ever treated as omittable), and no changes needed to `check_function`'s
  default-value validation either, since it already rejects `= expr` on
  any non-`Option<T>` parameter, `Result<T, E>` included.
  **Mechanically different from `Option<T>`'s own wrap in one way**:
  `Result<T, E>` is a hardcoded primitive with no enum declaration
  behind it, unlike `Option<T>` (plain Ziman source, an ordinary enum).
  `Option<T>`'s auto-wrap reuses enum construction (`EXPR_ENUM_ACCESS`)
  wholesale; `Result<T, E>`'s reuses the *other* existing machinery
  instead -- `ok`/`err` are themselves hardcoded core builtins (see
  `check_call`'s and `interpreter.c`'s own `strcmp(callee, "ok")`
  branches) -- so `wrap_bare_in_result` builds a synthetic `EXPR_CALL`
  to `"ok"`/`"err"` rather than an enum-construction node. Either way,
  the underlying idea is identical: reuse an execution path that already
  works, so nothing downstream of type-checking needs to change at all.
  Verified directly: a bare `T` and a bare `E` argument both auto-wrap
  correctly; an already-`Result<T, E>` value (via `ok(...)`/`err(...)`
  assigned to a typed `val` first, since a bare `ok(...)`/`err(...)`
  call was never itself a legal call-argument position -- a pre-existing
  restriction, unrelated to this feature) passes through unchanged; the
  `T == E` case always resolves to `Ok` for a bare value, with an
  explicit `err(...)`-constructed value still reaching the `Err` branch
  correctly; a generic function's own `Result<T, E>` parameter
  auto-wraps correctly using the call's substituted `T`/`E`; named and
  positional calls both work; a `Result<T, E>` parameter rejects a
  positional or named omission and rejects a declared default, exactly
  as designed; a wrong-type argument produces a clear three-way message
  (`"expects Result<T, E>, T, or E, found ..."`, or the two-way
  `"expects Result<T, E> or T, found ..."` when `T == E`).
  **Amended 2026-09-23 (external review, roadmap R1): `T == E` is now a
  compile error, not "always `Ok`".** The rule above was documented and
  deliberate, but `Result<string, string>` is the most common shape in the
  standard library, and the failure was silent in the most-written
  direction: `return "path must not be empty"` compiled to `Ok(...)`. For a
  language whose premise is first-try correctness from a generator, the
  sugar was most dangerous exactly where it was most likely to be used. A
  bare value where `T == E` is now rejected with a message saying to write
  `ok(...)`/`err(...)`; auditing the standard library found two places that
  relied on the old behaviour (both meant `Ok`, in `json` and `http`). The
  same review found `Option<Option<T>>` quietly turning a `None` into
  `Some(None)` under the sugar; that is rejected too. Also corrected: the
  "modeled on Rust's `impl From<T> for Option<T>`" justification here and
  in the `Option<T>` entry does not hold -- Rust has no such implicit
  conversion.
- **Extended the Option<T>/Result<T, E> bare-value auto-wrap from
  function-call arguments to the other three positions where a type is
  already known from context.** Prompted by a follow-up idea for a
  `throw` keyword (sugar for an early `return err(...)`) -- rejected
  `throw` itself (see below), but the discussion surfaced a real,
  narrower gap worth fixing on its own: `return 1` for a function
  declared to return `Result<int, string>` didn't actually work, since
  the auto-wrap built for stage 3/4 only ever touched
  `check_call`'s argument-checking, never `return`. Checked directly
  before assuming otherwise -- confirmed the gap, then generalized
  rather than special-casing just `return`: `check_expr_expecting`
  already gives `ok(...)`/`err(...)`/`channel_new()`/an empty array
  literal this same "resolve against the expected type" treatment in
  exactly four positions (a `val`/`var`'s own declared type, a struct
  literal field, a `with` override, and a function's returned value),
  so leaving three of them exact-match-only while only `return` gained
  the new sugar would have just relocated the inconsistency rather than
  removed it -- the same reasoning that already justified extending the
  original design from `Option<T>` alone to `Result<T, E>` alongside
  it. **Explicitly framed as an AI-generation concern, not just a
  human-readability one, when the user asked directly**: a rule that
  applies at one syntactically-identical position but not
  another is exactly the kind of partial-pattern-application a model
  is prone to getting wrong -- it has one uniform rule to apply
  everywhere now ("wherever a `Result<T, E>`/`Option<T>` value is
  expected, a bare value auto-wraps"), not a rule scoped to an
  arbitrary subset of the positions that idea could apply to.
  Implemented by changing `check_expr_expecting`'s signature from
  `Expr *e` to `Expr **e_slot` (needed so the auto-wrap case can replace
  the expression in place, the same reason `check_value_against_param_type`
  itself already takes one) and trying that same helper as one more
  case before its existing plain-`check_expr` fallback. Deliberately
  does *not* raise its own richer error on a mismatch the way
  `check_call`'s own copy does (`"expects X, Y, or Z"`) -- each of the
  four call sites keeps raising its own pre-existing, position-specific
  message ("field 'x' of 'Y' expects...", "returned X but function is
  declared to return Y", ...) for a genuine type mismatch, just without
  the extra "or T"/"or T, E" wording; teaching four different messages
  that wording wasn't worth it for a case that already had a perfectly
  serviceable error. `STMT_VAL`/`STMT_VAR`'s own declared-type checking
  doesn't go through `check_expr_expecting` at all (a pre-existing,
  unrelated split -- see that code's own comment for why) and got the
  identical treatment added as its own parallel branch instead, calling
  `check_value_against_param_type` directly against `&s->as.decl.init`
  (not the function's own local `init` copy, so a successful wrap
  persists into the AST the interpreter actually evaluates).
  **`throw`, rejected**: proposed as sugar for `return err(x)`.
  Once bare-value `return` auto-wrap exists on its own, `throw "x"` and
  `return "x"` do exactly the same thing -- no unwinding, nothing
  catchable up the call stack, exits only the immediate function --
  so `throw` would add a name with zero functional benefit over plain
  `return`. Worse, `throw` is borrowed from languages (Java, Python,
  JS, C++) where it *does* unwind arbitrarily far and *is* catchable;
  reusing that name for something that only returns from the current
  function risks teaching both human readers and an LLM generating
  Ziman code the wrong mental model, directly against this project's
  own "cheap for an LLM to generate correctly" goal. If the underlying
  itch is reducing `Result`-propagation boilerplate (`if is_err(r) {
  return err_of(r) }` after every fallible call), a Rust-style `?`
  operator would be the honest, valuable version of that idea -- a
  separate, larger design, not attempted here.
- **`?` -- the Rust-style propagation operator floated (and deferred)
  right above, picked back up and actually built.** Confirmed `?`
  itself was free to claim (grepped the lexer -- unused for anything
  today), then worked through two real design questions before writing
  any code.
  **`?` over `try`.** Considered Zig's prefix `try expr` as the
  syntactic alternative, since Zig's error unions need exactly the same
  constraint Ziman's `?` would (no `From`-style conversion, the error
  type has to already match) -- a stronger semantic fit than Rust's
  `?`, which auto-converts via `From<E1> for E2` and would set the
  wrong expectation. Went with `?` anyway once the user chose it
  explicitly: it's the one both Option<T> and Result<T, E> can share
  under a single glyph, matching the existing "punctuation over a new
  keyword" preference (see "Variadic parameters" above) more closely
  than a prefix keyword would. The false-friend risk (a model assuming
  Rust's auto-conversion) is handled by the error message itself
  spelling out the required `E` exactly, not by avoiding the symbol.
  **Both `Result<T, E>` and `Option<T>`, from the start, not
  Result-only with Option added later** -- the same "don't let the two
  drift apart" reasoning that already drove extending bare-value
  auto-wrap to all four context positions. `Option<T>`'s own version
  needed no `E`-matching constraint at all, since `None` carries no
  value to propagate: `opt?` only requires the enclosing function to
  return `Option<T2>` for *any* `T2`, independent of the inner `T`.
  **Nesting inside a larger expression (`return str(foo()?)`,
  `bar(foo()?, baz()?)`) -- deliberately not supported, a real
  architectural fork put to the user rather than picked silently.**
  Ziman's tree-walking `eval_expr` always returns a plain `Value`; there
  was no existing way for something buried inside an expression to
  signal "abort, return early from the enclosing function" the way
  `return`/`break`/`continue` already can, but only at the *statement*
  level (`ExecResult`'s `CTRL_RETURN`, checked after each statement, not
  after each sub-expression). Three ways to get full nesting were
  weighed: (1) a C-level non-local jump (`setjmp`/`longjmp`) giving
  `call_function` a real escape point, matching how some other
  tree-walking interpreters implement error propagation, but a wholly
  new category of control flow nothing else in this interpreter uses;
  (2) threading an early-return signal through `eval_expr`'s own return
  type and updating every call site (binary ops, call arguments, array
  literals, struct fields, index expressions -- dozens of places), a
  much larger and more error-prone surface for the same result; (3)
  restricting `?` to statement top-level, avoiding both. Recommended
  and chosen: (3). It gives up real expressiveness (an unwrap-then-
  transform-then-return in one line needs two statements instead), but
  needs no new interpreter control-flow primitive at all -- the
  restriction is exactly what makes the desugaring below possible with
  no changes to `eval_expr`'s signature or any of its many call sites.
  **Mechanically**: `?` parses as an ordinary postfix operator (`parse_postfix`,
  alongside `[index]`/`.field`), producing an `EXPR_TRY_UNWRAP` node
  with no restriction on where it may appear syntactically -- legality
  is enforced entirely in typecheck.c instead. `check_expr`'s own
  `EXPR_TRY_UNWRAP` case always raises "'?' can only be used as a
  val/var's entire initializer or as its own statement, not nested
  inside another expression" -- reachable only when a `?` *wasn't*
  already intercepted by one of the two legal statement forms, which
  both check `init->kind == EXPR_TRY_UNWRAP` / `expr_stmt.expr->kind ==
  EXPR_TRY_UNWRAP` themselves, before ever calling ordinary `check_expr`
  on it. `check_try_unwrap` does the real work: checks `inner`'s type is
  genuinely `Option<T>` or `Result<T, E>`, validates the enclosing
  function's own `fn_return_type` against it (both constraints above),
  and caches the *full, concrete* `fn_return_type` itself on the node
  (`e->as.try_unwrap.propagated_type`) -- the interpreter has no other
  way to learn the enclosing function's own return type at the point it
  needs to build a propagated `None`/`Err` value, so type-checking hands
  it over once, up front, the same "resolve once, the interpreter just
  trusts it" pattern used everywhere else this session. At runtime,
  `eval_try_unwrap` (interpreter.c) evaluates `inner` once, re-derives
  which of the two it turned out to be from the value's own runtime
  shape (a `ResultObj`'s `is_ok` vs. an `EnumObj`'s `variant_name`,
  exactly like `is_ok`/`Option.is_Some` already do), and either returns
  the unwrapped value or builds an `ExecResult` with `CTRL_RETURN` --
  the *exact* mechanism an ordinary `return` already produces, reused
  as-is, requiring zero changes to how a block/loop/if already
  recognizes and propagates an early return once it's produced. Verified
  this propagates correctly through nested control flow (a `?` inside an
  `if` inside a `while`), not just directly inside a function's top-level
  body, before trusting it.

- **Attributes (`#[...]`) and `#[derive(Json)]`.** Built exactly to the
  design recorded in an earlier ROADMAP.md entry (removed now that it's
  done) -- see that entry's own reasoning for *why* (rejecting true
  reflection, the priority-ordered per-field conversion rules, lenient
  deserialization, fixed snake_case-to-camelCase field mapping as a
  documented shortcoming, and the externally-tagged enum shape). This
  entry covers *how* it was actually built, including a few
  implementation-level decisions the design discussion left open.
  **`#[...]` grammar**: general on purpose (`parse_attributes` in
  parser.c parses any `IDENT` or `IDENT(IDENT, ...)` item inside
  `#[...]`), but only `derive(...)`'s items are actually collected (onto
  the new `StructDecl.derives`/`EnumDecl.derives` string arrays); any
  other attribute name parses fine and is silently a no-op, so the
  grammar doesn't need to change when a second attribute shows up later.
  Only legal directly before a `struct`/`enum` declaration -- attached to
  anything else, it's a syntax error.
  **Codegen strategy: generate Ziman source text, not AST nodes.** A new
  `src/derive.c` (`generate_derived_functions`, called once from
  `parse()` right before it returns) walks a freshly parsed module's own
  structs/enums, and for each `#[derive(Json)]` target, builds the
  `<name>_to_json`/`<name>_from_json` function bodies as a *string* of
  ordinary Ziman source (via a small growable `CodeBuf`), then feeds that
  string straight back through `lex()`/`parse()` (the exact same
  functions this pass is itself called from) and splices the resulting
  `FunctionDecl`s into the module's own `functions` list. Chosen over
  hand-constructing `Expr`/`Stmt` nodes directly: far less code, and
  trivially debuggable (the generated source can just be printed and
  read like any other Ziman program) for something with as much shape as
  a full function body with loops and branches. This works cleanly
  because it runs *immediately after parsing a single file*, before the
  loader's struct-vs-enum reclassification pass -- at that point a bare
  `Option<T>` type reference is still `TY_STRUCT`-shaped (see ast.h's
  `Type` comment), which turns out not to matter: the only thing this
  pass needs to tell apart is "is this specifically `Option<T>`"
  (checked by name, `struct_name`/`enum_name == "Option"`, which works
  identically before or after reclassification) versus "some other
  struct or enum" (dispatched by naming convention regardless of which
  it actually turns out to be) -- so no pipeline reordering was needed
  anywhere else. A spliced-in function is otherwise indistinguishable
  from a hand-written one to everything downstream (self-qualify,
  cross-module resolution, typecheck, the interpreter) -- zero special
  cases needed in any of those.
  **No auto-injected `use` statements.** The generated code calls
  `Json`/`JField`/`json_require_field`/`json_object_get`/etc. by their
  bare names, exactly as hand-written code in the same module would --
  which means the module declaring the struct/enum must `use` whatever
  the derived functions end up needing, the same as it would for any
  hand-written code doing the same job. Deliberately not auto-injected:
  auto-injecting a `use` would mean this pass runs before the dependency
  graph a `use` establishes is even known, a real ordering problem, and
  more importantly cuts against Ziman's explicit-over-implicit instinct
  elsewhere. A missing `use` just surfaces as an ordinary "undefined
  function" type error at the normal point in the pipeline.
  **Naming convention for generated names**: `<Type>_to_json`/
  `<Type>_from_json`, where `<Type>` is converted with a real
  multi-word-aware PascalCase-to-snake_case transform (`pascal_to_snake`
  in derive.c) -- `ValidationError` -> `validation_error`, `Patient` ->
  `patient`. Deliberately its *own*, fresh convention, distinct from the
  older, simpler "lowercase with no inserted underscores" shortcut
  `lib/time/time.zm`'s hand-written `_to_string`/`_from_string` pairs
  already use for their own compound names (`localdate_to_string`, not
  `local_date_to_string`) -- a cosmetic inconsistency between the two
  naming families, not a functional one. Every domain type in `uuid`/
  `time` got a hand-written `<type>_to_json`/`<type>_from_json` pair
  added (`lib/core/uuid.zm`, `lib/time/time.zm`) as a thin wrapper around
  its own existing `_to_string`/`_from_string`, named to match *this*
  new convention (`local_date_to_json`, `zone_offset_from_json`, ...) --
  this is what makes tier 2 of the priority order ("does the type have a
  `_to_string`/`_from_string` pair") actually work in practice: tier 1
  ("does the type have its own `_to_json`/`_from_json`") is the *only*
  rule the codegen itself implements for any struct/enum-kind field,
  and tier 2 types satisfy it one level of indirection out, via these
  hand-written wrappers, rather than needing a second, separate code
  path in derive.c.
  **Option<T> field decoding is tolerant of absence, not just null.** A
  struct field's own generated `from_json` uses `json_object_get`
  (returns `Option<Json>`) for an `Option<U>`-typed field, treating
  *both* a missing key and an explicit JSON `null` as `None` -- mirroring
  the exact hand-written pattern `health-record-backend-ziman`'s own
  `request.zm` already used for `middleName` before this mechanism
  existed. Every other field type uses `json_require_field(...)?`
  instead (hard error if the key is missing) -- only `Option<U>` gets
  the tolerant treatment, since it's the only field type where "absent"
  has an unambiguous, correct meaning.
  **Not supported in v1** (each a clear compile-time error, not a silent
  wrong answer): a generic struct/enum (`derive` rejects
  `type_params.count > 0` outright); a field of type `bigint`/`decimal`/
  `byte`/`Result<T, E>`/`Channel<T>` (falls through `emit_to_json`/
  `emit_from_json`'s default case with "derive(Json) does not support
  field type '...' yet" -- no current caller needed these, and adding
  them later is a localized addition to those two functions, not a
  redesign). Verified against real, non-generic structs and enums
  including a `Uuid`/`LocalDate`/`Instant`-bearing struct shaped exactly
  like `health-record-backend-ziman`'s own `Patient`, and a
  `service.ValidationError`-shaped enum mixing unit and payload variants
  -- see `tests/fixtures/derive_json_*`, `uuid_json_roundtrip`,
  `time_json_roundtrip`.
  **Follow-up fix: a real parser bug, not a derive one, surfaced by a
  field whose type lives in another module.** Deriving `Json` for a
  struct with an array field of a *cross-module* type (e.g.
  `things: [other_module.Thing]`) generates `ab_new<other_module.Thing>()`
  -- a generic call whose explicit type argument is qualified. This
  failed to parse: `next_looks_like_generic_call` (parser.c), the
  one-token lookahead disambiguating `identity<int>(5)` from `x < y`,
  only recognized an uppercase identifier or a primitive keyword right
  after `<`; `other_module` is lowercase, so it fell through to treating
  `<` as a comparison operator, producing a confusing downstream syntax
  error nowhere near the real cause. Fixed by looking two tokens further
  when the one right after `<` is a lowercase identifier: `'.'` then an
  uppercase identifier means a qualified type reference
  (`ab_new<repository.Patient>()`), unambiguously -- a struct field name
  can never start uppercase and a qualified type's second segment can
  never start lowercase (both already enforced elsewhere), so this
  isn't another heuristic, it's a precise test. Nothing before
  `#[derive(Json)]` had ever written a generic call with a qualified
  type argument, which is why this had gone unnoticed. See
  `tests/fixtures/multifile_generic_call_qualified_type_arg` (the parser
  fix in isolation) and `multifile_derive_json_cross_module_field` (the
  original derive scenario that found it).
  **Second follow-up: `json_to_json`/`json_from_json`, identity
  wrappers letting a bare `Json`-typed field compose with derive at
  all.** Motivated by `health-record-backend-ziman`'s
  `ValidationErrorResponse`: a response envelope shared across several
  resources, each with its own distinct `ValidationError` enum for the
  `error` field -- a shape `#[derive(Json)]` can't unify across modules
  (that needs a generic struct, and deriving a generic struct/enum is
  explicitly unsupported). Declaring the field plain `Json` instead
  sidesteps needing generics at all: each caller converts its own enum
  to `Json` before constructing the shared struct (`<enum>_to_json`,
  already generated for it) and decodes it back out with its own
  `<enum>_from_json` after reading the shared struct. This only works
  because tier 1 of the per-field priority order ("does the field's
  type have its own `_to_json`/`_from_json`") doesn't care what the
  type actually *is* -- `Json` satisfying it via a two-line identity
  function is no different from `Uuid`/`LocalDate` satisfying it via a
  real conversion. See `tests/fixtures/derive_json_bare_json_field`.

- **`db`/`datasource` core modules, `#[derive(Row)]`, and SQLite as the first
  backend.** Designed as a JDBC-shaped driver layer, not a document store:
  an earlier sketch (untyped `Json` rows keyed by table and id,
  equality-only queries, a hand-written in-memory actor) was dropped the
  moment the requirement became "relational databases that use SQL" --
  tables, columns, SQL text, bound parameters, result sets and
  transactions are the vocabulary, so that's what the API speaks.
  **No interfaces, so dispatch is by URL.** Ziman has no traits, so
  "pluggable backend" can't be an interface; `datasource.open(url)` picks the
  driver by scheme (`sqlite:` today, `postgres:` later over the `net`
  module) and returns the same `Connection` handle regardless, exactly the
  `handle: int` into a native-side table pattern `StringBuilder` already
  uses. **The in-memory backend is real SQLite (`:memory:`), not a
  hand-rolled emulation** -- emulating SQL means writing a SQL engine,
  whose semantics would then drift from Postgres; linking the system
  `libsqlite3` (`-lsqlite3`) means repository SQL is exercised against a
  real relational engine from day one. **Only primitives cross the native
  boundary**: `SqlValue` is `Null|Int|Float|Text|Bool|Bytes`, and
  parameters and result cells travel between `sqlite.zm` and `native_sqlite.c` as
  *tagged strings* (`N`, `I<int>`, `F<%.17g>`, `T<text>`, `B<hex>`, `b0/b1`)
  -- constructing a user-declared enum from C would mean knowing the
  loader's qualified name for it, which a native has no business knowing.
  `Uuid`/`LocalDate`/`Instant`/`decimal`/`bigint` are all `Text`, converted
  in the Ziman mapping layer, which sidesteps every dialect difference in
  how SQLite and Postgres type them (SQLite has no UUID or timestamp
  type). A native `sqlite::float_to_text` exists purely because `str(float)` is
  `%g` (six digits) and would silently lose precision on a bound value.
  **Placeholders** are Postgres-style `$1, $2`; the SQLite driver rewrites
  them to `?N` (skipping string literals and quoted identifiers), because
  SQLite reads `$name` as a *named* parameter numbered by first appearance,
  so `WHERE a = $2 AND b = $1` would otherwise bind backwards. SQLite
  connections turn `PRAGMA foreign_keys` on -- off by default, unlike
  Postgres -- so referential integrity behaves the same on both.
  **`DbError`** is `NotFound|Constraint|Connection|Sql`: the driver raises
  the last three (native errors arrive as `"category|message"`, split in
  `db.zm`); `NotFound` is for a repository whose lookup came back empty,
  kept in the one type so it covers the whole data-access layer.
  **Concurrency and transactions, stated honestly.** Each connection has
  its own mutex taken around every native call, so a shared `Connection`
  is safe statement by statement. A *transaction* is not safe to share
  (another thread's statements interleave inside it), and with no closures
  or destructors there is no `with_transaction(fn)` and a `?` returning
  early from inside one leaves it open. Accepted for now, not solved: it
  deserves its own design once the pain is real.
  **`#[derive(Row)]`** reuses the attribute/derive machinery unchanged:
  `<name>_from_row`/`<name>_to_params` are generated as source text like
  the Json pair, with the same per-field priority order (a struct/enum
  field dispatches by naming convention to `<name>_to_sql`/`_from_sql`,
  which `uuid.zm` and `time.zm` provide as thin `Text` wrappers; scalars
  map directly; `Option<U>` is NULL-or-value) but no camelCase transform --
  field names are already `snake_case`, which is how columns are named.
  Arrays, enums, generics and the remaining scalar kinds are compile-time
  errors. Repository SQL is hand-written, deliberately: derive only maps
  rows to structs and back, it doesn't generate queries.
  **A small type-checker fix came out of it:** an empty array literal
  passed straight to a call (`datasource.execute(conn, sql, [])`) now takes its
  type from the parameter, the same "expected type from context" trick a
  `val` annotation, struct field or `return` already had -- without it
  every parameterless query needed a separately annotated `val`.
  See `tests/fixtures/db_*`, `derive_row_*`, `empty_array_call_arg`.

- **`net` and `bytes` core modules: blocking TCP.** Built as the
  prerequisite for a Postgres driver (the `db` module's second backend),
  and the base an HTTP server would sit on. **Blocking, thread-per-
  connection, on purpose**: Ziman has real OS threads and `spawn`, so a
  blocked `accept`/`read` blocks only its own thread and no event loop or
  async machinery is needed -- which is also exactly what a wire-protocol
  client wants (blocking reads inside message parsing). **Same handle
  pattern as `db` and `StringBuilder`**: `Socket`/`Listener` are `{
  handle: int }` structs over a native-side table of descriptors, so a
  program can't hand an arbitrary number to `close(2)`; errors cross the
  native boundary as `"category|message"` strings and become a `NetError`
  enum in `net.zm` (`ConnectionRefused|HostNotFound|Timeout|Closed|Io`).
  **A clean close by the peer is `Err(Closed)`, never an empty array**, so
  an EOF cannot be silently forgotten; the one legitimate case (a body
  that ends when the connection does) matches `Closed` explicitly.
  `read_exact` exists because length-prefixed protocols need exactly `n`
  bytes however TCP splits them. **Data is `[byte]`**, with
  `read_text`/`write_text` for text protocols: `[byte]` is an array of
  interpreter values, several times the size of raw bytes, which is fine
  for protocol messages and HTTP headers and wrong for bulk data -- a
  packed buffer type is deferred until something real needs one, not
  designed speculatively. Text mode can't carry a 0 byte (Ziman strings
  are NUL-terminated), so `read_text` reports one rather than silently
  truncating. `bytes` holds the string/`[byte]` conversions and the
  big-endian integer encodings binary protocols are built from; only the
  two conversions are native. **Timeouts are minimal but present**
  (`SO_RCVTIMEO`/`SO_SNDTIMEO` via `set_timeout`, `EAGAIN` becomes
  `Timeout`): a database driver without them hangs forever on a dead
  peer. `SIGPIPE` is ignored and writes use `MSG_NOSIGNAL`, so a write to
  a closed peer is a `Closed` error rather than killing the process.
  **Deliberately not here yet**: server-side TLS, Unix-domain sockets,
  UDP, a connect timeout, and IPv6-only concerns beyond `getaddrinfo`
  handling both families. (Client TLS was added afterwards -- see the TLS
  entry below.)
  **The recurring hazard, stated once.** Closing is explicit and there are
  no destructors, so a `?` that returns early between `connect` and
  `close` leaks the socket -- the same problem `db` has with an open
  transaction. Both point at a missing scoped-cleanup feature (something
  like `defer close(s)`), deliberately left as its own later design
  rather than bolted onto `net`. Closing a socket another thread is
  blocked reading wakes that read with an error (`shutdown` first), but
  the descriptor number is then free for the OS to reuse, so close only
  from the owning thread.
  Tested entirely on loopback -- an echo server in a `spawn`ed thread on
  port 0, then five concurrent clients against a thread-per-connection
  accept loop: see `tests/fixtures/net_*`.

- **`crypto` and `pg`: SCRAM-SHA-256 and a PostgreSQL client in Ziman.**
  The second backend behind `datasource.open`, chosen by the same URL-scheme
  dispatch as SQLite. **`crypto` is native and deliberately small**:
  SHA-256, HMAC, PBKDF2, base64 and a byte-wise XOR (no bitwise operators
  in Ziman), self-contained rather than linking libcrypto, verified
  against `sha256sum`/`hashlib` and the RFC vectors. Native because
  PBKDF2 at 4096 iterations is ~33k compression-function calls -- hopeless
  in a tree-walking interpreter, and needed on every connection. It is
  explicitly not a general crypto library (not constant-time, no
  randomness, no ciphers).
  **`pg` is pure Ziman** on `net`+`bytes`+`crypto` (only exact float
  formatting is native), speaking protocol 3.0: startup, trust/cleartext/
  SCRAM-SHA-256 authentication, the simple query protocol for
  parameterless statements -- which is what lets `datasource.execute` run a
  multi-statement DDL script -- and the extended protocol (Parse/Bind/
  Describe/Execute/Sync, pipelined into one write) for bound `$N`
  parameters. **SCRAM is checked both ways**: the client verifies the
  server's signature, so a server that doesn't know the password is
  rejected, not just the reverse. Channel binding is off, the username
  is left to the startup packet, and a non-ASCII password isn't
  SASLprep-normalised (documented).
  **Text format throughout, decoded by the result column's type OID.**
  Integers (`int2/4/8`), floats, `bool` and `bytea` become the matching
  `SqlValue`; everything else -- text, `uuid`, dates and timestamps,
  `numeric`, `json` -- arrives as `Text` and the mapping layer converts,
  exactly as for SQLite. That is what makes the *same* repository SQL,
  schema and `#[derive(Row)]` code run unchanged on both backends. Two
  details exist for that reason: a `Bool` parameter is sent as `1`/`0`
  (accepted for `BOOLEAN` and `INTEGER` columns alike, where `t`/`f` would
  break the latter), and a boolean column read back is `Bool` under
  Postgres and an integer under SQLite, so `sql_as_bool` accepts both. A
  native `timestamptz` column would arrive in Postgres's own text form
  (`2026-09-23 18:20:06.886+00`), not ISO `T...Z`, so timestamps are
  stored as TEXT (as for SQLite) until a real need says otherwise.
  **One connection, one owner thread, a channel of requests.** Ziman has no
  mutex, and the protocol is strictly request/response, so the connection
  is owned by a spawned thread and callers send a `RunRequest` carrying a
  reply channel and block on it. That serialises concurrent callers with
  no lock -- so there is none for a `?` returning early to leave held --
  and it is the actor pattern, now living inside the driver rather than
  the application. `pg.Connection` wraps the channel opaquely; `datasource.Connection`
  is an enum (`Sqlite(sqlite.Connection) | Postgres(pg.Connection) |
  Mysql(mysql.Connection) | Pool(PoolState)`, see the pool entry below) and
  `execute`/`query`/`close` branch on the variant.
  A *transaction* is still server-side per-connection state, so other
  threads' statements interleave inside it, as already documented for
  `db`. After a failed statement the read continues to ReadyForQuery, so
  the connection stays in step for the next request; SQLSTATE class 23
  maps to `Constraint`, 08/28/3D to `Connection`, the rest to `Sql`.
  **Refused rather than degraded**: a `sslmode=require` URL is an error --
  silently dropping to plaintext would be the worse failure. Also not
  supported: MD5 authentication (deprecated), GSS/Kerberos, IPv6 literals in
  the URL, COPY, LISTEN/NOTIFY, and prepared-statement reuse (every
  parameterised statement is parsed afresh). `[byte]` is an array of
  interpreter values, so very large result sets are slow.
  **Tested against a real server**, not a mock: `make test-pg` (see
  `tests/run_pg_tests.sh`) starts a throwaway `postgres:17` container on
  `127.0.0.1:55432`, configures a role per authentication method
  (SCRAM, trust, cleartext), runs `tests/pg/*.zm` and removes the
  container; it is not part of `make test` so the ordinary suite stays
  hermetic.

- **`mysql`: a MySQL/MariaDB client, the third backend behind `datasource.open`.**
  Pure Ziman on `net`+`bytes`+`crypto`, sharing URL parsing with `pg` via a
  new URL parser (first a `url` module, now part of `db`) (`scheme://user:password@host:port/database?k=v`,
  percent-decoded, last `@` splits userinfo from host so a password may
  contain one). **Two authentication plugins**: `mysql_native_password`
  (SHA-1-based) and `caching_sha2_password` (MySQL 8's default, SHA-256-based).
  caching_sha2's "full authentication" -- the first login of a user, before
  the server has cached its hash -- sends the password in clear, so the
  driver refuses it without TLS rather than add an RSA public-key exchange;
  once the server has cached the login a plain connection takes the fast
  path. **Native additions**: `crypto::sha1` (only for the native-password
  scramble, not for anything needing collision resistance) and
  `bytes::f64_from_bytes`/`f32_from_bytes`/`f64_to_bytes` for the binary
  protocol's little-endian IEEE-754 columns and parameters.
  **Two protocols, like `pg`'s simple/extended split**: a parameterless
  statement goes through the text protocol (COM_QUERY, which is what lets
  `datasource.execute` run a multi-statement script and return the last result);
  a statement with `$N` parameters is rewritten to MySQL's positional `?`
  (skipping quoted strings, backtick identifiers and comments) and goes
  through prepare/execute/close, freshly for every call. **Result decoding**
  mirrors the value/error contract SQLite and `pg` already established:
  integers and floats become `Int`/`Float` (a BIGINT UNSIGNED above 2^63 is
  an error, since it doesn't fit a signed `int`), character columns `Text`,
  binary columns (BLOB/VARBINARY/BIT) `Bytes`, everything else (DECIMAL,
  dates, times, JSON, ENUM, SET) `Text` for the mapping layer -- MySQL has
  no boolean type, so a `BOOLEAN` column (really `TINYINT(1)`) reads back
  as `Int` 0/1, same shape as SQLite. Integrity violations (duplicate key,
  foreign key, NOT NULL, CHECK) are `Constraint`; access-denied/unknown-
  host/unknown-database are `Connection`; the rest `Sql`.
  **TLS negotiated the MySQL way** (a capability flag in the login packet
  requests it, then the connection upgrades in place before the rest of
  the handshake), with the same `sslmode`/`sslrootcert` meanings as `pg`.
  Not supported: the compressed protocol, LOCAL INFILE, the RSA exchange,
  client certificates, IPv6 literals, multiple result sets (only the last
  is kept), prepared-statement reuse, and a packet of 16 MiB or more.
  **Tested against a real server**: `make test-mysql` (`tests/run_mysql_tests.sh`)
  starts a throwaway `mysql:8.0` container on `127.0.0.1:55434`, creates a
  `mysql_native_password` and a `caching_sha2_password` user, runs
  `tests/mysql/*.zm` (in an order that exercises the sha2 user's
  first-login-then-cached path) and removes the container; not part of
  `make test`.

- **`defer`: scoped cleanup.** The language answer to a gap `net` and
  `db` both surfaced: with no destructors, a `?` (or `return`) that leaves
  a function between acquiring a resource and releasing it leaks a socket
  or leaves a transaction open. `defer f(args)` runs a call when the
  enclosing block exits, by any path.
  **Why this shape.** The alternatives were RAII/destructors (needs types
  with a drop hook -- a whole mechanism, and Ziman has no interfaces to
  hang one on), and a scoped `using`/`with` construct (needs a closeable
  protocol -- traits again, or closures for `with_transaction(fn)`, which
  Ziman lacks). `defer` needs neither: it is one statement, and it maps
  onto the interpreter's existing structure. **Block-scoped, not
  function-scoped**, deliberately unlike Go, where a `defer` inside a loop
  accumulates until the function returns -- a well-known trap; here a
  loop-body `defer` runs each iteration. **Arguments are evaluated when
  the `defer` statement runs** (Go's rule), so `defer close(s)` means what
  it says even if a `var` changes later; only the call is postponed.
  **A single call, and only a call**, not a block: a block would read
  variables at *exit* time, contradicting the argument rule, and would
  need the two to be reconciled; a helper function you defer a call to
  covers cleanup that needs logic, and a block form can be added later
  without breaking anything.
  **Mechanics -- almost nothing new at runtime.** Every way out of a
  block already funnels through two places: `call_function`'s body loop
  and `exec_stmts_in_scope` (every `{}` and every `for` iteration), and a
  scope's `Env` is exactly one block. So `Env` gained a list of pending
  calls, `STMT_DEFER` appends to it, and those two places run it last-in
  first-out before the scope is freed. `return`, `?`, `break` and
  `continue` all already unwind through them as ordinary control results,
  so none needed touching. **Arguments are captured by desugaring in the
  type checker**: `check_defer` type-checks the call like any expression
  statement (so named arguments, Option/Result auto-wrap, generics and
  variadics all work), then builds a *replay* call -- a copy whose
  arguments are references to hidden `$defer<N>$<i>` variables. At
  `defer` time the interpreter evaluates the real arguments into those
  variables and registers the replay; at exit it just evaluates the
  replay. That reuses `eval_expr`'s whole call path (builtin dispatch,
  natives, generics fixups, variadic collapsing) instead of duplicating
  or splitting it into "evaluate arguments" and "invoke" halves. The
  four builtins that inspect their argument *expressions* rather than
  values (`format` wants a literal template; `ok`/`err`/`channel_new` take
  their type from context) can't be replayed and are rejected.
  **The return value is computed before the defers run**, and a deferred
  call's result is **discarded, a `Result` included** -- the one place Ziman
  silently drops one. A deferred `net.close`/`datasource.rollback` has nowhere to
  report failure, and it is what makes `defer datasource.rollback(conn)` harmless
  after a successful `commit` (SQLite errors on a rollback with no
  transaction open; the discarded result swallows that). The cost is real:
  a failing cleanup call is invisible. **Fatal runtime errors skip defers**:
  `unwrap` on an `Err` or an out-of-bounds index exits the process rather
  than unwinding, so nothing deferred runs -- the OS closes the sockets and
  a database server rolls back an abandoned transaction, but it is a
  documented limit, not a guarantee. **What it does not fix**: a
  transaction on a *shared* connection can still be interleaved by another
  thread; `defer` fixes the early-return leak, not concurrency.
  See `tests/fixtures/defer_*` -- ordering, every exit path, argument
  timing, loop scope, threads, the transaction-rollback case against
  SQLite and the socket-closed case observed from the peer.

- **TLS (client side), in `net`, on the system OpenSSL.** Built so
  hosted Postgres -- and, later, an HTTPS client -- can be reached.
  **Library**: linking the system `libssl`/`libcrypto` (`-lssl -lcrypto`),
  the same trade as SQLite (a system dependency, the same portability
  caveat) -- implementing TLS ourselves would be huge and unsafe, which is
  the opposite of what a security feature should be. Ziman's own `crypto`
  module stays as it is: small, and only needs to be correct for SCRAM.
  **In-place upgrade, not a new handle type.** A native handle table entry
  gained an optional `SSL *`; `read`/`read_exact`/`write`/`read_text`/
  `close` all go through one `io_read`/`io_write` layer that uses it when
  present, so `set_timeout`, `read_exact` across record boundaries, `Closed`
  on a peer's close and `Timeout` all keep working over TLS and nothing
  built on `Socket` -- `pg`, an HTTP layer -- changes. In-place is also
  exactly what Postgres needs (`SSLRequest`, a one-byte answer, then
  upgrade the same connection), as do STARTTLS-style protocols generally;
  `connect_tls` is just `connect` + `start_tls`, closing the socket if
  the handshake fails. **Verification defaults to on** -- chain against the
  system store *and* host name (an IP literal is matched against IP SANs),
  TLS 1.2 minimum, SNI sent -- and turning it down is a deliberate call to
  the `_with` forms with a `TlsVerify` (`Full`/`Chain`/`Off`) and an
  optional CA file, never a silent default. **Errors**: `NetError` gained
  `Tls(message)` carrying OpenSSL's reason plus the verify result ("self-
  signed certificate", "IP address mismatch"); a receive timeout is
  distinguished from a TLS error (OpenSSL reports it as WANT_READ /
  SYSCALL+EAGAIN), and a peer that closes without a `close_notify` is
  `Closed`. A TLS handle has a lock around each operation, because OpenSSL
  forbids two threads using one `SSL` at once -- safe, at the cost that a
  blocked read delays a write on the same socket.
  **Postgres `sslmode` follows libpq**, on purpose, so anyone who knows
  one knows the other: `disable`, `prefer`, `require` (encrypted but the
  certificate is **not verified** -- documented as open to a man in the
  middle, exactly libpq's behaviour, not quietly made stricter), `verify-ca`
  and `verify-full`, plus `sslrootcert=<path>`. Two deliberate differences:
  the default is `disable` (libpq's is `prefer`) so existing plaintext
  URLs keep working and a downgrade-prone default isn't introduced, and
  with no `sslrootcert` the system CA store is used rather than
  `~/.postgresql/root.crt`. `allow` is refused, and a `prefer` handshake
  that fails is an error, not a retry over plaintext.
  **Tested for real**: `make test-tls` runs the client against two local
  `openssl s_server` instances with freshly generated self-signed
  certificates (its own CA each) -- full verification by IP and by name, an
  unknown CA, the wrong CA, a host-name mismatch, chain-only, off, a bad CA
  file, a page read to EOF, `read_exact` across records, a timeout, double
  upgrade, and a handshake against a non-TLS server; `make test-pg` now
  runs the Postgres container with TLS enabled and has the *server* confirm
  encryption via `pg_stat_ssl` for each `sslmode`, plus a fake server that
  answers `N` to prove `require` refuses and `prefer` falls back to
  plaintext. Neither is part of `make test`, which stays hermetic.
  **Not yet**: server-side TLS (`listen_tls` -- an HTTPS server will need
  it), client certificates, ALPN, and session resumption.

- **Connection pool (`datasource.open_pool`, `acquire`, `release`).** Built so a
  program isn't limited to one connection serialising every statement,
  and so a transaction can finally be safe.
  **The mechanism is a channel of idle connections**: borrowing is
  `receive` (which blocks while every connection is busy -- backpressure
  for free), returning is `send`. No mutex, no new primitive, and it works
  for both backends because a "connection" is already just a value.
  `defer` is what makes it usable rather than a leak waiting to happen:
  `defer datasource.release(pool, tx)` returns the connection on every way out,
  including a `?` that propagates an error -- which is exactly the hazard
  that had to be solved first.
  **A pool-backed `Connection`, not a separate `Pool` type**, so repository
  code written against `Connection` runs unchanged: `Connection` is an enum
  (`Sqlite(sqlite.Connection) | Postgres(pg.Connection) | Pool(PoolState)`,
  each driver's own opaque struct, `PoolState` holding the idle-connection
  channel and size), and `execute`/`query` on a pool borrow a connection for that one statement
  (`query_one` and the rest go through those). The type is recursive
  through a `Channel`, the same way `Json` is recursive through an array,
  and the checker accepted it without change. **A unit of work spanning
  several statements borrows exclusively with `acquire`**, which returns an
  *ordinary non-pool* `Connection`: repositories called with it use it
  directly and never borrow again, so a small pool can't deadlock on
  nested borrows, and on a plain connection `acquire` is the identity and
  `release` a no-op, so code written for a pool runs against either.
  **`begin`/`commit`/`rollback` on the pool itself are refused**: each
  would borrow a *different* connection and silently do the wrong thing,
  so it is an error that tells you to `acquire`. This is the property
  the design exists for: an exclusively borrowed connection can't be
  interleaved by another thread, which is what a shared single connection
  could never promise.
  **Reconnect, done lazily:** a statement that fails with
  `DbError.Connection` (a network error, or a server-side class 08/28/3D/57
  or MySQL shutdown code) has its connection closed and replaced by a fresh
  one from the pool's URL (`PoolState.url`); if the server is still down the
  dead one goes back so the pool never shrinks. The failed statement is not
  re-sent, because it may have executed and a retried INSERT would repeat
  it. `datasource.discard` does the same for an `acquire`d connection, and
  `with_transaction` calls it on a connection-class failure. A `SELECT 1` on
  every borrow would cost a round trip per statement, so only a connection idle for `check_after`
  seconds (default 30; `open_pool_with` sets it, 0 disables) is tested when lent out: the idle channel
  holds `IdleConnection { connection, since }` stamped when a connection is parked, and a failed probe
  replaces the connection before the borrower sees it (if no replacement can be opened the dead one
  goes back and the borrower gets the Err). A connection that dies while in use, or before the idle
  limit, still costs one failed statement, after which the connection is replaced. **Acquire
  timeout:** `acquire_timeout` polls `try_receive` every 2 ms, since channel
  `receive` has no timeout; a leaked borrow blocks plain `acquire` forever
  (`defer release` is the guard). **Still v1:** no min/max sizing or
  idle timeout, connections open eagerly and fail fast, `datasource.close(pool)`
  waits for every borrow to come back before closing, and a connection
  released mid-transaction goes back dirty (roll it back first; a `defer
  datasource.rollback` registered after the release defer runs before it).
  **A pool over `sqlite::memory:` is a single connection** whatever size is
  asked for, because every in-memory SQLite connection is its own private
  database -- N of them would be N different databases. (A shared-cache
  URI would allow real pooling; that is a follow-up.) For a SQLite *file*
  the native open now sets a five-second busy timeout, since several
  connections contend for SQLite's single writer lock and would otherwise
  fail at once with `SQLITE_BUSY`; a read-modify-write transaction there
  should start with `BEGIN IMMEDIATE`, as a deferred `BEGIN` that reads
  then writes can deadlock two connections against each other.
  **A `clock.sleep_millis` came along** because the tests needed connections
  held long enough to overlap, and it is useful in its own right (retry
  backoff); it blocks only the calling thread.
  **Tested**: `tests/fixtures/db_pool_sqlite` (hermetic, against a SQLite
  file: ten workers on three connections with the distinct-connection
  count bounded, 8 threads x 10 read-modify-write transactions losing no
  updates, early-return release on a pool of one, refusal of a transaction
  on the pool, `acquire` on a plain connection, the `:memory:` pool, bad
  sizes -- stable across 15 runs) and `tests/pg/pg_pool` (against real
  Postgres: 12 workers on 4 connections served by 2-4 distinct backend
  PIDs, per-statement borrows from many threads, and 80/80 increments
  under `SELECT ... FOR UPDATE`).

- **`http`: an HTTP/1.1 server toolkit, and the `\r` string escape.**
  **A toolkit, because there were no function values.** (Superseded: with
  R13's function values `server.serve(listener, ctx, handler, options)` now
  exists and a route table of handler values is possible; the reasoning below
  is the original one.) A framework's
  central move is `serve(listener, handler)` -- a library taking your
  code as an argument -- and Ziman can't express that: no closures, no
  first-class functions. So the application owns the two things that need
  its code (the accept loop and a `route` function), and `http` owns
  everything they call. This is the same constraint that shaped the
  router: **a hand-written `route` with one-line matchers (`server.route(req,
  "GET", "/patients/{id}")` returning `Option<Params>`), not a route table**
  -- a table can't hold the handler either, so it would only save the
  matching while forcing a stringly-typed name-based dispatch and a second
  place that has to agree with the first. When first-class functions
  arrive, a real table can be built on the same matcher without breaking
  it.
  **Reading is pure Ziman on `net`**: `read_request` accumulates chunks
  until CRLF CRLF, parses the request line and headers, then the
  `Content-Length` body -- and because values are immutable it returns an
  `Exchange` carrying the *updated `Session`*, whose buffer holds any
  bytes read past this request, which is what makes pipelining and
  keep-alive correct rather than lucky. `Expect: 100-continue` is answered
  before the body is read. **Deliberately bounded**: 16 KiB of headers, 1
  MiB of body, so an untrusted client can't grow a handler's memory
  without limit, and a read timeout (set on the socket by the
  application) turns a stalled client into a 408. **Refused rather than
  guessed at**: chunked request bodies (501), HTTP versions other than
  1.0/1.1, a NUL byte in the request (bodies are `string`s), a missing or
  non-numeric `Content-Length`.
  **`write_error` exists because an unreadable request has no `Request`**
  to hand `write_response` for its keep-alive decision; it always says
  `Connection: close`. `write_response` frames the response itself
  (`Content-Length`, `Connection`) and reports whether the connection
  stays open, so the connection loop is a handful of lines. `HEAD` gets
  the headers, with the length the body would have had. Case-insensitive
  header lookup is `http.header`.
  **Path parameters are strings on purpose**: converting `{id}` to a
  `Uuid` and answering 400 on a bad one is the application's job -- the
  same layering `request.zm` already has for JSON bodies. `param` on a name
  the pattern lacks is fatal, since that is a bug in the route function.
  **The `\r` escape** was added first because HTTP is CRLF: string literals
  can now write `"\r\n"`, where before the framing had to be built from
  bytes (the TLS tests still do, being older). It also let `json` accept
  CRLF line endings between tokens and round-trip `\r`, which a server
  taking JSON from arbitrary clients needs.
  **Tested end to end over real sockets** (`tests/fixtures/http_server`, a
  server written the way an application would write it, driven by a
  raw-socket client so the wire format itself is what's asserted): framing,
  keep-alive, pipelining, POST bodies, percent-decoded params, query
  strings, 404/405+`Allow`, `HEAD`, `Connection: close`, HTTP/1.0,
  `100-continue`, malformed requests (400), chunked (501), oversized body
  (413) and headers (431), a stalled request (408), and six concurrent
  clients -- stable across 10 runs; `http_routing` covers the matcher's
  edge cases. **Not yet**: TLS (`listen_tls`), chunked request bodies,
  HTTP/2, multipart, compression, `Date`/`Server` headers.
  **A side fix found by running a real server: stdout is now
  line-buffered** (`setvbuf(stdout, NULL, _IOLBF, 0)` at startup). C's
  default when stdout is a file or a pipe is *fully* buffered, so a server
  logging each request showed nothing until 4 KiB had accumulated or the
  process exited -- invisible while developing, and fatal for anything
  watching a log. Line buffering costs a little on a print-heavy program
  writing to a file, and is the right trade for a language whose main
  showcase is now a long-running server.

## Deliberately deferred (not v0)

(Items that used to be listed here and have since shipped: multi-file
modules and `use` imports, structured `--json` diagnostics, and a lint
(`--lint`); see ROADMAP.md.)

- Effects visible in function signatures (which functions do I/O or mutate
  outside their own frame) — real value, real design cost, tracked in
  ROADMAP.md.
- A package manager or build system beyond a Makefile.

## Non-goals

- Ziman is not trying to be a tree-native/structural-editing language (see
  the earlier discussion in this project's originating conversation for
  why that's a much larger, more novel effort). Source of truth is text.
- Not chasing raw performance in v0 — it's a tree-walking interpreter.
  Compilation to bytecode or native code is a later milestone if the
  language design proves out.

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
sentence that fixes it. Probed on 2026-09-24, each habit now does:
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
continuation one level. It needs only lexable text (an unterminated string
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
layout, so `tests/fixtures` is left as written.

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
phase, in-memory SQLite; 16 cores):

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

## Formatter: wrapping, comment alignment, sorted `use`, stdin (open item 9)

The formatter (see "The formatter" above) gained the four things it lacked, all in
the same text-level design, still self-checked for token equality.

**Line wrapping.** The formatter used to keep every line break where the author put
it. Now a line wider than the width (`--width=N`, default 120, 0 = never; comments
are not counted) is broken after a comma of its *outermost* bracket group, chosen
greedily as the last comma that keeps the first part within the width, and the
process repeats on the continuation until every line fits or has no legal break.
"Legal" is what the parser already accepts a newline after: a comma inside a call,
an array literal or a struct literal. It skips commas inside a generic list
(`Result<int, string>`: tracked as unmatched `<`, which also covers a comparison),
inside `fn(A, B) -> R` types, and in `#[...]` attributes, where a newline would not
parse; a line with no legal break is left long. The wrapper works by setting the
next item's "newline before" flag and re-emitting, so indentation of the
continuation comes from the existing bracket rule, and it never joins lines, which
keeps the formatter idempotent (checked over the library and backend). Reformatting
the library and the sample backend with it changed 33 files and every test still
passes, which is the check that the breaks are all places the parser accepts.

**Trailing comments** on consecutive code lines are aligned to one column (two
spaces after the longest line of the run); a blank line or a line without a trailing
comment starts a new run.

**`use` lines**: a run of consecutive `use` lines (no blank line, no comment line
between) is sorted by text, so `use http` precedes `use http.Request`. Order has no
meaning in Ziman (imports resolve after all are read), so this is safe; the
self-check compares against the sorted sequence.

**stdin**: `ziman --fmt -` reads stdin and always writes stdout, for an editor's
"format buffer". Still not done: joining lines that would now fit, wrapping at
binary operators or `&&`/`||`, wrapping long `use` or `if` conditions, and an LSP
"format document" (item 10).

## The language server

`ziman --lsp` (src/lsp.c) reuses the compiler front end rather than a second
implementation. Three small hooks make that possible without `exit(1)`: an error
sink (`ziman_set_error_sink`) that collects diagnostics instead of printing them
and unwinds to an outermost recovery point; loader source overrides
(`loader_set_overrides`) so unsaved editor buffers are what gets parsed; and
`typecheck_skip_main_check` so a library file is not reported for lacking `main`.
Each edit re-analyses the whole program (entry = nearest `main.zm`, else the file)
and republishes per-file diagnostics; declarations carry `source_file`, which is
what go-to-definition, hover and symbols read. Positions convert between Ziman's
byte columns and LSP's UTF-16 units. Hover and definition resolve by identifier
(plus a `module.` qualifier), not by scope, so a local shadowing a global name
resolves to the global.

## Map and Set

`Map<K, V>` and `Set<T>` replaced `collections.HashMap`/`HashSet`. As pure-Ziman
library types over immutable arrays every write copied the whole bucket array
(O(bucket_count)), and with no traits there was no way to give a TreeMap or any other
map the same API without duplicating every function. Making them built-in fixes both.

- **Persistent, not mutable.** The storage (src/mapobj.c) is a 32-way hash trie with
  bitmap-compressed nodes. `set`/`insert`/`remove` copy only the nodes on the path to the
  entry and share the rest, so the collections keep value semantics exactly like arrays
  (`val b = set(a, k, v)` leaves `a` alone) with near-O(1) writes. A mutable handle, like
  `StringBuilder`, was rejected: it would have made `Map` the one data type whose copies
  alias.
- **Insertion order.** Each entry stores the sequence number it was first inserted with;
  iteration collects and sorts by it (O(n log n), next to an O(n) loop body). Chosen over
  hash order so `print`, `for` and the key/value arrays are deterministic and unsurprising.
- **Surface.** Types are the reserved words `Map`/`Set` (like `Channel`, so
  a user `struct Map` is now an error). Literals are `Map<K, V> { k: v }` and `Set<T> { a }`
  with the type arguments written out -- consistent with "explicit over implicit", and it
  makes the empty literal need no inference. They parse into calls to the internal builtins
  `$map_lit`/`$set_lit`, so the loader and generic machinery needed no new node kinds.
  `len`, `contains`, `set`, `insert`, `remove` were extended by argument type; `map_get`,
  `map_keys` (returns a `Set<K>`), `map_values` and `set_items` are builtins rather than prelude generics
  because a generic native would need `map_get<string, int>(...)` (Ziman has no inference).
  `for k, v in m` reuses the existing two-binder `for`.
- **`for` walks a snapshot** (a sorted entry array taken when the loop starts), so the body
  may rebind the collection without a hazard to define.
- **Keys** must satisfy the `Hash` rule, checked wherever the type is written.
- **Equality and hash are order-independent**: `==` is same size plus every key of one
  found in the other with an equal value; the hash sums a mixed hash of each entry, which
  is commutative, so equal maps hash equal whatever order they were built in. That makes a
  map or set a legal key, struct field or set element.
- **Json:** `derive(Json)` maps a `Map<string|int, V>` to a JSON object (an int key as its
  decimal text, decoded with `parse_int`) and a `Set<T>` to an array. `Row`/`Properties`
  don't support them. **Not offered yet:** set algebra and other helpers (union,
  intersection, filter, ... -- see docs/ROADMAP.md), and a sorted `TreeMap`.

## JSON on Map

`Json.JObject` was `[JField]`, an array of `{key, value}`. It is now `Map<string, Json>`:
lookup goes from a scan to near-O(1), a duplicate key gets a defined meaning (last wins,
`parse_json_strict` rejects), building an object one key at a time no longer copies an
array per key, and the insertion-order iteration of `Map` keeps document order so output
is stable. `JField` is gone; derived code builds a `Map` literal and decodes through
`map_get`. A `JInt(int)` variant was added so integers round-trip exactly (`JNumber` is a
float, which loses precision above 2^53); `derive(Json)` writes `int` fields as `JInt`
and reads them with `int_from_json` (which also accepts a whole-valued `JNumber`).
Strings gained `\uXXXX`, `\b`, `\f` in both directions. Still true: `Json` has no `==`
(arrays only compare scalar elements); a recursive-type equality would also need a
cycle guard in the type checker's structural walk.

## Serialization formats

`ndjson`, `csv`, `toml` and `yaml` are converters between text and the `Json` tree (which
is the format-neutral data model; see "JSON on Map"), not new derives -- so a type's one
`#[derive(Json)]` serves every format and adding a format never touches `derive.c`. They
are explicit (`person_from_json(toml.parse_toml(text)?)`) rather than generated
`person_from_toml`, which would multiply derive output per format.

Choices worth recording. *Ambiguity is resolved toward safe:* a CSV or YAML number with a
leading zero (`007`, a zip) stays a string; the writers quote exactly the strings that would
re-read as another type, so text round-trips. *Unsupported syntax is an error, never a
misreading:* YAML anchors/aliases/tags/merge keys/multiple documents/tab indentation, TOML
`inf`/`nan`. *What the `Json` model cannot say is flattened or dropped, documented:* TOML/CSV
`null` is omitted/empty, dates are strings, CSV nests one level as dotted columns. YAML block
structure is parsed by first lifting each `- ` onto a line of its own, which turns "a sequence
item that is a mapping" into ordinary nested indentation -- the parser never needs a
special case for it. Speed is the interpreter's: these are for configuration and
export-sized data.

## MapBuilder and SetBuilder

A persistent `Map`/`Set` copies the trie nodes on the path to every changed entry, which is
cheap per call but adds up when a loop builds a whole map: measured in the non-GC build,
`collections.group_by` and `distinct` on 1,000 items produced 4.3 MB and 1.2 MB of garbage.
`MapBuilder<K, V>` and `SetBuilder<T>` are the mutable counterparts (like `ArrayBuilder` and
`StringBuilder`): a handle to a private trie that `mapb_set`/`setb_add` change in place
(`mapobj_set_mut`; growing a node is a `realloc`, which frees the old block). Two decisions:
`to_map`/`to_set` *snapshot* (an O(n) copy of the nodes, leaves shared) rather than freeze, so a
builder stays usable like `ab_to_array`, and a `shared` flag makes an overwrite after a snapshot
replace the leaf instead of updating it, so a snapshot never changes. Removal shrinks a node
without collapsing single-leaf children (still correct; lookups depend only on the hash path).
The persistent `Map` stays the default for ordinary code and for anything that keeps old
versions; builders are for loops that only add. Used in `collections.distinct`/`group_by`, the
`json`, `csv` and `yaml` object builders, and `#[derive(Json)]`'s map and set fields.

## Native handles and finalizers

A `Socket` or a SQLite `Connection` is an integer in Ziman code. It used to be an index into a
global table, which kept every socket and connection ever opened reachable forever: forgetting
`net.close` or `datasource.close` leaked the descriptor or the database for good (measured: 300 unclosed
connects exhausted `ulimit -n 300`; each unclosed in-memory database held ~88 KB). The integer is
now the native object's *address*, and a `HandleSet` (src/handles.c, a hash set in memory the
collector does not scan) says which addresses are open, so a closed or made-up handle is still an
error and not a crash, while being in the set does not keep the object alive. When nothing holds the
Socket/Connection value any more the collector finalizes it (closing the descriptor, freeing the SSL
object, closing the database), and out of descriptors (`EMFILE`) `net` collects at once and retries.
`os.collect_garbage()` runs a collection and the finalizers on demand. The non-collecting build has
no finalizers: it leaks by design.

`pg` and `mysql` connections own a thread blocked on a request channel. Their `Connection` holds a
`Guard` (prelude): a native whose finalizer sends the connection's "close" request when the
Connection value is unreachable, so that thread closes the socket and exits instead of waiting
forever. Still true: close promptly -- a collection may be far off, and a file database stays locked
until it runs.

## Stack depth (2026-09)

Deep recursion used to run off the end of the C stack and crash with no message (about 1,450 calls on the default 8 MB). The interpreter is tree-walking and
recurses in C, so the fix is not to count Ziman calls but to guard the C stack: `src/stack.c` reads each thread's real bounds once (`pthread_getattr_np`), keeps
the low limit plus a margin in a thread-local, and `ZIMAN_STACK_CHECK()` (one compare against the address of a local) runs at every call, statement, expression
and parse/check step. Measured on the prototype: within noise on call-, loop- and mixed-heavy programs, and no cost per spawn. A Ziman call costs 3.5-11 KB of C stack
depending on its shape, which is why a frame count would have been the wrong limit.

The stack sizes are decided at run time (`stack.h`): the main interpreter thread runs on a created thread whose stack, and the stack of each `spawn`, come from an eighth of
the memory the process may use, split by the threads it plans for, with options to override. The 768 MB cap is the garbage collector's: Boehm aborted once a thread's stack
was near 1 GB. Deeper causes are not investigated (a `GC_allocobj` failure with a huge reserved stack); the cap keeps every accepted configuration on the working side.
Not done: a fatal error in a spawned thread still ends the whole process (task A2).

## A runtime error ends the thread it happened in (2026-09)

`ziman_error` used to exit the process, so one bad request in a thread-per-connection server was an outage. A spawned thread now arms a `ThreadBoundary` (error.h) before it
runs: a *runtime* error on that thread is not printed and does not exit, it is stored as text and the thread `longjmp`s back to its entry, where `thread_failed` (interpreter.c)
calls the optional `on_error` handler -- a `fn(string)` value, chosen per spawn: `spawn f(x) on_error handler` -- or, with none, writes the error to stderr; then runs the
deferred calls the thread had pending. The jump skips the block exits that would have run them, so scopes are put on a thread-local list at their first `defer` and removed when
their calls have run normally. The handler runs before the deferred calls, while the thread's resources are still open (the server writes the 500 to a socket its own `defer`
has not closed yet). Errors of other phases, and errors on the main thread, are unchanged. `os.fatal` is the handler that keeps the old behaviour for a thread.
Not done: nothing catches a *native* crash (a segfault); and an error while a native holds a lock would leave it held (none does).

## `push` is amortised O(1) (2026-09)

An `ArrayObj` built by `push` carries an `ArrayStore { capacity, used }` shared by every array that views the same buffer. `push` on an array of length n whose store has room
and whose `used == n` (it is the newest version, the "tip") claims slot n with a compare-and-swap on `used`, writes the element there and returns a new `ArrayObj` of length n+1
over the same buffer; a losing thread, or a `push` on an older version, copies into a fresh buffer of twice the size instead. Slots below an array's length are never written
again, so arrays stay immutable values and threads need no lock. Literals, slices and builder results have no store: their first `push` copies. Loops that rebuild a value with
`concat`/`concat_arrays` stay quadratic; `--lint` flags them.

## `try` hoisting (2026-09)

`try expr` is the prefix spelling of `expr?` and, unlike it, may be nested. It is a parser rewrite, so the type checker and the interpreter keep the statement-level `?` only: `parse_block` runs `hoist_stmt` on
each statement it parses, which replaces every prefix `try` in the positions that run once and unconditionally (val/var initializer, assignment, return value, `if` condition, `switch` subject, `for` range or
iterable) by a hidden `val $tN = inner?` placed in front. So that the order of effects stays the written one, everything the statement evaluates before a `try` and that is not pure (any call, an index, a
division) is hoisted into a temporary as well; `hoist_expr` carries a `later` flag for a subtree that some later part of the statement's `try` has to wait for. Literals, identifiers, lambdas and arithmetic on
them stay put. A `try` in a position the hoister does not handle (`else if` and `while` conditions, the right of `&&`/`||`, `defer`/`spawn` arguments, an `if` or `switch` used as a value) is left in the tree and
refused by the type checker, so no conditional evaluation is ever changed. A lambda body is its own function and hoists on its own. Named arguments are hoisted in the order written, though `check_call`
later reorders them into parameter order. The formatter and the language server work on tokens and are not affected. The postfix `?` deliberately stays statement-level: the eye finds `try` at the left of the
line, and there is one spelling of each early exit per position, not two ways to nest.
