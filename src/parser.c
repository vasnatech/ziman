#include "parser.h"

#include <stdlib.h>
#include <string.h>

#include "error.h"

typedef struct {
    TokenList tokens;
    int pos;
} Parser;

static Token *cur(Parser *p) { return &p->tokens.items[p->pos]; }
static Token *previous(Parser *p) { return &p->tokens.items[p->pos - 1]; }

static Token *advance_tok(Parser *p) {
    if (cur(p)->type != TOK_EOF) p->pos++;
    return previous(p);
}

static int check(Parser *p, TokenType type) { return cur(p)->type == type; }

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance_tok(p);
    return 1;
}

static Token *expect(Parser *p, TokenType type, const char *what) {
    if (check(p, type)) return advance_tok(p);
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected %s, found %s", what, token_type_name(cur(p)->type));
    return NULL; /* unreachable */
}

static char *dup_token_text(Token *t) {
    char *s = malloc((size_t)t->length + 1);
    memcpy(s, t->start, (size_t)t->length);
    s[t->length] = '\0';
    return s;
}

static int starts_upper(const char *name) { return name[0] >= 'A' && name[0] <= 'Z'; }

/* Casing is grammar-significant, not just style: an uppercase identifier
 * immediately followed by '{' is always parsed as a struct literal (see
 * parse_primary), in every position including an `if`/`while` condition.
 * That's only sound if a value/function name can never start uppercase and
 * a struct name always does -- so both directions are enforced here, not
 * just documented. */
static void require_lower_start(Token *t, const char *what) {
    if (starts_upper(t->start)) {
        ziman_error("syntax", t->line, t->col, "%s must not start with an uppercase letter (uppercase is reserved for type names)", what);
    }
}

static void require_upper_start(Token *t, const char *what) {
    if (!starts_upper(t->start)) {
        ziman_error("syntax", t->line, t->col, "%s must start with an uppercase letter", what);
    }
}

/* Skips zero or more consecutive statement-separator newlines. */
static void skip_newlines(Parser *p) {
    while (match(p, TOK_NEWLINE)) {}
}

static Type parse_type(Parser *p) {
    if (match(p, TOK_KW_INT)) return type_scalar(TY_INT);
    if (match(p, TOK_KW_FLOAT)) return type_scalar(TY_FLOAT);
    if (match(p, TOK_KW_BOOL)) return type_scalar(TY_BOOL);
    if (match(p, TOK_KW_STRING)) return type_scalar(TY_STRING);
    if (match(p, TOK_LBRACKET)) {
        Type elem = parse_type(p);
        expect(p, TOK_RBRACKET, "']'");
        return type_array(elem);
    }
    if (check(p, TOK_IDENT)) {
        Token *t = advance_tok(p);
        require_upper_start(t, "a type name");
        char *name = dup_token_text(t);
        Type ty = type_struct(name);
        free(name); /* type_struct() dups it into the Type itself */
        return ty;
    }
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected a type, found %s", token_type_name(cur(p)->type));
    return type_scalar(TY_VOID); /* unreachable */
}

static Expr *parse_expr(Parser *p);

static ExprList parse_comma_separated(Parser *p, TokenType stop) {
    ExprList list = {0};
    if (!check(p, stop)) {
        expr_list_push(&list, parse_expr(p));
        while (match(p, TOK_COMMA)) {
            expr_list_push(&list, parse_expr(p));
        }
    }
    return list;
}

/* `name: value` pairs, comma-separated -- used by both a struct literal's
 * fields and a `with` expression's overrides. */
static FieldInitList parse_field_inits(Parser *p, TokenType stop) {
    FieldInitList list = {0};
    if (!check(p, stop)) {
        for (;;) {
            Token *name_tok = expect(p, TOK_IDENT, "a field name");
            expect(p, TOK_COLON, "':'");
            Expr *value = parse_expr(p);
            FieldInit fi;
            fi.name = dup_token_text(name_tok);
            fi.value = value;
            fi.line = name_tok->line;
            fi.col = name_tok->col;
            field_init_list_push(&list, fi);
            if (!match(p, TOK_COMMA)) break;
        }
    }
    return list;
}

