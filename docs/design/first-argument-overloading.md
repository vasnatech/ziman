# Overloading available to a declaration

A design for letting several `fn`/`native fn` declarations share a name, so that the dispatch
Ziman already performs in 50 places stops being hand-written in `src/typecheck.c` and starts being
read off the declarations.

| | |
|---|---|
| Status | **Superseded 2026-10-19 by [`full-overloading.md`](full-overloading.md)**, after the user chose full resolution over first-parameter. Kept for §3, which is the measurement that made the choice: the first-parameter rule would have rejected five sets the language already ships. Nothing here was built. |
| Raised | 2026-10-18, from "now that we have quite a number of overloaded functions, what do you think on relaxing the no-overloading rule?" (`docs/ROADMAP.md`, the entry this expands) |
| Written | 2026-10-19 |
| Measurements | taken from `lib/prelude.zm` and `src/typecheck.c` at `6a69848`, by script, not by eye |

## A warning about this document

The ROADMAP entry says the case should be written down and left to sit, which is the habit that
declined `..=` and the handle-validation redesign. **This document does not overturn that.** It
exists so that the sitting is done against a real design rather than a sketch, and its most useful
output is not the plan but §3: *the rule as proposed does not cover seven of the fifty sets that
already exist*, which was not known when the entry was written.

If this is built, §3 is the part to re-derive first — it is a measurement, and measurements move.

---

## 1. What the language actually does today

**Ziman is not missing argument-based overloading. It is missing overloading available to a
declaration.** Measured at `6a69848`:

```
50   overloaded names in lib/prelude.zm
131  declarations in those sets
70   hand-written dispatch arms in src/typecheck.c
```

`len` and `contains` have nine declarations each, `str` six, `snapshot`/`remove`/`mutable`/
`concat`/`clear`/`abs` four, `insert` three, and 38 names have two.

Every one of those sets is resolved by a hand-written arm. The `builtin fn` lines in
`lib/prelude.zm` are **documentation the arms are free to contradict** — `tools/check_builtins.py`
exists only because they drifted apart (`str` documented "accepts any type" while its arm refused
`string`).

Two mechanisms carry the 50 sets, and the difference matters for what this change deletes:

**A. Table-driven prefix dispatch** — the `WORLD_PAIRED` loop (`typecheck.c:1968`). 28 names.
It builds the target by *string concatenation*:

```c
if (target_type.kind == TY_ARRAY)                prefix = "array_";
else if (... "MutableArray" ...)                 prefix = "mutable_array_";
```

so `map(b, f)` becomes a call to the literal name `"mutable_array_" + "map"`. This is why the
prefix layer exists: 64 `array_`/`mutable_array_` declarations whose only job is to be the target
of a concatenation. 60 of them are now invisible (`internal`, F173, or plain non-`pub` `fn`s).

**B. Bespoke arms** — `concat`, `mutable`, `str`, `len`, `abs` and the rest. One `if (strcmp(name,
"...") == 0)` block each, reading argument types directly.

## 2. The proposed rule

From the ROADMAP entry, unchanged:

> Several `fn`/`native fn` declarations may share a name **when they differ in the first
> parameter's type**, and the checker resolves from the declarations. Overload sets that overlap
> on argument one are a compile error. No return-type dispatch, no ranking, no implicit
> conversions.

The evidence for this shape is good and is not re-argued here: first-argument dispatch sufficed for
all 28 paired names; the one case it could not express — `to_set` against `to_mutable_set`, same
argument types, differing only in return type — is the case where the restriction gave the *right*
answer, and they stayed two names (`prelude.zm:801`, `:1458`).

## 3. The rule does not cover seven of the fifty sets

This is the finding. Checked by script against every overload set in the prelude: **43 sets satisfy
the rule as written, 7 do not**, for two distinct reasons.

### 3a. Five sets differ on argument *two*, not argument one

```
mutable_set_union<T>(s: MutableSet<T>, other: Set<T>)
mutable_set_union<T>(s: MutableSet<T>, other: MutableSet<T>)
```

and identically `mutable_set_intersect`, `mutable_set_difference`, `mutable_set_is_subset`,
`mutable_map_set_all`. Argument one is `MutableSet<T>` in both arms; the choice is made by argument
two, which is "the other collection, from either world".

Under the rule as written these **overlap on argument one and are therefore a compile error** — the
rule would reject five sets the language ships today.

### 3b. `concat` cannot be an overload set at all

Four declarations (`prelude.zm:68-75`), and two separate problems:

```
concat(pieces: variadic string) -> string
concat<T>(parts: variadic [T]) -> [T]
concat<T>(b: MutableArray<T>, parts: variadic [T])
concat<T>(b: MutableArray<T>, parts: variadic MutableArray<T>)
```

The last two collide on argument one (3a again). Worse, the first two are separated by a rule that
is not about types at all: **one argument that is an array spreads.** `concat(["a", "b"])` is the
string `"ab"`, never a copy of the array; two or more arguments are always parts. That is
*arity-dependent reinterpretation of the same argument type*, and no per-parameter signature rule
can state it.

