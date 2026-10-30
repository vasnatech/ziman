# Larger, needs its own design pass

Part of the [roadmap](README.md) -- see its preamble for what an entry's status means and how it goes stale.


- **Module-level vals are initialised in file order, and that is a constraint on the program.**
  (Raised while fixing F247, 2026-10-30. `src/interpreter.c`'s `init_param_defaults` recommends one
  answer; this entry argues for a different one.)

  ```ziman
  val greeting = decorated("one")              // runs first

  fn decorated(text: string) -> string {
      return concat(prefix, text)
  }

  val prefix = "p:"                             // not initialised when decorated() reads it
  ```

  > `'prefix' is read before it is initialised: module-level vals are initialised in file order,
  > and this one is reached by an initialiser that runs earlier -- move 'prefix' above the val
  > whose initialiser uses it`

  Pinned by `tests/lang/global_val_read_before_initialised`, and it **segfaulted** until
  2026-10-26: the type checker proves the name resolves, so the only route to a NULL binding is a
  val that resolves and has no value yet, and `EXPR_IDENT` returned `b->value` without looking.
  The message is the fix for the crash, not for the problem.

  There is a **second eager pass** beside it. `init_param_defaults` hoists every omitted
  parameter's default to a module-level binding and runs *before* `init_global_vals`, because a
  val's initialiser can reach a function that relies on one: `pub val iso_date_format =
  datetime_format_of("yyyy-MM-dd")` calls `mutable_array_new<T>()` with both parameters omitted,
  and the other order crashed. Its own comment records **the half that ordering does not address**
  -- a default expression referencing a global val -- as unhit only because every default in the
  codebase today is a self-contained literal (`20`, `2.0`). So there are two passes, a fixed order
  between them, and a known gap in the pair. 288 module-level vals in `lib/`, 317 in the sample
  backend.

  ### The fix the source recommends, and why this entry does not

  `init_param_defaults` says the real answer is **evaluating globals lazily, on first reference,
  rather than two fixed eager passes**. That removes the constraint rather than reporting it, and it
  closes the gap: a default referencing a val and a val referencing a default both just work.

  But `g_global_env`'s own comment names the invariant it would cost:

  > Built once, before `main` runs, from a single thread; never mutated afterward, so concurrent
  > reads from a `spawn`ed thread are safe with no locking.

  Lazy initialisation makes the global env **mutable after `main` starts**, so every global read
  from a spawned thread needs synchronisation, or every lazy slot needs a once-guard. That is a
  real cost paid by every program to fix an ordering mistake in a few, and it is the kind of change
  that turns a data race into the next bug of this family rather than the last. Two smaller costs
  follow it: an initialiser with observable effects runs at a less predictable time, and one that
  is never referenced never runs at all.

  ### What this entry recommends instead: initialise in dependency order

  Keep initialisation **eager, single-threaded and before `main`** -- so the env stays immutable
  and the threading invariant holds -- and stop using *file order* to decide it. Build the
  dependency graph among module-level vals and hoisted parameter defaults, topologically sort it,
  and initialise in that order. The type checker already resolves every name in every initialiser,
  which is the information the graph needs.

  What that buys:

  * **The file-order constraint disappears**, which is the actual complaint.
  * **The two passes become one.** No fixed order between defaults and vals, and the "unaddressed
    half" is addressed by construction rather than by nothing having hit it yet.
  * **A better error for the case that is genuinely impossible.** "read before it is initialised --
    move it" becomes "these vals form a cycle: `a` → `tag` → `a`", which names a program that
    cannot be initialised in any order rather than one whose author put a line in the wrong place.
  * **No runtime cost at all.** The work is a sort at load time; a global read stays a slot read
    with no guard and no branch.

  What it gives up: the initialisation order stops being the order a reader sees in the file, which
  matters for an initialiser with side effects. The honest answer is that a module-level val whose
  initialiser has observable effects is already relying on something the language has never
  promised, and a cycle error is the only case where any of this is visible.

  ### The design pass, run 2026-10-30

  Six shapes were written and run. Two of the three questions below answer themselves from what
  they show, one finding corrects this entry, and one is a bug reachable today.

  **The graph needs a call graph, not resolved names -- this entry had that wrong.** "The type
  checker already resolves every name in every initialiser, which is the information the graph
  needs" is not enough, because **a lexical forward reference from an initialiser is already a type
  error**:

  ```ziman
  val greeting = apply(fn() -> string { return prefix })   // use of undefined variable 'prefix'
  val prefix = "p:"
  ```

  `check_global_vals`' deliberate scope cut means an initialiser can only *see* vals above it. So a
  topological sort over the references in initialisers would sort a graph with no forward edges in
  it at all -- a no-op. Every forward dependency that exists runs **through a function body**, which
  is exactly what this entry's own opening example does: `greeting`'s initialiser names `decorated`,
  and `decorated`'s body names `prefix`. The graph is therefore "for each val, the globals read by
  the transitive closure of the functions its initialiser can reach", which is a call-graph
  reachability pass that has to be written, not a by-product of name resolution.

  **And that closure cannot be exact, because a function value carries one too:**

  ```ziman
  fn maker() -> fn() -> string { return fn() -> string { return prefix } }
  fn call_it(f: fn() -> string) -> string { return f() }
  val g = call_it(maker())      // reads prefix at run time, through a value
  val prefix = "p:"
  ```

  So a **fourth question** joins the list, and it is the one that decides whether this is worth
  building: what an indirect call contributes to the graph. Treating it as a dependency on *every*
  global is sound and would make `val x = apply(f)` refuse to initialise at all; ignoring it is
  unsound and leaves this case exactly as it is today. The recommendation is to **ignore indirect
  calls and keep the runtime check as the backstop** -- it already exists, it costs one NULL test
  on a global read, and it is the only thing that can catch a dependency the call graph cannot see.
  That means the sort makes the common case work and the runtime error becomes the rare case's
  report rather than the normal one, which is a smaller claim than "the file-order constraint
  disappears" but is the true one.

  **The unaddressed half is not unhit. It reproduces, and the message misdiagnoses it:**

  ```ziman
  val limit = 20
  fn take(n: Option<int> = limit) -> int { return Option.unwrap_Some(n) }
  ```

  > `'limit' is read before it is initialised: ... move 'limit' above the val whose initialiser
  > uses it`

  `limit` **is** above it; file order is already correct and no move fixes this, because the cause
  is that `init_param_defaults` runs before `init_global_vals` wholesale. The pair is genuinely
  stuck: `iso_date_format` needs defaults first, this needs vals first, so no fixed order between
  two eager passes satisfies both and **only the sort (or laziness) fixes it**. That is the
  strongest argument for this entry that exists, and it was sitting in `init_param_defaults`' own
  comment as a hypothetical.

  **The answers.** (1) In **`typecheck`** -- the loader has no call graph, and typecheck resolves
  calls -- while noting the pass is new work rather than a free read of existing information.
  (2) A **type error**: `--check` passes on every failing shape above today, so these reach a user
  at run time, and building the graph where the calls are resolved turns them into a `--check` and
  `--lsp` diagnostic. (3) **Merge entirely.** Each hoisted default already has its own global slot
  (`Param.default_key`), so it is a node like any other, and the fixed order between the two passes
  -- the thing that causes the bug above -- is precisely what the sort replaces. A separate stage
  would keep the defect.

  F247's fix is unaffected either way -- that was one env slot holding two programs, not an
  ordering question.


- **`meta`, redesigned.** Designed in [docs/design/meta.md](../design/meta.md), not built. The
  derives-in-Ziman port is finished and what it left behind is `meta`, grown one field at a time
  by whichever generator needed it next. Two halves. The **model** gains what `derive(Entity)` and
  `#[repository]` actually read -- `#[pk]` as a member attribute and the derive's argument -- and
  **`MetaType` becomes a tagged `enum`** rather than `kind: string` + `args`, which is the change
  that matters: a `switch` over a Ziman enum is exhaustive with no default arm, so adding a type
  kind becomes a compile error in every generator that ignores it. The three bugs in question
  were all a kind flattening silently; under an enum that state cannot be represented. `Decl`
  follows, losing `is_enum` and `has_payload` -- the latter's own comment says it is always false
  for a struct field. **Its §8 is the finding**: `#[repository]` is not a derive. Its inputs
  live in a *different module* from the entity struct, it reads what the module already defines,
  and it is not a function of one declaration -- so it needs a second generator kind
  (`generate_module` over a `Target`) rather than fields bolted onto `Decl`. Whether that is
  worth having, or whether one feature needing its own entry point means it should stay in C, is
  the decision the document turns on. The **test** matters more: a `#[derive(Reflect)]` generator that dumps
  what it was handed, pinned by a fixture over real declarations. Every fixture today builds its
  `MetaType` by hand, so the C-to-Ziman boundary has never been tested at all -- which is why
  three bugs shipped in the bridge and none was caught by 1,293 fixtures. Step 5 ports
  `derive(Entity)`, taking the last generator out of `src/derive.c`.

- **An expression language, and `template.zm` using it.** Designed in
  [docs/design/expressions.md](../design/expressions.md). **Step 1 is built**: `lib/core/expr.zm`
  parses and evaluates the whole grammar over `Json` -- navigation with `?.`, the operators,
  `in`, the ternary, and the fixed function set -- with `tests/lang/expr_language` pinning every
  operator and every refusal. Steps 2-5 (snake_case keys, `template.zm` on a `Json` context,
  `template` adopting `expr` with `@if`/`@elif`/`@else`, and `render_text`) are not. **Two modules, one direction**:
  `lib/core/expr.zm` knows nothing about templates and `template.zm` depends on it, so `expr` is
  usable with no template at all -- which is the whole of the config-and-validation use -- and
  `template` ends up holding no grammar of its own. A SpEL-shaped language for
  configuration and validation rules and for dynamic text, evaluated over `Json` -- which is
  forced rather than chosen, because Ziman has no runtime reflection and a `Json` is the only
  thing an interpreter can navigate. `#[derive(Json)]` is what makes any struct available to it.
  The same grammar then replaces `template.zm`'s one-segment paths and its `@choose`, which §4
  of that doc argues are already a restricted expression language rather than an absence of one.
  **The cost is named in its §13**: generator logic in a template is invisible to the type
  checker, so the port keeps computation in Ziman and allows expressions only to read data. Five
  staged steps, two of which migrate ~1,100 lines of generator Ziman against byte-identical
  fixtures.

- ~~**Derives should be written in Ziman, not in C.**~~ **DONE 2026-10-28.** All five targets --
  `Ord`, `Row`, `Properties`, `Json` and the struct half of `Entity` that `derive.c` still owns --
  are answered by `lib/core/derive_*.zm` through `src/generators.c`, each proved byte-identical to
  the C it replaced before the C was deleted. **`src/derive.c` is 2,505 -> 1,759 lines**, and
  there is no C fallback for any target: a `ziman_generate_in_ziman` that answers NULL now means
  the corelib did not load, not that the C should take over. What building it cost, and the three
  bugs wiring `Json` found in the bridge, are in `docs/design/templates.md` and the commits. The
  problem statement below is kept because the argument it makes is the reason the work was done.
  Raised 2026-10-28, immediately after shipping derived queries (F217), from the observation that
  the generator keeps growing: *"ziman's derived functionality is getting bigger. We need to find a
  scaleable way to implement derived content within ziman and not C."*

  **Measured, because the growth is the argument.** `src/derive.c` was **463 lines** on 2026-09-23,
  the day the first derive landed; **1,090** by 2026-09-25 with `Row` and `Properties`; **1,340** on
  2026-10-24; and **2,419** today. Over a thousand of those lines arrived in four days, from
  `#![repository(..)]` and `#[repository]` alone. It now holds nine generators over five derive
  targets and two repository forms, **240 `cb_appendf` calls and 37 `ziman_error` refusals**.

  **The reason this is portable at all, and the thing to notice first: every generator is a string
  builder.** Each one's type is `declaration -> char *`, and the result goes through the ordinary
  lexer and parser (`parse_and_splice`). Not one of them constructs an AST node by hand. A Ziman
  function that takes a declaration and answers a `string` is *the same function* -- this is a port,
  not a redesign.

  **What the C costs today**, concretely rather than as a feeling:

  * **A project cannot add a derive.** Adding one means editing `derive.c` and `parse_attributes`,
    rebuilding the compiler, and shipping a binary. So `#[derive(Entity("table"))]` exists because
    *this* repository's sample backend wanted it, and an application with its own repeated shape
    has no way to write the equivalent.
  * **None of the language's own tools can see the generators.** `--fmt`, `--lint`, `--coverage`,
    the fixture suite and `--api` all apply to Ziman. 2,419 lines of C that write Ziman are outside
    every one of them.
  * **A generator can only be tested end to end** -- run a program, compare its output. There is no
    way to call `generate_struct_to_json` on a declaration and diff the text, which is the obvious
    unit test and is free the moment the generator is a Ziman function.

  **The bootstrap objection does not apply here, and that is measured rather than assumed**: `lib/`
  contains **zero** `#[derive(..)]` and **zero** `#![repository(..)]` sites. Every greppable hit in
  the prelude and the bundled modules is a *comment* about a derive. So a generator written in
  Ziman needs nothing that a generator produces, and loads as an ordinary module. The 140 derive
  sites and 20 repository attributes in the sample backend, and the 103 in this repository's own
  fixtures, are all user code -- none of it on the path a generator would load through.

  **What a design has to decide:**

  1. **How a declaration reaches Ziman.** A reified `Decl`/`Field` value -- name, visibility,
     fields with their names, types and attributes. This is the one piece of C that stays, and it
     is a data conversion rather than a code generator. It is also the real cost: that shape
     becomes a public API pinning compiler internals, where today `StructDecl` is free to change.
  2. **How a generator is found.** docs/design/journal/'s 2026-10-28 rule generalises -- *a bodyless `fn`
     carrying attribute X is filled by X's generator* -- to `#[derive(X)]` looking for
     `derive_<x>(decl) -> Result<string, string>`. **Where the project names that module is
     undecided and is the hard half**: there is no manifest file in this language today, and
     inventing one for this is a larger change than the port.
  3. **When it runs, and re-entrancy.** Generation already happens at one point in `load_module`,
     between dependency loading and import resolution. Running the *interpreter* there, inside the
     load of a module that is itself mid-load, is a new path through the loader. The generator's
     own module must be loaded to completion first, and a generator module that transitively uses
     the attribute it implements is a cycle to refuse by name.
  4. **Refusals, which are most of the value.** Those 37 `ziman_error` calls are why a mistyped
     derived query is a type error at the author's own line instead of three frames of library code
     at startup. `Result<string, string>` keeps that with the C side attaching the position -- but
     the refusals that list an entity's columns, or show two readings of a name, need the message
     to carry structure, or they degrade into one flat string.
  5. **Cost.** Generation becomes interpreted work, per module, per compile, where today it is
     string building at memcpy speed. The number to measure first is the North Star's 140 derive
     sites.
  6. **Trust.** A generator is code the compiler runs at build time. This project has not had to
     answer that question yet.

  **How to stage it**: port **one** generator end to end before designing the rest. `Ord` is the
  smallest -- two functions, about 40 lines, no imports to ensure, no `Option` handling -- and it
  exercises the struct *and* the enum shape. **Delete the C version rather than keeping it as a
  fallback**: two implementations of one generator is the failure this should not buy.

  **Started 2026-10-28. The generator exists and is proved equivalent; the bridge is not built.**
  `lib/core/meta.zm` is the reified declaration (`Decl`, `Member`, and the two helpers a signature
  needs) and `lib/core/derive_ord.zm` is `#[derive(Ord)]` written in Ziman. Its output is
  **byte-identical** to `src/derive.c`'s over a struct, a generic struct and an enum with and
  without payloads, which `tests/lang/derive_ord_in_ziman` asserts -- and that fixture is itself
  the argument for the port, being a test the C version could not have: a generator in C can only
  be exercised end to end, where this one is a function from a declaration to a string.

  **What the C still did at that point, and what was left.** `derive.c` ran its own
  `generate_struct_ord` on every real `#[derive(Ord)]`; nothing called the Ziman generator. The
  missing piece was the bridge, and the architecture for it was known rather than guessed --
  it was built as described:

  * **It is sequential, not re-entrant.** `ziman test` already loads, type-checks and runs several
    independent programs in one process, so a generator program loaded *before* the user program
    is the same shape, not a new one. `interpret_tests` is the harness to copy: set `g_program`
    and `g_global_env`, push a `<module initialisation>` frame, `init_param_defaults` and
    `init_global_vals`, then `call_function` under a `ThreadBoundary`.
  * **Two things have to be built**: a loader entry point that makes a program out of a *bundled*
    module (`load_module_graph` only takes a disk path, and a generator must ship in the binary),
    and a C function that builds a `Decl` value -- `struct_obj_new` is the primitive, and no
    native builds a struct today, so this would be the first.
  * **`g_program` and `g_global_env` are single globals**, which is what makes the ordering
    load-bearing: the generator must be fully initialised before the user program's load begins,
    and `interpret(user)` reclaims both afterwards. `--lsp`, which loads repeatedly, needs its own
    look.

  **What this is not**: a macro system. The input is a declaration and the output is source text,
  exactly as today. Nothing here proposes running user code over expressions or statements.

  **A template module is designed and not built** -- `docs/design/templates.md`, 2026-10-28. The
  port made the case for it visible: a generator written as `push`/`concat` hides the thing it
  exists to produce, and `derive.c` is 240 `cb_appendf` calls and 291 `\n` escapes assembling
  source a fragment at a time. `@{name}` holes, `@for`/`@if`/`@else`/`@end` on their own lines, bindings
  in `eql.bind`'s shape, parsed at module load like `repository.query`. **No expressions of any
  kind**: `@if` takes a bound boolean, `@elif` does not exist, and anything that needs computing
  is computed in Ziman where the type checker can see it. §12 then does the comparison, on `derive_ord`
  rather than `derive_row`, because that generator already exists both in C and in Ziman and its
  output is pinned byte for byte -- so the rewrite changes one variable instead of comparing two
  pieces of new code. It settles one question and not the other: **the no-expressions rule holds
  comfortably** (neither half of `Ord` needs `@if` at all, once the logic is in Ziman), while
  **whether templates pay at scale is still open** -- the struct half is a wash at 17 lines
  against 10, and the enum half's 36 lines of fragments are one readable block. `derive_row` is
  where that second answer is, and where `@if` finally gets exercised.

  **Built 2026-10-28** (design doc §13): `lib/core/template.zm`, with `derive_ord` rewritten on
  it and the pinned fixture unchanged byte for byte. Two things the build taught that the design
  had not: a text block drops the newline before its closing delimiter, so the blank line every
  generated function ends with needs *two* in the source -- the first attempt was off by one
  blank line per function and the fixture caught it; and a module whose strings contain Ziman
  broke `tools/gen_lib_manual.py`, which reads declarations off raw lines and found
  `pub fn @{snake}_compare..` inside a template. Both are the generators' problem now rather than
  one-offs.

  **`derive_row` ported 2026-10-28** (design doc §14), byte-identical to the C on the first run
  over a ten-field struct with every scalar, two short-spelled bundled types and three shapes of
  `Option`. It answers the scale question **without qualification, after one wrong
  turn**: the first port left the conversions as `concat` on the argument that `Option<U>` wraps
  *U's own conversion* and a template cannot contain itself. The second half does not follow --
  the recursion is in the *call*, and each level has a fixed shape with a hole for the nested
  text. All six emitted shapes are templates now: 41 lines of template against the C's 128, with
  60 lines of ordinary Ziman for dispatch, recursion and temporary names. The rule the port found
  is sharper than the design guessed: **every line of output comes from a template, and `concat`
  builds only the values that go into the holes.** **`@if` is still unused** -- an `Option` field
  and a required one are two shapes, so they are two templates chosen by an `if` in Ziman, not
  one template with a branch. `derive_json` and `derive_properties` decide whether it keeps its
  place.

  **§15 then redesigns the template model, on a complaint the port earned** (2026-10-28, user:
  *"I really don't like this minimal template definitions"*). Every line of output coming from a
  template was not enough: `derive_row` holds seven `must_parse` vals over a hundred lines, the
  composition between them is invisible, and the dispatch that picks one lives 150 lines away.
  The tell was the file's own comments, which paste the assembled output because the templates
  cannot show it. Designed, not built: **`@template name` … `@end`** so a source is a *sheet* of
  named templates and a generator has one `val`; **`@choose tag` / `@when` / `@otherwise`**, a
  switch on a bound string that keeps the no-expressions rule while putting every shape and the
  choice between them in one place; and **`@yield`**, a block hole that re-indents, which is how
  the generated code's mis-indentation gets fixed opt-in. **`@if`/`@else` are removed** -- zero
  users after two generators, and `@choose` covers them -- taking `template.flag` with them, so
  the binding kinds drop to `text` and `rows`. `derive_row`'s seven vals become one sheet of four
  templates, forty lines, in the order a reader meets them.

  **Built the same day**, both generators ported in the change and both fixtures unchanged byte
  for byte: seven `must_parse` vals across the two files became two. `@yield`'s re-indentation
  shipped and is used at column 0 everywhere, so the generated code still matches the C and the
  mis-indentation inside its `if` is one edit away. The rule that every body line carries its
  newline deleted a helper -- and the first implementation kept the old "last line carries none"
  habit and was off by a blank line per function, caught by the fixtures at once.

  **`derive_properties` ported next** (design §16), byte-identical, and it settles `@choose`: its
  two field shapes are a genuine either/or with no recursion, the case `@if` was designed for,
  and `@when required`/`@when optional` read better than `@if optional`/`@else` because they
  name the shapes. It also found the model's first real gap -- **`@for` cannot build an inline
  list**, since a directive owns its whole line, so `<name>_keys`'s one-line array is joined in
  Ziman and bound as a single hole. Not fixed: one occurrence across three generators is not
  enough to design an inline construct against. `derive_json` is the last of the five and says
  whether it is a one-off.

  **And the question the port cannot answer by itself** -- *what links `#[derive(Properties)]` to
  `derive_properties.zm`?* -- is designed in `docs/design/user-generators.md` (2026-10-28, design
  only). Nothing links them: the five targets are a `strcmp` in `derive.c` and the Ziman modules
  are referenced by no C at all. The proposal answers it for a project's own derive and for the
  bundled five at once: **discovery is the importing module's own `use` list**, which
  `resolve_repository` already proves as a mechanism and which invents no manifest;
  **`#![generator(X)]` declares the link** where the `derive_<snake(x)>` convention would have
  concatenated a name and checked nothing (question 5 above); and the entry point is a fixed
  `generate(d: meta.Decl, arg: Option<string>) -> Result<string, string>`. Two things it makes
  explicit rather than discovering later: generator globals need **per-module initialisation in
  dependency order**, which is what `use`-based discovery costs instead of a manifest, and
  **`--check` and `--lsp` stop being execution-free**, which is a real change and is argued for
  rather than assumed.

  This is the continuation of "Code generation from annotations needs a mechanism" below, whose
  questions 2 (writing into another module), 4 (materialising generated declarations) and 5
  (checking the name contract) are all easier to answer once the generator is Ziman.

