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

/* The `//` comments the most recent lex() passed over, in source order: the line each sat on and a
 * pointer into that source at the text after the slashes. The parser still drops comments -- these
 * are recorded beside the token stream, not attached to declarations -- and exist for `ziman --api`,
 * which prints a declaration's doc comment by walking back from its line. That is there because of
 * a measurement: round 3 of the 2026-10-12 benchmark had one task answer wrongly and another spend
 * 14 extra compiles hedging, both over behaviour their function's own doc comment states and
 * `--api` did not show (docs/reviews/tasks/ F89). Valid until the next lex().  */
typedef struct {
    int line;
    const char *text;  /* into the lexed source; not NUL-terminated at the line end */
    int length;
} CommentLine;

const CommentLine *lex_comments(int *count);

#endif
