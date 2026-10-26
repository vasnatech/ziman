# `sql`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

SQL as a value: a statement tree, a parser that reads one, and a formatter that writes
it back. No database, no engine, no connection -- `memdb` is built on this, and anything that
wants to read, rewrite or generate SQL can be too.


```
val statement = try sql.parse("SELECT name FROM patient WHERE id = $1")
print(sql.format(statement))

for s in try sql.parse_file("migrations/V001__create_users.sql") {
    ...
}
```

**`parse` then `format` then `parse` gives the same tree.** That round trip is the property this
module is tested against, over the sample application's 40 real migration files and its query
corpus, and it is what makes coverage safe to grow: a clause the parser learns to read and the
formatter forgets to write stops the suite rather than the reader.

## What it reads

`SELECT` (items with aliases, one `FROM` table with an alias, `WHERE`, `GROUP BY`, `HAVING`,
`ORDER BY` with `ASC`/`DESC`, `LIMIT`, `OFFSET`), `INSERT ... VALUES` (one or many rows),
`UPDATE ... SET ... WHERE`, `DELETE FROM ... WHERE`, `CREATE TABLE` (columns with their types and
constraints, and table-level constraints), `CREATE [UNIQUE] INDEX`, `ALTER TABLE ... ADD COLUMN`,
`DROP TABLE [IF EXISTS]`, and `BEGIN`/`COMMIT`/`ROLLBACK`.

Expressions: literals, column references (qualified or not), bound parameters (`?` and `$1`, both
numbered so a statement may mix them), `AND`/`OR`/`NOT`, the six comparisons, `IS [NOT] NULL`,
`LIKE`, `IN`, `BETWEEN`, `CASE WHEN ... THEN ... ELSE ... END`, `+ - * /`, `||`, function calls
including the aggregates, and parentheses.

## What it does *not* read, and the honest size of that

This is **not** ANSI SQL. ISO/IEC 9075 also has joins, set operations, subqueries, `DISTINCT`,
common table expressions, window functions, `MERGE`, `GRANT`/`REVOKE`, views, schemas, sequences,
domains, triggers, routines, cursors, savepoints, isolation levels, `GROUPING SETS`, `LATERAL`,
interval and row constructors, `IS DISTINCT FROM`, `SIMILAR TO`, `OVERLAPS`, temporal tables and
the SQL/JSON functions. For scale, PostgreSQL's own grammar is some twenty thousand lines and is
not full ANSI either. Growing toward it is a direction; the round trip above is what keeps each
step from undoing the last.

## Preserved rather than modelled

A formatter that quietly dropped a `FOREIGN KEY` would be worse than no formatter, so the parts
this module does not model are **kept as the text they were written as** and written back
verbatim: a column's constraints beyond `NOT NULL`, `PRIMARY KEY`, `UNIQUE` and `DEFAULT`, a
table-level constraint, and a statement form that is recognised only well enough to pass through
(`PRAGMA`, `SET`, `ANALYZE`, `VACUUM`). They round-trip exactly; they are simply opaque. Where a
reader needs one of them taken apart, that is a modelling increment, and the text is already
there to start from.

`use sql` -- or `use sql.Name` to bring one item in bare.

### `fold_name`

```
pub fn fold_name(name: string) -> string
```

A name folded for comparison, which is how SQL compares an unquoted identifier. The standard
folds to upper case and PostgreSQL folds to lower; either way two spellings of one name must
compare equal, and lower reads better in a message.

## Tokens

### `Token`

```
pub struct Token {
    kind: string
    text: string
    value: int
    // Where this token sits in the source, so a run of tokens can be recovered as the text it was
    // written as. That is what lets the parts this module does not model be **preserved verbatim**
    // rather than re-assembled from tokens with guessed spacing.
    start: int
    end: int
}
```

One token. `kind` is "name", "number", "text", "punct" or "param"; `text` is what was written,
except for a `text` token, which holds the string's value with its quotes removed and its `''`
pairs collapsed.

## The tree

### `Column`

```
pub struct Column {
    name: string
    type_name: string
    not_null: bool
    primary_key: bool
    unique: bool
    default_value: Option<SqlValue>
    // Every constraint on this column that is not one of the four above, as the text it was
    // written as: `REFERENCES patients(id) ON DELETE CASCADE`, `CHECK (n > 0)`, `COLLATE NOCASE`,
    // `AUTOINCREMENT`. Preserved rather than modelled, so `format` writes back what it read.
    extra: string
}
```

A column as the schema declares it. `type_name` is kept for messages and for a future increment
that applies affinity; nothing reads it to decide anything today.

### `BinNode`

```
pub struct BinNode {
    op: string
    left: Expr
    right: Expr
}
```

### `NotNode`

```
pub struct NotNode {
    inner: Expr
}
```

### `NullNode`

```
pub struct NullNode {
    inner: Expr
    negated: bool
}
```

### `ColRef`

```
pub struct ColRef {
    table: string
    name: string
}
```

A column reference as written: `table.column`, or `column` with an empty `table`. The qualifier
used to be parsed and dropped, which a formatter cannot afford.

### `CallNode`

```
pub struct CallNode {
    name: string
    args: [Expr]
}
```

### `LikeNode`

```
pub struct LikeNode {
    subject: Expr
    pattern: Expr
    negated: bool
}
```

### `InNode`

```
pub struct InNode {
    subject: Expr
    options: [Expr]
    negated: bool
}
```

### `BetweenNode`

```
pub struct BetweenNode {
    subject: Expr
    low: Expr
    high: Expr
    negated: bool
}
```

### `WhenArm`

```
pub struct WhenArm {
    condition: Expr
    value: Expr
}
```