static Expr *parse_postfix(Parser *p);

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);

    if (match(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT, t->line, t->col);
        e->as.int_val = strtoll(t->start, NULL, 10);
        return e;
    }
    if (match(p, TOK_FLOAT_LIT)) {
        Expr *e = expr_new(EXPR_FLOAT, t->line, t->col);
        e->as.float_val = strtod(t->start, NULL);
        return e;
    }
    if (match(p, TOK_STRING_LIT)) {
        Expr *e = expr_new(EXPR_STRING, t->line, t->col);
        e->as.string_val = (char *)t->start; /* lexer already gave us a private, NUL-terminated buffer */
        return e;
    }
    if (match(p, TOK_TRUE)) {
        Expr *e = expr_new(EXPR_BOOL, t->line, t->col);
        e->as.bool_val = 1;
        return e;
    }
    if (match(p, TOK_FALSE)) {
        Expr *e = expr_new(EXPR_BOOL, t->line, t->col);
        e->as.bool_val = 0;
        return e;
    }
    if (match(p, TOK_IDENT)) {
        char *name = dup_token_text(t);
        if (match(p, TOK_LPAREN)) {
            ExprList args = parse_comma_separated(p, TOK_RPAREN);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.args = args;
            return e;
        }
        /* An uppercase identifier immediately followed by '{' is always a
         * struct literal -- in every position, including an `if`/`while`
         * condition, never just here. See require_lower_start/
         * require_upper_start for the enforcement that makes this sound. */
        if (check(p, TOK_LBRACE) && starts_upper(name)) {
            advance_tok(p); /* '{' */
            FieldInitList fields = parse_field_inits(p, TOK_RBRACE);
            expect(p, TOK_RBRACE, "'}'");
            Expr *e = expr_new(EXPR_STRUCT_LIT, t->line, t->col);
            e->as.struct_lit.type_name = name;
            e->as.struct_lit.fields = fields;
            return e;
        }
        Expr *e = expr_new(EXPR_IDENT, t->line, t->col);
        e->as.ident_name = name;
        return e;
    }
    if (match(p, TOK_LPAREN)) {
        Expr *inner = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        return inner;
    }
    if (match(p, TOK_LBRACKET)) {
        ExprList elements = parse_comma_separated(p, TOK_RBRACKET);
        expect(p, TOK_RBRACKET, "']'");
        Expr *e = expr_new(EXPR_ARRAY_LIT, t->line, t->col);
        e->as.array_lit.elements = elements;
        return e;
    }
    if (match(p, TOK_WITH)) {
        Expr *base = parse_postfix(p);
        expect(p, TOK_LBRACE, "'{'");
        FieldInitList overrides = parse_field_inits(p, TOK_RBRACE);
        expect(p, TOK_RBRACE, "'}'");
        Expr *e = expr_new(EXPR_WITH, t->line, t->col);
        e->as.with_expr.base = base;
        e->as.with_expr.overrides = overrides;
        return e;
    }

    ziman_error("syntax", t->line, t->col, "expected an expression, found %s", token_type_name(t->type));
    return NULL; /* unreachable */
}

/* `primary` followed by any number of `[index]`/`.field` suffixes, e.g.
 * `matrix[i][j]` or `shapes[i].corner.x`, freely mixed. */
