#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void ziman_error(const char *phase, int line, int col, const char *fmt, ...) {
    fprintf(stderr, "ziman: %s error at %d:%d: ", phase, line, col);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    exit(1);
}
