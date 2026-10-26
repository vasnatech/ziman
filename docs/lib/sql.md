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

`SELECT [DISTINCT]` (items with aliases, a `FROM` table with an alias, joins, `WHERE`,
`GROUP BY`, `HAVING`, `ORDER BY` with `ASC`/`DESC`, `LIMIT`, `OFFSET`), `INSERT ... VALUES`
(one or many rows),
`UPDATE ... SET ... WHERE`, `DELETE FROM ... WHERE`, a `RETURNING` clause on any of those three,
`CREATE TABLE` (columns with their types and
constraints, and table-level constraints), `CREATE [UNIQUE] INDEX`,
`ALTER TABLE ... ADD [COLUMN]` and `ALTER TABLE ... ADD <table constraint>`,
`DROP TABLE [IF EXISTS]`, and `BEGIN`/`COMMIT`/`ROLLBACK`.

Joins: `[INNER] JOIN`, `LEFT`/`RIGHT`/`FULL [OUTER] JOIN` and `CROSS JOIN`, each with an `ON`
condition or a `USING` column list, as a flat left-deep list -- which is what SQL means by
`a JOIN b ON .. JOIN c ON ..`. A comma in the `FROM` list is read as the `CROSS JOIN` it means.
A parenthesised join expression would need a tree and is not read.

A **derived table** -- `(SELECT ..) AS x`, with an optional `(a, b)` renaming its columns -- is a
`FROM` entry like any other, and may be joined, nested, or given a compound query.

Set operations: `UNION`, `INTERSECT` and `EXCEPT`, each with an optional `ALL` or `DISTINCT`,
chained left to right, with the compound's `ORDER BY`, `LIMIT` and `OFFSET` after the last arm.
**The sequence is recorded as written and not grouped into a tree**, because the grouping is
dialect-specific: SQLite reads the three operators left to right and the standard binds
`INTERSECT` tighter, so a tree built here would have to pick an engine. A parenthesised arm
(`(SELECT ..) UNION (SELECT ..)`) is not read -- SQLite refuses it too, and recording it would
mean a formatter that could turn SQL one engine rejects into SQL it accepts.

A `*` stands anywhere in a result list, on its own or beside other items, and `t.*` names one
table's columns -- in a select list and in a `RETURNING` clause alike.

Expressions: literals, column references (qualified or not), bound parameters (`?` and `$1`, both
numbered so a statement may mix them), `AND`/`OR`/`NOT`, the six comparisons, `IS [NOT] NULL`,
`LIKE`, `IN` (a list or a subquery), `BETWEEN`, `CASE WHEN ... THEN ... ELSE ... END`,
`CAST(x AS type)`, a scalar subquery, `EXISTS (SELECT ..)`, `+ - * /`, `||`, function calls
including the aggregates and `COUNT(DISTINCT x)`, and parentheses.

## What it does *not* read, and the honest size of that

`RETURNING` is read although it is **not** in the standard, which reads the rows a write touched
as a data change delta table instead (`SELECT * FROM FINAL TABLE (INSERT ..)`, SQL:2011, with
`OLD`, `NEW` and `FINAL` as the options). Essentially only Db2 implements that form, while
PostgreSQL, SQLite from 3.35 and MariaDB all have `RETURNING`, so reading the clause people
actually write is worth more here than reading the one the standard specifies. The delta-table
form is not read.

`ROLLUP`, `CUBE` and `GROUPING SETS` are **not** read as the grouping constructs they are: they
parse as ordinary function calls in a `GROUP BY`, which is what SQLite does with them too, and an
engine then refuses the function. Reading them properly needs a `GroupTerm` the `GROUP BY` list
does not have yet.

