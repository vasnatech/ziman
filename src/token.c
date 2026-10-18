#include "token.h"

#include <string.h>

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF: return "end of file";
        case TOK_NEWLINE: return "newline";
        case TOK_INT_LIT: return "integer literal";
        case TOK_FLOAT_LIT: return "float literal";
        case TOK_BIGINT_LIT: return "bigint literal";
        case TOK_DECIMAL_LIT: return "decimal literal";
        case TOK_BYTE_LIT: return "byte literal";
        case TOK_STRING_LIT: return "string literal";
        case TOK_IDENT: return "identifier";
        case TOK_FN: return "'fn'";
        case TOK_VAL: return "'val'";
        case TOK_VAR: return "'var'";
        case TOK_IF: return "'if'";
        case TOK_ELSE: return "'else'";
        case TOK_WHILE: return "'while'";
        case TOK_FOR: return "'for'";
        case TOK_IN: return "'in'";
        case TOK_BREAK: return "'break'";
        case TOK_CONTINUE: return "'continue'";
        case TOK_SPAWN: return "'spawn'";
        case TOK_DEFER: return "'defer'";
        case TOK_VARIADIC: return "'variadic'";
        case TOK_TRY: return "'try'";
        case TOK_SWITCH: return "'switch'";
        case TOK_RETURN: return "'return'";
        case TOK_TRUE: return "'true'";
        case TOK_FALSE: return "'false'";
        case TOK_AS: return "'as'";
        case TOK_STRUCT: return "'struct'";
        case TOK_ENUM: return "'enum'";
        case TOK_WITH: return "'with'";
        case TOK_PUB: return "'pub'";
        case TOK_USE: return "'use'";
        case TOK_NATIVE: return "'native'";
        case TOK_BUILTIN: return "'builtin'";
        case TOK_RESULT: return "'Result'";
        case TOK_CHANNEL: return "'Channel'";
        case TOK_MAP: return "'Map'";
        case TOK_SET: return "'Set'";
        case TOK_KW_INT: return "'int'";
        case TOK_KW_FLOAT: return "'float'";
        case TOK_KW_BOOL: return "'bool'";
        case TOK_KW_STRING: return "'string'";
        case TOK_KW_BIGINT: return "'bigint'";
        case TOK_KW_DECIMAL: return "'decimal'";
        case TOK_KW_BYTE: return "'byte'";
        case TOK_LPAREN: return "'('";
        case TOK_RPAREN: return "')'";
        case TOK_LBRACE: return "'{'";
        case TOK_RBRACE: return "'}'";
        case TOK_LBRACKET: return "'['";
        case TOK_RBRACKET: return "']'";
        case TOK_COMMA: return "','";
        case TOK_COLON: return "':'";
        case TOK_DOT: return "'.'";
        case TOK_DOTDOT: return "'..'";
        case TOK_ARROW: return "'->'";
        case TOK_QUESTION: return "'?'";
        case TOK_HASH: return "'#'";
        case TOK_PLUS: return "'+'";
        case TOK_MINUS: return "'-'";
        case TOK_STAR: return "'*'";
        case TOK_SLASH: return "'/'";
        case TOK_PERCENT: return "'%'";
        case TOK_EQ: return "'='";
        case TOK_EQEQ: return "'=='";
        case TOK_BANGEQ: return "'!='";
        case TOK_LT: return "'<'";
        case TOK_GT: return "'>'";
        case TOK_LTEQ: return "'<='";
        case TOK_GTEQ: return "'>='";
        case TOK_AMPAMP: return "'&&'";
        case TOK_PIPEPIPE: return "'||'";
        case TOK_BANG: return "'!'";
    }
    return "unknown token";
}

static const char *const FOREIGN_DECL_KEYWORDS[] = {"let", "const", "mut", "auto"};

const char *ziman_foreign_decl_keyword(const char *name, int length) {
    for (size_t i = 0; i < sizeof FOREIGN_DECL_KEYWORDS / sizeof FOREIGN_DECL_KEYWORDS[0]; i++) {
        const char *word = FOREIGN_DECL_KEYWORDS[i];
        if ((int)strlen(word) == length && strncmp(word, name, (size_t)length) == 0) return word;
    }
    return NULL;
}
