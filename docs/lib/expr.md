# `expr`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

An expression language over `Json`, for rules and text that are *data* rather than source.

A configuration constraint read from a properties file, a validation rule stored in a row, a
message template rendered at run time: each is a string the program did not compile, holding
a question about a value. `age >= 18`, `address?.city == 'Ankara'`, `length(tags) > 0`.

**It evaluates over `Json` because it has no choice.** Ziman has no runtime reflection -- no
way to ask a value for a field by name -- so an interpreter cannot navigate a struct at all.
`Json` is the one dynamic value tree the language has, and `#[derive(Json)]` is what puts any
struct in reach of it:


```
val rule = try expr.parse("address?.city == 'Ankara' && age >= 18")
if try expr.eval_bool(rule, user_to_json(user)) { .. }
```

The cost of that is stated rather than hidden: converting a struct to `Json` allocates the
whole tree, which is right for one request or a config read at startup and wrong in a loop.

Designed in docs/design/expressions.md. `template.zm` will use this module rather than grow
its own grammar (its §10), which is the reason the two were designed together.

`use expr` -- or `use expr.Name` to bring one item in bare.

## The tree

### `MemberNode`

```
pub struct MemberNode {
    target: Expr
    name: string
    // `?.` rather than `.`: a `null` target answers `null` instead of failing. §8 of the design
    // is why absence is an error by default and this is how you say otherwise.
    safe: bool
}
```

### `IndexNode`

```
pub struct IndexNode {
    target: Expr
    key: Expr
}
```

### `UnaryNode`

```
pub struct UnaryNode {
    op: string
    operand: Expr
}
```

### `BinaryNode`

```
pub struct BinaryNode {
    op: string
    left: Expr
    right: Expr
}
```

### `TernaryNode`

```
pub struct TernaryNode {
    test: Expr
    then: Expr
    otherwise: Expr
}
```

### `CallNode`

```
pub struct CallNode {
    name: string
    args: [Expr]
}
```

### `Expr`

```
pub enum Expr {
    Lit(Json)
    // A bare name, resolved against the context object's keys.
    Root(string)
    Member(MemberNode)
    Index(IndexNode)
    Unary(UnaryNode)
    Binary(BinaryNode)
    Ternary(TernaryNode)
    Call(CallNode)
}
```

## The entry points

### `parse`

```
pub fn parse(source: string) -> Result<Expr, string>
```

Parse once, evaluate many times. An expression in a module-level `val` is parsed at load, so
a malformed one is a load-time failure rather than a surprise on the first request; one that
arrives at run time, from a config file or a row, goes through here and handles the `Err`.

### `must_parse`

```
pub fn must_parse(source: string) -> Expr
```

## Evaluating

### `eval`

```
pub fn eval(e: Expr, ctx: Json) -> Result<Json, string>
```

### `eval_bool`

```
pub fn eval_bool(e: Expr, ctx: Json) -> Result<bool, string>
```

### `eval_string`

```
pub fn eval_string(e: Expr, ctx: Json) -> Result<string, string>
```

### `names`

```
pub fn names(e: Expr) -> [string]
```

Every name an expression reads from the context, in the order it meets them: the **root** of a
path, never a segment under it, so `user.address.city` is `["user"]` and
`length(rows) + offset` is `["rows", "offset"]`.

**This walk lives here rather than in `template`, and that is the module boundary** (see
docs/design/expressions.md §10): the `Expr` enum is public, so a caller *could* walk it, and
the one caller that needs to -- `template`, to refuse a binding its text never mentions -- must
not learn the grammar to do it. A new node kind is then this function's problem alone.

A loop binder is not special here; a caller that binds names of its own subtracts them.

### `kind_of`

```
pub fn kind_of(v: Json) -> string
```
