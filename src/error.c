#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_error_context = NULL;
static int g_json_errors = 0;

void ziman_set_error_context(const char *context) {
    g_error_context = context;
}

const char *ziman_error_context(void) {
    return g_error_context;
}

void ziman_set_json_errors(int on) {
    g_json_errors = on;
}

/* Writes `s` as a JSON string literal (quotes included). Bytes >= 0x80 pass
 * through, so UTF-8 in a message stays UTF-8. */
static void print_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
        }
    }
    fputc('"', out);
}

void ziman_error(const char *phase, int line, int col, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *message = malloc((size_t)(n > 0 ? n : 0) + 1);
    vsnprintf(message, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);

    if (g_json_errors) {
        fprintf(stderr, "{\"severity\":\"error\",\"phase\":");
        print_json_string(stderr, phase);
        fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":", line, col);
        if (g_error_context != NULL) print_json_string(stderr, g_error_context);
        else fputs("null", stderr);
        fputs(",\"message\":", stderr);
        print_json_string(stderr, message);
        fputs("}\n", stderr);
    } else if (g_error_context != NULL) {
        fprintf(stderr, "ziman: %s error at %d:%d (in %s): %s\n", phase, line, col, g_error_context, message);
    } else {
        fprintf(stderr, "ziman: %s error at %d:%d: %s\n", phase, line, col, message);
    }
    exit(1);
}

void ziman_warning(const char *file, int line, int col, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *message = malloc((size_t)(n > 0 ? n : 0) + 1);
    vsnprintf(message, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);

    if (g_json_errors) {
        fputs("{\"severity\":\"warning\",\"phase\":\"lint\",\"file\":", stderr);
        print_json_string(stderr, file);
        fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":null,\"message\":", line, col);
        print_json_string(stderr, message);
        fputs("}\n", stderr);
    } else {
        fprintf(stderr, "%s:%d:%d: warning: %s\n", file, line, col, message);
    }
    free(message);
}
