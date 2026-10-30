# Validation from annotations

Status: **built and closed, 2026-10-30.** All five staging steps of §11 shipped (F246), plus
`#[valid]` cascading (F248) and `#[pattern(regex:)]` (F251), and every request type in the North
Star is converted. §10's four questions are decided (and a fifth, `message:`, answered while
building), and §6's deferral turned out to rest on a corpus claim that was false -- see both.

Asked for on 2026-10-29: *"check jakarta.validation package in Java. lets build a validation lib
using annotations. lets design it first."*

This is the design. Nothing is built yet. **It also answers an open decision**: `expressions.md`
§17 asks where validation rules live — a properties file, an attribute on a struct, or a database
row — and a validation library built on annotations *is* the attribute answer. §9 says what that
costs the `expr` plan.

## 1. What Jakarta Validation actually has

Checked rather than remembered (`jakarta.validation.constraints`, Jakarta EE 10/11 javadoc):
**22 constraint annotations** — `AssertFalse`, `AssertTrue`, `DecimalMax`, `DecimalMin`, `Digits`,
`Email`, `Future`, `FutureOrPresent`, `Max`, `Min`, `Negative`, `NegativeOrZero`, `NotBlank`,
`NotEmpty`, `NotNull`, `Null`, `Past`, `PastOrPresent`, `Pattern`, `Positive`, `PositiveOrZero`,
`Size` — each with a nested `.List` for repeating one on the same element, and `Pattern.Flag` for
regex flags. Every constraint carries `message`, `groups` and `payload`. Around them: `@Valid` for
cascading, `ConstraintViolation` with a property path, groups for create-versus-update, class-level
constraints for cross-field rules, custom constraints through `ConstraintValidator`, and message
interpolation from a `ValidationMessages` bundle.

That is the vocabulary. It is not the requirement.

## 2. What the requirement actually is

The North Star hand-writes its validation today: **106 rule uses across 23 service files**. Counted:

| what it does | uses | Jakarta's name for it |
| --- | --- | --- |
| a string must be one of a word list | 19 | **none** |
| an `Option<string>` must be one of a word list when present | 31 | **none** |
| every element of a list must be in a word list | 2 | **none** (container element) |
| required / not blank | 26 + 2 | `@NotBlank` |
| `size must be between 8 and 2147483647` | 4 | `@Size(min = 8)` |
| `must be a well-formed email address` | 5 | `@Email` |
| `must be greater than 0` | 3 | `@Positive` |
| `must be ≥ 1` / `≤ 32` | 2 | `@Min` / `@Max` |
| `must not be empty` (a list) | 1 | `@NotEmpty` |
| `endDate must not be before startDate` | 1 | a class-level constraint |
| `Cannot reset password for OAuth users` | 1 | **not validation** |

Three things fall out of that table and they drive the whole design:

1. **The most common rule by far — 52 of 106 — is "value ∈ word list", and Jakarta has no
   constraint for it.** In Java you reach for an enum type, a `@Pattern` with an alternation, or a
   custom constraint. Ours will be a first-class attribute.
2. **Jakarta's set is mostly unused here.** Nothing needs `@AssertTrue`, `@Null`, `@Digits`,
   `@Negative*`, `@Past`/`@Future`, `@DecimalMin/Max`. Seven attributes cover **104 of 106**.
3. **The last two are not constraints on a field at all** — one is cross-field, one is a business
   rule wearing a field error's clothes. §7 and §8.

## 3. The hard constraint: the messages are not ours to choose

Both test suites assert the message text byte for byte:

```python
body["errors"]["name"]  == "must not be blank"                      # user_profile_test.py:96
body["errors"]["email"] == "must be a well-formed email address"    # auth_test.py:143
```

and the Ziman suite asserts the same two. Those strings are **Hibernate Validator's defaults**,
reproduced on purpose because this backend is a port of a Spring one. So:

* the default message for a constraint is Hibernate's default message, where the two coincide;
* a `message:` argument overrides it per field, as Jakarta's does;
* **messages are not localised.** The backend has `messages/{en,es,fr,tr}.json`, and no validation
  text is in them. v1 keeps that: one English string per constraint, interpolated. Localisation
  would mean a bundle lookup per violation and a key naming scheme, and nothing asks for it.

One artefact is worth a decision rather than an inheritance: `@Size(min = 8)` with no maximum
prints **"size must be between 8 and 2147483647"**, because Java's annotation default for `max` is
`Integer.MAX_VALUE`. Ziman has no such default to leak. That message is **not** asserted anywhere
(only the two above are), so §10 asks whether to reproduce it or say "size must be at least 8".

## 4. The wire shape constrains the model