- **Derived queries: a repository function's name is its query. Shipped 2026-10-28 (F217);
  designed in full first, in `docs/design/derived-queries.md`.**
  Raised 2026-10-27, after converting all 26 repositories of the sample backend to `eql`. The
  bodies written during that conversion are derivable from the names they sit under, because the
  repositories are hand-ports of Spring Data interfaces and their own comments cite fourteen of the
  originals. Counted rather than guessed: **91 of 144 repository function instances are derivable
  from entity field names alone**, and the derivable ones are the repeated ones -- six name shapes
  cover 69 functions (`delete_by_patient_id` x19, `find_all_by_patient_id` x16, the scoped `delete`
  x15, `find_by_id_and_patient_id` x12).

  This supersedes the `#[scope]` sketch, which was the small version of the same idea: a scope
  attribute on a field, putting the owner into every generated signature. The grammar makes it
  unnecessary -- `find_by_id_and_patient_id` states the scope, its order, and which functions have
  one, which is the three open questions `#[scope]` could not answer.

  **The reason to build it rather than copy Spring Data** is in the design doc's §4.2: Spring binds
  parameters positionally, so `findByIdAndPatientId(patientId, id)` -- two UUIDs transposed --
  compiles, boots and answers the wrong rows forever. Matching parameters by *name* against the
  predicate's terms makes that a compile error, and it is the same failure mode `#[pk]` and
  `insert_one` were built to remove.

  **What shipped** is §§1-8 exactly as designed -- the design needed no revision once it met the
  compiler, which is the whole argument for writing the grammar down first. The nine questions the
  implementation settled on its own are the design doc's new §10, and the three decisions that are
  about the *language* rather than about the feature are in docs/design/journal/'s 2026-10-28 entry.
  Seventeen fixtures (`tests/lang/derived_query*` and `cli_emit_derived_query`), twelve of them
  refusals.

  **Not shipped, and not a gap**: the 49 non-derivable instances. Every write but the keyed ones,
  the domain words (`find_current_by_..`, `find_recent_by_..`), projections, paging and the
  non-column groupings stay hand-written under the rule that a hand-written body wins. Paging is
  the one of those that could become a design question later; the rest are not meant to be reached.

  **Adopted the same day, and adoption found a real limit** -- `docs/design/derived-queries.md`
  §11, and docs/design/journal/'s 2026-10-28 entry. 30 functions in 19 repositories of the sample backend,
  121 `repository.query` vals down to 97. But `find -> Result<E, DbError>` and
  `delete -> Result<unit, DbError>` are **unused there**, because they raise `DbError.NotFound`
  and the generator has to write its message -- and the message that reaches a 404 body needs the
  entity's *label* ("Vital sign"), which the application had already decided is not derivable from
  the struct's name, plus the row id, which the generator never sees. Two suites assert the exact
  wording. That costs the two most repeated shapes in §2's table (`find_by_id_and_patient_id` x12,
  the scoped `delete` x15).

  **That was fixed the same day with one optional argument** (F218): `#![repository(Medication,
  label: "Medication")]`, a fact about the module's entity rather than per-function configuration.
  The generator writes `"Medication not found: <id>"`, the text the application had by hand, so
  both forms are usable and the adoption finished at **68 functions in 19 repositories**, 121
  `repository.query` vals down to 59.

  **One thing is left, and it is a coupling rather than a gap** -- design doc §12. `#[repository]`
  is only accepted in a module carrying `#![repository(E)]`, and that attribute both *names the
  entity* (all a derived query needs) and *generates the nine CRUD functions*. **Seven
  repositories refused the attribute, every one of them over the CRUD** -- `patient` and `user`
  because it would make `delete_all(conn)` over every account callable, `audit_event` because an
  audit row is not updated or deleted, `recipe_ingredient` because its rows are never addressed by
  id, `notification` because `eql.insert_one` is the only one of the nine it wants. Four of them
  have **13 derivable functions** between them, each a shape already derived elsewhere.

  **Built as `crud: false`** (F219): `#![repository(Notification, label: "..", crud: false)]`
  names the entity and generates none of the nine. A per-module opt-out, which keeps it clear of
  the per-function configuration §8.4 refused. The four modules took it and the count came back
  12 rather than 13 -- `notification.count_by_patient_id` takes an `Option<string>` and branches
  on it, so it is a function with a predicate rather than a predicate with a name.


