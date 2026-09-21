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

/* Skips zero or more consecutive statement-separator newlines. */
static void skip_newlines(Parser *p) {
    while (match(p, TOK_NEWLINE)) {}
}

static Type parse_type(Parser *p) {
    if (match(p, TOK_KW_INT)) return TY_INT;
    if (match(p, TOK_KW_FLOAT)) return TY_FLOAT;
    if (match(p, TOK_KW_BOOL)) return TY_BOOL;
    if (match(p, TOK_KW_STRING)) return TY_STRING;
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected a type, found %s", token_type_name(cur(p)->type));
    return TY_VOID; /* unreachable */
}

static Expr *parse_expr(Parser *p);

static ExprList parse_arg_list(Parser *p) {
    ExprList args = {0};
    if (!check(p, TOK_RPAREN)) {
        expr_list_push(&args, parse_expr(p));
        while (match(p, TOK_COMMA)) {
            expr_list_push(&args, parse_expr(p));
        }
    }
    return args;
}

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
            ExprList args = parse_arg_list(p);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.args = args;
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

    ziman_error("syntax", t->line, t->col, "expected an expression, found %s", token_type_name(t->type));
    return NULL; /* unreachable */
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
    return parse_primary(p);
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

static FunctionDecl *parse_function(Parser *p) {
    Token *kw = expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->line = kw->line;
    fn->col = kw->col;

    expect(p, TOK_LPAREN, "'('");
    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            Token *param_name = expect(p, TOK_IDENT, "a parameter name");
            expect(p, TOK_COLON, "':'");
            Type param_type = parse_type(p);
            Param param = {dup_token_text(param_name), param_type, param_name->line, param_name->col};
            param_list_push(&fn->params, param);
            if (!match(p, TOK_COMMA)) break;
        }
    }
    expect(p, TOK_RPAREN, "')'");

    fn->return_type = TY_VOID;
    if (match(p, TOK_ARROW)) {
        fn->return_type = parse_type(p);
    }

    fn->body = parse_block(p);
    return fn;
}

Program *parse(TokenList tokens) {
    Parser p = {tokens, 0};
    Program *program = calloc(1, sizeof(Program));

    skip_newlines(&p);
    while (!check(&p, TOK_EOF)) {
        function_list_push(&program->functions, parse_function(&p));
        skip_newlines(&p);
    }

    return program;
}
