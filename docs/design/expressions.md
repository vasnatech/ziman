# An expression language, and what it does to the template module

Status: **design, not built.** Written because two requests arrived together and turned out to be
one: a SpEL-like expression module for configuration rules and dynamic output, and expressions
inside `lib/core/template.zm`. They are designed here together, and built as **two
modules with one direction between them**: `lib/core/expr.zm` is the expression language and
knows nothing about templates; `lib/core/template.zm` **uses** it. Designing them in one document
is what keeps that boundary honest -- decided apart, the template module grows its own small
expression language and the codebase ends up with two.

**How to read this.** §§1-4 are the constraint and the observation that make the whole thing
small. §§5-9 are the language, which stands alone. §10 is the module boundary. §§11-15 are what
it does to `template.zm`, including the one real cost. §16 is the staging and §17 the decisions
still open.

**The name.** `expr`, after what it is, rather than `spel` after the acronym of the Spring
feature it resembles -- `template.zm` is not named `freemarker`. Trivially changed if the
resemblance is worth naming.

## 1. What is being asked for

Two uses, named by the person asking:

* **Configuration and validation rules.** A constraint written as text -- in a properties file, an
  attribute, a database row -- evaluated against a request or a config object. `age >= 18`,
  `insurer_name != null && coverage_types.length > 0`.
* **Dynamic output.** Text with values interpolated, rendered at runtime: messages, notifications,
  exported documents.

And one relationship: **`template.zm` uses the expression module**, rather than containing it or
sitting beside it.

Putting expressions into the template module at all was argued against once -- it is a
compile-time code generator whose no-expressions rule was deliberate -- and the argument was
overruled. §14 says what the rule was actually buying and what replaces it, because "the rule is
gone" is not the same as "the reason for the rule is gone".

## 2. The constraint that decides everything: Ziman has no reflection

There is no way to ask a value for a field by name at run time. Not a missing builtin -- the
language has no dynamic field access at all, and adding one would mean carrying field-name
metadata on every struct at run time, which is a much larger decision than an expression module.

So a runtime interpreter **cannot** navigate `user.address.city` over a `User` value. Whatever the
expression language walks, it is not a Ziman struct.

## 3. What it walks instead: `Json`

`Json` is already a complete dynamic value tree -- null, bool, number, string, array, object -- and
as of 2026-10-28 every struct that writes `#[derive(Json)]` has a `<name>_to_json`. So the context
is a `Json` and nothing new is needed:

```ziman
val ctx = user_to_json(user)
val rule = try expr.parse("address.city == 'Ankara' && age >= 18")
if try expr.eval_bool(rule, ctx) { .. }
```

No new value type, no reflection, no compiler work. The feature that makes this possible is the
derive port that just finished; before it, `to_json` existed but was written by `src/derive.c`,
which is the same thing from the caller's side but would have made this module depend on the C.

**What it costs, stated here rather than discovered later**: converting a struct to `Json`
allocates the whole tree. That is right for validating one request or reading config at startup,
and wrong in a hot loop. §14 repeats it among the limits.

## 4. `template.zm` already has an expression language

This is the observation that makes the integration small, and it is worth stating before any
syntax.

Today's template module computes nothing -- except that it does, twice:

* **`@{r.variant}` is a path expression.** A hole is `@{name}` or `@{binder.cell}`: a one- or
  two-segment path resolved against bindings and loop frames. That is `a` and `a.b` with the
  grammar stopping there.
* **`@choose tag` / `@when simple` is an equality comparison.** It reads a bound value and
  compares it to a literal. The design (§4 of templates.md) describes this as "reads a bound value
  and nothing else", which is accurate about the *restriction* and hides that a comparison is
  what it is.

So an expression language does not introduce a concept here. It **generalises two constructs that
already exist** and currently stop at one segment and one operator. That reframing is what makes
the migration mechanical rather than a rewrite.

## 5. The value model

Expressions evaluate over `Json`, and produce `Json`. One type in, one type out, no second ladder
of expression-only values.

| Json          | in an expression                               |
| ------------- | ---------------------------------------------- |
| `JNull`       | `null`                                          |
| `JBool`       | the only thing a condition accepts              |
| `JNumber`     | int and float are one kind, as in JSON          |
| `JString`     | compared and concatenated, never coerced        |
| `JArray`      | indexed, `.length`, `in`                        |
| `JObject`     | navigated by key                                |