- **Code generation from annotations needs a mechanism, not a series of exceptions. Not started;
  this is the design talk.**
  Raised 2026-10-26, from the conversation that added `#[derive(Entity("table"))]` and then tried
  to design a `#![repository(..)]` that generates the ten CRUD functions a repository writes by
  hand. The annotation is not the point of this entry -- **the point is that designing it kept
  hitting the same wall from different directions**, and each time the answer was a workaround
  rather than a mechanism.

  **The wall is one line**: `generate_derived_functions(module)` is called from `parser.c`, at the
  end of parsing **one file**, before any import is resolved. A generator therefore sees the
  declaration it is attached to and nothing else -- not the imported type, not the schema, not the
  module it would like to write into. Every awkward part of the current derives is downstream of
  that:

  * **A generator cannot read a type it does not own.** `#![repository(VitalSign)]` in
    `repository/vital_sign.zm` cannot see `VitalSign`'s fields: the entity module may not be parsed
    yet. So the design had to become "emit thin typed wrappers over runtime functions that take the
    metamodel as a value" -- which is a good design, but it was forced rather than chosen, and it
    puts a runtime cost and a runtime failure mode where a compile-time one belonged.
  * **A generator cannot emit into another module.** The attribute must sit where the code should
    land, so the choice was between putting repository code in the entity (inverting the layering:
    entities are data, `datasource` is four layers up) and a module attribute that cannot see the
    entity. Neither is a good answer to "generate this module's functions from that module's type".
  * **Compound keys fall out of reach for a reason that is purely mechanical.** With `#[pk]` on
    fields the metamodel knows the key columns, and a two-column key is expressible -- but the
    repository-side generator still cannot emit a two-argument `find`, because it does not know how
    many there are. The feature is blocked by the file boundary, not by the design.
  * **Collisions are undefined.** A hand-written function whose name a generator would emit is a
    duplicate declaration. There is no `skip:`/`only:`, and silently not emitting is the
    parse-and-do-nothing failure this project refuses elsewhere (see `#[deprecated]`'s own history).
  * **Generated code is invisible.** Twenty-one entities now carry three generated vals each that
    appear in no file; `--emit-derived` is the only way to see them. That is tolerable for three
    vals and not for ten functions.
  * **The names are strings.** `pascal_to_snake(name) + "_columns"`, and `compare`/`error_message`
    find `<type>_compare`/`<type>_message` by name at run time. Nothing checks that a generator's
    output and its consumer agree on a spelling; `tools/check_builtins.py` checks the rename hints
    and cannot check this.

  **What a solution has to decide**, in rough order of how much it changes:

  1. **When generation runs. Decided 2026-10-26: after a module's dependencies are loaded.** The
     specification that settled it is one line -- `#![repository(VitalSign)]`, everything else
     taken from the entity, and no hardcoded `Uuid` -- which cannot be met any other way: the
     parameter name and type in `delete(conn, id: Uuid)` are compile-time text, and they live on
     the `#[pk]` field in another module. The alternatives were to repeat the fact in the
     annotation or to assume `id: Uuid` (right for 27 of 27 entities today, and silently wrong the
     first time it is not -- a wrong parameter *name* with the right type compiles and reads fine).

     **`load_module` already has the insertion point.** It parses the module, then loads every
     dependency to completion, then resolves named imports. Generation belongs between the second
     and third: dependencies are loaded, so a generator can reach an imported `StructDecl` through
     the module's own `uses`, and imports are not yet resolved, so `ensure_module_import` still
     works for whatever the generated code needs. Today the call sits at the end of `parse_module`,
     which is before all of it.

     Three edits and one helper -- move the call, and give `derive.c` a way to look up a type
     through the importing module's loaded uses, which it has never needed. **The claim to verify
     rather than assert** is that none of the seven existing targets depends on running before
     anything else is known: each reads only its own declaration and splices into the same module,
     so each should be indifferent, and the fixture suite is what says so.
  2. **Whether a generator may write into a module it is not declared in**, and if so how that is
     spelled and how a reader finds out it happened.
  3. **What happens on a collision**: refused with the name, or overridden by the hand-written one
     with a report. Silence is not on the list.
  4. **Whether generated declarations are visible in the source** -- a `--emit-derived --write`
     that materialises them, a generated sibling file, or nothing beyond the current flag.
  5. **Whether the name contract can be checked** rather than conventional.

  **Why it is worth doing rather than working around again.** The four derives that exist (`Json`,
  `Row`, `Ord`, `Properties`, `Entity`) each stayed inside one declaration, so the wall never came
  up. The first generator that wants to span two declarations hit all six bullets in one afternoon,
  and the design that came out is shaped by the mechanism rather than by the problem. The next one
  will be too.

  **What not to do first**: build `#![repository(..)]` on the current mechanism. It is possible --
  thin wrappers over runtime functions keyed off the metamodel -- and `eql`'s runtime CRUD half is
  worth building on its own merits, since it is callable without any annotation. But the wrapper
  layer should wait for this design pass, or it becomes the seventh exception.

  **The warning was followed and two more of the five are now answered.** The design pass happened
  first (`docs/design/derived-queries.md`, 2026-10-27) and `#![repository(..)]` was built on the
  moved generation point rather than on the old one, so there was no seventh exception. Question 3
  is settled: **a collision is overridden by the hand-written declaration**, reported by
  `--emit-derived`, which adoption then turned into a deliberate mechanism rather than a
  grandfather clause (docs/design/journal/, 2026-10-27). And the mechanism this entry asked for has a shape:
  **a bodyless `fn` carrying attribute X is filled by X's generator** -- no registry, no owner, no
  what-if-two rule -- which is what `#[repository]` is (docs/design/journal/, 2026-10-28). Still open:
  question 2, writing into another module; question 4, materialising generated declarations; and
  question 5, checking the name contract.

- **Relaxing the no-overloading rule: let a *declaration* carry the dispatch the arms hand-code.
  Shipped 2026-10-19 (F174-F184), and built on the same day (F185-F201).**
  Raised 2026-10-18, at the end of the collection-pairing pass (F149-F162), from the question
  "now that we have quite a number of overloaded functions, what do you think on relaxing the
  no-overloading rule?" The evidence below is the case as it was argued; **what was actually
  built, and where it stopped, is under "What shipped".** This entry predicted the opposite --
  "not started, and deliberately not started yet", on the grounds that this project writes a case
  down and lets it sit, which is what declined `..=` and the handle-validation redesign. The
  prediction was wrong, and the thing that made it wrong is worth keeping: reading
  `infer_call_type_args` dissolved the one objection that had looked fatal.

  **The rule to restate first, because it is not what the docs said.** Ziman does not lack
  argument-based overloading: it has **28 overloaded builtin names over 87 declarations** (`len`
  and `contains` nine each, `str` six, `snapshot`/`remove`/`mutable`/`concat`/`clear` four).
  What it lacks is overloading *available to a declaration*. Every one of those sets is resolved by
  a hand-written arm in `typecheck.c`, and `builtin fn` entries in `lib/prelude.zm` are
  documentation that the arms are free to contradict. Two paragraphs of this file asserted the
  absence of a feature the language has in 28 places; both are corrected above.

  **What the pass proved about the mechanism, since this is the whole argument.**

  *First-argument dispatch was sufficient every time* -- 28 names, not one needing more. And the
  single case where it was **not** sufficient, `to_set` against `to_mutable_set` (same argument
  types, differing only in what they return), is the case where the restriction gave the right
  answer: they stayed two names. That boundary is worth keeping, not relaxing.

  *Nine of the ten things that broke during the pass trace to the arms, not to overloading.*
  A diagnostic quoting `$mutable_array_lit`, a name that does not lex. A discarded `mutable(42)`
  accepted in silence, and `remove`'s mirror-image trap, both because the purity gate is keyed per
  *name* while purity is per *overload*. `concat`'s spread rule written twice and the two copies
  disagreeing, so `concat([1, 2])` answered nothing. The quadratic lint keyed per name, leaving two
  dead branches and advising a `MutableString` for an array. Eight named-argument call sites broken
  and `sort<int>([])` made unspellable, both because overloading is builtin-only and a builtin
  takes neither named arguments nor type arguments. A fixture repointed off `slice`. Only the
  `[string]` ambiguity in `concat` is a cost of overloading *itself*.

  *And the declarations drift because nothing makes them binding.* `str` documented "accepts any
  type" while its arm refused `string`; `print`'s recorded reason stayed false for an hour.
  `tools/check_builtins.py` exists only to catch that divergence.

  **The shape worth designing**, kept as narrow as the evidence supports:

  > Several `fn`/`native fn` declarations may share a name **when they differ in the first
  > parameter's type**, and the checker resolves from the declarations. Overload sets that overlap
  > on argument one are a compile error. No return-type dispatch, no ranking, no implicit
  > conversions -- F148's boundary exactly, which held.

  **That narrow shape is not what shipped**: the user chose resolution over *every* argument
  position instead, for the reason recorded below. Everything else in the box held.

  What that buys, all of it observed rather than imagined: the ten prefixed implementations under
  `reverse`/`filter`/`sort`/`sort_desc`/`sort_with` would simply *be* the overloads and F162's arm
  would disappear; an error would name a real function because one exists, dissolving the
  `array_filter` leak; purity, the discard gate and the lint become per-declaration, which is what
  `remove` and the `concat` lint needed; `check_builtins.py` becomes redundant; and library code
  gets the power the prelude has -- today `lib/graphics` cannot express what `lib/prelude.zm` does.

  **What it costs.** The `concat` `[string]` conflict is the warning: a `[string]` is both "the
  pieces of a string" and "one array of parts", and only having two *names* kept those apart. Once
  a type can belong to two sets in library code, an author can build that ambiguity, so the overlap
  rule above and an error that explains *why* a set is ambiguous are part of the feature, not
  polish. `--api builtins`, `src/lsp.c`, the VS Code grammar and the formatter all move from
  name to set -- and note those last two hand-kept lists went stale three times in one day
  (F151, F152, F160), which this would also fix by making them derivable.

  **What shipped (2026-10-19): `docs/design/full-overloading.md`, F174-F184.** The user chose
  **full** resolution -- every argument position, not the first only -- after the first-parameter
  design measured out at 43 of the 50 existing sets. The superseded document is kept beside it for
  that measurement. The cost objection raised against full resolution (that it is circular, since
  an overload must be chosen before a lambda or an `[]` can be typed) **did not survive contact
  with the code**: `infer_call_type_args` already walks the arguments skipping exactly the ones
  `expr_needs_expected_type` flags, so resolution is a filter over self-describing positions plus
  the syntactic *shape* of the rest -- no speculation, no backtracking, no constraint solver. The
  earlier note below described the first-parameter design and is superseded.

  Seven stages: the resolver running beside the arms (F176), declaration-site checking for
  overlapping sets (F177), the 28 paired names converted into real overload sets (F178-F180),
  purity and the discard gate read off the declaration rather than the name (F182), the last
  hand-kept list generated instead of maintained (F183), and the feature opened to library and
  program code (F184). Stage 4 came in **void** (F181 -- corrected by F185, which found the
  reason given was wrong), and stage 6 turned out three-quarters already done. `make check` now
  runs `tools/overload_sets.sh`, which reports **39 resolver-owned sets, 0 unresolvable**. For a
  converted set the declaration *is* the rule rather than documentation an arm may contradict --
  but `tools/check_builtins.py` was **not** made redundant as stage 6 predicted, because the
  polymorphic builtins are still dispatched and still need its guard; F183 withdrew that
  prediction.

  **Stage 8 -- folding the polymorphic builtins in too -- converted five arms of seventy and
  stopped on three constraints** (F186-F191). The "expects ..." sentence must be derivable from
  the declarations; a set gets one message, not one per arm; and, the general one:

  > An overload set resolves from arguments that describe themselves. A function whose job is to
  > accept *anything* receives exactly the expressions that do not.

  `abs`, `swap`, `slice`, `to_set` and `len` take collections and numbers and converted. `str`
  passed the first two tests, was converted, broke `str(switch ... )` in real code because a value
  block carries no type of its own, and was reverted. 65 arms remain and are **not** a backlog:
  the collection vocabulary is the natural home of overloading in this language and the universal
  verbs are not. The estimate for this stage ran ~50 -> ~3 -> 5 as each constraint was found by
  reading the code rather than counting names.

  **And it was used immediately.** The Map/Set pairing pass (F192-F201) rests on it entirely:
  one `filter` over all six collections, one `keys`/`values`/`items`/`get`/`get_or` over both Map
  worlds, the four set operations over both Set worlds, variadic `remove_all` and `add`. Names
  invisible to `--api builtins` went **20 -> 1** (`array_order_of`, unadvertised on purpose per
  F172). That is the "another row of the collection table wanting an overload" test below, met
  within a day of the feature landing.

  **Superseded note (first-parameter design):** Written
  after the collection pass closed, on the user's request, and it changes one thing in this entry:
  measured against the prelude as it stands, **the rule above covers 43 of the 50 sets, not all
  of them.** Five (`mutable_set_union`/`_intersect`/`_difference`/`_is_subset`,
  `mutable_map_set_all`) are distinguished by argument *two*, `concat` by an arity rule no
  signature can state, and `mutable` by a catch-all `(item: T)` arm that would need the
  specificity ranking this rule forbids. The document proposes resolving on argument one and then
  two, leaving `concat` and `mutable` as documented arms, and stages the work so that each step
  has an observable invariant. It does not argue for starting.

  **What was to watch, and still is.** The argument against was that a second `[string]`-style
  ambiguity -- one type belonging to two sets, giving two readings silently -- would be the
  failure mode this language most consistently refuses. It has not appeared. The declaration-site
  overlap check (F177) and the `overload_sets.sh` gate exist so that a second one is a compile
  error rather than a surprise, and `ovl_indistinguishable_at` is what makes that checkable.

  **Two loose ends left deliberately.** `#[when_unmatched("...")]` (F187) was built for stage 8's
  per-set hints, and stage 8 then stopped at five conversions, none of which needed it -- it has
  **no production user**, only a fixture, and is either owed one or owed a retirement.
  `effectful_despite_answering` (F200) is a one-name veto list carrying a written promise that a
  third case means writing the attribute rather than extending the list.

- **`lib/graphics`: a 2D drawing model, with document and image backends -- all five phases
  built (F141-F145).** One `Scene` renders two ways: to PDF (vector paths, gradients, the standard
  fourteen fonts, an embedded TrueType face carrying Unicode text, images with alpha) and to an
  image (anti-aliased fills under both winding rules, stroking with caps/joins/dashes, clip paths,
  cached glyph rendering). **Phase 6, a document layout layer, is now designed and not built**
  (`docs/graphics/phase-6-layout.md`, F146) -- and that document says plainly that it is a guess,
  because nothing has used the drawing model yet.
  Raised 2026-10-16 out of asking what reaching export parity with the Spring backend would take.
  Of the reference backend's ~13 export formats, seven are reachable with the modules Ziman already
  has (csv, flat-json, html, markdown, plain text, xml, vcard, all served by `csv`/`json`/`xml`/
  `markdown`/`mime`) and four are named exclusions (FHIR, C-CDA, HL7 v2, SMART Health Cards).
  **PDF is the only one Ziman cannot reach at all**, and a PDF writer on its own would be a
  single-purpose answer to a question that is really "render a document, and also render an image".

  **The design is written before the code**, one document per phase in `docs/graphics/`, with the
  architecture and the shared invariants in its `README.md`. Tracked as **F141 to F145** in
  docs/reviews/tasks/:

  | phase | modules | effort | state |
  | --- | --- | --- | --- |
  | 1 | `geom`, `shape`, `paint`, `scene` -- value types, no rendering | M | built |
  | 2 | `pdfobj`, `pdf` -- PDF with the standard fourteen fonts | L | built |
  | 3 | `truetype`, `font` -- TrueType parsing, metrics, CID embedding | L | built |
  | 4 | `image` -- pixel buffer, PNG, PDF image XObjects | M | built |
  | 5 | `raster`, `stroker` -- scan-conversion, stroking, glyph rendering | L | built |

  **Why it is shaped like Java 2D, and the three places it is not.** Java 2D's contribution is that
  `Shape` bottoms out in a `PathIterator` of five segment types plus a winding rule, and that
  vocabulary is almost exactly PDF's content stream -- so one backend is a serialiser and the other
  a scan-converter over the same five cases. Not copied: `Paint` there is a pixel producer, so a
  custom paint forces rasterisation (which is why PDF out of Java 2D degrades to embedded bitmaps),
  and the paint set here is closed; `Graphics2D` is a mutable state machine, which fights a language
  whose values are immutable and whose `Mutable*` types are thread-bound, so the model is an
  immutable display list -- one scene renders to PDF *and* an image, and tests assert on structure
  rather than pixels; and `getOutline()` is the right escape hatch for a raster device and the wrong
  default for a document, so text stays a first-class operation.

  Cairo, Skia, Quartz 2D, Direct2D, Canvas 2D, `lyon`/`tiny-skia` and ReportLab each contributed
  something and the README says what. Two worth repeating here: cairo's pattern set is a closed
  six-case enum, which independently confirms the closed-`Paint` decision, while its sixth case is a
  callback pixel producer and its PDF backend rasterises fallback regions for anything it cannot
  express natively -- the no-silent-degradation invariant's failure mode, in a mature library, from
  one open escape hatch. And ReportLab points at what all five phases leave out.

  **What they leave out, and the likely sixth phase: document layout.** Every phase is a drawing
  API, and the motivating use case does not think in coordinates -- it thinks in headings,
  paragraphs, tables and page breaks. ReportLab has a canvas *and* Platypus on top, because reports
  written straight against a canvas become unmaintainable arithmetic. **Designed 2026-10-16** in
  `docs/graphics/phase-6-layout.md` and tracked as F146, with the original reservation kept at the
  top of it: a drawing model nothing has used is the wrong moment to design a layout model over it,
  and the export work will say what it needs. `pdf.render_pages` already takes a list of scenes for
  this. The document's own advice is to write one real export page against `scene` and `pdf` first
  and let what hurts become the requirements.

  **One prerequisite, found by checking:** `math` has no `atan2`, `asin` or `acos`, and arcs, round
  joins and decomposing a matrix all need the first. That is part of phase 1.

- **An interactive debugger: breakpoints, stepping, variable inspection.** Split out of F6 on
  2026-10-05 when the rest of that task closed (see docs/reviews/tasks/), because it is the one
  part of it that is a project rather than a feature and should not sit behind a checkbox that looks
  almost finished.

  What already exists, and is why this is not urgent: a runtime error prints a stack trace with
  every frame's file, line and column; `--locals` adds each frame's variables and their values under
  it, innermost scope outwards; `--profile` says where the time went; `--coverage` says what never
  ran. Between them they answer "where did it fail", "with what values", "what is slow" and "what is
  untested" without an interactive session, which is most of what a debugger gets used for -- and
  they answer it in a CI run or someone else's bug report, where nothing can attach.

  What stepping would add that those cannot: watching state change *as* it changes, rather than
  seeing it once at the point of failure. That is the real gap, and it matters most for a bug whose
  failure point is far from its cause.

  Two shapes, and the choice is the design pass:
  - A **DAP server** (Debug Adapter Protocol), so VS Code and every other DAP client drives it. The
    LSP in src/lsp.c is the precedent for both the protocol plumbing and the scale -- this is that
    size of job, not smaller.
  - A **terminal REPL** over the interpreter loop: `break file:line`, `step`, `next`, `print x`.
    Much less protocol, no editor integration, and it needs the same core underneath either way.

  The core underneath is the same for both and is the actual work: a breakpoint check on the
  statement path (`exec_stmt` already carries every statement's line, and `--coverage` showed what a
  flag-guarded test there costs, which is nothing measurable), a way to suspend the interpreting
  thread and resume it, and enough of `--locals`' scope walk to answer an inspection request for an
  arbitrary frame rather than only the dying one. Deferred, not rejected: the evidence so far points
  elsewhere, since the LLM benchmark's own finding was that diagnostics -- not step-debugging --
  were what let a model correct itself.

- ~~**Grow the LLM-generation benchmark from 10 tasks to the 50 the review asked for.**~~
  **Done 2026-10-10; what replaces it is variance, not scope.**
  *Done 2026-10-10 (F26 wrote the 50 tasks; F27/F34/F36/F49 ran them).* All 50 tasks exist and have
  now been run at least once: round 1 is **0 of 50** across five independent batches with five
  different habit mixes, and round 2 talked every cold attempt on tasks 11-50 into compiling using
  nothing but the compiler's own output. **What this entry asked for is finished; what replaces it
  is variance, not scope** -- every per-task figure is n=1 (the "two independent fresh model
  instances" below means one per *round*, which is isolation, not replication), so nothing yet
  measures two runs of the same task. The 2026-10-10 wave is also the clearest case for reading the
  attempts rather than the score: it scored 10/10 in round 2 and still contained three real
  diagnostic defects (F50, F51). Also split out
  of F6 on 2026-10-05 rather than closed with it, because 10 tasks is a fifth of the asked-for scope
  and calling that done would misrepresent it. `tests/bench/llm_tasks/` holds the fixed task set
  (**50 tasks plus a README as of 2026-10-10** -- this sentence said "the fixed 10-task set" until
  2026-10-16, contradicting the note above it in its own entry)
  and docs/reviews/llm-benchmark-2026-09-28.md the method -- each task run cold with zero Ziman
  context, then iterated against the compiler alone for up to five fix attempts, two independent
  fresh model instances per task.

  Worth growing because this is the one measurement here with a track record of changing the
  language: its finding -- that diagnostics naming an exact fix let a model self-correct every time,
  while a bare "undefined function" did not -- produced the "did you mean" suggestions, and the
  re-validation caught a real bug in them (fuzzy search offering `hash` for `has`). A 10-task set
  cannot separate a real effect from which error a given run happened to look at first, which is
  exactly what the third task's inconclusive result came down to.

- ~~**Memory is never reclaimed while a program runs (in the default build)
  — fine for a short-lived script, a real problem for the North Star goal
  of an always-on backend.**~~ **The headline stopped being true on 2026-09-29, when the collector
  became the default build; the updates below have said so since, but the headline a reader scans
  did not (struck 2026-10-16).** *Update 2026-09-23: `make gc` builds
  `ziman-gc`, which routes every allocation through the Boehm collector;
  the two plans below (refcounting, per-thread arenas) were not built and
  are kept for their reasoning. See docs/design/journal/'s "Memory
  reclamation" entry for what shipped, what it measured, and what is left.*
  *Update 2026-09-29: the collector is now the default build and does not
  leak (re-verified), but does not scale with concurrent clients -- both
  plans below are superseded by a corrected design, one independently
  collected heap per thread, with no dependency on any particular
  library's usage pattern (the per-thread-arena plan's mistake). See
  docs/MEMORY.md for the full diagnosis, the reasoning for the correction,
  and the design -- not started, tracked as F2's concurrency half
  (docs/reviews/tasks/). **Superseded 2026-10-07: that task (F7) is closed, and not by
  this design.** The gc_heap migration was measured and abandoned -- a gc_heap object pays
  *both* collectors, and each half of the fix is the other's prerequisite -- while the
  scaling problem itself was fixed by removing allocation rather than by moving it: four
  sites (binding names, call argument arrays, a scratch array inside the mark phase, and
  loop scopes) took allocation calls on a server workload from 9.96M to 2.57M, after which
  throughput stops falling as clients are added and is flat from 8 to 64. See docs/design/journal/'s
  "The gc_heap migration, measured and abandoned" and "Three allocation sites, and what
  they did to F7". A reader following the design below is following the abandoned path.*
  *Update 2026-10-08: "does not leak", re-verified twice above, has
  one measured exception, and it is not in the collector.
  `src/iter.c:133` releases a libc `getline` buffer through
  `zgc.h`'s `free`, which is a no-op in this build, so Boehm never
  reclaims a block it did not allocate: 1.27 GB RSS against 28 MB
  under `ziman-nogc` -- the build whose own comment says nothing is
  ever freed is the one that holds (A17, plus three `strndup`
  siblings in `src/lsp.c`). The rule the macro hides is that memory
  libc allocated has to be freed by libc, which
  `src/native_http.c:15-27` already does with `push_macro`. So the
  claim to make is narrower than the one made here: nothing Ziman
  allocates leaks.* Every `Value` payload (`ArrayObj`/`StructObj`/
  `EnumObj`/`ResultObj`, every `string` buffer) is heap-allocated and
  never freed, on purpose — see docs/design/journal/'s "single-writer/actor
  pattern" entry: *"this interpreter never frees an `ArrayObj`/
  `StructObj`/`EnumObj`/`ResultObj` once built ... the interpreter is a
  short-lived process, so the OS reclaims everything at exit."* That's
  also exactly why sharing a `Value` across a `Channel` needed zero new
  machinery: nothing frees the thing being pointed to, so a value
  crossing threads is just a safe pointer copy. The justification holds
  for `ziman program.zm` today — a CLI invocation that runs to
  completion and exits. It stops holding the moment a program is meant
  to run forever, which is the entire point of "write a slice of the
  health-record backend in Ziman."
  Concretely, not hypothetically: `health-record-backend-ziman`'s
  `repository.save()` calls `push(store.patients, p)`, and arrays are
  immutable — `push` always builds a **brand new backing array**,
  copying every existing patient's `Value` into it. The moment
  `run_patient_actor`'s loop reassigns `store = unwrap(result)`, the
  *previous* array becomes garbage — and is never freed. Across N
  total creates, that's roughly `1+2+...+N` = O(N²) worth of array
  backing buffers accumulated over the process's lifetime, on top of
  every `format`/`concat` call building a response line and every
  character `parse_json` appends one at a time while unescaping a
  string. None of this is specific to any one feature (a `s[i]` string-
  indexing operator, if added, would contribute the exact same way
  `substring` already does) — it's a property of the whole interpreter,
  surfaced by asking what happens once `main.zm`'s current "run a fixed
  handful of operations, then exit" demo shape is replaced by "run
  forever, serving requests, until killed."
  The fix Ziman's own existing design actually points toward is
  **reference counting, not a tracing GC** — and it fits with unusually
  little downside for this language specifically: every `Value` is
  immutable once built and every struct/array/enum literal is
  constructed strictly bottom-up (all of a value's fields must already
  exist before the value itself can be built), so **the value graph can
  never contain a cycle**. That's precisely the situation where
  refcounting has no meaningful weakness compared to a tracing
  collector (no cycle-collector ever needed) — cycles are the classic
  reason production languages avoid *pure* refcounting: Python is the
  textbook example, using refcounting as its primary mechanism but
  bolting on a second, generational cycle-detecting collector
  specifically because a mutable object graph can form a cycle (`a.next
  = b; b.prev = a` keeps both alive forever once nothing else points to
  either). Ziman's value semantics rule that case out by construction
  — nothing is ever mutated after being built, so nothing can ever be
  wired into a cycle — meaning refcounting alone, with no second
  mechanism bolted on, would actually be *sufficient* here in a way
  it isn't for a language with mutable references. The cost is real,
  though: every value-sharing site (`push`/`insert`/`set`, a struct
  literal or `with` copying a field, a channel `send`, an ordinary
  `val`/`var` binding or reassignment) needs an increment, and every
  scope exit or rebinding needs a decrement — a genuinely large,
  cross-cutting change, comparable in scope to the generics work, not
  something to fold into a smaller, unrelated feature.
  **Correction (external review, 2026-09-23; see R9):** the "can never
  contain a cycle" premise above is wrong. `Channel<T>` is mutable in
  place, so `struct Node { peers: Channel<Node> }` with the node sent into
  its own channel is a genuine cycle (reproduced). Pure refcounting needs
  channels to hold weak references or to be left out of the counted graph;
  do not implement against this entry as written.
  Not started, not designed in detail — needs its own pass (representation
  of the refcount itself, where increments/decrements actually need to be
  inserted given the interpreter's current "just copy the `Value` struct"
  sharing style, and whether `Channel<T>`'s cross-thread sharing needs
  atomic increments/decrements or can stay under the channel's existing
  mutex) before implementation starts.
- **A likely-better alternative to the refcounting entry above:
  per-thread arena allocation, freed in one shot when the thread's work
  is done — the same design Erlang/BEAM already uses for exactly this
  problem, and a more natural fit for Ziman's concurrency model than
  refcounting is.** The motivating shape: a real Ziman HTTP backend
  handles each incoming request on its own thread (`spawn`), doing a
  burst of allocation (parsed JSON, intermediate structs, formatted
  response strings) that's entirely garbage the moment the request
  finishes — exactly the pattern arena/region allocators exist for
  (Apache httpd's per-request pool, nginx's memory pools, Rust's
  `typed-arena`). Give each such thread its own arena; every allocation
  it makes comes from that arena; when the thread's function returns,
  free the *entire* arena in one bulk operation. No per-value
  bookkeeping at all — no refcounts to increment/decrement, and (unlike
  the refcounting entry above) no need to lean on the value graph being
  acyclic either, since nothing is being freed value-by-value in the
  first place.
  The one place this breaks if implemented naively: Ziman's actor
  model *is* threads passing values to each other over a `Channel`, and
  today that's a cheap pointer copy specifically because nothing is
  ever freed — see docs/design/journal/'s "single-writer/actor pattern" entry
  (*"nothing frees the thing being pointed to, so a value crossing
  threads is just a safe pointer copy"*). A request thread that builds
  a `CreatePatientRequest` and sends it to the long-lived actor, which
  stores it in `store.patients` for the rest of the program's life,
  would leave the actor holding a pointer into the request thread's
  arena — a dangling pointer the moment that arena is freed, silent
  memory corruption rather than a leak. Erlang solves this the same way
  it solves the general problem: each process has its own *private*
  heap, and `send` between processes *copies* the message from the
  sender's heap into the receiver's, never shares a pointer across the
  boundary. Ziman would need the same fix — `send`/`receive` deep-copy
  the `Value` across the channel instead of copying the struct/pointer
  — before per-thread arenas are safe to bulk-free. Given values are
  already immutable, this copy is behaviorally invisible (no aliasing
  to preserve), just a real runtime cost paid once per channel
  operation rather than never.
  Not started, not designed in detail — needs its own pass (an arena
  allocator implementation, hooking every allocation site to it instead
  of bare `malloc`, the deep-copy-on-send/receive mechanism and exactly
  what "deep" means for each `Value` variant, and what owns/frees the
  entry module's own top-level allocations, which never run inside a
  `spawn`ed thread at all) before implementation starts. Preferred over
  the refcounting entry above specifically because it needs no per-
  value bookkeeping during normal execution and matches the actor model
  already in place, rather than being a general mechanism bolted in
  beside it — but the two aren't necessarily exclusive (a long-lived
  actor's own heap could still eventually want its own reclamation
  story once *it* accumulates garbage across many processed messages,
  which per-thread arenas alone don't address).
  **Correction (external review, 2026-09-23; see R9):** the granularity
  doesn't match the server that motivates this. `server.zm` is thread-per-
  *connection*, and with keep-alive one thread serves thousands of
  requests, so a per-thread arena frees nothing until the client
  disconnects. The region has to be per *request*, which needs an escape
  rule for state that deliberately outlives one (the buffered `Session`
  carried between requests for pipelining) and for values sent over a
  channel (copy-on-send).
  **Second correction (2026-09-29):** "per request" is itself the wrong
  generalization -- a request is a concept `server.zm` invented, not
  something every Ziman thread has (a `spawn`ed worker consuming a
  `Channel` forever, a batch job, a CLI script have no request to hang a
  reset point on). See docs/MEMORY.md for the corrected design: one
  independently-collected heap per thread, with no notion of "request"
  anywhere in it.
- **[Resolved: `Map`/`Set` are now built-in types on a persistent hash trie, O(log32 n)
  writes -- see docs/design/journal/ "Map and Set". The analysis below is the history.]**
  **`HashMap`/`HashSet`'s `set`/`add`/`remove` are O(bucket_count), not
  O(1) — a genuine gap next to Java's `HashMap` (O(1) amortized `put`),
  and a trie-shaped bucket structure could close it without touching
  the language at all.** Surfaced comparing `collections.zm`'s
  just-added load-factor growth (see docs/design/journal/'s "Collections
  module" entry on growth) against Java's `HashMap`: growth fixed
  `get`/`has`'s degradation into a linear scan past ~16 entries, but
  `set`/`remove` stay O(bucket_count) ≈ O(n) *every call*, growing or
  not — the same root cause as the memory-reclamation entry above.
  `set(m.buckets, idx, new_bucket)` must allocate and copy a whole new
  backing array, since Ziman's arrays are immutable value types with no
  in-place mutation anywhere in the language — Java's `HashMap` doesn't
  pay this cost because its bucket array is mutable, so `put` just
  links a node into one bucket's chain in place. This isn't a bug in
  how the module was written; it's the same cost `push`/`set` already
  have on any plain Ziman array, just newly visible because a hash
  table does one per operation instead of one per bulk rebuild.
  The fix doesn't need a language change, though — it needs a different
  *data structure* for the buckets, one built for exactly this
  constraint: a **Hash Array Mapped Trie (HAMT)**, the technique
  Clojure's and Scala's persistent (immutable-by-default) hash maps use
  for the identical reason. Instead of one flat array of `bucket_count`
  slots, indexed by the full hash, a HAMT is a shallow trie of small,
  fixed-arity nodes (32-wide is the usual choice), each level indexed by
  one chunk of the hash's bits. Updating one entry only requires
  allocating and copying *one small node* (O(arity) = O(1)) at each of
  O(log₃₂ n) levels on the path to it — everywhere else in the trie is
  untouched and safely shared by reference, since nothing is ever
  mutated. That turns `set`/`remove` into genuine O(log n) — for any
  realistic `n`, indistinguishable from O(1) in practice (a trie holding
  a billion entries is only 6 levels deep at arity 32) — achieved
  entirely within Ziman's existing "arrays are immutable, copy-on-write"
  semantics. No new interpreter machinery, no refcounting dependency:
  this is pure `.zm` source, the same way the current flat-bucket-array
  version is.
  Real cost, not a small tweak: a HAMT needs a node representation
  (probably a small tagged struct/enum distinguishing an internal node
  from a leaf/collision-list), a bitmap or dense-array scheme for
  "which of the 32 slots at this node are populated" (a naive always-32-
  wide node wastes a lot of space for a sparsely populated trie — real
  HAMT implementations pack this with a 32-bit presence bitmap, which
  needs bit-counting. *Update 2026-09-28: this is no longer an open
  question* -- the `bits` core module (docs/design/journal/ "A bits module") now
  gives `int_popcount`/`int_and`/`int_or`/`int_shift_left`/... directly, so
  a 32-bit presence bitmap and its popcount are a native operation, not an
  all-arithmetic substitute to invent, same as the `byte`/`uuid`/`number`
  modules once had to work around case-by-case), collision handling when
  two keys hash into the same leaf path all the way down, and a
  hash-splitting scheme (which chunk of `hash(key)`'s bits picks the slot
  at each level). `hashmap_keys`/`hashmap_values`/`hashmap_size` would need
  reimplementing against the new shape too (`size` stops being a trivial
  field read if the trie doesn't track a running count itself, which it
  should). Not started, not designed in detail — needs its own pass (node
  representation, collision strategy, whether `HashSet<T>` shares the same
  trie implementation
  parameterized differently or duplicates it the way `HashMap`/`HashSet`
  already do today) before implementation starts. The current flat-
  bucket-array version stays as the interim implementation either way —
  this is a strict improvement to swap in later, not a correctness fix.
- **Effects visible in function signatures** (which functions do I/O or
  mutate outside their own frame). Real value per docs/design/journal/'s principles,
  real design cost — needs its own doc before implementation starts. Now
  also entangled with the module system above: an effect story and a
  native-vs-pure-Ziman module boundary are naturally related questions
  (does calling into a native module always count as an effect?).
  *Update 2026-10-10: considered under the AI-first ranking and deliberately
  left here rather than promoted.* The gap is real and easy to show -- a pure
  `tidy(name: string) -> string` and a `tidy_and_log` that writes and deletes
  a file have **identical signatures**, so `--api`, the type checker and a
  reader all see the same thing. And the notation is in the training
  distribution: cold benchmark attempts wrote `/ {IO}`, `! {IO}` and `-> T !
  IO` unprompted. But it is **4 of 30 attempts, all in the first wave and none
  in the two since**, so this is a habit models carry rather than a thing they
  repeatedly fail on -- the AI-first case is weak and the case for it remains
  the design one above. The native-boundary question now has a number against
  it too: `fs` declares 21 natives and `os` 28, so "calling a native is an
  effect" makes nearly everything effectful and the annotation stops
  discriminating, while "it isn't" leaves `fs.write_text` invisible -- failing
  at the exact case that motivates the feature. Propagation is the other cost:
  every caller inherits transitively, and every generic function needs effect
  polymorphism.
- ~~**Modules / multi-file programs.**~~ Implemented (`src/loader.c`):
  - **One file = one module**, identified by its filesystem path — no
    separate `module Foo` declaration to keep in sync with the filename.
  - **`use path/to/mod`** (slash, not dot — a module path is filesystem-
    literal and deliberately never shares a token with `.field` access,
    which already means something else). Resolved **project-relative
    first**, against the directory containing the entry file passed to
    `ziman`, falling back to a bundled core module (see "Bundled core
    modules" below) if no project file by that name exists — `use
    collections` and `use io` both find one this way now, with zero
    project-local files.
  - **Every cross-module reference is qualified — `user.greet(u)`, never
    a bare `greet(u)` that happens to resolve to some import — unless
    explicitly imported by name (`use models/user.greet`, see "Per-name
    imports" below), which is still visible right there in the file's own
    `use` list, not silent.** Same "derivable from the line itself"
    reasoning as `concat`/`str` existing instead of overloading `+`.
    There is no wildcard import form (`use mod.*`). Implementation note:
    a qualified reference looks identical to plain field access (`p.x`)
    until the parser sees what follows the second identifier, so it's
    parsed speculatively and backed out of (restoring position) if it
    doesn't match either `IDENT "("` or an uppercase `TYPE_IDENT "{"` —
    confirmed not to disturb plain field access via the existing struct
    test suite passing unchanged.
  - **Name-collision aliasing reuses `as`** (`use admin/user as
    admin_user`) — new meaning for an existing keyword, never ambiguous
    with cast-`as` since they only appear in different grammatical
    positions.
  - **Exports require `pub`; default is private to the module** — same
    instinct as `val`/`var`: an important property is explicit at the
    declaration, not inferred from whether something outside happens to
    use it. Corollary, confirmed working (regression test
    `multifile_name_isolation`): struct/function names only need to be
    unique *within* their own module, not across the whole program,
    unlike today's single-file rule.
  - **No separate manifest file** (`Cargo.toml`/`package.json`-style) —
    that solves third-party package versioning, which doesn't exist yet
    (already a stated non-goal). The app's dependency graph is just the
    entry file's `use` statements, followed transitively; a module is
    loaded and parsed at most once (a diamond dependency shares one
    instance, confirmed via `multifile_diamond`), and a cycle is a clear
    compile-time error (`multifile_cycle`) instead of infinite recursion.
  - ~~**A native module is `native fn name(params) -> type` with no `{ }`
    body at all.**~~ Fully implemented, registration mechanism included
    (`src/native.c`, `src/native_io.c`): the type checker treats a
    `native fn`'s signature like any other function's, and at call time
    the interpreter looks up a C function pointer registered under the
    function's exact fully-qualified name (`io::read_file`) and calls it
    — falling back to the existing "no registered implementation" error
    only if nothing was ever registered for that name. Every native
    function is a plain C function of one fixed shape regardless of its
    declared Ziman signature: `Value fn(Value *args, int argc, int line,
    int col)` — see docs/design/journal/'s new entry for why this is
    deliberately *not* a 1:1 mirror of the Ziman signature (inspired by
    JNI, which does the same for a harder reason — bridging C's
    primitives to Java's object model — that Ziman's uniform `Value`
    representation doesn't actually have). `line`/`col` let a native
    function raise a `ziman_error` that reads exactly like any other
    runtime error (e.g. `io.read_file` on a missing path).
  - **Implementation strategy, not a language-semantics decision:** the
    loader rewrites every module's own top-level names, and every
    cross-module reference, to `<module_path>::<name>` and merges
    everything into one flat Module *before* typecheck()/interpret() ever
    run — so neither needed to learn anything about multiple modules at
    all, only the loader did. Known cosmetic side effect: an error
    message or a printed struct shows the qualified name (e.g.
    `models/user::User`) rather than the plain declared one — harmless,
    a little ugly, not worth fixing until it actually bothers someone.
  - ~~**Today's built-in scalar/array machinery stays implicit for
    now.**~~ Half-resolved: **the "always-auto-imported native module"
    idea landed** (`src/prelude.h`/`prelude.c`) — a fixed, small native
    module whose declarations are merged bare into every program, no
    `use` required and never qualified, the one deliberate exception to
    "every cross-module reference is qualified." `parse_int`/
    `parse_float`/`parse_bigint`/`parse_decimal`/`floor`/`ceil`/`round`/
    `math.round`/`concat`/`substring`/`split`/`trim`/`to_upper`/
    `to_lower` moved there — every one of them has exactly one fixed
    signature, so `typecheck.c`/`interpreter.c` needed zero new dispatch
    code for them; the *existing*, fully generic native-fn machinery
    (already built for `io`) handles type-checking and calling them with
    no per-function special-casing at all. Verified as a genuinely
    zero-breaking-change internal restructuring: every existing test
    fixture calling these unqualified still passes unchanged.
    **What's still core, and why it has to stay that way**: `print`,
    `str`, `format`, `len`, `push`/`insert`/`set`, `contains`, `abs`,
    `min`/`max`, `hash` — every one of these needs to work under *one*
    unqualified name across *multiple* types (`abs(5)` and `abs(5.0)`
    and `abs(5n)` and `abs(5.0d)` all share the name `abs`). A native fn
    is exactly one fixed signature per declared name — a *native* has no
    argument-based overloading — so a truly polymorphic-by-name
    builtin can't move to the prelude without either generic native fn
    signatures (`native fn abs<T: Numeric>(x: T) -> T`, which doesn't
    exist and hasn't been designed) or overloading.
    **Updated 2026-10-18:** "Ziman has no argument-based overloading
    anywhere" is what this said, and it stopped being true. F148 gave
    the dispatched builtins `builtin fn` declarations, and the
    collection-pairing pass (F149-F162) grew them to **28 overloaded
    names over 87 declarations** — `len` and `contains` with nine each,
    `concat` and `remove` with four. What the language does not have is
    overloading *available to a declaration*: every one of those sets is
    resolved by a hand-written arm in `typecheck.c`. See "Relaxing the
    no-overloading rule" below, which is the entry this paragraph now
    points at rather than the assumption it used to state. This isn't a remaining "not gotten to it" gap the way the
    type-specific functions were — it's the actual, structural boundary
    of what the prelude mechanism can express.
    **Updated again 2026-10-19: that boundary moved.** Overloading
    available to a declaration shipped (F174-F184), and a `native fn`
    name may now carry several declarations — `remove` and `filter`
    already do. So the second of the two routes this paragraph named is
    no longer missing, and the sentence above describing the structural
    limit now describes a limit that was lifted. What that leaves is a
    *choice* rather than a wall: `abs` and `len` had their hand-written
    arms deleted and resolve from their declarations (F188, F190), but
    both are still `builtin fn` with a core implementation, because
    moving them to `native fn` buys nothing the resolver did not already
    give. `str`, `contains`, `clear` and `snapshot` have reasons of
    their own to stay core, and the reasons are measured, not assumed:
    `contains` carries twelve distinct diagnostics, `clear` and
    `snapshot` carry hints keyed on the type that was rejected, and
    `str` takes expressions that cannot describe themselves. Those are
    recorded under "Relaxing the no-overloading rule" below. The operators themselves
    (`+`/`-`/`==`/etc.) were never a candidate either way, for the same
    "no operator overloading" reason (see docs/design/journal/'s "Numbers"
    entry) — those stay hardcoded in `eval_binary`/`check_binary`
    regardless of what happens to function-shaped builtins.
- ~~**Generics — in progress, now a five-stage plan.**~~ **All five stages are
  implemented (2026-10-10, F39): this heading outlived them.** Generic structs,
  bounded type parameters, generic enums, cross-module generics and generic
  functions all shipped and are struck through below. The only residue is the
  limit stated in this paragraph -- a generic function body can do nothing with
  an abstract `T` beyond `Eq`/`Hash`/`Ord` -- and extending *that* was the `Row`
  bound, which C18 withdrew on 2026-10-09 after finding it cannot work the way
  `compare` does. Design decided
  (docs/design/journal/'s "Generics" entry): explicit type arguments everywhere
  (no inference), a new `TY_TYPEVAR` substituted on demand rather than
  monomorphized (`Value` is already runtime-type-erased, so there's no
  per-instantiation runtime representation to generate — only the type
  checker's substitution logic is new). The original "no bounds/
  constraints at all" decision has been narrowed, not reversed: still no
  general trait/interface system, but a single hardcoded `Eq` bound is
  being added specifically because `Map`/`Set` need it (see the
  "Collections module" entry below) — a generic function body still can't
  do anything else with an abstract `T`.
  - ~~**Stage 1: generic structs.**~~ Implemented (docs/syntax/
    "Generics"): `struct Box<T> { value: T }`, constructed as
    `Box<int> { value: 5 }`; field access, `==`, and `with` all
    substitute correctly through nested generics (`Box<[T]>`,
    `Wrapper<T> { inner: Box<T> }`) and arrays of a generic struct.
    Same-module only, like `enum`. Arity/genericity mismatches
    (`Box { ... }` with no type args, or type args on a non-generic
    struct) are compile-time errors, and a type parameter name colliding
    with a real struct/enum name is rejected at declaration time (it
    would otherwise silently shadow real references to that type inside
    the generic struct's own fields).
  - ~~**Stage 2: bounded type parameters (`<T: Eq>`).**~~ Implemented
    (docs/syntax/ "Generics"): the one hardcoded bound, meaning
    "supports `==`," parses on a struct/enum/function's own type
    parameters (`struct HashMap<K: Eq, V> { ... }`) and is enforced at every
    concrete instantiation — `HashMap<Channel<int>, V>` is a compile-time
    error, since `Channel` doesn't support `==`, checked via the same
    `type_supports_equality` machinery arrays/structs/Result/enum already
    use. A bound name other than `Eq` (`<T: Ord>`) is also a compile-time
    error. Usable inside a generic declaration's own body since Stage 5
    landed (`fn are_equal<T: Eq>(a: T, b: T) -> bool { return a == b }`)
    — see that entry below for what building that half surfaced.
  - ~~**Stage 3: generic enums.**~~ Implemented (docs/syntax/
    "Generics"): `enum Option<T> { Some(T) None }`, constructed as
    `Option<int>.Some(5)`/`Option<int>.None` — the actual `Option<T>`
    motivation landing. Construction spells out type arguments like a
    generic struct; consumption (`Option.is_Some(a)`/
    `Option.unwrap_Some(a)`) deliberately never does, since the concrete
    instantiation is already fully known from the argument's own value —
    the type checker reads it off the argument's resolved type instead of
    requiring (redundant) restatement, and rejects
    `Option<int>.is_Some(a)` as a compile-time error for writing it
    anyway. `is_X`/`unwrap_X`/`==` all substitute through
    `decl->type_params`/the value's own `type_args` the same way struct
    fields do. A bound (`enum Option<T: Eq> { ... }`) works identically to
    a struct's.
  - ~~**Stage 4: cross-module generics.**~~ Implemented (docs/syntax/
    "Generics"): `collections.Box<int> { ... }`,
    `collections.Option<int>.Some(5)`, and a qualified type annotation
    (`val x: collections.Option<int>`) all work. This was never a
    generics-specific gap: the qualified-reference parser
    (`mod.Name`/`mod.Type { ... }`) just needed extending to also accept
    `<Args>`, and enum construction/consumption needed a second dot on
    top of that (`mod.EnumName.Variant`) that plain field access and
    module-function calls never did — building this also happened to
    close the pre-existing, generics-independent "cross-module enum
    access" gap noted under the `enum` entry above, and a related latent
    hole where a qualified *type annotation* (`val x: mod.Shape`) only
    ever resolved against a struct, never an enum. This is what actually
    lets a shared `collections` module (below) export anything generic.
  - ~~**Stage 5: generic functions.**~~ Implemented (docs/syntax/
    "Generics"): `fn identity<T>(x: T) -> T`, called as
    `identity<int>(5)` (cross-module too: `mod.identity<int>(5)`). Uses
    the one-token lookahead heuristic docs/design/journal/'s "Generics" entry
    describes to disambiguate a generic call's `<` from a real
    comparison, which structs/enums didn't need (an uppercase name can
    never be a real variable, but a generic function's lowercase callee
    genuinely can appear on the left of `<`). This is also the stage
    where the `Eq` bound finally gets *exercised* inside a body, not just
    enforced at instantiation (`fn are_equal<T: Eq>(a: T, b: T) -> bool
    { return a == b }`) — surfaced two real bugs while wiring it up,
    both fixed and covered by regression fixtures:
    - A bounded type parameter used *as a type argument to another
      bounded generic type* in the enclosing declaration's own signature
      (`fn hashmap_get<K: Eq, V>(m: HashMap<K, V>, ...)` -- checking
      `HashMap<K, V>`'s own `K: Eq` bound against the caller's `K`, itself just a bare,
      unsubstituted type variable at that point) needs the same "which
      declaration's type parameters are in scope right now" context a
      function *body* needs for `==`, but during *declaration
      validation*, before any body is checked. Generalized the
      body-only tracking into one shared "type parameters of whichever
      generic declaration is currently being validated" variable, set
      around struct fields', enum variants', and function params/return
      types' well-formedness checks too, not just around a function's
      own body.
    - A generic function's own type parameter referenced *from inside its
      body* in a position other than a param/return type
      (`Option<V>.Some(...)`, a `val` annotation, a cast) was never
      reclassified from a provisional struct/enum reference into a type
      variable, since only a struct/enum *declaration*'s own field/
      variant types went through that step before. Fixed with a new
      parser-side walker (`reclassify_type_params_in_stmt`/`_in_expr`,
      mirroring loader.c's self_qualify_expr/self_qualify_stmt shape)
      that reclassifies every type-bearing spot in a generic function's
      parsed body, run once right after the body itself is parsed.
    Needed to write `hashmap_get`/`hashmap_set`/`hashset_add`/etc. once
    instead of once per concrete instantiation.
- ~~**Collections module (`Box`/`Option`/`HashMap`/`HashSet`) — the
  concrete payoff of the generics work above.**~~ Implemented
  (`lib/core/collections.zm` — see "Bundled core modules" below for why
  it lives there and not under `examples/`): a `collections` module,
  written in ordinary Ziman, no new native/C code, exporting `Box<T>`
  (later removed outright — nothing ever used it beyond being the first
  generic struct ever written, see docs/design/journal/'s "Bundled core
  modules" entry), `Option<T>` (plus a small `option_unwrap_or<T>`
  helper — later moved into the prelude instead, see the "Prelude"
  entry in docs/design/journal/), `HashMap<K: Eq + Hash, V>`, and
  `HashSet<T: Eq + Hash>` (`HashSet`
  needs both bounds too, for the same O(1)-bucket reason as `HashMap`)
  as plain structs/enums plus free functions
  (`hashmap_new`/`hashmap_get`/`hashmap_set`/`hashmap_has`/
  `hashmap_remove`, `hashset_new`/`hashset_add`/`hashset_contains`/
  `hashset_remove`). `HashMap`/`HashSet` use real bucket-array storage
  (separate chaining, `hash(key) % bucket_count`), verified against both
  a small demo (`examples/collections/main.zm`, also
  `tests/lang/multifile_collections`) and a 50-entry stress fixture
  (`multifile_collections_stress`) confirming collisions within a
  bucket chain correctly. Bucket count was originally fixed at creation
  (16, no resizing/rehashing) — later given real load-factor-triggered
  growth instead, plus `hashmap_size`/`hashmap_keys`/`hashmap_values`
  (`hashset_size`/`hashset_items`), once "the hashmap implementation
  isn't great" surfaced the fixed bucket count as a real problem (chains
  degrading lookups to O(n) past ~16 entries) rather than an acceptable
  simplification — see docs/design/journal/'s "Collections module" entry (the
  one titled about growth, not this one) for the full writeup, including
  why growing doesn't also make set/remove O(1). `Entry`/`buckets`/
  `count` are implementation details Ziman has no per-field privacy to
  actually hide (a caller *can* reach `m.buckets` directly) —
  documented as "don't," not enforced.
- ~~**Bundled core modules — closes the "every project keeps its own
  copy" gap noted above under native modules.**~~ Implemented
  (`src/corelib.h`/`corelib.c`): `collections` moved from
  `examples/collections/collections.zm` to `lib/core/collections.zm`,
  and the Makefile now generates `src/collections_source.h` from it (a
  `#define COLLECTIONS_SOURCE "..."`, one long escaped string literal —
  a `#define` rather than a `static const char *` since the latter
  isn't a constant expression and can't initialize `corelib.c`'s
  `CORE_MODULES` table) at build time, embedding it straight into the
  `ziman` binary. `loader.c`'s `load_module` tries a project-relative
  file first, same as always, and only falls back to
  `corelib_source_for(use_path)` if that file doesn't exist — a project
  file of the same name always wins, so this is strictly a fallback,
  not a shadowing/override mechanism. Deliberately narrower than the
  prelude (prelude.h): a core module is only parsed when a program
  actually `use`s it, and a whole-module `use collections` stays
  qualified (`collections.HashMap`, never a bare `HashMap`) exactly like
  any other module — a bare `HashMap` only becomes possible via a
  separate, later mechanism (per-name imports, below), never as a
  side effect of this one — this is a stdlib, not language-level
  machinery, so the usual `use`/`pub` boundary still applies. Every
  fixture that used to vendor its own copy of `collections.zm`
  (`multifile_collections`, `multifile_collections_stress`,
  `multifile_bigint_hashmap`, `multifile_decimal_hashmap`) had it
  deleted and now resolves through the fallback with no changes to the
  fixture's own code — the clearest evidence the mechanism is a drop-in
  replacement for vendoring, not just a demo of one.
  ~~`io.zm` (the `io` native module's signature file) is the other
  concrete case this mechanism resolves.~~ Done too, the same day: moved
  from `examples/native_io/io.zm` to `lib/core/io.zm`, one line added to
  `corelib.c`'s `CORE_MODULES` table and to the Makefile's
  `CORELIB_MODULES` list (which now generalizes the header-generation
  rule to a pattern rule, `src/%_source.h: lib/core/%.zm`, rather than
  one hardcoded rule per module). The three `native_io_*` test fixtures'
  vendored `io.zm` copies (all byte-identical to the original) were
  deleted the same way `collections.zm`'s vendored copies were, and all
  three still pass unchanged.
- ~~**Per-name imports (`use path.Name [as alias]`) — closes the
  "generic collection code repeats `collections.` *and* `<K, V>` on
  nearly every line" ergonomics gap the bundled `collections` module
  above surfaced.**~~ Implemented (see docs/design/journal/'s entry for the
  full "why this and not type-argument inference" reasoning):
  `use collections.HashMap` then plain `HashMap<int, string>`, no
  `collections.` prefix — one name per import, no list form. Resolved
  and privacy-checked once at the `use` line (`resolve_named_imports`,
  loader.c), not re-resolved at every call site, and it's a compile-time
  error to import the same local name twice or to collide with
  something the importing module declares itself. An `as` alias's
  casing must match the imported name's own (checked at parse time,
  before the loader even knows what the name resolves to — `HashMap`
  starting uppercase is already enough to know an alias for it must too,
  since Ziman's grammar depends on that invariant regardless of what the
  name turns out to be). Only cuts the module-prefix repetition, not the
  explicit-type-argument one — `hashmap_new<int, string>()` still needs
  its type arguments spelled out; that's the half of the friction this
  was never meant to fix.
- ~~**`bigint`/`decimal` — arbitrary-precision integer and decimal.**~~
  Implemented (docs/syntax/ "Numbers", docs/design/journal/'s "bigint"/
  "decimal" entry): a third and fourth numeric type. `bigint` is
  sign-magnitude, modeled on Java's `BigInteger`, backed by
  `src/bignum.c`'s own arbitrary-precision arithmetic (add/sub/mul/
  truncating-divmod/compare, base-1e9 limbs), with full `int` parity:
  `+ - * / %`, unary `-`, `< > <= >= == !=`, `as` casts to/from
  `int`/`float`/`decimal` (lossy, saturating/truncating, never silent),
  `str`/`abs`/`min`/`max`/`hash`, `parse_bigint`. A literal needs an
  explicit `n` suffix (`123n`) — bare digits still always mean `int`.
  `decimal` is modeled on Java's `BigDecimal`, built directly on
  `bigint` (`src/decimal.c`: an unscaled magnitude plus a scale), with
  full `float` parity: `+`/`-`/compare align scales first and are exact;
  `*` needs no alignment and is exact; `/` doesn't always terminate, so
  it rounds half-up-away-from-zero to `max(a.scale, b.scale)`; `%` is an
  exact remainder; `==`/`hash` compare/hash numeric value ignoring
  trailing-zero scale differences (`1.0d == 1.00d` is `true`,
  deliberately diverging from Java's well-known `BigDecimal.equals()`
  gotcha where scale participates in `equals()` but not `compareTo()`);
  `math.round(d, scale)` is the `floor`/`ceil`/`round` equivalent (a
  separate name, not an overload — when this was written a native took
  exactly one signature per name; **since F184 it does not**, and
  `round(x: float) -> int` against `math.round(d: decimal, scale:
  int) -> decimal` differ in argument one, so merging them into one
  `round` set is now possible and simply has not been argued for. See
  "Relaxing the no-overloading rule"). A literal needs an explicit `d` suffix (`19.99d`,
  `20d`). Both compose with everything already built: a generic type
  argument (`Box<bigint>`/`Box<decimal>`), an array element, and a
  `HashMap` key/value (`bigint` satisfies both `Eq`/`Hash`) all verified
  working. `make test-bignum`/`make test-decimal` run each engine's own
  correctness suite (factorials up to `100!` cross-checked against the
  published exact value for `bigint`; scale-aligned arithmetic,
  half-up-away-from-zero division/round rounding both directions, and
  a 30-plus-digit multiply/divide round-trip for `decimal`) independent
  of the language-level tests/lang harness.
- ~~**`byte` — a fifth scalar type, motivated by wanting to represent
  raw octets (a UUID's bytes) without overloading `[int]` for it.**~~
  Implemented (docs/syntax/ "Numbers", docs/design/journal/'s "byte" entry):
  unsigned, 0-255, literal suffix `b` (`200b`), range-checked at parse
  time (`300b` is a compile-time error, not a silent wrap). Deliberately
  *not* a sixth numeric type on the `int`/`float`/`bigint`/`decimal`
  model: no `+ - * / %`, no unary `-`, no `< > <= >=`, no
  `abs`/`min`/`max` — nothing that needs a `byte` needs arithmetic on
  one, and Ziman having no operator overloading means adding those
  would force an arbitrary wrap-vs-saturate answer to a question
  nothing's actually asking. Supports `==`/`!=`, `hash`, `str`/`print`/
  `format`, and a saturating cast to/from `int` only (`300 as byte` is
  `255`, `-5 as byte` is `0`, matching `bignum_to_i64_saturating`'s
  "clamp, don't wrap" precedent; no cast to/from `float`/`bigint`/
  `decimal` — unused code otherwise). Composes with everything generics
  already built at essentially zero marginal cost: `byte` satisfies both
  `Eq` and `Hash` (just another case in the existing scalar-kind
  checks), so `HashMap<byte, V>`, `[byte]` equality, and `contains` on a
  `[byte]` all work with no new machinery. The actual motivating case
  was UUID (below) — this was step one, done in isolation so it could
  be tested (and get its own test fixtures) independent of UUID's own
  design.
- ~~**`random`/`uuid` core modules — the actual motivating case for
  `byte`.**~~ Implemented (docs/syntax/ "Modules", docs/design/journal/'s
  "random/uuid" entry): two new bundled core modules. `random` is one
  native function, `random.bytes(n: int) -> [byte]`, reading straight
  from `/dev/urandom` — Ziman's first source of genuine
  non-determinism (`clock`, below, is the second). `uuid` is pure Ziman
  on top of it: a `Uuid` struct (`bytes: [byte]`), `uuid_new()`
  generating an RFC 4122 v4 UUID, `uuid.format` formatting the
  canonical dashed-hex string. Sets the version/variant bits with pure
  integer arithmetic (`(b % 16) + 64`, `(b % 64) + 128`) instead of
  bitwise operators, which Ziman has none of (no `& | ^ << >>`) —
  confirms `byte`'s int-cast-but-no-bitwise-ops design was actually
  sufficient for the one thing that needed bit-level control. Also
  confirms a core module can depend on another core module
  (`uuid.zm`'s `use random.bytes`) with zero new loader logic,
  since the corelib fallback triggers on the `use` path string alone,
  never on where the *importing* module's own source came from.
  `Uuid` satisfies `Eq` — usable as a struct field
  compared with `==`, e.g. a `Patient.id: Uuid` replacing the
  `next_id: int` counter `health-record-backend-ziman` used to use —
  (*update 2026-09-23: it is also `Hash` and `Ord` now, so it is a usable
  `HashMap` key; see roadmap R11/R12*). ~~Not wired into
  `health-record-backend-ziman` yet~~ — done the same day:
  `Patient.id` is `uuid.Uuid` now, no more counter.
- ~~**`time`/`clock` core modules — seven `java.time`-shaped types
  (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`,
  `ZoneOffset`, `ZoneId`), closing `health-record-backend-ziman`'s last
  remaining documented gap (a real date type).**~~ Implemented
  (docs/syntax/ "Date and time", docs/design/journal/'s "time/clock"
  entry). `ZoneOffset` only at first; IANA named zones (`zoneid_of`, read from
  the system's zone files by the `zone` module) came later.
  `Instant`/`Duration` are `{seconds: int, nanos: int}` (matching both
  C's `struct timespec` and `java.time`'s own internal shape) after a
  `bigint`-nanoseconds design was tried and dropped once nanosecond
  precision was specifically wanted: plain `int` seconds have ~292
  *billion* years of range with no `bigint` needed at all, `int`
  nanoseconds-since-epoch would only have ~292. Calendar math
  (`LocalDate`'s epoch-day conversion) is Howard Hinnant's public-domain
  days-from-civil algorithm, translated directly into Ziman since it
  was already written against truncating integer division. One new
  native primitive, `clock.now_raw() -> [int]` (`[seconds, nanos]` from
  one atomic `clock_gettime()` call), the same "native returns a small
  scalar array, pure Ziman gives it a shape" split `random`/`uuid`
  already established. No `<`/`>` on any of the seven types — each gets
  an explicit `_compare(a, b) -> int` instead, same "don't reopen
  Ord/Cmp without a forcing case" posture as everywhere else. ~~Not
  wired into `health-record-backend-ziman` yet~~ — done the same day:
  `Patient.date_of_birth`/`created_at` are `time.LocalDate`/
  `time.Instant` now, closing that project's last documented gap.
  ~~No way to parse a date string back into a `LocalDate`~~ — resolved
  later, once `request.zm`'s own hand-rolled `parse_iso_date` surfaced
  it as a real, concrete need: every type got a `_parse` (the exact
  inverse of its `_to_string`) plus, for the calendar-shaped types and
  `Instant`, a general pattern-based `_format`/`_parse_pattern` pair —
  see docs/design/journal/'s "time" formatting/parsing entry.
- ~~**`json` core module — a parser/serializer, motivated by
  `health-record-backend-ziman`'s "no JSON, no real request/response
  types" gap.**~~ Implemented (docs/syntax/ "JSON", docs/design/journal/'s
  "json" entry). Before writing it, verified directly (not assumed)
  that Ziman can represent arbitrary JSON at all, given an old ROADMAP
  note that a self-referential struct can't be constructed: `enum Json
  { JNull JBool(bool) JNumber(float) JString(string) JArray([Json])
  JObject([JField]) }` (`JField { key: string, value: Json }`) compiles
  and runs correctly, including genuinely nested values — the old
  limitation is about a *direct* self-reference with no escape
  (`struct Node { next: Node }`, truly infinite), not recursion through
  an array, which has a finite base case (`[]`). `JNumber` holds a
  `float` (matching virtually every other JSON implementation, not
  `decimal`/`bigint` — nothing needs exact precision for a JSON number
  specifically). `parse_json`/`json_to_string` are a real recursive-
  descent parser and serializer, not string-searching; string escapes
  cover `\" \\ \/ \n \t`, not `\r \b \f`/`\uXXXX` (Ziman's own string
  literal syntax can't write the first three, and `\uXXXX` needs a
  codepoint-to-UTF-8 encoder nothing here has). `json_object_get` is
  the one convenience lookup beyond parse/serialize. `Json` does not
  support `==`/`!=` (array equality needs a scalar element type,
  `Json` itself isn't one) — a real, known gap, left alone. Stops at
  the general engine — a schema-specific parser for one particular
  request shape (`parse_patient_create_request`-style) is expected to
  live in `health-record-backend-ziman` itself, not in this module.
- ~~**`docs/CORELIB.md` — a generated signature reference for the core
  modules, once the surface got big enough that hand-knowing or
  hand-listing them stopped being realistic.**~~
  Implemented (`tools/gen_corelib_reference.sh`, `make
  corelib-reference`; docs/design/journal/'s "CORELIB.md" entry). Extracts
  every `pub struct`/`pub enum`/`pub fn`/`pub native fn` straight from
  `lib/core/*.zm`'s own source rather than hand-copying them — this
  project had already caught and fixed stale hand-written comments/docs
  repeatedly by this point, so a hand-maintained signature list was
  guaranteed to drift the same way eventually. Confirmed against source
  before considering it done: the generated file's total `pub` line
  count matched `grep -c '^pub ' lib/core/*.zm`'s count exactly. Not part
  of the default `make`/`make all` build (it's a doc, not a build
  input) — an opt-in target, same treatment `test-bignum`/`test-decimal`
  already get.
- **Compilation to bytecode or native code.** v0 is a tree-walking
  interpreter on purpose — revisit only once the language design itself
  has stabilized against real use, not before.

  *Update 2026-10-12 (F82): measured, and the answer is "not yet" on this entry's own criterion
  rather than on appetite.* **The gate here is language stabilisation, not speed**, and it is
  plainly unmet: 31 commits in the two days to 2026-10-12 changed the language or its library --
  `#[deprecated]` gained two declaration kinds and a module form, `ServeOptions` gained fields
  twice, the type checker's expected-type plumbing changed, and the *formatter's output* changed,
  which reformatted 12 files of the reference backend. A bytecode backend is a second consumer of
  every such decision; F66 is the sharpest case, where one call site passing no expected type was a
  six-form bug.

  **"Bytecode or native" is three different projects, and two of them are already answered.**
  *Startup* is **3.5ms** for a trivial program (process, prelude, type check), so there is nothing
  to win and a compile step to lose. *Throughput* is about **19M loop iterations per second** on a
  tree-walker that has already had its easy wins taken (F2 found the GC rather than `strcmp` was
  the cost, dispatch is cached, slots are pre-resolved) -- a bytecode VM buys 2-5x over a *naive*
  walker and less over this one, and no program is waiting on it: the reference backend serves
  about 1,600 rps at concurrency 8. *A deployable artifact* bytecode does not give you at all --
  you would still ship the interpreter plus a blob -- and plain `.zm` modules with no bytecode
  format are an **AI-first feature**, since a model can read the library.

  **Two arguments that look like they favour it and do not.** A faster interpreter would not speed
  the generation benchmark, whose cost is agent time. And it would not speed `make check`: the 957
  fixtures take **25 seconds** of a roughly twelve-minute gate, the rest being two C builds and the
  backend's 40 server-spawning suites. **The cost nobody had priced** is a second execution engine
  against two collector builds, every change having to stay green in all of them.

  **What would reopen it**, so this is a testable "not yet" rather than a standing no: a real
  program where Ziman's speed is the blocker; `make check` becoming interpreter-bound (it is off by
  a factor of about 25); or the language going quiet for a sustained stretch, which is what this
  entry asked for in the first place.
- **First-class functions / closures.** *Update 2026-09-23 (R13): stage 1
  shipped -- function types and references to named top-level functions, no
  capture; see docs/design/journal/'s function-values entry. What follows is the
  original reasoning, kept for the record; capturing lambdas are still
  open.*
  *Update 2026-10-10 (F39): that "no capture" is out of date, and what is left
  is one specific thing rather than "closures".* **Capture by value shipped:**
  a lambda literal captures a `val` (`fn(n) { n * factor }`), and it may
  outlive the function that built it -- `fn multiplier(by: int) -> fn(int) ->
  int { return fn(n) { n * by } }` works, capturing a parameter. Verified by
  running each case rather than from this entry.
  **Dropped 2026-10-10 (F40): mutable state in a lambda is already both
  expressible and guarded, so there is nothing here worth a language feature.**
  Capture is by value and what gets copied is a *handle*, so a lambda holding a
  `MapBuilder`/`StringBuilder`/`ArrayBuilder` mutates shared state through it --
  a memoising closure with a live cache needs no change (fixture
  `closure_builder_capture`). The unsafe case is a compile error: a lambda that
  captured a builder carries `Type.func_captures_thread_bound`, so `spawn` and
  `send` refuse it even though its written type (`fn()`) says nothing about a
  builder (A14; fixture `diag_lambda_builder_crosses_thread`). And the
  accumulate shape that looks like it needs a mutable capture is what `fold`
  is for, with the accumulator in the signature instead of hidden in a capture.
  So the `var` rule costs a spelling, not a capability -- and it buys something:
  since captures are by value, allowing a `var` would leave `fn(n) { n * factor }`
  ambiguous about whether it sees later writes, while a handle puts shared
  mutable state at its declaration where it is visible.
  *Also corrected: `metrics.zm`'s header gives the capture rule as the reason
  its counters are native, and that overstates it -- the binding reason is that
  a builder cannot cross a `spawn` at all, so thread-shared state needs native
  atomics regardless.*
  **What remains, and it is a parser gap rather than a feature:** a function
  type cannot be written as a *type argument*, so `Channel<fn(int) -> int>` is
  inexpressible -- `f: fn(int) -> int` as a parameter and `val f: fn(int) -> int`
  as an annotation both work. The `send`-side thread-bound guard is therefore
  present but unreachable, written for a case the parser forbids; whoever fixes
  the parser should know that check is already waiting for it.
  **The original framing, kept for the record -- capturing a `var`**, so that a
  lambda could hold mutable state directly: the classic `fn counter() -> fn() -> int { var n = 0
  return fn() { n = n + 1 return n } }` is refused, with a message that says
  why -- captures are by value, so capturing a `var` would silently snapshot
  something the author expects to stay live. That is not academic:
  `lib/core/metrics.zm`'s own header gives it as the reason every counter,
  gauge and histogram in that module is native ("a Ziman lambda cannot mutate
  what it captures, so state shared across threads has to sit behind a handle
  the way `StringBuilder` and `Iter` do"). So the open question is narrower
  and sharper than "closures": should a lambda capture a `var` by reference,
  what owns the cell, and what does that mean across threads -- which is the
  "new runtime representation bundling code + captured environment" this entry
  anticipated, plus the thread-safety question the metrics module already
  answers in C.
  *Separately, and small: `multiplier(3)(5)` does not parse (`expected ')',
  found '('`) -- calling a returned function requires binding it first. A
  parser limitation, independent of capture.*
  Originally: function calls only
  resolve a bare identifier at parse time; no function values, no passing
  a function as an argument. Considered building this before `spawn`
  (whose natural long-run shape would take a closure) and deliberately
  didn't: nothing concrete forces the question yet. The more likely real
  forcing case is `net`/`http` routing (a table of path → handler
  function) once that module exists — and what that needs is probably
  smaller than full closures anyway: a plain reference to an
  already-declared function (closer to a C function pointer than a JS
  closure, since a top-level function has no enclosing scope to capture
  from) would likely suffice for a routing table, without the harder
  closure questions (capture by value or reference, a new runtime
  representation bundling code + captured environment). Worth deciding
  that distinction first, whenever the real need arrives.
- **String interpolation.** Once string concatenation is settled, decide
  whether interpolation is a second way to do the same thing (against
  principle #2) or genuinely a different need.
  *Update 2026-10-10 (F38): measured, and there is no AI-first case for it.*
  **Across all 30 cold benchmark attempts no interpolation form appears even
  once** -- `f"..."` 0/30, `"${x}"` 0/30, `"#{x}"` 0/30. What models reach for
  is an operator for concatenation -- **re-counted 2026-10-10 (F48) as `+`
  6/30 and `++` 8/30, 14 of 30 between them**, not the 16/8/24 first recorded
  here, which had counted `+` tokens rather than string concatenations (`i = i
  + 1` is in most of these files) -- and
  that collision is already handled the way this language says it should be:
  it fails at compile time with the sentence that fixes it, a message several
  benchmark runs called the best in the set. So the question this entry poses
  is unchanged by the evidence and the entry stays open on its own terms.
  Recorded because the opposite was assumed -- the 2026-10-10 priority ranking
  put interpolation in tier 1 on the reasoning that a model writes an
  f-string by reflex, which the data refutes.
- **A real package registry** (npm/crates.io-shaped, not Maven Central-
  shaped). Worth naming the distinction: Java's JARs are *compiled*
  artifacts, but Ziman modules are plain `.zm` source with no bytecode/
  compiled form at all — so npm and crates.io are the closer precedents
  regardless, since both are fundamentally source-distributing registries
  (crates.io hosts source tarballs; cargo compiles locally, same as
  Ziman's interpreter would just parse locally). Firmly sequenced after
  the module system itself is proven out with a real first-party
  standard library (nothing to register or resolve against yet) — and a
  much bigger undertaking than anything in the language itself once it's
  time:
  - **A manifest file** (`ziman.toml`/`ziman.json`-shaped) declaring a
    package's own name, version, and dependencies — exactly the thing
    the module system's design deliberately did *without*, since there
    was no third-party versioning to support yet. This is the point
    where that non-decision gets revisited, not before.
    *Update 2026-09-28:* `internal` visibility (docs/design/journal/'s
    "`internal` visibility..." entry) now has a coarse, working notion of
    "package" — `same_package` (loader.c) is one boolean comparison,
    "both bundled corelib, or both this project" — specifically designed
    as the one seam a manifest file would redefine later (comparing
    manifest identities instead of that flag) without `internal` itself,
    its enforcement call sites, or any already-written `internal`
    declaration needing to change. Not a manifest file, not dependency
    resolution — just confirms the visibility half of "what package am I
    in" won't need reworking once this is built.
  - **A versioning + resolution scheme.** Leaning toward Cargo's model
    (one resolved dependency graph, a lockfile pinning exact versions)
    over npm's historical nested/duplicate-version tolerance — a single
    unambiguous resolution per build fits "one way to do things" better
    than allowing two versions of the same dependency to coexist.
  - **The registry itself is a real piece of internet infrastructure**,
    not a language feature: hosting, publish/fetch, namespacing,
    availability — comparable in scope to the rest of this project, not
    a small addition to it.
  - **Supply-chain trust is a real, not theoretical, concern** — npm
    specifically has a well-documented history of malicious/compromised
    packages. Whatever gets built should design around this from the
    start (lockfiles pinning exact resolved versions at minimum;
    namespacing and any review/signing story are open questions), not
    bolt it on after an incident.
  - **A concrete first candidate for what should actually move out of the bundled corelib once
    this exists: the `db` drivers, `mysql.zm` and `pg.zm` specifically** (2026-09-29). Each is a
    real wire-protocol implementation end to end (`pg.zm` 850 lines, `mysql.zm` 1,338 —
    "the largest externally-specified surface in the library," per the language review,
    §4.4), already marked experimental ahead of the rest of the core (see F4/docs/design/journal/'s
    "Experimental modules" entry) precisely because they carry protocol/maintenance risk the
    rest of corelib doesn't. Today every `ziman` binary embeds them regardless of whether a
    given program ever opens a Postgres or MySQL connection, the same "one binary, no install
    step" tradeoff the whole corelib makes ("Bundled core modules" elsewhere in this file) --
    reasonable for `json`/`collections`/`regex`, much less so for two entire SQL wire protocols
    (SCRAM auth, wire-format encode/decode) a typical program never touches. Once packages are
    real, `mysql`/`pg` (and plausibly `sqlite`, for the same reason, though it links a system
    library rather than reimplementing a protocol) become ordinary published packages a project
    opts into with a dependency line, not permanently part of every binary; `db`/`datasource`
    (the driver-agnostic `Connection`/pooling layer) stay in the corelib proper, since every
    driver package would need them. `internal` visibility (docs/design/journal/'s own entry) already
    treats `db`'s request-plumbing structs as shared *within* today's one bundled-corelib
    package; splitting drivers out means `same_package`'s definition (today "both bundled, or
    both not") has to grow to recognize "this driver package and the corelib it depends on"
    as cooperating packages too, not just "corelib vs. everything else" -- one more concrete
    thing the eventual manifest-based redefinition needs to handle, not a new problem.
- ~~**Attributes (`#[...]`) and a first `derive` on top of them, starting
  with `#[derive(Json)]`.**~~ Implemented (`src/derive.c`, docs/design/journal/'s
  "Attributes and `#[derive(Json)]`" entry). Grew out of a "should Ziman
  have reflection" conversation, prompted directly by
  `health-record-backend-ziman`'s `patient/response.zm`/`request.zm`
  hand-writing the exact same field-by-field JSON mapping a real
  Jackson-based backend gets from annotations + runtime reflection.
  Rejected true reflection outright (needs a value of unknown type until
  runtime, cutting against every static-typing decision this language
  has made) in favor of *compile-time* derivation — a general `#[...]`
  attribute mechanism, with `derive(Json)` as its first consumer,
  generating both `<type>_to_json` and `<type>_from_json` for a struct
  or enum. Per-field/per-payload conversion follows a priority order
  (custom `_to_json`/`_from_json` first, then a `_to_string`/
  `_from_string` pair, then a plain scalar, then `Option<U>`/`[U]`
  recursion); deserialization is lenient (unknown JSON fields ignored);
  an enum derives an externally-tagged `{"type": ..., "value": ...}`
  shape (Rust serde's default), with `Option<T>` excluded from that
  tagging since it already has its own null/unwrapped handling in the
  per-field rules. Two things this v1 deliberately still leaves open,
  recorded so a future version doesn't rediscover them: field name
  mapping (`snake_case` -> `camelCase`) is fixed and unconfigurable, and
  there's no per-*field* annotation yet (e.g. overriding one field's
  JSON key, or a future `#[derive(Validate)]`-style per-field
  constraint).

- **Database access follow-ups.** `db`/`datasource`, `#[derive(Row)]`, `net`,
  `crypto`, a shared URL parser (now in `db`), and Postgres/MySQL drivers (`pg`/`mysql`)
  exist (docs/design/journal/'s `db`/`datasource`, `net`, `crypto`/`pg` and `mysql`
  entries); SQLite, Postgres and MySQL are the three backends. Still open: (1) ~~TLS~~ **done for clients** (docs/design/journal/'s TLS entry) -- ~~still
  open: server-side TLS (`listen_tls`, for an HTTPS server), client
  certificates, ALPN~~ -- **all three now done** (F64, F65);

  *Update 2026-10-11 (F64): two of those three were already or are now done.* **Server-side TLS
  has existed for some time** -- `net.listen_tls`, with its own TLS suite test, and `http`'s own
  docs tell you to hand `serve` a listener from it -- so this list outlived its first item.
  **Client certificates are now done on both sides**: `net.TlsServerOptions` (`client_ca_file`,
  `require_client_cert`, via `net.listen_tls`) makes a listener authenticate its clients,
  `net.TlsClientOptions` (`cert_file`, `key_file`, via `net.start_tls` /
  `net.connect_tls`) presents one, and `net.peer_certificate_subject` is the identity to
  authorize against. Two things the test transcript records because neither is obvious: a client
  whose certificate is refused still gets `Ok` from the handshake and only learns on the first read
  (TLS 1.3 validates the client certificate after its Finished), and requiring a certificate with
  no CA file to check it against is refused at `listen` rather than failing every connection with a
  complaint about the certificate. **Not done: mutual TLS through `http.client`**, which threads a
  lone `ca_file` positionally through six call layers; the right fix is to thread a
  `net.TlsClientOptions` there instead, which is mechanical but wide enough to be its own task.

  *Update 2026-10-11 (F65): ALPN closes this list.* `TlsServerOptions.alpn` and
  `TlsClientOptions.alpn` offer protocol lists and `net.negotiated_protocol` reports what was
  agreed -- the signature the HTTP/2 entry below guessed, down to the name. The listener's order is
  its preference, which is the choice RFC 7301 leaves to the server, and the test pins that by
  having the two sides disagree on purpose. A client that offers ALPN and shares nothing with the
  listener is **refused** with a fatal alert rather than served a protocol it did not ask for. (2) ~~scoped cleanup~~ **done as `defer`** (docs/design/journal/'s `defer` entry).
  What remains of it: a block form (`defer { ... }`), the deferred call's
  discarded `Result` (a failing cleanup is invisible), fatal errors
  skipping defers, and no true `with_transaction(fn)` (no closures); (3) ~~connection pool~~ **done as v1**
  (docs/design/journal/'s pool entry) -- still wanted, and deliberately left for
  a better implementation: ~~reconnect and an acquire timeout~~ (done: a
  connection-class failure replaces the connection, `datasource.acquire_timeout`,
  `datasource.discard`, and a `SELECT 1` health check of long-idle connections on borrow,
  `datasource.open_pool_with`), min/max sizing and
  idle timeouts, resetting a connection on release, and real pooling over
  in-memory SQLite (shared-cache); (4)
  ~~**schema management/migrations**~~ (done: the `migrate` module, a
  directory of `V<n>__<name>.sql` files applied once each with checksums, with
  per-engine subdirectories for SQL that differs; no down migrations); (5) per-field `#[derive(Row)]` annotations
  (column-name override, primary key, generated columns) -- the same
  deferred per-field-annotation gap `#[derive(Json)]` documents; (6)
  Postgres gaps: MD5 authentication, prepared-statement reuse, COPY,
  LISTEN/NOTIFY, IPv6 literals in the URL, SASLprep, and decoding native
  `timestamptz`/`date`/`uuid`/`numeric` columns into their Ziman types
  rather than through `Text`; (7) **performance** on big result sets --
  `[byte]` is an array of interpreter values, so a packed byte-buffer
  type would help the driver as much as `net`; (8) MySQL gaps: the RSA
  public-key exchange (so caching_sha2's first login works without TLS),
  the compressed protocol, LOCAL INFILE, client certificates, IPv6
  literals, multiple result sets (only the last is kept), prepared-
  statement reuse, and decoding DECIMAL/DATE/DATETIME/TIME/JSON columns
  into their Ziman types rather than through `Text`.
- **`net` follow-ups.** ~~A packed byte-buffer type (`[byte]` is an array
  of interpreter values -- wasteful for bulk data)~~ **done, and this entry
  had gone stale: `[byte]`, `[bool]`, `[int]` and `[float]` are stored packed
  -- one real byte/byte/int64/double per element rather than an 80-byte boxed
  value (docs/syntax/ "Arrays"). Measured 2026-10-10 rather than trusting
  the doc: two million `[byte]` elements cost 40.9 MB against 540.2 MB for two
  million boxed one-field structs, 13x, so there is nothing left to pack.**
  ~~Unix-domain sockets
  (local Postgres often uses one)~~ **done 2026-10-10: `net.connect_unix(path)`
  and `net.listen_unix(path)`, with the `Socket` behaving identically afterwards.
  The two differences from a TCP listener are reported rather than hidden -- the
  107-byte path limit, and that binding an existing path is refused because a
  stale socket and a live one are indistinguishable from there.** UDP, and a
  connect timeout beyond the OS default, remain. ~~An `http` module (server first, pure Ziman on `net`)~~
  **done as a toolkit** (docs/design/journal/'s `http` entry); still open there:
  ~~TLS (`listen_tls`), chunked request bodies~~ **done**, HTTP/2 (its own entry below), ~~multipart,
  compression, `Date`/`Server` headers~~, and -- once Ziman has function
  values -- a real `serve(listener, handler)` and route table on top of
  the same matcher.

  *Update 2026-10-11 (F61): three of those four are settled, and only one needed code.* `Date` was
  already on every response, and `Server` is a decided **no** -- the library does not say what it is,
  because that only tells an attacker what to look for, and the backend's `headers_test` asserts the
  absence. Response **compression** is now `server.compressed`, **off unless asked for**
  (`ServeOptions.compress_responses`): compressing a response that holds a secret beside
  attacker-influenced text leaks it through the length (BREACH), and an API whose login answer carries
  a JWT is that shape. 13,200 bytes to 117 on the wire when enabled. **Multipart was left for F62** (below). Adding one
  `ServeOptions` field broke eight call sites, so `serve_options()` now supplies the defaults and
  `with server.serve_options() { .. }` is the way to build them; that was the last such break.

  *Update 2026-10-11 (F62): the fourth is done too, so this list is closed.* `server.multipart(req)`
  reads a `multipart/form-data` body into `Part` values -- bytes, not text, because a part may be a
  file. I argued against building it on the grounds that nothing consumes one yet; Menderes decided
  to build it, and file upload being on the roadmap makes a parser worth having before its first
  caller. `multipart_boundary` is deliberately a separate function so a handler can answer **415**
  for a request that is not multipart at all while every rejection of a body is a **400**, and
  `Part.filename` is documented as the client's claim rather than a path, because it is the one
  field here that invites a traversal. What remains under this entry is HTTP/2 alone.

- ~~**A `Row` bound, generalizing `Eq`/`Hash`/`Ord`'s mechanism to a fourth case**~~ **-- design
  pass done 2026-10-09 (C18), and the answer is no, with F1's decision to add it withdrawn.**
  `Ord`'s mechanism is runtime dispatch on a value's own type and generics are erased, so
  `to_params: fn(E) -> [SqlValue]` fits it and `from_row: fn(Row) -> Result<E, DbError>` cannot:
  `E` is only in the return type, so at the call there is no value to dispatch on. Covering one of
  a derive pair's two functions under a bound named after the pair is worse than covering neither.
  Two further findings, both checked rather than argued: struct literals have no field defaults, so
  nothing could "disappear" from a literal without a second feature; and F1's safety argument is
  already covered by the type checker, which rejects the cross-wiring it worried about. What is
  left is eighteen lines of boilerplate in one file of one sibling repo. Full reasoning, and the
  three things that would reverse it, in docs/design/journal/ "The `Row` bound, and why `from_row` cannot
  work the way `compare` does". Original entry below, unchanged, since it is what the decision was
  taken against (see
  docs/design/journal/ "The traits/interfaces stance", F1). `Crud<E, Req, Res>` (E9,
  `health-record-backend-ziman`'s `service/crud.zm`) needs `from_row: fn(Row) ->
  Result<E, DbError>` and `to_params: fn(E) -> [SqlValue]` as explicit function-value
  fields on every literal, because there is no way to say "`E` already has a
  `#[derive(Row)]` pair" as a bound, the way `<T: Ord>` already says "`T` already has
  a `_compare`." A `Row` bound (`fn crud_find_by_id_and_patient_id<E: Row, Req,
  Res>(...)`) would resolve `<snake_type>_from_row`/`<snake_type>_to_params` the same
  way `compare<T: Ord>` already resolves `<snake_type>_compare`
  (`find_compare_function` in the type checker), and the two fields would drop out of
  every `Crud` literal -- nine of them today, growing as more resources migrate.
  Not started, not designed in detail -- needs its own pass: extending the bound
  grammar/checker to a fourth name (`Eq`/`Hash`/`Ord`/`Row`), teaching it a two-
  function signature pair rather than the one-function shape every existing bound
  has, deciding whether `#[derive(Row)]` alone satisfies it or a hand-written pair
  matching the naming convention also counts (`Ord` accepts both today), and a
  fixture proving `Crud<E: Row, Req, Res>` resolves the right pair for two different
  `E`s in the same file -- the exact copy-paste-mismatch class this bound exists to
  rule out at compile time, not just tidy up.

- **`Cell<T>`: one primitive that makes the mutable layer open to library code -- done 2026-10-14 as
  `Mutable<T>` (F108); the worked actor example followed on 2026-10-16 and closed the entry.** Raised 2026-10-13 by
  Menderes, out of planning to use Ziman on other projects -- UI applications among them -- where the
  objection is structural rather than about any one type: *"every time we hit a wall we have to change ziman
  itself to add mutable types instead of implementing one with ziman."* That is correct today, and it is the
  reason to do this; it is not a complaint about the five that exist.

  **What is already open, which narrows the problem a long way.** `#[thread_bound]` is a user-available
  attribute, not an internal marker: a struct written in Ziman carries it and gets the compile-time refusals
  (not a `Channel` element, not a `spawn` argument, not a module-level `val`), and those refusals are
  transitive -- verified through a struct field, an `Option<ArrayBuilder<int>>` and an `[ArrayBuilder<int>]`,
  on both boundaries, with a lambda that captured one refused where it is written. So the safety story a new
  mutable type would need is already there and already generalises. What is closed is exactly one thing:
  **there is no way to make a field change.**

  **And that wall is lower than it looks.** A mutable type is expressible in Ziman *today*, with a
  one-element `ArrayBuilder` standing in for a cell -- a `Counter` struct whose `ab_get`/`ab_set` pair gives
  bump-and-read, copied by value while the state behind the handle is shared, usable from a closure, and
  still refused across a thread. It works; it is simply unnameable and nobody would guess the API. The gap
  is a name and an honest signature, not a capability.

  **The proposal.** One new native generic, `Cell<T>`, `#[thread_bound]`, with `cell_new(value)`,
  `cell_get(c)` and `cell_set(c, value)`. Three functions, one type. Everything open-ended then composes in
  library code rather than in C: a ring buffer is a struct over `MutArray` plus two `Cell<int>`s; an LRU is
  `MutMap` plus `MutArray` plus a cell; a UI signal is a `Cell<T>` plus a `MutArray` of subscribers; a node
  with mutable children is a struct holding a `MutArray<Node>`, and the tracing collector already handles the
  cycles a parent pointer makes. The language stops growing each time a new mutable shape is wanted.

  **What this is not, and should not become: mutable struct fields.** A struct is a value and is copied, so a
  mutable field means mutating a copy and wondering why the original did not change -- the aliasing confusion
  the language has avoided everywhere else, and `with` already covers update-as-a-value. `Cell` keeps the
  distinction visible: the struct stays a value, the cell is plainly a handle. No methods are needed either.

  **A decision this has to make, found while checking the above and worth fixing either way.** A struct
  holding a handle is **comparable and hashable today**, by accident rather than by decision: the handle is
  an `int` field, so `Counter { slot: ab_new<int>() } == Counter { slot: ab_new<int>() }` is `false` (two
  empty builders, unequal), `hash` of one is stable within a run, and such a struct is accepted as a `Map`
  key. That is identity equality wearing value equality's clothes, keyed on a pointer value that means
  nothing across runs -- exactly where a UI framework's memoisation key or node comparison would go wrong.
  `Cell` should be **neither comparable nor hashable**, as `Channel` already is not, so a struct containing
  one is refused as a `Map` key and `==` on it is a type error rather than a surprise; and the four builders
  should be changed to match, which is a small breaking change nothing in this repository or the reference
  backend relies on (checked).

  **Cost, honestly.** A mutable type built in Ziman is a function call per operation through a handle, where
  `MutArray<int>` is a packed buffer -- so the native collections stay as the fast paths and `Cell` is for
  shapes, not for hot loops.

  **Do it when a real program asks.** The same test every other decision here takes: not before something
  concrete wants a mutable type the five do not cover. A UI project is the likely first caller, and the
  cheapest thing to do first is the one named at the end of the mutability discussion -- a worked actor
  example (a counter or small cache owned by a spawned loop, talked to over a `Channel`), which would show
  whether process-lifetime mutable state needs anything new at all.

  *Update 2026-10-13: designed in full the same day, and renamed. The type is `Mutable<T>` and the
  functions are `mutable_new`/`mutable_get`/`mutable_set`, plus a fourth, `mutable_update(c, f)`.
  `Cell` was dropped because the companion decision renaming the builders makes "every `Mutable*`
  type is thread-bound" a rule, and `Cell` would have been a third type outside it after
  `StringBuilder` and `Iter`; `Mutable<T>` is instead the root that `MutableArray`/`MutableMap`/
  `MutableSet` specialise -- which is also what the `MutArray`/`MutMap` spellings above became. The
  equality and hashing decision this entry raised stands and is now joined by two more found while
  checking it: a handle prints a raw address, and `#[derive(Json)]` does not refuse a handle field.
  Everything else here -- the objection, the transitive `#[thread_bound]` story, the refusal of
  mutable struct fields, the cost, and "do it when a real program asks" -- is unchanged. See
  docs/design/journal/, "`Mutable<T>`: the root of the family, and why there is no `MutableInt`" and "The
  builders become mutable collections".*

  *Update 2026-10-16: the worked actor example is written --
  `tests/lang/actor_owns_mutable_state.zm` -- and its answer is that
  process-lifetime mutable state needs nothing new. One actor owns a `MutableMap`, a `Mutable<int>`
  and a `MutableString` at once and serves four threads making a hundred requests over one
  `Channel`, with no lock, because one thread is the only thing that touches them; the state is
  created inside the actor, so the `#[thread_bound]` refusals never arise -- they are what makes
  this shape the one that works. Writing it did find one gap, now fixed: `send` gave its value
  argument no type context, so `send(replies, Some(n))` was unwritable even with `replies` declared
  `Channel<Option<int>>`, which is the natural shape for a cache `get`. See docs/design/journal/'s
  "Cost, and when" for both that and the one subtlety worth copying (each client needs its own
  reply channel).*

  *Update 2026-10-14: built, F108. So "do it when a real program asks" below was not the test it
  ended up taking -- the whole mutable-collection rename (F103/F104) made `Mutable<T>` the root of a
  family rather than a fifth member of one, and at that point it cost three natives and one ordinary
  Ziman function, because `#[thread_bound]`, the non-comparability (F107) and the `Mutable*` naming
  check (F106) all applied to it without a line of new code. The worked actor example named at the
  end of this entry is still not written.*

- ~~**Refactor the `collections` module: seven duplicate spellings, one family split across two
  namespaces, and one name that can never move.**~~ **Done 2026-10-14 (F120): the module is deleted
  and the whole family is `array_*`.** Raised 2026-10-14 by Menderes, out of asking
  whether `collections.sort*` should simply be deleted now that `array_sort*` exists.

  *Update 2026-10-16: raised on 2026-10-14 and overtaken the same day -- F120 stage 3 landed at
  00:17 -- with no note added, so everything below read as an open question that was already
  settled. **And it was settled in the opposite direction to the one this entry weighs.** The entry
  asks "where does the sort family live", concludes the answer needs a trigger (promote
  `is_sorted`/`binary_search`/... once something sorts a `MutableArray` and then searches it), and
  says that until then the seven wrappers should stay and the 167 call sites be left alone. F120
  instead deleted `lib/core/collections.zm` outright and moved the call sites (stage 2), so arrays
  now have the shape `Map` and `Set` always had: one bare `array_*` family and no module. Nothing
  was promoted into the flat global namespace, which is the cost this entry was trying to avoid --
  `array_find`, `array_sort_with`, `array_frequencies` and the rest all carry the prefix, verified
  by running them. Both "hard constraints" dissolved with the module: `collections.join` never
  needed to be able to move, because it became `array_join`. The removal also dragged along five of
  the compiler's own suggestion strings, eight comments and two fixtures, recorded in `d4d3b3d`.
  The reasoning below is kept for why promoting names into a flat namespace is expensive, which is
  still the live argument whenever a new bare name is proposed.*

  **The state.** Seven functions exist under two names: `map`, `filter`, `flat_map`, `reverse`,
  `sort`, `sort_desc` and `sort_with` are bare `array_*` in `lib/prelude.zm` *and* one-line
  `collections` wrappers over them (`pub fn sort_with<T>(items, cmp) { return
  array_sort_with<T>(items, cmp) }`). **167 call sites** across both repositories use the
  `collections` spelling -- `sort` 72, `map` 55, `sort_with` 13, `sort_desc` 9, `filter` 9,
  `reverse` 6, `flat_map` 3 -- while the bare ones had zero until F97 documented them. The other
  24 `collections` functions have no bare counterpart at all.

  **This is the rule working, not a leftover.** F97 promotes a function only when the other side
  already has one: a `map_*`/`set_*` twin, or a `mutable_*` twin. The three bare sorts are exactly
  the three with a mutable twin (`mutable_array_sort`/`_desc`/`_with`). So the duplication is the
  *cost* of the rule rather than an unfinished migration, which is why "just delete the wrappers"
  is not obviously right.

  **Deleting them splits the sort family.** It has eleven members and three are bare: remove the
  wrappers and `array_sort(xs)` is bare while `collections.sort_by_keys(xs, ks)`,
  `sort_indices`, `is_sorted`, `binary_search`, `lower_bound` and `upper_bound` still need the
  import. A reader then has to remember which sort lives where, which is worse than a duplicate
  name that always works.

  **Two hard constraints, both found while looking.** `collections.join(items, sep)` can **never**
  be promoted: the bare `join` is already the *path* join over strings, with different semantics
  entirely, so "everything becomes bare" is not a reachable end state. And promoting the rest would
  take `find`, `first`, `last`, `count`, `any`, `all`, `is_sorted` into a flat global namespace
  where every name added is a name a user can no longer have -- the reason F97 declined to promote
  more than it had to.

  **So the question is not "remove or keep" but where the sort family lives**, and there is a path
  that closes it without a decision: the deferred `is_sorted`/`binary_search`/`lower_bound`/
  `upper_bound` group gets promoted by the existing rule the moment it has mutable twins, and its
  trigger is a program that sorts a `MutableArray` and then searches it (checked 2026-10-14: has
  not fired). After that the split is gone and deleting the seven wrappers is tidying rather than a
  trade. Until then the documented answer stands: write `array_sort_with`, and leave the 167
  existing call sites alone.

  **What a refactor should also look at while it is in there**, since these are the same question:
  whether `collections` should be two modules (the array *algorithms* against the handful of
  map/set helpers like `frequencies`, `index_by` and `group_by` that return collections keyed by
  something); and whether `order_of`, which is private, wants to be the one public primitive the
  sort family is written over, the way `sort_positions` already is for the natives.

  *Update 2026-10-19: both loose ends above are now closed, and the trigger this entry named has
  fired. The first module question went with the module in F120. The deferred
  `is_sorted`/`binary_search`/`lower_bound`/`upper_bound` group -- which this entry says gets
  promoted "the moment it has mutable twins", checked 2026-10-14 as not yet fired -- got them in
  F167, so the split the entry worried about is gone: all four take either world under their bare
  names. And `order_of` **is** the one primitive the sort family is written over, as the last
  paragraph wondered, by subtraction rather than by promotion: F169 deleted
  `array_sort_indices`/`_desc`, the two one-line wrappers that were its only other spelling, after
  F166 left them with no caller anywhere outside `lib/prelude.zm`. It stays `array_`-prefixed and
  undocumented-as-API; nothing was lost, since the wrappers were `order_of(keys, false)` and
  `order_of(keys, true)`.*

- ~~**Interval notation for ranges: `[a..b]`, `(a..b)`, `[a..b)`, `(a..b]`.**~~ **Declined
  2026-10-16 by Menderes, after reviewing the evidence below.** `a..b` stays the one range form, and
  `..=` is not taken either; the reasoning is in the update note under this paragraph. Raised
  2026-10-12 by Menderes. ~~**Undecided, and may well be declined**~~ -- recorded so that both the
  option and its price are on the record rather than rediscovered.

  *Update 2026-10-16: **declined**, and the decision recorded here rather than left open, because an
  entry that reads as pending work has its own cost -- this file twice sent a reader to recommend
  already-finished items on the day this was decided.*

  ***The argument that settled it is one this entry had as a sub-point rather than the whole case:
  the proposal's value and its structural cost are the same forms.*** The pitch is that all four
  combinations become expressible. But the nesting-safe subset is only `[a..b]` and `(a..b)` --
  inclusive-both and exclusive-both -- while the ends anyone actually reaches for are exclusive
  (`a..b`, which exists) and **inclusive-end**, i.e. `a..b]`, a *mixed* form. So the useful half of
  the feature is exactly the half that breaks the delimiter-matching invariant the formatter's
  continuation indent (F72), the TextMate grammar and every editor's bracket matcher rely on. Drop
  the mixed forms to keep that invariant and nothing of value is left to ship.

  **Declining also keeps `[a..b]` in reserve, which is worth more than this feature.** This entry
  listed "slicing landing as `xs[a..b]`" as a *reopen* trigger; it cuts the other way. `xs[0..2]` is
  a clean `expected ']', found '..'` today (verified), slicing exists only as the function
  `slice<T>(a, start, end)`, and subscript slicing is a far more commonly wanted feature -- Python,
  Rust and Go all have it. Spending `[a..b]` on a range *value* forecloses it permanently, so
  keeping it free is a decision in its own right and not a deferral.

  **`..=` is declined too, which is the part that needed a reason rather than a cost.** It is cheap,
  nesting-safe, leaves `[` and `(` alone, matches a real latent habit (1 of 50 cold attempts reached
  for it) and the existing diagnostic already names the concept. Against that: it puts **two
  spellings on the single most common construct in the language**, and the count below is now **260**
  `for ... in a..b` sites in `lib`/`tests`/`examples` (277 counting docs) -- grown from the 235
  recorded here on 2026-10-12. Every counted loop would become a choice between `0..n + 1` and
  `0..=n`, and a model choosing between near-synonyms is the failure mode the AI-first goal exists to
  avoid. No cold run has ever *needed* an inclusive range; the measured failure was four runs
  **avoiding** ranges to dodge an off-by-one, which is hesitation, and hesitation is what F93's
  diagnostics already address. The current message is doing that work well: "a range is written
  `a..b` and stops before `b`, so an inclusive end is written `a..b + 1`".

  **The reopen trigger, tightened.** The one signal that would reopen either half is a run that
  writes an off-by-one `+ 1` and **gets it wrong** -- a defect, not a hesitation. The avoidance
  already measured is explicitly *not* that signal, and should not be read as evidence the feature is
  needed; mistaking the two is what would reopen this on no new information. Slicing landing as
  `xs[a..b]` no longer reopens anything: it is now the reason `[a..b]` stays unspent. The idea is that each end's bracket states its own
  inclusivity, so a reader never has to know a convention: `[` and `]` include the endpoint, `(` and
  `)` exclude it, and all four combinations are expressible. That is a real argument and the same one
  that favours Kotlin's `until` over a bare `..`: self-documenting beats diagnosed.

  **What exists, and why this is not urgent.** `a..b` is exclusive of `b` and always has been;
  `docs/syntax/` has said so in those words throughout, and F93 put the semantics into three
  diagnostics -- including the stray-semicolon message that 30 of 50 cold attempts hit, which is the
  widest-reach sentence in the compiler. `..=` is refused with a message naming the exclusive end.
  Critically, **the measured failure was confusion, not absence**: across rounds 1-4 and the variance
  wave, four runs avoided ranges rather than risk an off-by-one, and *not one ever needed an inclusive
  range*. So `[a..b)` would respell something that works, and `[a..b]` would add a form nobody has yet
  reached for.

  **What the design pass has to weigh, all of it measured rather than supposed:**
  - **`[` already means two things** -- array literal and index -- and `[a..b]` collides with a third
    the language should keep in reserve: `xs[0..2]` is a clean `expected ']', found '..'` today, and
    is the natural spelling for a slice, which Ziman has only as the function `slice<T>`. Spending
    `[a..b]` on a range *value* forecloses that.
  - **`(` already means grouping and an argument list**, so `(a..b)` reads as a parenthesised
    expression to both a parser and a person.
  - **`[a..b)` and `(a..b]` do not nest.** Delimiter matching is an invariant the formatter's
    continuation indent (F72) and the TextMate grammar both rely on, and every editor's bracket
    matcher assumes it.
  - **The generation cost is the largest of any syntax change yet proposed.** No cold run has ever
    written interval notation; agents write `0..n`, and `..=` appeared in 1 of 50 attempts. Ranges sit
    in the most common construct in the language -- **235 `for ... in a..b` sites** across `lib/`, the
    examples, the benchmark references, the tests and the reference backend -- so this would make the
    training reflex fail on essentially every counted loop. Compare the `variadic` keyword decision,
    whose blast radius is a handful of declarations.
  - **Migration** is those 235 sites plus the grammar, the formatter, the pinned-message fixtures and
    `docs/syntax/`.

  **Cheaper answers if inclusivity turns out to be the real want**, in order of cost: accept `..=`
  (matches the one measured habit, matches Rust, and F93's message already names the concept, so the
  change is small), or a word -- `a to b` -- if self-documentation is the goal. Both leave `[` and `(`
  alone.

  **Reopen on any of:** a run that writes an off-by-one `+ 1` and gets it wrong; a run that reaches for
  an inclusive end and cannot express it; or slicing landing as `xs[a..b]`, which would settle
  `[a..b]`'s fate by taking the syntax. Absent one of those, the F93 diagnostics are the cheaper half
  of this idea and are already shipped.

- **Locale support: a `Locale` and the functions that would have to take one.** Raised 2026-10-23
  ("we may need Locale support"), which is the trigger F44 had been waiting for -- that entry
  sequenced indexed placeholders "behind a decision about localisation that nobody has asked for".
  Somebody has now asked.

  **Nothing is designed yet. What this entry is for is the surface**, because the cost of locale
  support is not one module -- it is that a dozen existing functions currently answer a
  locale-independent question and would have to grow a locale-dependent one beside it. Measured on
  2026-10-23 rather than assumed:

  > **(Closed 2026-10-23.)** `to_upper(s, l)` / `to_lower(s, l)`, with the one-argument form using
  > `locale.default()`. `towupper_l` gives *simple* case mapping, so `ß` still does not become `SS`
  > -- one code point in, one out is the POSIX interface's shape. The Turkish dotted/dotless `i`,
  > which is the mapping that actually bites, is correct.

  **1. Case mapping is ASCII-only, and silently so.** `to_upper("héllo straße İ")` is
  `HéLLO STRAßE İ` -- the `é` unchanged, the `ß` unchanged where German wants `SS`, the dotted
  capital `İ` unchanged where Turkish wants a dotless lowercase. `docs/syntax/` says "ASCII only" and
  the implementation agrees; what is missing is any way to ask for more.

  > **(Closed 2026-10-23.)** `equals_ignore_case(a, b, l)`. The trap this item named is gone:
  > `equals_ignore_case("héllo", "HÉLLO", en)` is true. The ASCII form is unchanged and is what
  > protocol code wants -- 58 sites in `lib/` now pass `locale.root()` to say so.

  **2. `equals_ignore_case` is ASCII-only too, which is the sharpest trap of the four.**
  `equals_ignore_case("hello", "HELLO")` is true and `equals_ignore_case("héllo", "HÉLLO")` is
  **false**. A caller testing user input against a constant gets the right answer for English and
  the wrong one for everything else, with no error and no hint.

  > **(Closed 2026-10-23, and this item's own prediction held exactly.)** `compare(a, b, l)` over
  > `strcoll_l`, a *separate* comparator reached with `sort_with` -- it could not replace
  > `compare<T: Ord>`, for the reason given below. German sorts `ärger` beside `apple` and Swedish
  > after `Zulu`, measured. `compare` is a builtin, so its arm in `src/typecheck.c` runs before an
  > overload set would be consulted; the three-argument form is checked there and dispatched in
  > `src/interpreter.c`.

  **3. String order is code points, which is nobody's collation.**
  `sort(["zebra", "ärger", "apple", "Zulu"])` is `[Zulu, apple, zebra, ärger]`: capitals before
  lowercase because ASCII puts them there, and `ärger` *after* `zebra` because U+00E4 > U+007A.
  German collation puts `ärger` next to `apple`, Swedish puts it after `z` -- so the current answer
  is not a default that happens to suit one locale, it is one that suits none. This is the hardest
  piece: `compare<T: Ord>` is a single total order used by `sort`, `Map` and `Set`, and a
  locale-aware order cannot replace it (a `Map`'s key order must not depend on a locale) -- it has
  to be a *separate* comparator passed to `sort_with`.

  > **(Partly closed 2026-10-23.)** `number.number_format_for(l)` reads the host's decimal point
  > and grouping separator, so `1234567.89` renders `1.234.567,89` under `tr_TR` and
  > `1 234 567,89` under `sv_SE` (and ungrouped under `ar_SA`, which groups with nothing).
  > **`str(x)` is deliberately not locale-aware and will not become so** -- it is what a wire
  > format, a SQL value and a PDF object number are written with. What is still open is the parsing
  > half: `parse_float` accepts neither `1234,56` nor `1,234.56`, which is right for a wire format
  > and wrong for a form field, and a locale-aware `parse` has no home yet.

  **4. Numbers are the C locale.** `str(1234567.89)` is `1234567.89`: no grouping separator, and no
  way to ask for one. `parse_float` rejects both `1234,56` and `1,234.56`, which is correct for a
  wire format and wrong for a form field.

  **5. Dates have no names at all.** `time.zm`'s own header: "there is no locale, month name or
  weekday support". `datetime_format_of` takes a pattern, so the hole is shaped like a pattern
  letter that does not exist rather than a missing module.

  > **(Half closed on 2026-10-23, then closed the same day.)** `MMM`/`MMMM` landed first in English
  > only, which is what this item's shape predicted -- filling the pattern letters was the easy
  > half. Then `EEE`/`EEEE` joined them and all four read the host's locale data through
  > `locale.month_name` / `locale.day_name`, so `EEEE, dd MMMM yyyy` renders
  > `Cuma, 23 Ekim 2026` under `tr_TR` and `金曜日, 23 10月 2026` under `ja_JP`. A compiled pattern
  > stays locale-free and the locale goes to the call that renders.
  >
  > **What this item did not cover and is still absent:** era, quarter and week-of-year fields,
  > 12-hour time (`hh`/`a`), and a localised *ordering* of the fields -- the formatter renders the
  > pattern it is given, so a caller still writes `dd MMMM yyyy` or `MMMM dd, yyyy` themselves.
  > That last one is the part a real CLDR-backed formatter would answer and POSIX cannot.

  **6. No message catalogue, and `format_dynamic` -- the function F44 kept as the landing site for
  one -- was deleted on 2026-10-22** as unused. That is the one piece this entry makes *more*
  expensive than F44 assumed: a catalogue needs a runtime template, `format` refuses a computed
  first argument by design, and the function that bridged that is gone. It was 40 lines over a
  scanner `format` and `printf` still share, so restoring it is cheap -- but it has to be restored,
  and indexed placeholders (`{0}`, `{1}`), which F44 costed, would go into it.

  **Two Unicode questions sit underneath all of this and are not locale-dependent, so they could be
  settled first and separately.** `len` counts code points as of 2026-10-23, not graphemes: a flag
  or a skin-tone emoji counts as several. And nothing normalises -- precomposed `é` (U+00E9) has
  `len` 1, decomposed `e` + U+0301 has `len` 2, and `==` says they differ, so two strings that
  render identically are unequal. Any locale story that compares user input needs an answer to the
  second one; a `Locale` does not help with it.

  **The shape to decide, not decided here.** Whether a `Locale` is an explicit argument
  (`to_upper(s, locale)`), an ambient value, or a module (`locale.to_upper`); whether the data is
  bundled (CLDR is tens of megabytes and this interpreter embeds its whole standard library as C
  string constants) or read from the host (ICU, or the platform's own); and whether the first slice is just **case
  mapping plus month and weekday names**, which covers most of what a backend actually renders, with
  collation and number formatting deferred.

  > **Correction (2026-10-23, same day):** this entry first said a host library would be "a
  > dependency this project has otherwise avoided". That is false and was not checked before being
  > written. `make/c.mk` links `-lsqlite3 -lssl -lcrypto -lcrypt -lz` unconditionally, so there are
  > already four, none of them optional; ICU would be a fifth of a kind already accepted rather than
  > a first of a new kind. On this machine it is present (icu-uc 77.1) with 31.9 MB of data in
  > `libicudata`, which is the real figure to weigh -- against bundling, where the comparison is to
  > the interpreter embedding its whole standard library as C string constants today.

  **Not started. The next step is a decision about data, not code**: bundled, host-provided, or a
  deliberately tiny table for a handful of languages. Everything above is reachable from either
  answer, and the measurement is here so the next pass does not have to re-take it.
