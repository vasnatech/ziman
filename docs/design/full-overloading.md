# Overloading available to a declaration

A design for letting several `fn`/`native fn` declarations share a name, resolved from **every**
argument position, so that the dispatch Ziman already performs in 50 places stops being
hand-written in `src/typecheck.c` and starts being read off the declarations.

| | |
|---|---|
| Status | **Built.** Stages 1-3 and 5-7 done (F176-F184); stage 4 void, see §10. |
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

`concat` turned out to be only half an exception: its arm stays, because nothing in a call chooses
among its four shapes, but the *dispatch* went, the four becoming `native fn` declarations marked
`#[compiler_resolved]` while the arm records its choice on `call_fn`. What a bespoke arm has to
keep is the decision, not the id -- see §16.

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
4. ~~**Convert the remaining 68 arms.**~~ **Not done, and the reason given in F181 was wrong**
   (corrected 2026-10-19, F185). The stage assumed the remaining arms were shaped like the paired
   ones. They are not: of the 50 names with more than one declaration, **28 were genuine overload
   sets and are now real declarations; the other 22 are polymorphic builtins** — one
   implementation with its type rule written out as several signatures, `len` being nine
   signatures over a single `call_len`.

   F181 concluded they *cannot* be converted, because there is no `array_len` or `map_len` for a
   call to resolve to and converting would mean "inventing N implementations where one exists".
   **That does not follow.** A native is found by its *declaration's* name, so nine declarations
   all called `len` resolve to the one registration — no new implementation at all. F180 had
   already shipped a native overload set (`filter`, two `native fn` declarations), which is the
   same mechanism with the harder requirement of two symbols rather than one.

   So the stage is **open, not void**, and §11 states what actually gates it: the arms carry
   measured diagnostics that the resolver's generic candidate list would replace with something
   worse.
5. **Per-declaration purity** — done (F182), and smaller than written. The `t.kind != TY_VOID`
   test was called a proxy, and for the 28 converted sets it is now *exact*: `t` is the resolved
   declaration's own return type, and a member that mutates has nothing to return. What was left
   to remove was the **name list beside it** — those 28 names are gone from `PURE_BUILTINS`, and
   purity for them is derived from the resolved declaration. The 22 polymorphic builtins keep the
   list, because for them the name is the implementation and there is no declaration to ask (§10).
   The discard gate and the lint were the same mechanism; nothing else moved.
6. **The lists** — done (F183), with one claim of this plan withdrawn. The VS Code grammar was
   the only list still hand-kept, and it was badly stale: `ArrayBuilder`, `sb_append`, `mapb_set`,
   `concat_arrays`, all deleted in F149–F162, and none of the 28 names F178–F180 unprefixed. It
   is generated from `--api builtins` now and gated by `git diff --exit-code`, the way the
   manuals are. `src/lsp.c` was already derived (F168) and `--api builtins` already prints from
   the parsed prelude, so neither needed moving; the formatter keeps no name list at all.
   **`check_builtins.py` is not deleted**, and this plan was wrong to say it would be: it gates
   that every *dispatched* builtin has a declaration, and §10's 22 polymorphic builtins are
   exactly that population — still dispatched, still needing the guard.
7. **Open it to library code** — done (F184). The declaration-site rule is no longer confined to
   the prelude: any module may declare an overload set, under exactly the rule the prelude lives
   under. Cross-module worked without further change — `shapes.area(3)` and `use shapes.area`
   both resolve, because the loader rewrites a qualified call to a qualified *name* and the
   resolver sees every declaration carrying it. The call-site ambiguity error of §5 became
   reachable for the first time here and gained the escape route §5 requires.

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


## 10. Two populations, and only one of them is an overload set

Added 2026-10-19 (F181), from trying to carry out stage 4 and finding it had no content.

"One name, several declarations" describes two unrelated things, and conflating them is what made
stage 4 look like work:

| | what it is | how a call reaches an implementation |
|---|---|---|
| **Overload set** (28) | several real functions sharing a name | the resolver picks one from the arguments |
| **Polymorphic builtin** (22) | *one* implementation, type rule written as several signatures | a builtin id; the C function switches on the value |

`sort` is the first. `len` is the second: nine `builtin fn` lines over `call_len`, which asks the
value what kind it is. The nine lines are documentation of one function's type rule — they are not
nine functions, and today no resolution happens.

