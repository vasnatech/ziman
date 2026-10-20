# Overloading available to a declaration

A design for letting several `fn`/`native fn` declarations share a name, resolved from **every**
argument position, so that the dispatch Ziman already performs in 50 places stops being
hand-written in `src/typecheck.c` and starts being read off the declarations.

| | |
|---|---|
| Status | **Design only. Nothing is built.** |
| Raised | 2026-10-18, from "now that we have quite a number of overloaded functions, what do you think on relaxing the no-overloading rule?" (`docs/ROADMAP.md`) |
| Scope chosen | 2026-10-19, by the user: **full overloading**, not first-parameter only |
| Written | 2026-10-19 |
| Supersedes | `first-argument-overloading.md`, kept beside this for its §3 measurements |
| Measurements | from `lib/prelude.zm` and `src/typecheck.c` at `033efe2`, by script |

## 1. What the language does today

**Ziman is not missing argument-based overloading. It is missing overloading available to a
declaration.** Measured at `033efe2`:

```
50   overloaded names in lib/prelude.zm
131  declarations in those sets
70   hand-written dispatch arms in src/typecheck.c
```

`len` and `contains` have nine declarations each, `str` six, `snapshot`/`remove`/`mutable`/
`concat`/`clear`/`abs` four, `insert` three, and 38 names have two. Every set is resolved by a
hand-written arm, and the `builtin fn` lines in the prelude are documentation those arms are free
to contradict — `tools/check_builtins.py` exists only because they drifted.

Two mechanisms carry them. **Table-driven prefix dispatch** (the `WORLD_PAIRED` loop,
`typecheck.c:1968`) covers 28 names by building the target name through string concatenation,
`"mutable_array_" + "map"`, which is why 64 prefixed declarations exist whose only job is to be
the target of a concatenation. **Bespoke arms** cover the rest.

## 2. The rule

> Several `fn`/`native fn` declarations may share a name. A call resolves to exactly one of them,
> chosen from the arguments at **every** position. A set whose members cannot be told apart by any
> argument is an error at the *declaration*. No return-type dispatch, no specificity ranking, no
> implicit conversions.

"Every position" is what makes this *full* overloading rather than the first-parameter rule the
superseded document proposed. That rule covered 43 of the 50 existing sets; five
(`mutable_set_union`, `_intersect`, `_difference`, `_is_subset`, `mutable_map_set_all`) are
distinguished by argument **two**, and under a first-parameter rule they would have been rejected
outright.

## 3. The problem full resolution has to solve, and why it is smaller than it looks

Resolving from all positions appears circular. `src/typecheck.c:2313`, `expr_needs_expected_type`,
names five kinds of expression that **cannot be typed until something says what is expected**:

```
[]                               an empty array literal
fn(x) { ... }                    an unannotated lambda
a value block
ok(...) / err(...) / channel_new(...) / a bare Some(x)
a bare None
```

Those are exactly the arguments collection calls carry. To pick the overload you want argument
types; to type those arguments you want the overload.

**It is not actually circular, and the compiler already proves it.** `infer_call_type_args`
(`typecheck.c:2455`) solves the identical problem for C3's type-argument inference: it walks the
arguments, **skips every one `expr_needs_expected_type` flags**, types the rest, and unifies what
it learned. Overload resolution can be built on the same partition, which is already written,
already correct about the awkward cases (`Some("es")` contributes, `None` does not), and already
exercised by the corpus.

So: **resolve from the self-describing arguments at every position, never check a context-dependent
argument before the overload is chosen.** That needs no speculative checking, no backtracking, no
undo log over the 24 AST-rewrite sites, and no constraint solver. It is a filter, not a search.

### What a context-dependent argument still tells you for free

Its *shape* is syntax, readable without checking:

| argument | constrains the parameter to |
|---|---|
| `fn(a, b) { ... }` | a function type **of arity 2** |
| `[]` | an array (never `string`, never a `Mutable*`) |
| `ok(x)` / `err(x)` | a `Result<_, _>` |
| a bare `Some(x)` / `None` | an `Option<_>` |
| a value block | nothing |

Lambda arity alone separates `fold(a, init, fn(acc, x))` from `map(a, fn(x))`, which is most of
what collection sets need. These are filters on the candidate set, not type checks, and cost
nothing.

## 4. Resolution

At a call `f(a1 … an)`, with the set of declarations named `f` visible at the call site:

1. **One member** → today's path exactly, unchanged. This is the overwhelmingly common case and
   must not get slower.
2. **Arity filter.** Drop members that cannot accept `n` arguments, counting a variadic tail.
3. **Self-describing filter.** For each position `i` whose argument does not need an expected
   type: check it once, cache the type, and drop members whose parameter `i` cannot unify with it.
   Positions are independent — this is where "full" lives.
