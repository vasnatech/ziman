# Validation from annotations

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
— so the first use of `#[valid]` should be a deliberate one, and until there is one, **this is
designed and not built**. The corpus has no cascading case today.

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

## 10. Open questions

1. **The `2147483647` artefact** (§3): reproduce Hibernate's message for a `size` with no maximum,
   or say "size must be at least 8"? Nothing asserts it, so this is free to choose, and the four
   call sites are hand-written today. Recommendation: say the honest thing.
2. **Where the library lives.** `lib/core/validate.zm` for the `Violation` type and the message
   texts, `lib/core/derive_validate.zm` for the generator — `lib/core` rather than `lib/db`,
   because none of it is SQL (`derive_*` precedent: the SQL three are in `lib/db`).
3. **`#[email]`'s rule.** The backend's `is_email` is a hand-written check; Hibernate's `@Email`
   accepts far more than most people expect (it is RFC-ish). Reuse `is_email`'s rule and say so, or
   match Hibernate? Recommendation: reuse what the backend has, since its tests pin the behaviour.
4. **Whether the backend adopts it in the same pass.** 106 rule uses across 23 files, with the
   error text asserted by two test suites — the same shape as the `eql` and derived-query adoptions,
   which both found real bugs in the code they replaced. Recommendation: build the library and
   convert **one** service first, then decide.

## 11. Staging

1. **The attribute grammar**: int and identifier argument values, refused by name as
   `#[column]`'s are. Parser only; `meta` unchanged.
2. **`lib/core/validate.zm`**: `Violation`, and one function per constraint (`is_blank`,
   `size_of`, …) so the *rule* is ordinary Ziman that a person can call and a fixture can test
   without a generator.
3. **`lib/core/derive_validate.zm`**: `#[derive(Validate)]` over the seven, generating calls into
   (2). Fixtures pin the generated text, as every other generator's do.
4. **One service converted**, message-for-message, with its tests unchanged — the proof.
5. Then the rest, or not, as (4) advises.