**That is a description of how things are, not an argument that they must stay so** — a
distinction this document got wrong for a day (§11). Nine declarations sharing a name may share
one implementation too, since a native is located by its declaration's name.

The second population also explains the two sets that §4 predicted would be unresolvable.
**`concat` and `mutable` were both polymorphic builtins**, so the resolver never looked at them,
and the declaration-site check was measuring a population it does not govern. Against the
population it does, the bar is not "exactly two offenders" but **zero**, and
`tools/overload_sets.sh` now asserts that. `concat` has since become a third population of one,
`#[compiler_resolved]` (§16), which the same tool counts apart and checks inversely; `mutable`
remains a polymorphic builtin for the reasons in §4.

The distinction matters beyond bookkeeping, because it is the honest answer to "does Ziman have
overloading": it has 28 overload sets and 22 polymorphic builtins, and `docs/ROADMAP.md`'s
"28 overloaded builtin names" was counting the first group by a method that happened to agree.
Stage 7 opens the first to library code. The second is a property of a builtin and will stay one.


## 11. Could the polymorphic builtins become declarations after all?

Added 2026-10-19 (F185), correcting §4 and §10.

**Yes, mechanically.** `len`'s arm is pure type rule — check the argument is an array, string,
map, set, builder or `Mutable`, answer `int` — and nine `native fn len(...)` declarations state
exactly that. All nine would be named `len`, so all nine find the one registration: no new
implementation, no new C code. F180 shipped the harder version of this already, `filter` being
two `native fn` declarations needing *two* symbols.

The scale, measured at `8df7c0f`: **71 dispatched builtin names, 70 arms, 11 with a recorded
reason they can carry no signature.** Those 11 are genuinely stuck — `format` and `printf` read a
template against the argument list, `error_message` looks up a `<type>_message` function, the `$`
names are parser emissions — and `concat` and `mutable` stay for §4's reasons. That leaves
roughly **50 convertible**, taking most of the 70 arms, the `BUILTINS_NO_NAMED_ARGS` table and
`check_builtins.py` with them, and giving each converted name named arguments, type arguments and
per-declaration purity.

**What actually gates it is the diagnostics, not the mechanism.** The arms carry measured,
hand-written messages. `len`'s is:

> `'len' expects an array, string, map, set, builder or Mutable, found Point`

The resolver would answer *"no declaration of 'len' accepts its argument 1"* and list three of
nine signatures with "… and 6 more". For a nine-member set that is **worse**: it shows shapes
where the sentence named a category. This has already gone wrong twice in one day — F178's first
candidate list printed `sort/1` twice over, and F180 deleted the measured `map()` → "did you mean
the `Map` type" hint along with the table, caught only because a fixture pinned its full text.

So the open design question is **how a declaration keeps the hint its arm carried** — a per-set
"when nothing matches, say this" that `len` could carry while losing its arm. That wants
designing before any conversion, and the work should not start without it.


## 12. Stage 8: converting the polymorphic builtins — step 1 done, and what it found

Added 2026-10-19 (F186). §11 proposed deriving the "expects …" sentence from the declarations so
that converting a polymorphic builtin would not cost its measured diagnostic. Step 1 built the
derivation and compared it against all 16 arms whose members differ by argument one. **It covers
about half, and the half it misses is the interesting half.**

**Derivable, and as good as the hand-written line:**

```
hand:     'len' expects an array, string, map, set, builder or Mutable, found P
derived:  'len' expects string, [T], Map<K, V>, Set<T>, Mutable<T> or a builder, found P

hand:     'slice'/'swap'/'to_set' expects an array or a MutableArray, found P
derived:  the same list, from the declarations
```

> **This second row was wrong when written, and F207 made it true.** The derivation collapsed
> every builder world into the phrase *"a builder"* whenever a non-builder type stood beside it,
> so `slice` actually said *"'slice' expects [T] or a builder"* — which names a category of four
> when exactly one of them is accepted. The collapse is a summary, and summarising one thing loses
> the thing; it now applies only when there is more than one builder to collapse, so `slice`,
> `swap`, `to_set`, `push` and `insert` name `MutableArray<T>` and `len` keeps *"or a builder"*.
> Five `.err` fixtures recorded the old wording, which is how the claim survived unread.

**Not derivable — a measured clause the signatures do not contain:**

| | the clause |
|---|---|
| `str` | *"(it's already a string)"* when the argument is one |
| `clear` | *"an immutable collection is emptied by writing an empty one"* |
| `push`, `insert`, `set` | *"(or, for set/insert/remove, a map or set)"* — a cross-reference to its siblings |
| `remove` | names `Map` and `Set` as alternatives it does not itself take |

