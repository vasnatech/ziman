#ifndef ZIMAN_LEXER_H
#define ZIMAN_LEXER_H

#include "token.h"

typedef struct {
    Token *items;
    int count;
    int capacity;
} TokenList;

/* Tokenizes source (must stay alive as long as the tokens are used --
 * Token.start points into it). Exits the process with a lexical error
 * on malformed input. */
TokenList lex(const char *source);

void token_list_free(TokenList *list);

#endif
