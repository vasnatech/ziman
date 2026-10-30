# Lexical rules and grammar

Part of the [v0 syntax reference](README.md).

## Lexical rules

- Comments: `// to end of line`. No block comments in v0.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Casing is **enforced, not just conventional**: a
  `val`/`var`/parameter/function name must not start uppercase, and a `struct`/`enum`/
  enum-variant name must -- this is what makes struct-literal and enum-access parsing
  unambiguous everywhere a primary expression can appear, including an `if`/`while` condition.
- Integer literals: `[0-9]+` (decimal only in v0).
- Float literals: `[0-9]+\.[0-9]+` (a digit is required on both sides of
  the dot — `.5` and `5.` are not valid literals), optionally with an exponent:
  `[0-9]+(\.[0-9]+)?[eE][+-]?[0-9]+` (`1e3`, `2.5E-2`, `1.5e-10`). An exponent
  makes the literal a `float` even without a point (`1e3` is `1000.0`), and it is
  only read when digits follow the `e` (`1e` is still `1` and a name). An
  exponent literal takes no `n`/`d`/`b` suffix (`1e3d` is a lexical error): write
  a decimal as `15000d` or `1500.5d`.
- String literals: `"..."` with escapes `\n`, `\t`, `\r`, `\"`, `\\`. No
  interpolation in v0. A plain string cannot span lines -- but one **literal** can be written over
  several lines, with a `\` after each piece but the last:

  ```
  return "Result<E, DbError> (one row, NotFound when there is none), " \
      "Result<Option<E>, DbError> (one row, absence being the answer) or " \
      "Result<[E], DbError> (every matching row)"
  ```

  The pieces are joined end to end, in the lexer, with nothing added between them -- a space at a
  seam is written by whoever wants one. **The result is one literal**, which is what `format`
  needs: its first argument must be a literal, so a long format string had no way to be wrapped
  before this, and `concat` of constant pieces is a call and an allocation to build a constant.
  A text block continues the same way, since the rule is about literals and not about quoting.

  **The `\` is required rather than inferred from adjacency**, which is what C and Python do. There
  two string literals on separate lines are one string, so a forgotten comma in a vertical list of
  strings silently joins two of them; here that is a syntax error and stays one. A `\` that follows
  anything but a string literal, or that is not followed by one, is refused where it is written.
- Text blocks: `"""` at the end of a line opens a multi-line string, and the next `"""` closes it.

  ```
  val query = """
      SELECT id, name
        FROM person
       WHERE name = "Ada"
      """
  ```

  The newline after the opening `"""` and the one before the closing `"""` are not part of the
  value, so `query` above is the three lines `SELECT id, name`, `  FROM person`,
  `WHERE name = "Ada"` with no trailing newline (add a blank line before the closing `"""` for
  one). The indentation shared by every non-blank line, and by the closing `"""` when it sits on a
  line of its own, is removed, so the block can be indented with the code around it; a smaller
  indent on the closing line strips less. Blank lines stay empty and trailing blanks on each
  line are dropped. The escapes are the same as in a plain string (`\"""` puts three quotes in
  the text); a `"` or `""` needs none. `ziman --fmt` leaves a text block exactly as written.
- Booleans: `true`, `false`.
- Statement termination: a newline ends a statement unless the token before it can't end one
  (a binary operator, `,`, `(`, `=`, or a keyword expecting more), checked against the previous
  token only -- never indentation. In practice: write code the way you'd write Go.
- **Every comma-separated list can span multiple lines** -- a struct literal's fields, a `with`
  override list, a call's arguments, an array literal's elements, a declared parameter list all
  tolerate a line break after each item/comma, the same tolerance a `struct`/`enum`
  *declaration* already had. A trailing comma before the close (`Point { x: 1, y: 2, }`) is
  still a syntax error -- only the line break is tolerated, not an extra comma.

## Grammar (informal EBNF)

```ziman ignore: a grammar, not Ziman
program        := { use_decl | top_level_decl }   (* use may appear anywhere
                     at the top level, though conventionally listed first *)
top_level_decl := [ "pub" ] ( function_decl | native_fn_decl | struct_decl | enum_decl | global_val_decl )

global_val_decl := "val" IDENT [ ":" type ] "=" expr
                    (* a module-wide constant -- see "Modules" for
                       evaluation order and why there's no global "var" *)

use_decl       := "use" IDENT { "/" IDENT } [ "as" IDENT ]
function_decl  := "fn" IDENT "(" [ param_list ] ")" [ "->" type ] block
native_fn_decl := "native" "fn" IDENT "(" [ param_list ] ")" [ "->" type ]
                    (* no body at all -- see "Modules" *)
struct_decl    := "struct" TYPE_IDENT "{" NEWLINE* { field NEWLINE* } "}"
field          := IDENT ":" type                  (* same shape as param *)
enum_decl      := "enum" TYPE_IDENT "{" NEWLINE* { variant NEWLINE* } "}"
variant        := TYPE_IDENT [ "(" type ")" ]     (* zero or one payload;
                    see "Enums" for why never more than one *)
param_list     := param { "," param }
param          := IDENT ":" [ "variadic" ] type
                    (* "variadic" marks a variadic parameter, only legal on
                       the last param in the list -- see "Functions" --
                       "Variadic parameters" below *)
type           := "int" | "float" | "bool" | "string" | "[" type "]"
                 | TYPE_IDENT | IDENT "." TYPE_IDENT   (* qualifier.Type *)
                 | "Result" "<" type "," type ">"      (* Result<Ok, Err> *)
                 | "Channel" "<" type ">"              (* Channel<T> *)
                    (* a bare TYPE_IDENT names a struct or an enum -- the
                       grammar can't tell which (both are just PascalCase
                       identifiers); it resolves to whichever one is
                       actually declared under that name. See ast.h's Type
                       comment for how the compiler resolves this. *)

block          := "{" NEWLINE* { statement NEWLINE* } "}"

statement      := val_decl | var_decl | assign_stmt
                 | if_stmt | while_stmt | for_stmt
                 | break_stmt | continue_stmt | spawn_stmt | return_stmt | expr_stmt

val_decl       := "val" IDENT [ ":" type ] "=" expr
var_decl       := "var" IDENT [ ":" type ] "=" expr
assign_stmt    := IDENT "=" expr
if_stmt        := "if" expr block [ "else" (if_stmt | block) ]
while_stmt     := "while" expr block
for_stmt       := "for" IDENT [ "," IDENT ] "in" expr [ ".." expr ] block
                    (* ".." is only legal here, directly after the first
                       expr -- not a general expression form. See "for
                       loops" below for what each binder count/".." combo
                       means. *)
break_stmt     := "break"      (* only legal inside a while/for body --
                                   see "For loops" below *)
continue_stmt  := "continue"   (* same restriction as break_stmt *)
spawn_stmt     := "spawn" IDENT [ "." IDENT ] "(" [ arg_list ] ")"
                    (* the named function is resolved statically, exactly
                       like an ordinary call's callee -- never a
                       function value or closure. See "Concurrency". *)
return_stmt    := "return" [ expr ]
expr_stmt      := expr

expr           := logic_or
logic_or       := logic_and { "||" logic_and }
logic_and      := equality { "&&" equality }
equality       := comparison { ("==" | "!=") comparison }
comparison     := term { ("<" | ">" | "<=" | ">=") term }
term           := factor { ("+" | "-") factor }
factor         := cast { ("*" | "/" | "%") cast }
cast           := postfix [ "as" type ]
postfix        := primary { ("[" expr "]") | ("." IDENT) }   (* indexing and
                    field access, freely mixed and chained: matrix[i][j],
                    shapes[i].corner.x *)
unary          := ( "!" | "-" ) unary | postfix
call           := primary { "(" [ arg_list ] ")" }
arg_list       := expr { "," expr }
primary        := INT_LIT | FLOAT_LIT | STRING_LIT | "true" | "false"
                 | IDENT | qualified_call | qualified_struct_lit | enum_access
                 | field_accessor
                 | "(" expr ")" | array_lit | struct_lit | with_expr
field_accessor := [ IDENT "." ] TYPE_IDENT "::" IDENT
                    (* `Row::dept`, or `model.Item::qty` for a qualified type:
                       a function value answering that field. One field only.
                       See "Function values". *)
array_lit      := "[" [ expr { "," expr } ] "]"
struct_lit     := TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
qualified_call        := IDENT "." IDENT "(" [ arg_list ] ")"
qualified_struct_lit  := IDENT "." TYPE_IDENT "{" [ field_init { "," field_init } ] "}"
enum_access    := TYPE_IDENT "." IDENT [ "(" [ arg_list ] ")" ]
                    (* `Shape.Circle(5.0)`/`Shape.Point` (construction) or
                       `Shape.is_Circle(s)`/`Shape.unwrap_Circle(s)` (a
                       generated consumption call) -- same shape either
                       way, disambiguated by the type checker rather than
                       the grammar. See "Enums". *)
with_expr      := "with" postfix "{" [ field_init { "," field_init } ] "}"
field_init     := IDENT ":" expr

(* TYPE_IDENT is an IDENT starting with an uppercase letter. A struct literal is recognized
   purely by that casing in every position a primary expression can appear, including an
   `if`/`while` condition -- no restricted position the way Rust needs for bare struct literals.

   qualified_call/qualified_struct_lit/enum_access look identical to plain field access
   (IDENT "." IDENT) until the parser sees what follows the second identifier and, for
   enum_access, the first one's casing -- falling back to ordinary `.field` access (postfix)
   if none match. A module qualifier is always lowercase (enforced on `use ... as`), so an
   uppercase first identifier can only mean enum_access. *)
```

Left out of v0 (tracked in docs/roadmap/): a call as a primary only chains off a plain or
single-qualified identifier, never an arbitrary expression (a function *value* is called
through a local name -- `make()(x)` isn't valid); no field *assignment* (`p.x = 1` isn't valid,
`p = with p { x: 1 }` is). `arr[i] = x` *is* valid, on an array through a plain name and on a
`MutableArray` -- see "Arrays" below. `if`/`switch` *are* usable as expressions in a
restricted set of positions -- see "`if` and `switch` as values" below.