This is **not** ANSI SQL. ISO/IEC 9075 also has `LATERAL` and a parenthesised join expression in
a `FROM`, `CORRESPONDING` on a set operation, common table expressions, window functions, `MERGE`,
`GRANT`/`REVOKE`, views, schemas, sequences, domains, triggers, routines, cursors, savepoints,
isolation levels, `GROUPING SETS`, interval and row constructors, `IS DISTINCT FROM`,
`SIMILAR TO`, `OVERLAPS`, temporal tables and the SQL/JSON functions. For scale, PostgreSQL's own grammar is some twenty thousand lines and is
not full ANSI either. Growing toward it is a direction; the round trip above is what keeps each
step from undoing the last.

## Preserved rather than modelled

A formatter that quietly dropped a `FOREIGN KEY` would be worse than no formatter, so the parts
this module does not model are **kept as the text they were written as** and written back
verbatim: a column's `COLLATE`, `AUTOINCREMENT` or `GENERATED`, a *named* constraint in a
column's position, a table-level constraint whose shape is none of the four, and a statement form
recognised only well enough to pass through (`PRAGMA`, `SET`, `ANALYZE`, `VACUUM`).

The four constraint shapes themselves **are** modelled, because `memdb` enforces them and an
engine cannot enforce a string: `PRIMARY KEY (cols)`, `UNIQUE (cols)`, `CHECK (expr)` and
`FOREIGN KEY (cols) REFERENCES t (cols)` with their `ON DELETE`/`ON UPDATE` actions, at table
level and -- for `CHECK` and `REFERENCES` -- in a column's own position, where they stay so that
`format` writes them back where they were. They round-trip exactly; they are simply opaque. Where a
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

### `ConstraintRule`

```
pub enum ConstraintRule {
    PrimaryKey([string])
    Unique([string])
    Check(Expr)
    ForeignKey(ForeignKeyRule)
    // A shape this module does not model, carried by the constraint's `text` and written back
    // verbatim.
    Opaque
}
```

A column as the schema declares it. `type_name` is kept for messages and for a future increment
that applies affinity; nothing reads it to decide anything today.
What a constraint *says*, for the four shapes the standard makes and this module now models.
Until constraint enforcement, every one of these was a run of text nobody looked inside -- which
was honest while nothing enforced them and useless the moment something did.

### `ForeignKeyRule`

```
pub struct ForeignKeyRule {
    // The child's columns -- this table's.
    columns: [string]
    table: string
    // The parent's columns. Empty where none were named, which means the parent's primary key.
    references: [string]
    // `CASCADE`, `SET NULL`, `SET DEFAULT`, `RESTRICT`, `NO ACTION`, or empty for the default --
    // which *is* `NO ACTION`, spelled differently so a reader can tell "not written" from
    // "written as the default".
    on_delete: string
    on_update: string
}
```

`FOREIGN KEY (a, b) REFERENCES parent (x, y) ON DELETE CASCADE`.

### `TableConstraint`

```
pub struct TableConstraint {
    // From `CONSTRAINT <name>`, or empty. An engine quotes it in its message when it is there.
    name: string
    rule: ConstraintRule
    // The constraint exactly as written, brackets and spacing included. `format` writes it for an
    // `Opaque` rule, and an engine's message quotes the expression inside it for an unnamed
    // `CHECK` -- which is what SQLite quotes, spacing and all.
    text: string
}
```

### `Column`