These are not restatements of the type rule; they are advice about what the author probably meant,
and no amount of reading the declarations produces them.

**So §11's conclusion stands only halfway.** Derivation removes the diagnostic objection for the
sets whose arms say nothing but the type rule. For the rest, converting still needs a way for a
declaration to carry an extra line — the per-set hint §11 called for, which this step was meant to
test the necessity of and has now established. **It is necessary.**

The derivation shipped anyway, because it improves the 28 sets that are already declarations:
`sort(Set<int> {...})` said *"no declaration of 'sort' accepts its argument 1"* and now says
*"'sort' expects [T] or a builder, found Set<int>"*.

**Step 2 is therefore not "convert the smallest sets" but "design the hint".** Converting the
easy half first would leave the language with two kinds of builtin error for no reason a reader
could see, and would spend the easy cases before the hard mechanism exists to check against.

### Step 2 — `#[when_unmatched("...")]` (F187)

An attribute on any member of a set, adding one line to the "no member matched" error. It follows
`#[deprecated("...")]` exactly — same grammar position, same string argument, already round-tripped
by the formatter — so it needed no new syntax, only a new name. The bare form is a syntax error:
for `deprecated` the message is optional decoration, here it is the entire content.

It was proven against a **user-declared** set rather than against a converted builtin, which is
possible only because stage 7 opened overloading to program code. That matters for sequencing: the
mechanism has a real user and real fixtures *before* any builtin is converted, so step 3 can check
each conversion against a message that already works.

### Step 3 — the first conversion, and the runtime answer (F188)

`abs` is converted: its arm is deleted and the resolver checks the call against its four
`builtin fn` declarations, which were already there documenting what the arm checked. Its derived
message is the hand-written one bar an Oxford comma.

**The runtime needed no change, and the reason is worth stating.** §11 assumed a converted
builtin would resolve to a native registration. `abs` has none -- it is dispatched by a builtin
*id* (`B_abs`), and `interpreter.c` consults `call_fn` **before** the id, so recording the
resolved declaration there would send the call into `call_function` with nothing to run. The
resolver therefore does not record a `builtin fn` on `call_fn`: **types come from the
declarations, dispatch stays on the id.**

> **The other half landed on 2026-10-22**, and `abs` is the name that shows both steps whole: its
> four declarations became `native fn`, so the id, its branch and `call_abs` are gone and dispatch
> is a registration like any other. One registration under the bare name serves all four, as
> `len`'s nine do -- a scalar parameter has no world prefix for the symbol fallback to build
> `int_abs` from, and there would be nothing to gain if it did. So the sentence above describes a
> *stage*, not a resting place: a converted builtin keeps its id only until someone writes the
> shim. That makes every remaining conversion a type-checking
change only, cheaper than §11 supposed, and it means the declarations do not need natives behind
them.

> **Corrected by F207 (2026-10-21): "a type-checking change only" does not hold for a *variadic*
> builtin.** `collapse_variadic_args` runs on the `call_fn` path alone, so the id path is handed
> the raw arguments and has to spread a trailing array itself. `push`'s arm had refused
> `push(a, [3, 4])`, so `B_push` had never been given one, and deleting the arm made the checker
> honour the declaration's `variadic T` and accept it — appending the `[int]` itself and printing
> a garbage element. The fix was four lines and needed no new mechanism, `remove_all` (F198) and
> `add` (F199) having the same split for the same reason; the point is that the runtime is not
> automatically out of scope, and the thing to check for a variadic set is whether its id arm
> reads `variadic_tail_is_array_spread`. See §14.

**What it cost, and what step 4 owes.** `BUILTINS_NO_NAMED_ARGS` answers two questions that have
now come apart: it is the registry of names the interpreter dispatches *and* the list of names
refusing a named argument. A converted builtin could take named arguments, but removing it from
the table to allow that also unregisters it -- which made `abs`'s own declaration invalid, caught
immediately. **Splitting that table is step 4**, and until then a converted builtin keeps the
restriction it had.

### What an arm carries decides whether it can go (F190)

F186 sorted the arms by whether their sentence is derivable. That was the wrong axis, or at least
not the only one. Counting the error messages in each arm sorts them better:

