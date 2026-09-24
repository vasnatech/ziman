#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const char *g_error_context = NULL;

void ziman_set_error_context(const char *context) {
    g_error_context = context;
}

const char *ziman_error_context(void) {
    return g_error_context;
}

void ziman_error(const char *phase, int line, int col, const char *fmt, ...) {
    if (g_error_context != NULL) {
        fprintf(stderr, "ziman: %s error at %d:%d (in %s): ", phase, line, col, g_error_context);
    } else {
        fprintf(stderr, "ziman: %s error at %d:%d: ", phase, line, col);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    exit(1);
}