One `WHEN cond THEN value` of a `CASE`.

### `CaseNode`

```
pub struct CaseNode {
    arms: [WhenArm]
    fallback: [Expr]
}
```

### `Expr`

```
pub enum Expr {
    Lit(SqlValue)
    Col(ColRef)
    Param(int)
    Not(NotNode)
    Bin(BinNode)
    IsNull(NullNode)
    Call(CallNode)
    Like(LikeNode)
    In(InNode)
    Between(BetweenNode)
    Case(CaseNode)
    Star
}
```

An expression. Recursion is direct rather than through an array: a struct field may be the enum
that contains it, as long as some variant ends the chain -- `Lit` and `Col` do.

### `SelectItem`

```
pub struct SelectItem {
    expr: Expr
    alias: string
}
```

### `OrderTerm`

```
pub struct OrderTerm {
    expr: Expr
    descending: bool
}
```

### `Assignment`

```
pub struct Assignment {
    column: string
    value: Expr
}
```

### `SelectStmt`

```
pub struct SelectStmt {
    items: [SelectItem]
    // Empty when the statement has no `FROM`: `SELECT 1` is legal in both real engines and is the
    // liveness probe `datasource`'s pool sends, so it has to be legal here.
    table: string
    // `FROM patient p` / `FROM patient AS p`. One table and one alias rather than a list of table
    // references: joins are a later increment and will want a list, and changing the shape then is
    // a deliberate step the round-trip test protects.
    table_alias: string
    filter: Option<Expr>
    groups: [Expr]
    having: Option<Expr>
    order: [OrderTerm]
    limit: Option<Expr>
    offset: Option<Expr>
}
```

### `InsertStmt`

```
pub struct InsertStmt {
    table: string
    columns: [string]
    rows: [[Expr]]
}
```

### `UpdateStmt`

```
pub struct UpdateStmt {
    table: string
    sets: [Assignment]
    filter: Option<Expr>
}
```

### `DeleteStmt`

```
pub struct DeleteStmt {
    table: string
    filter: Option<Expr>
}
```

### `CreateStmt`

```
pub struct CreateStmt {
    table: string
    columns: [Column]
    // The table-level constraints -- `PRIMARY KEY (a, b)`, `FOREIGN KEY ... REFERENCES ...`,
    // `UNIQUE (..)`, `CHECK (..)`, a named `CONSTRAINT` -- as the text they were written as.
    constraints: [string]
    if_not_exists: bool
}
```

### `AlterStmt`

```
pub struct AlterStmt {
    table: string
    column: Column
}
```

### `CreateIndexStmt`

```
pub struct CreateIndexStmt {
    name: string
    table: string
    columns: [string]
    unique: bool
    if_not_exists: bool
}
```

### `DropStmt`

```
pub struct DropStmt {
    table: string
    if_exists: bool
}
```

### `Statement`

```
pub enum Statement {
    Select(SelectStmt)
    Insert(InsertStmt)
    Update(UpdateStmt)
    Delete(DeleteStmt)
    Create(CreateStmt)
    CreateIndex(CreateIndexStmt)
    Alter(AlterStmt)
    Drop(DropStmt)
    Begin
    Commit
    Rollback
    // A statement recognised only well enough to pass through -- `PRAGMA`, `SET`, `ANALYZE`,
    // `VACUUM` -- carrying its own text so `format` writes back exactly what was read. Empty for
    // an empty statement.
    Passthrough(string)
}
```

One parsed statement.

Named `Statement` and not `SqlStatement`: qualified it reads `sql.Statement`, where
`sql.SqlStatement` would stutter the way `random.random_bytes` and `uuid.uuid_new` were weighed
for -- and imported by name, `Statement` is unambiguous in a program that is working on SQL.

## Parsing statements

### `parse`

```
pub fn parse(sql: string) -> Result<Statement, DbError>
```

One statement. Trailing `;` and empty statements are tolerated; anything past the first
statement is an error, because `datasource.execute` is one statement and `migrate` splits a
script before it gets here.

## Many statements

### `parse_many`

```
pub fn parse_many(text: string) -> Result<[Statement], DbError>
```

Every statement in `text`, split on the `;` *tokens* rather than on the characters.

**That is the whole reason this is better than splitting text**: the lexer already knows a
string from an identifier from a comment, so a `;` inside `'a;b'`, inside `"a;b"`, inside a
`--` or `/* */` comment cannot be mistaken for a separator. `migrate` keeps its own text-level
splitter because it must divide SQL this module may not be able to *parse*; this one divides
SQL it can.

An empty statement -- a stray `;`, a trailing one, a file of comments -- contributes nothing.

### `parse_file`

```
pub fn parse_file(path: string) -> Result<[Statement], DbError>
```

Every statement in the file at `path`, resolved through `ZIMAN_RESOURCE_PATH` the way `migrate`
resolves a migrations directory -- so a test can name `schema.sql` and `data.sql` the same way
it names anything else it ships with.

## Writing

### `format_value`

```
pub fn format_value(v: SqlValue) -> string
```

A value as SQL. A text value is quoted with `''` doubling, and bytes become a hex literal, which
both SQLite and PostgreSQL read back -- `X'..'` is the standard's spelling.

### `format_expr`

```
pub fn format_expr(e: Expr) -> string
```

`e` as SQL. Brackets appear where precedence needs them and nowhere else, so the text reads the
way it was written rather than as a fully parenthesised tree.

### `format`

```
pub fn format(s: Statement) -> string
```

`s` as one line of SQL. `parse` reads back exactly the tree this was written from, which is the
property the fixtures assert over every statement they can find.