```
pub struct Column {
    name: string
    type_name: string
    not_null: bool
    primary_key: bool
    unique: bool
    default_value: Option<SqlValue>
    // The constraints written in this column's own position that this module models: a bare
    // `CHECK (e)` and a bare `REFERENCES t (c)`, each with this column as its column list. They
    // stay here rather than moving to the table's list so `format` writes them back where they
    // were -- the sample application's migrations put every foreign key in column position.
    //
    // A *named* column constraint (`CONSTRAINT k CHECK (..)`) is **not** modelled and goes to
    // `extra` whole, so nothing of it is lost and nothing of it is enforced.
    constraints: [TableConstraint]
    // Every constraint on this column that is modelled by neither the four flags above nor
    // `constraints`, as the text it was written as: `COLLATE NOCASE`, `AUTOINCREMENT`,
    // `GENERATED ALWAYS AS ..`, a named constraint. Preserved rather than modelled, so `format`
    // writes back what it read.
    extra: string
}
```

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
    // `COUNT(DISTINCT x)`: the duplicates are dropped before the aggregate sees them.
    distinct: bool
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
    // A scalar subquery, `(SELECT ..)`, which must answer one row of one column.
    Sub(SubNode)
    // `EXISTS (SELECT ..)`. `NOT EXISTS` is `Not(Exists(..))` -- `NOT` has its own precedence
    // level now, so it needs no flag of its own here.
    Exists(SubNode)
    InSub(InSubNode)
    Cast(CastNode)
    Like(LikeNode)
    In(InNode)
    Between(BetweenNode)
    Case(CaseNode)
    Star
    // `t.*`, holding the table name or alias it was written with. A separate variant rather than a
    // qualifier on `Star`, so `count(*)` and every other reader of a bare star is untouched.
    TableStar(string)
}
```

An expression. Recursion is direct rather than through an array: a struct field may be the enum
that contains it, as long as some variant ends the chain -- `Lit` and `Col` do.

### `TableRef`

```
pub struct TableRef {
    // The table's name, or empty for a derived table -- a `(SELECT ..)` in the `FROM`, which has a
    // query instead of a name.
    name: string
    alias: string
    // The query a derived table is. Empty for a named table; one element otherwise. An array
    // rather than an `Option`, the way `CaseNode.fallback` is, so a `TableRef` need not name the
    // type that contains it.
    query: [SelectStmt]
    // `AS x (a, b)` -- the names the derived table's columns take, which may be shorter than the
    // query's list and rename only its first columns. Empty where none were written.
    column_aliases: [string]
}
```

A table in a `FROM`, with the alias it was given.

### `Join`

```
pub struct Join {
    kind: string
    table: TableRef
    on: Option<Expr>
    using: [string]
}
```

One join onto whatever is to its left. `kind` is `INNER`, `LEFT`, `RIGHT`, `FULL` or `CROSS`,
and a join carries either an `ON` condition or a `USING` column list, never both -- `CROSS`
carries neither.

**A flat list rather than a tree.** `a JOIN b ON .. JOIN c ON ..` is left-deep, which is what
SQL means by it, so a list of joins onto the first table says the same thing as a tree of pairs
and reads better. A parenthesised join expression would need the tree, and is not read here.

### `SubNode`

```
pub struct SubNode {
    query: SelectStmt
}
```

A subquery: a whole `SELECT` nested inside an expression. `Expr` holds a `SelectStmt` and a
`SelectStmt` holds `Expr`s, which is mutual recursion between an enum and a struct -- allowed,
because `Lit` and `Col` end the chain.

### `InSubNode`

```
pub struct InSubNode {
    subject: Expr
    query: SelectStmt
    negated: bool
}
```

### `CastNode`

```
pub struct CastNode {
    inner: Expr
    type_name: string
}
```

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

### `SetOp`

```
pub struct SetOp {
    kind: string
    // `ALL`, `DISTINCT`, or empty where neither was written -- kept as the word it was rather than
    // reduced to a flag. `DISTINCT` is the default and changes nothing, but SQLite *refuses* it
    // after a set operator where PostgreSQL accepts it, so an engine has to be able to tell
    // `UNION DISTINCT` from `UNION`; a formatter that normalised one into the other would turn SQL
    // one engine rejects into SQL it accepts.
    quantifier: string
    query: SelectStmt
}
```

One arm of a set operation, with the operator that joins it to everything on its left. `kind` is
`UNION`, `INTERSECT` or `EXCEPT`; `all` is the `ALL` that keeps duplicates.

**A flat list, as joins are, and for a sharper reason: the grouping is dialect-specific.** The
standard and PostgreSQL bind `INTERSECT` tighter than `UNION` and `EXCEPT`, and SQLite reads all
three strictly left to right -- so `a UNION b INTERSECT c` is two different queries depending on
the engine. A tree built here would have to pick one, and this module has no dialect. The
sequence as written is what it records; `memdb` decides how to group it.

### `set_op_keeps_all`

```
pub fn set_op_keeps_all(op: SetOp) -> bool
```

Whether a set operation keeps duplicates.

### `SelectStmt`

```
pub struct SelectStmt {
    distinct: bool
    items: [SelectItem]
    // Empty when the statement has no `FROM`: `SELECT 1` is legal in both real engines and is the
    // liveness probe `datasource`'s pool sends, so it has to be legal here. Otherwise exactly one
    // element, with `joins` carrying the rest.
    from: [TableRef]
    joins: [Join]
    filter: Option<Expr>
    groups: [Expr]
    having: Option<Expr>
    order: [OrderTerm]
    limit: Option<Expr>
    offset: Option<Expr>
    // The set operations applied to this query, left to right. Empty for a simple `SELECT`.
    //
    // On a *compound* query the head carries them, and the head's `order`, `limit` and `offset`
    // belong to the **whole** compound rather than to its first arm -- which is what SQL means by
    // them, since an arm may not have an `ORDER BY` of its own (`SELECT .. ORDER BY .. UNION ..` is
    // an error in both engines). An arm's own `set_ops`, `order`, `limit` and `offset` are always
    // empty, so a reader can treat an arm as a simple query.
    set_ops: [SetOp]
}
```

### `InsertStmt`

```
pub struct InsertStmt {
    table: string
    columns: [string]
    rows: [[Expr]]
    // `RETURNING a, b` or `RETURNING *`, as select items so an alias and an expression both work.
    // Empty where the statement has no such clause, which is the overwhelming majority.
    returning: [SelectItem]
}
```

### `UpdateStmt`

```
pub struct UpdateStmt {
    table: string
    sets: [Assignment]
    filter: Option<Expr>
    returning: [SelectItem]
}
```

### `DeleteStmt`

```
pub struct DeleteStmt {
    table: string
    filter: Option<Expr>
    returning: [SelectItem]
}
```

### `CreateStmt`

```
pub struct CreateStmt {
    table: string
    columns: [Column]
    // The table-level constraints -- `PRIMARY KEY (a, b)`, `FOREIGN KEY ... REFERENCES ...`,
    // `UNIQUE (..)`, `CHECK (..)`, each with the name a `CONSTRAINT` prefix gave it.
    constraints: [TableConstraint]
    if_not_exists: bool
}
```

### `AlterAction`

```
pub enum AlterAction {
    AddColumn(Column)
    AddConstraint(string)
}
```

What an `ALTER TABLE` does. Two things are read: adding a column, and adding a table-level
constraint -- the second kept as the text it was written as, which is the treatment
`CREATE TABLE`'s table constraints already get and for the same reason.

**An enum rather than a column plus a flag**, because the two cases cannot both be true and a
shape that cannot say otherwise is worth the enum. Before this, `ADD CONSTRAINT k UNIQUE (a)`
read `CONSTRAINT` as the *name of a new column* and wrote itself back as
`ADD COLUMN CONSTRAINT k PRIMARY KEY a` -- the brackets gone, the meaning gone, and the round
trip none the wiser because the wrong tree parses back to itself.

### `AlterStmt`

```
pub struct AlterStmt {
    table: string
    action: AlterAction
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

### `format_constraint`

```
pub fn format_constraint(tc: TableConstraint) -> string
```

A constraint written back. **Derived from the rule rather than echoed from `text`**, so a
constraint built in code formats as well as one that was parsed; `text` is for the `Opaque` rule,
which has nothing else, and for the messages an engine quotes.

### `format`

```
pub fn format(s: Statement) -> string
```

`s` as one line of SQL. `parse` reads back exactly the tree this was written from, which is the
property the fixtures assert over every statement they can find.