static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            Token *br = advance_tok(p);
            Expr *idx = parse_expr(p);
            expect(p, TOK_RBRACKET, "']'");
            Expr *indexed = expr_new(EXPR_INDEX, br->line, br->col);
            indexed->as.index.array = e;
            indexed->as.index.index = idx;
            e = indexed;
        } else if (check(p, TOK_DOT)) {
            Token *dot = advance_tok(p);
            Token *field_tok = expect(p, TOK_IDENT, "a field name");
            Expr *access = expr_new(EXPR_FIELD_ACCESS, dot->line, dot->col);
            access->as.field_access.base = e;
            access->as.field_access.field_name = dup_token_text(field_tok);
            e = access;
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    if (check(p, TOK_BANG) || check(p, TOK_MINUS)) {
        Token *t = advance_tok(p);
        Op op = t->type == TOK_BANG ? OP_NOT : OP_NEG;
        Expr *operand = parse_unary(p);
        Expr *e = expr_new(EXPR_UNARY, t->line, t->col);
        e->as.unary.op = op;
        e->as.unary.operand = operand;
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_cast(Parser *p) {
    Expr *e = parse_unary(p);
    while (match(p, TOK_AS)) {
        Token *t = previous(p);
        Type target = parse_type(p);
        Expr *cast = expr_new(EXPR_CAST, t->line, t->col);
        cast->as.cast.operand = e;
        cast->as.cast.target_type = target;
        e = cast;
    }
    return e;
}

static Expr *parse_binary_level(Parser *p, Expr *(*next)(Parser *), const TokenType *ops, const Op *op_kinds, int op_count) {
    Expr *left = next(p);
    for (;;) {
        int matched = -1;
        for (int i = 0; i < op_count; i++) {
            if (check(p, ops[i])) { matched = i; break; }
        }
        if (matched < 0) break;
        Token *t = advance_tok(p);
        Expr *right = next(p);
        Expr *e = expr_new(EXPR_BINARY, t->line, t->col);
        e->as.binary.op = op_kinds[matched];
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static Expr *parse_factor(Parser *p) {
    static const TokenType ops[] = {TOK_STAR, TOK_SLASH, TOK_PERCENT};
    static const Op kinds[] = {OP_MUL, OP_DIV, OP_MOD};
    return parse_binary_level(p, parse_cast, ops, kinds, 3);
}

static Expr *parse_term(Parser *p) {
    static const TokenType ops[] = {TOK_PLUS, TOK_MINUS};
    static const Op kinds[] = {OP_ADD, OP_SUB};
    return parse_binary_level(p, parse_factor, ops, kinds, 2);
}

static Expr *parse_comparison(Parser *p) {
    static const TokenType ops[] = {TOK_LT, TOK_GT, TOK_LTEQ, TOK_GTEQ};
    static const Op kinds[] = {OP_LT, OP_GT, OP_LE, OP_GE};
    return parse_binary_level(p, parse_term, ops, kinds, 4);
}

static Expr *parse_equality(Parser *p) {
    static const TokenType ops[] = {TOK_EQEQ, TOK_BANGEQ};
    static const Op kinds[] = {OP_EQ, OP_NEQ};
    return parse_binary_level(p, parse_comparison, ops, kinds, 2);
}

static Expr *parse_logic_and(Parser *p) {
    static const TokenType ops[] = {TOK_AMPAMP};
    static const Op kinds[] = {OP_AND};
    return parse_binary_level(p, parse_equality, ops, kinds, 1);
}

static Expr *parse_logic_or(Parser *p) {
    static const TokenType ops[] = {TOK_PIPEPIPE};
    static const Op kinds[] = {OP_OR};
    return parse_binary_level(p, parse_logic_and, ops, kinds, 1);
}

static Expr *parse_expr(Parser *p) { return parse_logic_or(p); }

static Stmt *parse_statement(Parser *p);

static Stmt *parse_block(Parser *p) {
    Token *open = expect(p, TOK_LBRACE, "'{'");
    Stmt *block = stmt_new(STMT_BLOCK, open->line, open->col);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        stmt_list_push(&block->as.block.stmts, parse_statement(p));
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return block;
}

static Stmt *parse_decl(Parser *p, StmtKind kind) {
    Token *kw = previous(p);
    Token *name_tok = expect(p, TOK_IDENT, "an identifier");
    require_lower_start(name_tok, "a variable name");
    Stmt *s = stmt_new(kind, kw->line, kw->col);
    s->as.decl.name = dup_token_text(name_tok);
    if (match(p, TOK_COLON)) {
        s->as.decl.declared_type = parse_type(p);
        s->as.decl.has_declared_type = 1;
    }
    expect(p, TOK_EQ, "'='");
    s->as.decl.init = parse_expr(p);
    return s;
}

static Stmt *parse_if(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_IF, kw->line, kw->col);
    s->as.if_stmt.cond = parse_expr(p);
    s->as.if_stmt.then_block = parse_block(p);
    /* `else { ... }` and `else if ... { ... }` chains stay on the same
     * line as the closing brace before them, so no newline-skipping here. */
    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) {
            s->as.if_stmt.else_block = parse_if(p);
        } else {
            s->as.if_stmt.else_block = parse_block(p);
        }
    }
    return s;
}

static Stmt *parse_while(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_WHILE, kw->line, kw->col);
    s->as.while_stmt.cond = parse_expr(p);
    s->as.while_stmt.body = parse_block(p);
    return s;
}

static Stmt *parse_return(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_RETURN, kw->line, kw->col);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_RBRACE)) {
        s->as.ret.value = parse_expr(p);
    }
    return s;
}