`controller.zm` answers `{"timestamp", "status", "errors": {"<field>": "<message>"}}` — a **map**,
one message per field. Three consequences:

* a second violation on one field is **lost**, so the model must say which one survives (first,
  by declaration order — the same rule `accumulate` has today);
* the key is the **JSON name** of the field (`newPassword`), not the Ziman name (`new_password`),
  so a violation carries the wire name, computed exactly as `derive(Json)` computes a key —
  `meta.camel_case`. Getting this from a different place would be the same class of bug as the
  column override (`column-override.md` §2);
* `ConstraintViolation`'s property path is overkill: a flat field name is the whole of what the
  wire can carry. Cascading (§6) is where that stops being true.

## 5. The shape of it

```ziman
#[derive(Validate)]
pub struct BodyMeasurementRequest {
    #[one_of(values: type_words)]
    type: string
    #[one_of(values: interpretation_words)]
    interpretation: Option<string>
    #[not_blank]
    #[size(min: 1, max: 64)]
    unit: string
    #[positive]
    measurement_value: float
    #[email]
    notify: Option<string>
}
```

generating one function, in the shape every other generator here answers:

```ziman
pub fn body_measurement_request_violations(v: BodyMeasurementRequest) -> [validate.Violation]
```

Six decisions in that sketch:

**(a) `Option<T>` means "absent is allowed", so there is one attribute per rule rather than two.**
This is the single biggest simplification over the hand-written code, which has `not_one_of` *and*
`optional_not_one_of` (19 + 31 uses) saying the same thing about different types. A constraint on
an `Option<T>` field checks the value when it is present and passes when it is not — which is
exactly Jakarta's semantics, where `@Size` on a null is valid and only `@NotNull` rejects null. And
it is the rule `#[column]` and the derived-query parameters already use: the type decides
optionality, the attribute decides the rule.

It also means **`@NotNull` has no equivalent and needs none**: a required field is a non-`Option`
field, and `derive(Json)`'s `from_json` already refuses a request whose required key is missing.
`#[not_blank]` is about emptiness, not absence.

**(b) `#[one_of(values: <name>)]` takes an identifier, not a list of literals.** The word lists are
already `val type_words = [..]` in the module — eleven words in one case — and they are shared with
other code. Repeating them inside an attribute would be a second place for them to be wrong. An
attribute argument that is a *name the generated code emits* has a precedent in this language:
`#[test(cases: NAME)]`. The generator writes `contains(type_words, v.type)` and never reads the
list itself.

**(c) The violation type belongs to the library and the application maps it.** `validate.Violation
{ field: string, message: string }`. The backend's `FieldError` and `ServiceError.Validation` are
application types in `service/service.zm`; a library that answered them would have the dependency
upside down. The backend maps `[Violation]` to its own error in one line.

**(d) One function per struct, named for the struct, answering a list.** Not a `Result`: a list is
what the caller accumulates and the wire takes, and an empty list is the success case without an
`Ok(unit)` to unwrap. `violations` rather than `validate` because the function answers data about a
value rather than doing something to it.

**(e) The constraint set is the evidence, not the vocabulary.** v1 is seven attributes:
`#[one_of(values:)]`, `#[not_blank]`, `#[not_empty]`, `#[size(min:, max:)]`, `#[email]`,
`#[min(..)]`/`#[max(..)]`, `#[positive]`. Named after Jakarta's where they coincide, so a reader who
knows Bean Validation reads them without a table. `#[pattern(regex:)]` is the eighth and arrives
when something needs it — `regex` is already a bundled module, so it costs a line.

> **Arrived 2026-10-30, and the way it arrived is the point.** By the time something needed it,
> `src/parser.c`'s `FIELD_ATTRS` was gone (F250): adding it was **one line in
> `derive_validate.zm`'s own `attributes()` and one case in its `rule_for`**, no compiler change at
> all, and the "a struct field takes one of: …" listing grew by itself. First evidence that a
> project could add an attribute of its own, which is what user-generators.md §5a was written for.
> The argument is **named** `regex:`, for `#[column(name:)]`'s reason — a lone positional value
> reads fine until a second argument exists, and `message:` is that second argument already.

**(f) Attribute arguments grow by one kind.** `#[column(name: "..")]` established named arguments
with string values (`column-override.md`, `meta.md` §13). This needs two more: an **int** value
(`size(min: 8)`) and an **identifier** value (`one_of(values: type_words)`). Both fit
`meta.AttrArg`'s existing `value: string` — the compiler hands over the text and the generator
decides what it means, which is what `Decl.arg` already does for `#[derive(Entity("medications"))]`
— so this is a parser change and not a `meta` change.

## 6. Cascading