**No truthiness.** A condition must be a `JBool`. `@if name` where `name` is a string is an
error, not "non-empty means true". Every language that took the other road spends the next decade
explaining which values are falsy, and a config rule that silently means something else is worse
than one that refuses.

**No implicit coercion** except between the two number forms. `"5" == 5` is `false`, not an error:
comparing different kinds is a legitimate question with the answer `false`. Ordering across kinds
(`"a" < 5`) *is* an error, because there is no answer.

## 6. The grammar

Small on purpose, and listed in full so that what is absent is visible.

```
expr      := ternary
ternary   := or ( '?' expr ':' expr )?            -- a ? b : c
or        := and ( '||' and )*
and       := equality ( '&&' equality )*
equality  := compare ( ('==' | '!=') compare )*
compare   := additive ( ('<' | '<=' | '>' | '>=' | 'in') additive )?
additive  := multiply ( ('+' | '-') multiply )*
multiply  := unary ( ('*' | '/' | '%') unary )*
unary     := ('!' | '-')? postfix
postfix   := primary ( '.' name | '?.' name | '[' expr ']' )*
primary   := literal | name | '(' expr ')' | call
literal   := string | number | 'true' | 'false' | 'null'
call      := name '(' ( expr ( ',' expr )* )? ')'
```

* **String literals take single quotes**, as SpEL does, because these live inside Ziman string
  literals and inside template sheets. `'Ankara'` needs no escaping where `\"Ankara\"` does.
  Double quotes are accepted too; single is the documented form.
* **`?.` is safe navigation**: `address?.city` is `null` when `address` is `null`, rather than an
  error. §8 is why this is a separate operator and not the default.
* **`+` concatenates** when either side is a string, and adds when both are numbers. This is the
  one piece of overloading, and it earns its place in a language whose main job is building text.
* **`in` tests membership** in an array or a key in an object.

### Function calls

A **fixed set**, not user-extensible in v1:

```
length(x)  empty(x)  lower(s)  upper(s)  trim(s)
starts_with(s, p)  ends_with(s, p)  contains(s, p)
matches(s, pattern)          -- regex, via lib/core/regex.zm
str(x)  int(x)  float(x)     -- explicit conversion, since there is no implicit one
```

User-supplied functions are deliberately deferred. They need a calling convention from a `Json`
value list into a Ziman function and a story for errors, and nothing in the two driving uses needs
them. Adding them later is additive; taking them back is not.

### What is deliberately absent