4. **Shape filter.** Apply the table in §3 for the remaining positions.
5. **Resolve.**
   - one survivor → bind it, then proceed **exactly as today**: `infer_call_type_args`, then the
     ordinary argument loop, which gives every context-dependent argument its expected type from
     the chosen declaration. Report any mismatch against *that* signature, never against the set.
   - zero → §5's "no overload" error, naming the position that eliminated the last candidate.
   - more than one → §5's ambiguity error.

**Each argument is checked at most once.** Step 3 caches, and step 5 reuses. A call in a set of one
costs one extra pointer comparison over today.

**The empty-literal exception survives by construction.** `[]` is never checked in step 3 — it is
context-dependent — so `first<int>([])` and `map<int, int>([], f)` keep working. Today's
`WORLD_PAIRED` loop has a comment recording that checking the first argument eagerly made both of
those errors they had not been; this design cannot reintroduce that, because the same predicate
guards it. It still deserves fixtures.

### Declaration-site checking

A set is checked once, when its module is checked, independently of any call. Two members are
**indistinguishable** when, for every position, their parameter types unify with the same arguments
— including the case where one parameter is a bare type variable, which unifies with everything.

Running this over `lib/prelude.zm` as it stands must reject exactly two sets:

- **`mutable`** — `mutable<T>(item: T)` matches every call its three siblings match
  (`prelude.zm:1452-1455`). No specificity ranking means no ordering between them exists.
- **`concat`** — and the mechanical reason is narrower and more interesting than the semantic one
  this document first gave. Stage 2 found it fails at **the arity where a variadic tail is
  empty**:

  ```
  concat()            lines 68/69 -- variadic string vs variadic [T], and no argument to look at
  concat(b)           lines 74/75 -- both (b: MutableArray<T>, parts: variadic ...), tail empty
  ```

  **Two members whose fixed prefixes are identical always collide at their minimum arity**, since
  the position that separates them is in a tail that is not there. That is a general rule about
  variadic overloads, not a fact about `concat`, and any future set with two variadic members
  sharing a prefix will hit it.

  The semantic problem is real too and is why `concat` could not be resolved even with the arities
  fixed: *one argument that is an array spreads*, so `concat(["a", "b"])` is the string `"ab"` and
  never a copy of the array, while two or more arguments are always parts. That is arity-dependent
  reinterpretation of one argument type, and no per-parameter signature states it.

**Both keep their hand-written arms**, documented as exceptions with the reason attached. If the
check rejects a third set, this document is wrong and should be rewritten before any further work
— that is stage 2's whole purpose.

**Measured 2026-10-19 (stage 2, F177): 50 sets, 48 resolvable, exactly these 2 not.** The same
answer from the fixtures, every `lib/` module and the North Star, none of which adds a set of its
own — overloading is not available to library code until stage 7. `tools/overload_sets.sh` holds
the prediction as a gate.

## 5. Errors

A model recovers from a compile error only when it says what to write instead, and this language's
stance is to refuse features whose failure mode is "two readings, silently". These messages are the
feature, not its polish.

**Candidate lists are truncated.** `len` and `contains` have nine members; printing nine signatures
buries the answer. The rule:

> **Five or fewer: print them all. More than five: print three, then `… and {n} more`.**

The three are the candidates that got **furthest** — the most argument positions matched before
elimination — so the list is ranked by relevance rather than by declaration order. Ties break by
declaration order, so the message is deterministic. The footer names how to see the rest:
`ziman --api builtins len`.

**No overload matched** — names the position that did the eliminating:

```
error: no overload of 'len' takes a Mutable<int> as its 1st argument
  |
7 |     print(len(counter))
  |               ^^^^^^^
  = len(s: string) -> int
  = len(a: [T]) -> int
  = len(m: Map<K, V>) -> int
  = ... and 6 more  (ziman --api builtins len)
  = 'counter' is a Mutable<int>; read it with get(counter)
```

**Ambiguous at the call** — more than one survivor:

```
error: 'concat(["a", "b"])' is ambiguous -- a [string] is both the pieces of a
       string and one array of parts
  |
3 |     val out = concat(["a", "b"])
  |                      ^^^^^^^^^^
  = concat(pieces: variadic string) -> string   would answer "ab"
  = concat(parts: variadic [T]) -> [T]          would answer a copy of the array
  = name the parameter to choose -- concat(parts: ["a", "b"])
```

**Ambiguous at the declaration** — caught before any call exists:

```
error: 'mutable' cannot be an overload set -- 'mutable(item: T)' takes a bare type
       variable, so it matches every call the other three also match
  |
  |  builtin fn mutable<T>(item: T) -> Mutable<T>
  |                        ^
  = the others are mutable([T]), mutable(Map<K, V>), mutable(Set<T>)
  = there is no specificity ranking, so no ordering between these exists
  = give this one its own name, or constrain its parameter
```