Jakarta's `@Valid` on a field validates the nested object and prefixes the property path. Ours:
`#[valid]` on a field whose type also derives `Validate`, producing a violation whose field is the
dotted path — `ingredients[0].name`.

**Explicit rather than automatic**, as in Jakarta: a nested struct that happens to derive `Validate`
being validated silently is the kind of thing that surprises a reader of the outer struct. And the
flat wire map (§4) means a dotted path becomes a key, which is a *behaviour change the client sees*
— so the first use of `#[valid]` should be a deliberate one.

**Built 2026-10-30.** This section said "designed and not built — the corpus has no cascading case
today", and that was simply wrong: `recipes` validates every ingredient of a `RecipeRequest` and
reports `ingredients[0].name`. It was hand-written, which is why a survey of *attributes* did not
see it, and the user found it by reading the model: `RecipeIngredientRequest` is used by
`RecipeRequest`, `RecipeRequest` is validated, so why not the ingredient? **A deferral justified by
"no case exists" is only as good as the search for one.**

Four shapes, because the generated code differs: `[T]` and `Option<[T]>` walk with an index,
`T` and `Option<T>` do not, and an absent `Option` has nothing to walk. The nested function is
found **by name** (`<snake>_violations`), which is F243's rule and means a hand-written one will do
and another module's needs importing — checked in `src/derive.c`, so a missing one reads
"add derive(Validate) to 'Plain'" rather than "call to undefined function 'plain_violations'".

**A cascade reports a different field**, so §4's one-message-per-field rule is untouched by it:
three bad elements are three violations, and `#[not_empty]` and `#[valid]` on the same list do not
compete. It also satisfies `derive(Validate)` on its own — a struct whose only attribute is
`#[valid]` has something to do.

## 7. Cross-field rules are not an attribute

`endDate must not be before startDate` is one rule in 106, and it is the one Jakarta needs a
class-level constraint and a custom validator for. The design: **don't**. The generated function
answers a list; a hand-written function can answer another, and the caller concatenates:

```ziman
val problems = concat(medication_request_violations(req), date_order(req))
```

Which is what the backend does today, with no machinery. A class-level attribute would need an
expression language in the attribute or a named validator function to point at — and the moment
that is wanted, §9's `expr` path is the better answer than inventing a second one.

## 8. What is deliberately not in it

* **Groups** (`@Validated(Create.class)`). The backend already has a request type per operation, so
  the thing groups exist for — one bean validated differently in two places — does not arise. Two
  request structs are clearer than one struct with group annotations.
* **`payload`**, which exists in Jakarta to carry metadata to a custom error handler, and has no
  reader here.
* **Method and parameter validation.** Ziman has no proxying, and a service function's parameters
  are checked by the type checker.
* **Custom constraints as a mechanism** (`ConstraintValidator`). A hand-written function beside the
  generated one (§7) covers the same ground without an interface to implement.