No assignment. No method calls on arbitrary objects (there are no objects -- there is `Json`). No
bean references (`@foo`), no type references (`T(java.lang.Math)`), no constructors. No collection
projection or selection (SpEL's `.![]` / `.?[]`) -- the two uses do not need them and each is a
small language of its own.

## 7. The API

```ziman
pub struct Expr { .. }                                     -- opaque, parsed once

pub fn parse(source: string) -> Result<Expr, string>
pub fn must_parse(source: string) -> Expr

pub fn eval(e: Expr, ctx: Json) -> Result<Json, string>
pub fn eval_bool(e: Expr, ctx: Json) -> Result<bool, string>
pub fn eval_string(e: Expr, ctx: Json) -> Result<string, string>
```

`parse`/`must_parse` mirrors `template.parse`/`must_parse`, for the same reason: an expression in a
`val` at module scope is parsed once at load, and a malformed one is a load-time failure rather
than a surprise on the first request. An expression that arrives at run time -- from a config file
or a database row -- uses `parse` and handles the `Err`.

The three `eval_*` entry points exist because a caller almost always knows which it wants, and
`eval` returning `Json` would otherwise put an unwrap at every call site. `eval_bool` on a
non-bool is an error, per §5.

## 8. Errors, and when they surface

Parse errors are parse-time and carry a column. Everything else is eval-time, which is the honest
cost of the model the person chose: **a rule is data, so a rule can be wrong, and nothing finds
that out until it runs.**

The one decision with real consequences is **a path that does not exist**.

* `a.b` where `a` has no `b` is an **error**, not `null`.
* `a?.b` is `null`.

This is the opposite of what a templating language usually does, and the reason is the use case:
these are *validation rules*. A rule that reads `user.emial == null` and quietly passes because the
key is missing is a rule that does nothing while appearing to do something. Making absence
explicit costs one character where it is intended and catches the typo everywhere else.

An error message names the path, the segment that failed, and the keys the object did have --
the same shape as the template module's refusals.

## 9. Why not reuse this for `eql`

`eql` predicates look like expressions and are not: they compile to SQL, where `a.b` is a join and
`contains` is a `LIKE`, and the value model is the database's, not JSON's. Sharing a parser would
mean one grammar with two incompatible halves of its semantics disabled depending on the caller.
Noted here so the question is answered once. If `eql` ever wants richer predicates, the thing to
share is the *parser*, as a second backend -- not this module.

## 10. The module boundary

`expr` is a module in its own right and does not know templates exist. `template` depends on it.
The dependency runs one way and there is no interface between them beyond the public API in §7:

```
json  <--  expr  <--  template  <--  derive_ord / derive_row / derive_properties / derive_json
```

**What this buys, concretely.** `expr` is usable without a template -- which is the whole of the
config-and-validation use, where there is no template at all. And `template` holds no grammar: a
directive's condition and a hole's contents are *expression source*, handed to `expr.parse` and
kept as an `Expr`.

**When each half runs.** A sheet is parsed once (templates.md §6), and the expressions inside it
are parsed then too, by the same pass -- so a malformed expression in a generator's sheet is a
module-load failure, exactly like a malformed directive is today. Rendering evaluates the
already-parsed `Expr` against the context. `template` never parses an expression at render time
and never evaluates one itself.

**The one thing to watch** is that `template` must not acquire a private operator, a private
coercion or a private built-in function "just for sheets". If a sheet needs something the
language does not have, it goes in §6 and both halves get it. That is the rule the shared module
exists to make cheap, and the reason this document covers both.

## 11. `template.zm`: the data model becomes `Json`

Today a render takes `[Binding]`, built with `text` and `rows`, with `Row` holding cells. Per §4,
that is a degenerate `Json`:

| today                        | as `Json`                           |
| ---------------------------- | ----------------------------------- |
| `template.text("n", "v")`    | a `JString` at key `n`              |
| `template.rows("rs", [..])`  | a `JArray` of `JObject` at key `rs` |
| `Row`                        | one `JObject`                       |

So the binding set becomes one `Json` object and the three constructors become sugar over building
it. **This is a simplification of the data model, not an addition to it** -- one type where there
were three, and the §5 note in templates.md about why it could not be a `Map` is resolved by using
the recursive type that was missing from that comparison.

```ziman
pub fn render(sheet: Sheet, name: string, ctx: Json) -> Result<string, string>
```

What is given up: templates.md §5's guarantee that **every value a hole can hold is already a
`string`**, which made "how is a number formatted" a question that could not be asked. It can be
asked now, and the answer is: a hole renders a `JString` as itself, a number the way `str` does, a
bool as `true`/`false`, and **refuses an array, an object or null** -- the last because a hole
holding `null` is nearly always a mistake, and `@{x ?: ''}` says otherwise in four characters.

## 12. The directives after the change

`@{...}` holds an expression rather than a path -- `expr` source, parsed with the sheet and
evaluated at render, per §10. `@for` iterates an array. And the dispatch construct changes:

```
@if <expr>
@elif <expr>
@else
@end
```

**`@choose`/`@when`/`@otherwise` is removed**, because with expressions it is exactly
`@if tag == 'x'` / `@elif tag == 'y'` written in more words and one more concept. This is
templates.md §15's own argument -- *"carrying two dispatch constructs when one is unused is how a
language accretes"* -- applied in the direction the facts now point.

It is worth being plain that this reverses §15, which removed `@if` and kept `@choose`. §15 was
right on its evidence: `@if` with no expression language could only read a boolean someone else
computed, which is a worse `@choose`. The thing that changed is not the judgement but the premise.

`@yield` is unaffected. `@@` is unaffected.

## 13. A second entry point, for runtime text

A sheet with `@template` sections is a code generator's shape: several named outputs from one
document. Runtime templating usually wants one anonymous template and a context:

```ziman
pub fn render_text(source: string, ctx: Json) -> Result<string, string>
```

Same parser, same directives, same `expr`, no `@template` wrapper. This is what the "dynamic output" use calls,
and keeping it a separate entry point means a notification template cannot accidentally depend on
the sheet machinery.

## 14. The cost: generator logic stops being typechecked, unless a rule holds

This is the real objection to putting expressions in `template.zm`, and it does not go away by
being overruled -- it changes from a reason not to, into a thing to manage.

templates.md §4's argument was: **logic in a template is invisible to the type checker, because it
lives in a string.** That is as true now as it was. The five derive generators currently compute in
Ziman -- `if i < last { "," } else { "" }` -- where the typechecker sees every branch, and the
fixtures pin the result byte for byte.

The temptation after this change is to move that into the sheet. Some of it *should* move and some
must not, and the line is:

> **An expression may read the data. It may not encode the generator's decisions.**

`@{f.comma}`, where the generator computed `","` or `""`, becomes `@if !f.last` -- the sheet gets
more readable and nothing meaningful leaves the typechecker's sight, because `f.last` is a fact
about the data either way. But `derive_row`'s choice of which conversion a field type needs, or
`derive_json`'s recursion through `Option<[string]>`, stays in Ziman. Those are programs, and a
program in a string is the thing §4 refused.

The fixtures are what make this enforceable rather than aspirational: every generator's output is
pinned, so a migration that changes behaviour fails loudly, and a sheet that grows a program will
show up as a sheet nobody can read.

## 15. Limits, up front

* **Converting a struct to `Json` allocates the whole tree.** Right for a request or a config;
  wrong in a loop.
* **Expressions are not typechecked.** A rule is data; a wrong rule is found when it runs.
* **No user-defined functions in v1.** The fixed set in §6 is the whole vocabulary.
* **Keys are the JSON keys.** `derive(Json)` writes camelCase, so a rule reads `user.middleName`
  unless the snake_case option in §15 is taken.
* **One grammar and one evaluator**, in `expr`. `template` calls it and implements none of it,
  so there is nothing to drift (§10). The limit this leaves is the opposite one: a sheet cannot
  have an operator the language lacks without the language gaining it.

## 16. Staging

Each step is useful alone and provable before the next:

1. **`lib/core/expr.zm` over `Json`**: parser, evaluator, the fixed function set, fixtures for
   every operator and every refusal. Nothing depends on it yet, and it is independently useful --
   the config-and-validation use needs no template and can be built on step 1 alone.
2. **snake_case keys.** The person asked for the `json` module to support snake_case creation with
   a parameter. Two places it could live: a `#[derive(Json)]` option that generates a second
   function, or a runtime `json.snake_keys(j)` rekey. **The rekey is the recommendation** -- it
   works on any `Json`, including one parsed from the wire, and it does not touch a generator
   whose output is pinned by fixtures.
3. **`template.zm` takes a `Json` context** (§11), with `text`/`rows`/`row` kept as sugar. The five
   generators migrate mechanically and the fixtures prove the output is unchanged. No dependency
   on `expr` yet: this step is the data model alone.
4. **`template` starts using `expr`** -- holes and conditions become expression source, parsed
   with the sheet -- and **`@if`/`@elif`/`@else` replace `@choose`** (§12). The generators migrate
   again, against the same fixtures. This is the step that creates the dependency in §10, and it
   is deliberately last among the template changes: everything before it is provable without the
   expression language existing.
5. **`render_text`** (§12), and whatever the first real notification template needs.

Steps 3 and 4 are each a migration of about 1,100 lines of generator Ziman, done against
byte-identical fixtures. That is the honest price of putting expressions in the template module,
and it is payable precisely because those fixtures exist.

## 17. Open decisions

* **Does `@if` take the same expression grammar as `@{...}`, or a boolean-only subset?** A
  subset would mean `template` passing a mode flag into `expr.parse`, which is the first crack in
  §10's "`template` holds no grammar". Same
  grammar is simpler to specify; a subset would catch `@if user.name` at parse time rather than at
  render. Leaning: same grammar, refuse at render, consistent with §5.
* **Is `expr` the right name** (see the header), and does it belong in `lib/core` beside
  `json` and `template`, or is it large enough to be its own thing?
* **Should `parse` be exposed on `template.zm` at all, or only `must_parse`?** A sheet is a
  literal in a generator; a runtime template is not.
* **Is `matches` worth the dependency on `regex.zm`?** It is the one function in §6 that pulls in
  another module.
* **Where do rules live for the validation use** -- a properties file read by `properties.zm`, an
  attribute on a struct, or a database row? This changes nothing about the language and everything
  about the ergonomics, and it is the next thing to decide.
