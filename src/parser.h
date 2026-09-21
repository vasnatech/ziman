#ifndef ZIMAN_PARSER_H
#define ZIMAN_PARSER_H

#include "ast.h"
#include "lexer.h"

/* Parses a full token stream into a Program. Exits the process with a
 * syntax error (line:col + message) on malformed input. */
Program *parse(TokenList tokens);

#endif
