#ifndef ZIMAN_INTERPRETER_H
#define ZIMAN_INTERPRETER_H

#include "ast.h"

/* Runs `main()`. Assumes `program` already passed typecheck(). Exits the
 * process with a runtime error (line:col + message) on things the type
 * checker can't catch statically (division by zero, etc). */
void interpret(Program *program);

#endif
