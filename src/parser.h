#ifndef ZIMAN_PARSER_H
#define ZIMAN_PARSER_H

#include "ast.h"
#include "lexer.h"

/* Parses a full token stream into a Module. Exits the process with a
 * syntax error (line:col + message) on malformed input. */
Module *parse(TokenList tokens);

#endif