| arm | messages beyond arity | |
|---|---|---|
| `len`, `str`, `set`, `unwrap`, `abs` | 1 | a pure type rule — **convertible** |
| `clear` | 2 | |
| `concat` | 3 | |
| `snapshot`, `mutable_map_set_all` | 4 | |
| `remove` | 6 | |
| `contains` | 12 | |

**Three of those five were not, and the axis missed why.** F191 found `str` unconvertible the next
day and `set` unconvertible for a different reason; `unwrap` is a *rewrite* into an enum access
rather than a type rule. Counting messages sorts the arms that can go by *message quality*, and
says nothing about whether the call can be **resolved** — which is the constraint F191 added and
the one that actually binds. `len` and `abs` did convert; the other three are each blocked by
something this table cannot see.

An arm with more than one is not stating a type rule; it is **answering differently depending on
what you passed**. `clear` tells a `Mutable` *"it is always initialised, and emptying it would
need exactly the zero value its type may not have"* and tells everything else *"an immutable
collection is emptied by writing an empty one"*. `snapshot` tells a `Mutable` to use
`mutable_get` and says why the names differ. These are keyed on the **rejected type**, and
`#[when_unmatched]` is one line for the whole set — it cannot express them.

So the remaining conversions are bounded by this, not by F186's derivability: **`len` and `str`
and `set`**, and nothing else without a per-type hint. Whether to build one is a separate
question and should be answered by how often a converted set actually wants it, not in advance.

### A third constraint, and the one that ends the stage (F191)

`str` passes both earlier tests — one message, a derivable sentence — and **still cannot be
converted**. Its argument is where a *value block* appears: `str(switch cut.fits { ... })` is
real code in `tests/lang/graphics_layout.zm`. A value block carries no type of its own, so the
resolver cannot narrow six candidates and the call is ambiguous. Converted, then reverted.

That is the third and most general constraint, and it is about the **shape of the arguments a
function habitually receives**, not about its messages:

> An overload set resolves from arguments that describe themselves. A function whose job is to
> accept *anything* — `str` renders a value, whatever it is — receives exactly the expressions
> that do not.

`len`, `abs`, `slice`, `swap` and `to_set` take collections and numbers, which always describe
themselves. `str` takes expressions. So the collection vocabulary is the natural home of
overloading in this language, and the universal verbs are not.

`set` is not convertible for a plainer reason: it has no arm of its own, sharing one with
`insert` inside the map/set mutator.

**Stage 8 ends here.** Converted: `abs`, `swap`, `slice`, `to_set`, `len` — **65 arms, from 70**.
The estimate went ~50 → ~3 → 5 as each constraint was found by reading rather than counting.

## 13. Splitting a shared arm, and what it tells you that counting names does not

Added 2026-10-21 (F206). F191 ended stage 8 with five arms converted and a stated reason for each
refusal. One of those reasons was not a reason: **`set` was recorded as unconvertible because it
"has no arm of its own", sharing one with `push` and `insert`.** That is a fact about the code, not
about `set`, and it left the real question unasked. F204 hit the same wall from the other side —
`push` had a variadic floor *and* a shared arm, and clearing the floor did not help.

So the arm was split, one per name, and each name was measured by F190's test afterwards.

### The split changed the messages before it changed anything else

The shared arm computed `expected_argc` from the **name**, in its second statement, before argument
one had been type-checked. Every message downstream of that inherited the assumption that a name
has an arity. It does not; a *(name, argument-one type)* pair does. Four shapes were getting an
answer about a form they could not have had:

| call | the shared arm said | what is true |
|---|---|---|
| `insert(mutableSet, 2)` | "'insert' takes exactly 3 argument(s)" | no `insert` of a MutableSet exists, at any arity |
| `set(mutableSet, 2)` | "'set' takes exactly 3 argument(s)" | same |
| `push(mutableString, "a")` | "expects an array (or, for set/insert/remove, a map or set)" | the word is `append` |
| `set(aSet, 2)` | "use insert(set, item)" | F199 made that `add`, two passes earlier |

Each arm now reads argument one first and asks about arity second. **None of that needed the
split** — it needed argument one's type, which the shared arm had and threw away. The split is what
made it obvious.

### The counts, and the two conversions they unblock

| arm | messages beyond arity | |
|---|---|---|
| `push` | 1 | the type rule, derivable — **convertible** |
| `insert` | 1 | the type rule, derivable — **convertible** |
| `set` | 3 | two keyed on the rejected type — **not convertible** |