static Stmt *parse_statement(Parser *p) {
    if (match(p, TOK_VAL)) return parse_decl(p, STMT_VAL);
    if (match(p, TOK_VAR)) return parse_decl(p, STMT_VAR);
    if (match(p, TOK_IF)) return parse_if(p);
    if (match(p, TOK_WHILE)) return parse_while(p);
    if (match(p, TOK_RETURN)) return parse_return(p);

    /* `IDENT = expr` (assignment) vs. any other expression statement
     * (currently only calls are useful here, e.g. `print(x)`). Both start
     * with an expression, so parse one and see what follows. */
    Token *start = cur(p);
    Expr *first = parse_expr(p);
    if (match(p, TOK_EQ)) {
        if (first->kind != EXPR_IDENT) {
            ziman_error("syntax", start->line, start->col, "left-hand side of assignment must be a variable name");
        }
        Stmt *s = stmt_new(STMT_ASSIGN, start->line, start->col);
        s->as.assign.name = first->as.ident_name;
        s->as.assign.value = parse_expr(p);
        return s;
    }
    Stmt *s = stmt_new(STMT_EXPR, start->line, start->col);
    s->as.expr_stmt.expr = first;
    return s;
}

static ParamList parse_params(Parser *p) {
    ParamList params = {0};
    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            Token *param_name = expect(p, TOK_IDENT, "a parameter name");
            require_lower_start(param_name, "a parameter name");
            expect(p, TOK_COLON, "':'");
            Type param_type = parse_type(p);
            Param param = {dup_token_text(param_name), param_type, param_name->line, param_name->col};
            param_list_push(&params, param);
            if (!match(p, TOK_COMMA)) break;
        }
    }
    return params;
}

static FunctionDecl *parse_function(Parser *p) {
    Token *kw = expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    require_lower_start(name_tok, "a function name");

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->line = kw->line;
    fn->col = kw->col;

    expect(p, TOK_LPAREN, "'('");
    fn->params = parse_params(p);
    expect(p, TOK_RPAREN, "')'");

    fn->return_type = type_scalar(TY_VOID);
    if (match(p, TOK_ARROW)) {
        fn->return_type = parse_type(p);
    }

    fn->body = parse_block(p);
    return fn;
}

/* `native fn name(params) -> type`, no body at all -- its implementation is
 * a C function registered by module path + name, looked up at call time.
 * The type checker treats its signature exactly like any other function's
 * (see check_function's `is_native` early-return); the interpreter reports
 * a clear "no registered implementation" error if nothing was registered
 * for it (see interpreter.c) -- no real native module exists to back one
 * yet, this is the language mechanism only. */
static FunctionDecl *parse_native_function(Parser *p) {
    Token *kw = expect(p, TOK_NATIVE, "'native'");
    expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    require_lower_start(name_tok, "a function name");

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->is_native = 1;
    fn->line = kw->line;
    fn->col = kw->col;

    expect(p, TOK_LPAREN, "'('");
    fn->params = parse_params(p);
    expect(p, TOK_RPAREN, "')'");

    fn->return_type = type_scalar(TY_VOID);
    if (match(p, TOK_ARROW)) {
        fn->return_type = parse_type(p);
    }
    return fn;
}

static StructDecl *parse_struct_decl(Parser *p) {
    Token *kw = expect(p, TOK_STRUCT, "'struct'");
    Token *name_tok = expect(p, TOK_IDENT, "a struct name");
    require_upper_start(name_tok, "a struct name");

    StructDecl *sd = calloc(1, sizeof(StructDecl));
    sd->name = dup_token_text(name_tok);
    sd->line = kw->line;
    sd->col = kw->col;

    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        Token *field_name = expect(p, TOK_IDENT, "a field name");
        require_lower_start(field_name, "a field name");
        expect(p, TOK_COLON, "':'");
        Type field_type = parse_type(p);
        Param field = {dup_token_text(field_name), field_type, field_name->line, field_name->col};
        param_list_push(&sd->fields, field);
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return sd;
}

Module *parse(TokenList tokens) {
    Parser p = {tokens, 0};
    Module *module = calloc(1, sizeof(Module));

    skip_newlines(&p);
    while (!check(&p, TOK_EOF)) {
        int is_public = match(&p, TOK_PUB);
        if (check(&p, TOK_STRUCT)) {
            StructDecl *sd = parse_struct_decl(&p);
            sd->is_public = is_public;
            struct_list_push(&module->structs, sd);
        } else if (check(&p, TOK_NATIVE)) {
            FunctionDecl *fn = parse_native_function(&p);
            fn->is_public = is_public;
            function_list_push(&module->functions, fn);
        } else {
            FunctionDecl *fn = parse_function(&p);
            fn->is_public = is_public;
            function_list_push(&module->functions, fn);
        }
        skip_newlines(&p);
    }

    return module;
}
