# `validate`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

The rules `#[derive(Validate)]` generates calls to, and the messages they answer.

**Every rule is an ordinary function.** A person can call one, a fixture can test one, and the
generator writes nothing but calls -- which is the same division `derive_row` has with `db`'s
converters. Nothing here knows an attribute exists.

**A rule answers the message it would report, or None.** Not a bool: the message belongs with the
rule that knows why it failed, and the generated code should not hold a catalogue of text.

**`Option<T>` is how a field says absence is allowed**, so every rule that can apply to one has
an `Option` overload that passes when the value is absent. That is Jakarta Validation's
semantics (`@Size` on a null is valid; only `@NotNull` rejects null), and it is why there is one
`#[one_of]` where the hand-written code it replaces had `not_one_of` *and*
`optional_not_one_of` -- 50 uses of the same rule split by type. The type decides optionality,
the attribute decides the rule. See docs/design/validation.md.

**The messages are Hibernate Validator's**, because the North Star's
test suites assert them byte for byte (`"must not be blank"`,
`"must be a well-formed email address"`). One is deliberately *not* inherited: Java prints
"size must be between 8 and 2147483647" for a minimum with no maximum, because its annotation
default is `Integer.MAX_VALUE`. Ziman has no such default to leak, nothing asserts that text, and
`size_at_least` says what it means.

`use validate` -- or `use validate.Name` to bring one item in bare.

### `Violation`

```
pub struct Violation {
    field: string
    message: string
}
```

One broken rule.

**`field` is the name the *wire* uses** -- `newPassword`, not `new_password` -- because the
response is keyed by what the client sent, and `#[derive(Validate)]` computes it exactly as
`#[derive(Json)]` computes a key. The application maps these to its own error type; a library
that answered `ServiceError` would have the dependency upside down.

### `violation`

```
pub fn violation(field: string, message: string) -> Violation
```

### `first`

```
pub fn first(messages: [Option<string>]) -> Option<string>
```

The first message in `messages`, or None when every rule passed.

**One violation per field is all the wire can carry** -- the response is a map keyed by field
name (docs/design/validation.md §4) -- so a field with several constraints reports the first that
failed, in the order they were written. `#[derive(Validate)]` generates a call to this for such a
field; every rule in the list runs, which costs nothing a validation is slow enough to notice and
keeps the generated text one expression.

### `with_message`

```
pub fn with_message(found: Option<string>, message: string) -> Option<string>
```

`found` with its text replaced, if it found anything. What `message:` on a constraint does:
the rule still decides *whether* the field is in violation, and the author decides what the
reader is told -- which is Jakarta's `message` attribute exactly.

## Presence

### `not_blank`

```
pub fn not_blank(value: string) -> Option<string>
```

Not null, and not only whitespace. Jakarta's `@NotBlank`.

```
pub fn not_blank(value: Option<string>) -> Option<string>
```

### `not_empty`

```
pub fn not_empty(value: string) -> Option<string>
```

Not empty: a string with at least one character, or a list with at least one element. Jakarta's
`@NotEmpty`, which unlike `@NotBlank` counts whitespace as content.

```
pub fn not_empty<T>(values: [T]) -> Option<string>
```

```
pub fn not_empty<T>(values: Option<[T]>) -> Option<string>
```

## One of a fixed list

### `one_of`

```
pub fn one_of(value: string, allowed: [string]) -> Option<string>
```

```
pub fn one_of(value: Option<string>, allowed: [string]) -> Option<string>
```

```
pub fn one_of(values: [string], allowed: [string]) -> Option<string>
```

Every element must be one of `allowed`; **the first that is not is the one reported**, because
the wire carries one message per field (see `Violation`).

```
pub fn one_of(values: Option<[string]>, allowed: [string]) -> Option<string>
```

## Size

### `size_at_least`

```
pub fn size_at_least(value: string, min: int) -> Option<string>
```

```
pub fn size_at_least(value: Option<string>, min: int) -> Option<string>
```

### `size_at_most`

```
pub fn size_at_most(value: string, max: int) -> Option<string>
```

```
pub fn size_at_most(value: Option<string>, max: int) -> Option<string>
```

### `size_between`

```
pub fn size_between(value: string, min: int, max: int) -> Option<string>
```

```
pub fn size_between(value: Option<string>, min: int, max: int) -> Option<string>
```

## Bounds

### `at_least`

```
pub fn at_least(value: int, bound: int) -> Option<string>
```

```
pub fn at_least(value: Option<int>, bound: int) -> Option<string>
```

### `at_most`

```
pub fn at_most(value: int, bound: int) -> Option<string>
```

```
pub fn at_most(value: Option<int>, bound: int) -> Option<string>
```

### `positive`

```
pub fn positive(value: int) -> Option<string>
```

Strictly greater than zero. Jakarta's `@Positive`, and the message the North Star already uses.

```
pub fn positive(value: Option<int>) -> Option<string>
```

```
pub fn positive(value: float) -> Option<string>
```

```
pub fn positive(value: Option<float>) -> Option<string>
```

## Email

### `email`

```
pub fn email(value: string) -> Option<string>
```

**The rule is Hibernate Validator's `EmailValidator` and `DomainNameUtil`**, which is what
`@Email` means in a Spring application, so a backend ported field by field keeps answering 400
to exactly the addresses it answered 400 to before. It is looser than most people guess:
`user@localhost` passes, because Hibernate accepts a domain of one label, and `a_b.c` passes,
because Hibernate's domain pattern allows punctuation a hostname may not have. Matched anyway --
being *stricter* than the implementation a port is replacing is the same class of divergence as
being looser, and the loose cases are the ones a test suite pins.

Split at the *last* `@`, so `a@b@c.d` has the local part `a@b`, which the character check then
refuses. The local part is 1 to 64 characters with no dot first or last and no two together; the
domain is 1 to 255 characters of dot-separated labels, each non-empty and without a leading or
trailing hyphen.

```
pub fn email(value: Option<string>) -> Option<string>
```

### `is_email`

```
pub fn is_email(value: string) -> bool
```

Whether `value` reads as an email address, by the rule above. Public because it answers a
question a caller may have outside validation, and because a port replacing a hand-written check
wants to compare the two before it deletes one.

## Pattern

### `matches`

```
pub fn matches(value: string, pattern: string) -> Option<string>
```

The value must match `pattern`, which is `regex`'s syntax. Jakarta's `@Pattern`, whose message
names the pattern because nothing else can say what was wanted.

**A pattern that will not compile is a violation and not a crash**: the pattern comes from an
attribute, so a bad one is a mistake in the source, and reporting it against the field is how a
reader finds it. `regex.compile` keeps its last 256 patterns, so compiling here on every call is
a lookup after the first.

```
pub fn matches(value: Option<string>, pattern: string) -> Option<string>
```