`push` and `insert` each come out with one sentence, and it is the same sentence whatever was
rejected: *"the other collections have their own word: set(map, key, value), add(set, item),
append(sb, s)"*. That matters because it is the shape `#[when_unmatched]` can carry — one line for
a whole set. F186 listed this cross-reference under "not derivable" and it looked like a reason
these three could never be converted; it was an artifact of three names sharing one message, and
splitting dissolved it. **A clause that cannot be derived from the declarations is not the same as
a clause that has to vary by argument**, and F186 did not distinguish them.

`set` keeps two refusals that genuinely vary: a MutableArray is told *"an element is written with
'b[i] = value'"*, a set of either world is told *"a set has no 'set' — use add(set, item)"*. Both
are measured, both are wanted, and `#[when_unmatched]` cannot say a different thing per argument.
So `set` stays arm-handled, and now for the F190 reason instead of a bookkeeping one.

### The decision the arm was hiding

`insert` on a `Set` was **accepted, undeclared, and contrary to a decision already taken.** F199
settled that `insert` is positional only and gave both set worlds `add`; the arm went on accepting
`insert(aSet, x)` for two passes because it reached the map/set mutator before anything could ask
whether a `Set` had an `insert` declaration. It was absent from `ziman --api builtins` the whole
time, and `docs/SYNTAX.md` was still teaching it.

That is the general lesson, and it is sharper than the one about messages: **a hand-written arm can
keep a spelling alive after the declarations have retired it, and no guard notices, because the arm
answers before the declarations are consulted.** `tools/check_builtins.py` checks that every rename
hint points at a declaration that exists and every old name does not — it cannot check for a name
the checker accepts and no declaration mentions. Converting an arm to declarations is what closes
that gap permanently, which is one more argument for conversion than §11 had.

Carried out in F206: 19 call sites moved to `add` (18 fixtures, one in `lib/core/toml.zm`), and
`insert` on either set world is now refused.

## 14. `push` and `insert` become declarations, and what the arm had been hiding

Added 2026-10-21 (F207). §13 split the shared arm and measured the three names; this converts the
two it found convertible. `make overload-sets-check` moves from **39 resolver-owned sets and 20
polymorphic builtins** to **41 and 18**, which is the measurement to quote — the "70 arms" of §1
came from a script that was never committed, and the running total has been re-derived by hand
since, incorrectly at least once (§13's split *raised* the arm count by two before this lowered it
again).

**One collection arm is left, `set`.** This said two, `set` and `remove`, "both there for
per-argument messages rather than for anything the declarations cannot express" -- and that second
clause is what made `remove`'s convertible: on 2026-10-22 its three immutable members became
`native fn` and the arm was deleted. Of its four messages, three *were* what the resolver says
anyway (two arity checks and `argument 2 of 'remove' expects int`), and the list of accepted types
it hand-wrote is now derived from the six signatures, so it can no longer fall behind them. Only
the fourth was real -- a `MutableString` answered with "take 'snapshot' and use 'substring'" -- and
it moved to `#[when_unmatched]`, which is one line per set, so every unmatched `remove` now gets
that advice whether or not it passed a MutableString. F190's criterion again: the content survives,
the aim does not. **No fixture had ever asserted that message while the arm owned it**, which is
why the trade was invisible until the arm was deleted; `tests/lang/remove_mutable_string_rejected`
pins it now.

### The dispatch question, which had an answer already

Each set is now **mixed**: the array form stays `builtin fn` and is run by its builtin id, the
MutableArray form is a `native fn`, so `if (!fn->is_builtin) e->call_fn = fn` records it and
`interpreter.c`'s symbol fallback derives `mutable_array_push` / `mutable_array_insert` from it.
That fallback reads **parameter one only** — which F205 found as a *limit*, when it stopped two
`append` declarations from sharing a name — and parameter one is exactly where these two
declarations differ, so the limit is the mechanism here. The arm's callee rewrite and its
`rewritten_from` bookkeeping are both deleted.

`add` has had this shape since F199 (`builtin fn` for a `Set`, native for a `MutableSet`), so
nothing about the mixed set is new. That is worth saying plainly because it was the part that
looked risky in advance and turned out to be precedent.

### The part that was not free: a variadic builtin's id path