* **`@Null`, `@AssertTrue`, `@AssertFalse`, `@Digits`, `@Negative*`, `@DecimalMin/Max`,
  `@Past`/`@Future`.** Zero uses in 106. Each is cheap to add later; adding seven now is seven
  promises with no reader (`meta.zm`'s rule).
* **i18n** (§3).
* **Constraint composition** (an annotation defined as several others). One use would be
  `#[not_blank] #[size(min: 8)]` collapsing to `#[password]`, and two attributes on a field already
  read fine.

## 9. What this costs the `expr` plan, and what it does not

`expressions.md` §17's open question is where validation rules live. This answers it: **in
attributes on the struct**, for the rules that are properties of a field. That does *not* make
`expr` pointless, and the division is worth stating now so neither half grows into the other:

* **an attribute** is a rule about one field, decided at compile time, checked by generated code —
  no expression needed, and a typo in a constraint name is a compile error;
* **an `expr` rule** is a rule that arrives at run time — from a properties file or a database row
  — or one that spans fields. It is data, so it can be wrong, which is why §8 of that document
  makes a missing path an error rather than a null.

So: attributes for the 104, hand-written functions for the cross-field 2, and `expr` for a rule
somebody edits without a rebuild. **`expr` keeps its reason to exist and stops being the answer to
this question.** It also unblocks `#[column(required: true)]`, which was deferred precisely because
this decision was open (`column-override.md` §4) — `required:` is now redundant: `#[not_blank]`
and a non-`Option` type say it where it belongs.

## 10. Decided

1. **The `2147483647` artefact** (§3): **say the honest thing.** `#[size(min: 8)]` with no maximum
   reports `"size must be at least 8"`, and the three functions `size_at_least`, `size_at_most` and
   `size_between` exist for that reason rather than one taking two bounds — a sentinel for "no
   maximum" is exactly the Java default that produced the message.
2. **Where it lives**: `lib/core/validate.zm` and `lib/core/derive_validate.zm`, as recommended.
   None of it is SQL.
3. **`#[email]`'s rule**: the backend's `is_email`, ported — which turns out to be **Hibernate's**
   `EmailValidator` and `DomainNameUtil` reimplemented, deliberately, so that a port of a Spring
   application answers 400 to exactly the addresses Spring did. So the two readings of this
   question were never in conflict. The port is faithful and was checked against the original over
   38 addresses, which found a crash the original has: `substring(value, at + 1, byte_len(value))`
   indexes characters with a byte count, so a non-ASCII address (`üser@exämple.com`, 16 characters
   in 18 bytes) raises "substring index out of bounds" where it should answer `true` — a 500 on
   `PUT /api/patients/me`. `validate.is_email` uses `len`; the backend's copy goes when the service
   that holds it is converted.
4. **Adoption**: library first, then one service, as recommended.

A fifth, answered while building: **`message:`**. Every constraint takes it, as every Jakarta
constraint does, and it is a parser-level optional argument (`message:string?`) on each — so a
misspelling is refused rather than ignored. `validate.with_message` is what the generated code
wraps the rule in, which keeps the rule deciding *whether* and the author deciding *what the reader
is told*.

## 11. Staging

1. ✅ **The attribute grammar**: int and identifier argument values, refused by name as
   `#[column]`'s are. Parser only; `meta` unchanged.
2. ✅ **`lib/core/validate.zm`**: `Violation`, and one function per constraint, with an `Option`
   overload wherever absence can be allowed. 17 `#[test]` units in `tests/stdlib/validate_test.zm`
   pin every message, including the ones the North Star's Python suite asserts.
3. ✅ **`lib/core/derive_validate.zm`**: `#[derive(Validate)]`, generating calls into (2) and
   nothing else. `tests/lang/derive_validate.out` pins the generated text; five refusal fixtures
   pin what it will not generate, and four more pin the attribute grammar.
4. ✅ **One service converted**: `body-measurements`, message for message. Its two rules were
   `not_one_of("type", ..)` and `optional_not_one_of("interpretation", ..)` — the same rule twice,
   split by whether the field may be absent — and they are one `#[one_of]` each now, the field's
   type saying the rest. The word lists moved to the model module with the struct, since that is
   the scope the generated code reads them in. `service.broken_rules` is the one translation from
   `validate.Violation` to the application's `FieldError`. `src/test/unit/validation_test.zm` pins
   the messages, including that a request breaking two fields still reports two.
5. ✅ **The rest**: every request type in the backend, 2026-10-29. **24 request structs** carry
   attributes now, and the helper counts outside `service/service.zm`'s own definitions went
   `accumulate` 37 → 3, `optional_not_one_of` 30 → 1, `not_one_of` 18 → 5, `missing_field` 26 → 1,
   `list_not_one_of` 2 → 0, `validation_result` 32 → 5.

What the rollout found, which is the part worth keeping:

* **The shape that does not convert is a rule on something that is not a field.** Six
  `?type=`/`?category=`/`?mealType=` **query parameter** checks stayed hand-written across four
  services, because a query parameter has nothing for an attribute to sit on. That was visible from
  stage 4 and it is the only recurring leftover.
* **Three rules are not facts about one field**: a date against *today* (`dateOfBirth` must be
  past), one field against another (`endDate` not before `startDate`), and an **indexed** element
  (`ingredients[0].name`). These need `field_errors` — `broken_rules` without the
  `Option<ServiceError>` — so generated and hand-written violations accumulate onto one list and
  the client is still told about every field at once. A `#[past]` attribute would take the first;
  the second wants a rule over the whole struct, which is Jakarta's class-level constraint and is
  not designed here; the third is §6 cascading.
* **`message:` earned itself immediately.** Four password fields keep Spring's
  `"size must be between 8 and 2147483647"` through `#[size(min: 8, message: "..")]`. §3 called that
  number an annotation default leaking, and so it is — but this backend is a *port*, nothing else
  sends that text, and an attribute that says "inherit the quirk, deliberately" is better than a
  hand-written check that hides the decision. The library still says the honest thing by default.
* **One rule was not the attribute it looked like.** `register`'s password tested `req.password ==
  ""`, not `is_blank`, so a password of spaces fell through to the length check. That is
  `#[not_empty(message: "must not be blank")]`, not `#[not_blank]` — the kind of difference a
  conversion only notices because the attribute forces the question.
* **A behaviour the conversion improved, deliberately**: `file-attachments` answered the *first* of
  its two blank fields and stopped. It reports both now, which is what every other resource here
  already did.