Three properties every one of these must keep:

- **Name the candidates, with signatures.** "Ambiguous call" alone is unrecoverable.
- **Say what each *would have done*, in the problem's terms** — `"ab"` versus a copy of the array,
  not a restatement of the types.
- **Give the disambiguating spelling.** Every ambiguity needs an escape or the feature has made
  something unsayable. Named arguments are the natural one: a real declaration accepts them and a
  builtin never did.

## 6. What this deletes, and what it moves

**Deleted:** the `WORLD_PAIRED` table and its loop; the 64 `array_`/`mutable_array_` prefixed
declarations, which *become* the overloads; `tools/check_builtins.py`; the pairing entries in
`BUILTIN_NO_SIGNATURE`; and 68 of the 70 bespoke arms, `concat` and `mutable` excepted.

**Moved from name-keyed to set-keyed**, which is the bulk of the work:

| | today | after |
|---|---|---|
| `PURE_BUILTINS` | purity keyed per *name* | per declaration |
| `BUILTINS_NO_NAMED_ARGS` | **128 entries, 99 distinct** — 29 names written twice, once per world | gone; a declaration takes named arguments |
| `--api builtins` | one line per declaration | a set, and `--api builtins <name>` for one |
| `src/lsp.c` | one completion per name | one per set, with signatures |
| the formatter, the VS Code grammar | hand-kept name lists | derivable |

**Becomes correct rather than tidier — purity.** `PURE_BUILTINS` lists `push`, `insert`, `set`,
`remove`, `swap`, `reverse`, `filter`, `sort`: pure in the array world, effectful in the builder
world. The gate keys on the name, and the builder case is excluded by a **proxy**, `t.kind !=
TY_VOID` in the discard check (`typecheck.c:5975`), which holds only because every builder overload
happens to return void. The comment there records two bugs it already caused — a discarded
`mutable([1, 2])` reporting `$mutable_array_lit`, a name that does not lex, and a discarded
`mutable(42)` accepted in silence. Per-declaration purity removes the proxy and the coincidence it
rests on. The same applies to the discard gate and the quadratic lint.

## 7. Staging

Each stage ships on its own and is gated; none is "the big one".

1. **Resolver behind the arms.** Build steps 1-5 of §4 and run it in parallel with the existing
   dispatch over the corpus, the fixtures and the 50-task benchmark, asserting the two agree.
   Disagreements are the specification. Nothing user-visible.
2. **Declaration-site checking** over `lib/prelude.zm`. Must reject exactly `mutable` and `concat`.
   A third rejection means §4 is wrong, and the document is fixed before stage 3.
3. **Convert the 28 paired names.** Delete `WORLD_PAIRED`; the prefixed declarations lose their
   prefixes and become the overloads. Empty-literal and lambda-arity fixtures are the gate.
4. **Convert the remaining 68 arms.** Largest stage, lowest risk: each arm becomes declarations,
   one at a time, with its own fixtures already in place.
5. **Per-declaration purity, discard and lint.** Remove the void-return proxy.
6. **The lists.** `--api builtins`, `lsp.c`, the formatter and the grammar move to sets; delete
   `check_builtins.py`.
7. **Open it to library code**, which is the only stage that changes what a program can say —
   today `lib/graphics` cannot express what `lib/prelude.zm` does, and every overload in the
   language is currently a builtin.

Stages 1-6 are refactors with one observable invariant: **the corpus, the fixtures and the
benchmark behave identically.** Stage 7 is the feature.

## 8. Non-goals

- **No return-type dispatch.** `to_set`/`to_mutable_set` take the same arguments and differ only
  in what they answer; they stay two names. This is the case that proves the restriction earns its
  keep.
- **No specificity ranking.** A bare type variable in a resolved position makes a set ambiguous at
  the declaration (§4).
- **No implicit conversions.** Nothing coerces to make a member match.
- **No arity-based reinterpretation.** `concat` keeps its arm.
- **No change to `internal`/`pub`.** F172 kept reachability and advertising apart; this does not
  disturb either.

## 9. What would falsify this

- **Stage 2 rejects a set other than `mutable` and `concat`.** Then the prelude holds a shape §4
  did not anticipate, and the rule is wrong rather than the declaration.
- **Stage 1 finds disagreements that are not plainly bugs in the arms.** The arms are the
  incumbent behaviour; a resolver that is "more correct" in a way that breaks the corpus means the
  feature is bigger than it looks.
- **Shape filtering turns out to be insufficient** — two members separated only by a lambda's
  *parameter types* at equal arity, or by the element type of an `[]`. Those are genuine
  ambiguities under this design; if real code wants them, the design needs a way to say which,
  and named arguments may not be enough.
- **A second `[string]`-shaped ambiguity appears in real library code.** The ROADMAP names this as
  the thing that would argue against the feature outright, and it stays the right trigger.