§12 concluded that a conversion is "a type-checking change only". **For `push` that was false, and
the failure was silent.** `collapse_variadic_args` runs on the `call_fn` path alone, so a builtin
dispatched by id receives the raw arguments and must spread a trailing array itself. The arm had
refused `push(a, [3, 4])` outright — argument two had to equal the element type — so `B_push` had
never been handed a spread, and the declaration's `variadic T` was dormant behind the arm.
Deleting the arm made the checker honour the declaration, accept the call, and append the `[int]`
into an `[int]`:

```
push([1, 2], [3, 4])   before: type error, "argument 2 of 'push' expects int ... found [int]"
                       after:  [1, 2, 140432405179904]
```

The fix reads the checker's own `variadic_tail_is_array_spread`, which `B_map_remove_all` (F198)
and `B_set_insert_all` (F199) already do for the same reason — **`push` was the one variadic
builtin that had never needed it, because its arm refused the shape.** Same finding as §13's, one
layer down: an arm can conceal a gap between what the declarations promise and what the runtime
does, and deleting it is what makes the promise load-bearing. For any remaining variadic set, the
thing to check before converting is whether its id arm reads that flag.

### What it cost in diagnostics, stated rather than buried

Two losses, both on argument two and neither measured:

| | before | after |
|---|---|---|
| `push([1], "s")` | `argument 2 of 'push' expects int (the array's element type), found string` | `argument 2 of 'push' expects int or [int], found string` |

The parenthetical said *why* `int`, and the `or [int]` is the spread form the declaration really
does accept. This is the ordinary message any declared generic variadic gives, and it is the same
trade `abs`, `len`, `slice`, `swap` and `to_set` already took.

Against that, a gain the conversion did not have to pay for: `#[when_unmatched]` advice now
reaches the *arity* failure too. It had been appended only to the per-argument messages, so
`insert(aSet, x)` — where both declarations are eliminated on arity before any type is compared —
said *"no declaration of 'insert' takes 2 argument(s)"* and nothing else, losing the sentence §13
had just written for it. The advice is about the **name**; which check eliminated the last
candidate is not something the author chose.

## 15. The prefixed spellings go, and the effort runs out of prefixes

F208. §14 left `push` and `insert` with a `MutableArray` declaration each, and `remove` had had
one in `collections.zm` since F200 — so `mutable_array_push`, `mutable_array_insert` and
`mutable_array_remove` were second spellings of names that already dispatched on argument one.
They were `internal`, which means nothing advertised or completed them, and **767 call sites
across this repo and the North Star used them anyway** (509 here, 258 in the backend), because the
prelude's own code did. That
is the measurement worth keeping from this stage: `internal` hides a name from the listing, not
from the corpus, and the corpus is where a reader learns the vocabulary. A name kept "callable but
hidden" after its replacement ships is a name that goes on being written.

Deleting the three declarations is almost the whole change — the C symbols keep the prefix, which
is the one place §1 ever wanted it — with one exception worth naming: `src/derive.c` *emits*
Ziman, and three `#[derive]` bodies wrote `mutable_array_push(...)` into generated code. A
conversion's blast radius includes the code generators, not only the checker and the runtime.
That is the third distinct place §12's "a type-checking change only" has turned out to be false:
the runtime in §14, the generated code here.

**`remove` on a MutableArray was the one of the three that was not yet converted at all, and its
declaration said otherwise.** It read `builtin fn`, but the builtin id never ran: the arm rewrote
the callee to `mutable_array_remove` first, and `call_remove` only ever handled an array. So the
declaration was documentation, the arm was the implementation, and `make overload-sets-check`
counted the set as resolver-owned the whole time. **The counts do not move for this stage — 41
resolver-owned sets and 18 polymorphic builtins, exactly as F207 left them** — which is the right
answer and worth stating: this stage removed duplicate spellings, not arms.

Converting it surfaced what the arm had been hiding, in the shape §13 and §14 both found.
Discarding a non-void prelude overload's result is an error (F182's return-type rule), and
`remove(b, len(b) - 1)` is how `pop` is spelled. That call compiled only because the gate read the
**rewritten** name, did not recognise it, and said nothing. The fixture asserting that `pop` works
had been passing for an accidental reason. The fact now lives on the declaration, as
`#[effectful]`, replacing the pair of type names in `effectful_despite_answering` — see
docs/DESIGN.md's "The last prefixed appenders go, and a mutation gets to say it answers something"
for why an attribute and not a third entry in the list.