`concat` is also the set the ROADMAP names as the warning — a `[string]` is both "the pieces of a
string" and "one array of parts", and only having two names kept those apart. **The honest
conclusion is that `concat` keeps its arm** under any version of this feature.

### 3c. `mutable` has a catch-all arm

```
mutable<T>(items: [T])            -> MutableArray<T>
mutable<K, V>(entries: Map<K, V>) -> MutableMap<K, V>
mutable<T: Eq + Hash>(items: Set<T>) -> MutableSet<T>
mutable<T>(item: T)               -> Mutable<T>      <-- matches everything above
```

The fourth matches every call the other three match. Resolving it needs *specificity ranking* —
"a concrete type beats a bare type variable" — which the proposed rule explicitly forbids ("no
ranking").

### What follows

Three options, and the doc recommends the third:

1. **Keep the rule and shrink the ambition.** 43 sets convert, 7 keep their arms. The prefix layer
   still dies (all 28 paired names are in the clean 43). But `typecheck.c` keeps a dispatch
   mechanism *and* gains a resolver, which is worse than either alone.
2. **Extend to all parameters.** Resolve left to right: first on argument one, then argument two
   among the survivors. Covers 3a and most of 3b. But it is a materially larger feature, and the
   ROADMAP's reason for first-argument-only was that it is the boundary F148 held.
3. **Extend to argument one *and two*, and nothing further; `concat` and `mutable` keep their
   arms.** Covers 48 of 50. Two arms remain, both documented as exceptions with the reason
   attached, neither of them a pairing question.

**Recommended rule, revised:**

> Several `fn`/`native fn` declarations may share a name **when they differ in the type of their
> first parameter, or — when their first parameters are identical — of their second.** The checker
> resolves from the declarations, left to right, with no further positions consulted. A set whose
> members are still indistinguishable after argument two is a compile error at the *declaration*,
> naming both. A parameter whose type is a bare type variable makes the set ambiguous: no
> specificity ranking, no return-type dispatch, no implicit conversions.

Under this rule, `mutable`'s catch-all is rejected at the declaration, which is correct and useful:
it tells the author the set cannot be expressed and must stay two names or an arm.

## 4. Resolution

At a call `f(a1, a2, ...)`:

1. Collect the set named `f` visible at the call site. One member → today's path exactly, unchanged.
2. Check `a1`. Keep members whose first parameter type unifies with it.
3. **Zero survivors** → "no overload of `f` takes a *T* as its first argument", listing each
   member's first parameter type.
4. **One survivor** → resolved. Check remaining arguments against it, and report mismatches against
   *that member's* signature, not against the set.
5. **More than one** → check `a2` the same way. Zero → the same error naming argument two. One →
   resolved. More than one → ambiguity (§5).

Unification is the existing rule, not a new one: a type argument written at the call (`map<int,
string>(...)`) constrains before the set is narrowed, which is also what fixes `sort<int>([])`
being unspellable today — a builtin takes neither type arguments nor named arguments, and a real
declaration takes both.

**The empty literal keeps its exception.** `[]` has no element type until something supplies one,
so it cannot be checked before the set is narrowed. Today the `WORLD_PAIRED` loop dispatches it to
the array half unchecked, with a comment recording that checking first made `first<int>([])` and
`map<int, int>([], f)` errors they had not been. **The resolver must keep that**: an empty array
literal in argument one selects the `[T]` member without being checked. This is the single most
likely regression in the whole change, and it has fixtures.

## 5. Ambiguity, and the error that explains it

**This is the part that must be right.** A model recovers from a compile error only when the error
says what to write instead; the language's stated stance is to refuse features whose failure mode
is "two readings, silently", and an ambiguous overload is exactly that shape unless the diagnostic
carries it.

Two distinct errors, at two different times:

**At the declaration** — a set that cannot be resolved, caught when the module is checked, before
any call exists:

```
error: 'mutable' cannot be an overload set -- 'mutable(item: T)' takes a bare type
       variable as its first parameter, so it matches every call the other three also match
  |
  |  builtin fn mutable<T>(item: T) -> Mutable<T>
  |                        ^
  = the other members are mutable([T]), mutable(Map<K, V>), mutable(Set<T>)
  = a set is resolved by the first parameter's type, then the second; a bare type variable
    has no type to compare, so no ordering between these exists
  = give this one its own name, or constrain its parameter
```

**At the call** — a set whose members are genuinely indistinguishable for this argument:

```
error: 'concat(["a", "b"])' is ambiguous -- a [string] is both the pieces of a
       string and one array of parts
  |
3 |     val out = concat(["a", "b"])
  |                      ^^^^^^^^^^
  = concat(pieces: variadic string) -> string    would answer "ab"
  = concat(parts: variadic [T]) -> [T]           would answer a copy of the array
  = write concat(items: ...) to name the parameter, or join(...) for the string
```

Three properties both messages must have, and they are requirements, not style:

- **Name every candidate, with its signature.** "Ambiguous call" alone is unrecoverable — the
  reader cannot see what the alternatives were.
- **Say what each candidate would have done**, in the problem's terms, not the type system's.
  `"ab"` versus a copy of the array is the difference that matters.
- **Give the disambiguating spelling.** Every ambiguity must have an escape, or the feature has
  made something unsayable. Named arguments are the natural one, since a real declaration accepts
  them and a builtin never did.

## 6. What this deletes, and what it moves

**Deleted outright:**

- the `WORLD_PAIRED` table and its loop (`typecheck.c:1968`), the 28-name prefix dispatch;
- the 64 `array_`/`mutable_array_` prefixed declarations — they *become* the overloads;
- `tools/check_builtins.py`, whose whole job is catching declarations that drifted from arms;
- `BUILTIN_NO_SIGNATURE`'s pairing-related entries.

**Moves from name-keyed to set-keyed**, and this is where the real work is:

| | today | after |
|---|---|---|
| `PURE_BUILTINS` | a flat name list, with purity keyed per *name* | per declaration |
| `BUILTINS_NO_NAMED_ARGS` | **128 entries, 99 distinct — 29 names listed twice**, once per world | gone; a declaration takes named arguments |
| `--api builtins` | prints each declaration | prints a set |
| `src/lsp.c` | one completion entry per name | one per set, with the signatures |
| the formatter, the VS Code grammar | hand-kept name lists | derivable |

That `128 vs 99` is worth dwelling on: the table models a world where a name has one meaning, so
pairing a name meant writing it twice. Its consumers dedupe (`lsp.c`'s `add_global_name` has a
comment for exactly this). It is the clearest single symptom of the mismatch.

**Becomes correct rather than merely tidier — purity.** Today `PURE_BUILTINS` contains `push`,
`insert`, `set`, `remove`, `swap`, `reverse`, `filter`, `sort` — names that are pure in the array
world and effectful in the builder world. The gate keys on the *name*, and the builder case is
excluded by a **proxy**: `t.kind != TY_VOID` in the discard check (`typecheck.c:5975`). It works
only because every builder overload happens to return void. The comment there records two bugs this
already caused — a discarded `mutable([1, 2])` reporting `$mutable_array_lit`, a name that does not
lex, and a discarded `mutable(42)` accepted in silence. **Per-declaration purity removes the proxy
and the coincidence it rests on.** The same applies to the discard gate and the quadratic lint,
both keyed per name today, the lint having left two dead branches and advice about a
`MutableString` for an array.

## 7. Staging

Each stage is independently shippable and gated; none of them is "the big one".

1. **Resolver, behind the existing arms.** Build the set-based resolver and run it in parallel with
   the current dispatch, asserting they agree across the whole corpus and the 50-task benchmark.
   Disagreements are the real specification. Nothing user-visible.
2. **Declaration-site checking.** The "cannot be an overload set" error (§5), run over
   `lib/prelude.zm` as it stands. This should reject exactly `mutable`, and nothing else. If it
   rejects more, §3 is wrong and this document needs rewriting before any further stage.
3. **Convert the 28 paired names.** Delete `WORLD_PAIRED`; the prefixed declarations lose their
   prefixes and become the overloads. The prefix layer disappears. Fixtures for the empty-literal
   exception (§4) are the gate.
4. **Per-declaration purity, discard and lint.** Remove the void-return proxy.
5. **The lists.** `--api builtins`, `lsp.c`, formatter, grammar move to sets; delete
   `check_builtins.py`.
6. **Open it to library code.** Only now does `lib/graphics` gain what the prelude has — and this
   is the stage with no prior art inside the repo, because every overload today is a builtin.

Stages 1-5 are refactors with an observable invariant: *the corpus, the fixtures and the benchmark
all behave identically*. Stage 6 is the only one that changes what a program can say.

## 8. Non-goals

- **No return-type dispatch.** `to_set`/`to_mutable_set` stay two names. This is the boundary that
  held, and the case that proves the restriction earns its keep.
- **No ranking or specificity.** A bare type variable in a resolved position makes the set
  ambiguous at the declaration (§3c).
- **No implicit conversions.** Nothing coerces to make a member match.
- **No arity-based dispatch.** `concat` keeps its arm, documented as an exception.
- **No change to `internal`/`pub`.** Visibility and overloading are separate; F172 kept
  reachability and advertising apart and this does not disturb either.

## 9. What would falsify this

- **§3's count moves.** It is a script's answer about one file on one day. Re-run it.
- **Stage 2 rejects a set other than `mutable`.** Then the prelude contains a shape this design did
  not find, and the rule is wrong rather than the declaration.
- **Stage 1 finds disagreements that are not obviously bugs in the arms.** The arms are the
  incumbent behaviour; if the resolver is "more correct" in a way that breaks the corpus, the
  feature is bigger than it looks.
- **A second `[string]`-shaped ambiguity appears in real library code.** The ROADMAP names this as
  the thing that would argue against the feature outright, and it remains the right trigger: a
  failure mode of "two readings, silently" is the one this language most consistently refuses.