**What is left prefixed is a different case, and this effort should stop here.**
`mutable_array_new`/`_filled` and the other constructors have no bare form to supersede them, and
they are the whole of the prefix now: `mutable_array_get`/`_set` went in F209, which is outside
this effort because it was not a conversion to an overload set at all. Those two were the target of
`b[i]`'s rewrite rather than a second spelling of a function, and the answer was to stop rewriting
— a MutableArray is indexed now, not called — rather than to find them a bare name. See
docs/DESIGN.md's "`b[i]` stops being a call". `mutable_set_add` is the one remaining `internal`
prefixed native under a bare name (`add`, since F199), and it is the one candidate a §16 would be
about.


## 16. `concat`, and separating the decision from the dispatch

Added 2026-10-21. §6 excepted `concat` from the arms this effort deletes, §10 filed it under the
polymorphic builtins the resolver never looks at, and string.zm carried a long comment calling it
*the conversion that cannot be done*, with three walls to prove it. The comment was right about
the walls and wrong about the conclusion.

**The three walls.** Each was found by trying, and each is still there:

1. `concat()` with no arguments must answer `""`. The string form and the array form both accept
   zero arguments, so the call contains nothing to choose by.
2. `concat(parts)` for a `[string]` must answer `"xyz"`, while the array form accepts the same
   argument as a single part and would answer `[string]`. The two differ **only in return type**,
   which F148 ruled out as a basis for choosing — the same reason `to_set` and `to_mutable_set`
   are two names.
3. `concat(b, [5], mutable([6]), [7])` mixes an array and a builder in one variadic tail, and a
   variadic parameter has one type. Two declarations can each state half of the tail; neither can
   state the whole of it, and a single one would need a union type, which docs/DESIGN.md refuses.

**What they are walls in front of.** All three block *picking a declaration from the arguments*.
None of them is a reason for the name to keep a builtin **id** — and that was the conflation. A
builtin id buys two things at once: a rule that chooses, and a dispatch that runs. `concat` needs
the first and never needed the second. Splitting them is the whole change: `#[compiler_resolved]`
marks the four declarations, the arm in `check_call` chooses exactly as before and records its
choice on `e->call_fn` instead of leaving an id for the interpreter to walk.

**What that is worth.** The declarations become the signature of record, so `--api builtins`,
editor completion and the error messages all read from them; named arguments work for the first
time (`concat(pieces: "p")` was a syntax error while the name was dispatched by id); the ~48-step
`else if` walk through the interpreter's dispatch is gone, which is why `push` and `insert` got
*faster* when they converted (§14); and `PURE_BUILTINS` loses an entry, purity now being read off
the declaration by the return-type rule rather than asserted by name.

**The fourth wall, and why the attribute is not a loophole.** The declaration checker enforces
distinguishability too: two same-origin declarations may share a name only when every call can
tell them apart, which is exactly what walls 1 and 3 say `concat` cannot do. `builtin fn` members
were exempt because the resolver never looked at them; `native fn` members are not. So the
conversion needs the exemption to be *stated*, and stating it on the declaration is better than
the alternative the author of this change first reached for — a list of names in `typecheck.c`,
which is the thing every stage of this effort has been deleting.

The risk is obvious: an attribute that turns off the check for a set no call can resolve is a way
to ship a broken set. `tools/overload_sets.sh` closes that by running the check **inversely** on
the new population — a set that carries `#[compiler_resolved]` while every call *can* tell its
declarations apart is reported as a defect, the attribute being dead weight that disables a live
check. So the exemption cannot spread to a set that does not need it, and the two populations stay
measured: **52 resolver-owned sets, 0 unresolvable; 9 polymorphic builtins; 1 compiler-resolved.**

**One member, one expectation.** A second name wanting this attribute should be read as a reason
to doubt the second, not as a precedent. Walls 1 and 2 are peculiar to `concat`: it is the only
prelude name whose two worlds answer *different types* to the *same* argument. `mutable` stays a
polymorphic builtin for its own reasons (§4) and is not a candidate.

## 17. `set`, the last collection arm, and the metric that could not see it

Added 2026-10-26. §14 ended *"One collection arm is left, `set`"*, and this is the end of that sentence. The arm is deleted, the array member is a `native fn` like the other four declarations, and `BUILTINS_WITH_NAMED_ARGS` is **empty**.

### What the arm was still doing

Ten checks, of which eight were things the resolver says anyway:

| check | who says it now |
|---|---|
| two arity checks (`< 1`, `!= 3`) | the resolver |
| `argument 2 of 'set' expects int` | the resolver |
| `argument 3 of 'set' expects T (the array's element type)` | the resolver, bar the parenthetical |
| *"expects an array, a Map, a MutableMap, a MutableString or a Mutable cell"* | **derived from the five signatures** |
| `Map`, `MutableMap`, `Mutable`, `MutableString` | already declined to the resolver |
| the array member's own type checking | `native_array_set` |
| `Set`/`MutableSet` → *"use add(set, item)"* | merged `#[when_unmatched]` |
| `MutableArray` → *"written with 'b[i] = value'"* | merged `#[when_unmatched]` |

The hand-written type list is the entry worth dwelling on. It is the same shape as the one §14 found had fallen behind `remove`'s declarations by naming two functions that no longer existed, and it was one `set` declaration away from doing it again: a `MutableString` member arrived on 2026-10-22, and the list was updated by hand that day. Derived, it cannot fall behind.

### The merged message, and the bug it exposed

Both refusals became one `#[when_unmatched]` line, the trade `remove`, `clear` and `snapshot` made before it (F190: the content survives, the aim does not). **Unlike `remove`'s, both halves were already pinned** — `collection_word_per_type.diag` and `mutable_array_set_rejected.err` — so the cost was visible the moment the arm went rather than afterwards. That is the §14 complaint answered: *"no fixture had ever asserted that message while the arm owned it, which is why the trade was invisible until the arm was deleted."*

And the fixtures earned it, because the premise of the trade was **wrong**. §13 assumed a merged line would at least be *shown*. It was not:

```
set(aSet, item)   ->  argument 1 of 'set' expects Mutable<int>, found Set<int>
```

Two arguments match exactly one declaration — the `Mutable<T>` cell — so the call never reaches the overload resolver, and the resolver is where the set-wide `#[when_unmatched]` lookup lives. The hint, which the array member carries for the whole set, was never consulted. The message is true and worse than useless: nothing about a set suggests a cell.

The resolver had always read the attribute set-wide, and its own comment says why -- *"it is about the name, not about which declaration happened to be looked at first"*. The two single-candidate paths read it off the one declaration they had. `when_unmatched_for_name` gives them the same lookup, which is a fix to the **mechanism** rather than a workaround for `set`: any set whose hint lives on a sibling member now shows it wherever a single candidate fails on a type. `make test` found exactly the two fixtures that pinned `set`'s messages and no others, which bounds the change.

### The metric could not see the arm

`make overload-sets-check` reports **71 resolver-owned sets, 0 unresolvable; 4 polymorphic builtins; 1 compiler-resolved** — and reported exactly that before this change too, measured by running the tool against the previous binary.

So the number this effort has quoted since §1 never tracked the quantity being reduced. A **mixed** set already counts as resolver-owned: four of `set`'s five declarations were ordinary `fn`, so the set was counted whole while one member was still dispatched by id and one arm still answered before any declaration was consulted. §14 quoted a move from "39 and 20" to "41 and 18" as *"the measurement to quote"*, and for that step it was — but it cannot see an arm standing in front of a set it already counts.

What tracked the last arm was **prose**: §14's closing sentence. That is worth stating plainly rather than filing as a curiosity, because the same shape bit the effort twice already — §14 notes the running total "has been re-derived by hand since, incorrectly at least once", and §13 found `insert` on a `Set` accepted for two passes with no guard able to notice. **A gate that counts sets does not count arms.** The honest measurements for this change are: 74 lines of arm deleted, one builtin id and its dispatch branch gone, `ziman_builtin_count()` down by one, and `BUILTINS_WITH_NAMED_ARGS` from one entry to none.

The shadow gate that would have caught it is gone on purpose — `make/ziman.mk` records it standing from F176 to F180 and removed because *"with the last arm gone it has no second opinion to compare against, and a gate that can only pass is worse than no gate"*. That reasoning was sound and is now, finally, accurate.

### What the conversion bought beyond the deletion

`set` takes named arguments. `set(items: a, index: 2, x: 0)` was rejected before for no better reason than that `set` had an arm — the restriction §14's `BUILTINS_WITH_NAMED_ARGS` comment called *"a restriction with no reason behind it"*. Emptying that table is the end of its story: every name that was dispatched-but-armless has now either been converted or had its arm deleted.

**No collection arm remains.** `concat` keeps the §16 shape -- the decision without the dispatch -- and `mutable` stays a polymorphic builtin for the §4 reasons. Neither is a collection verb, so the sentence §14 left open is closed rather than moved on.
