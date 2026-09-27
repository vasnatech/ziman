#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_error_context = NULL;
static int g_json_errors = 0;
static const char *g_error_file = NULL;
static const char *g_entry_file = NULL;
static int g_error_count = 0;
#define MAX_ERRORS 25
#define MAX_RECOVERY_DEPTH 512
static jmp_buf *g_recovery[MAX_RECOVERY_DEPTH];
static int g_recovery_depth = 0;

static ZimanErrorSink g_sink = NULL;
void ziman_set_error_sink(ZimanErrorSink sink) { g_sink = sink; }
void ziman_reset_errors(void) { g_error_count = 0; g_recovery_depth = 0; g_error_file = NULL; g_error_context = NULL; }

void ziman_set_error_file(const char *file) { g_error_file = file; }
void ziman_set_entry_file(const char *file) { g_entry_file = file; }
int ziman_error_count(void) { return g_error_count; }

void ziman_recovery_push(jmp_buf *jb) {
    if (g_recovery_depth < MAX_RECOVERY_DEPTH) g_recovery[g_recovery_depth] = jb;
    g_recovery_depth++;
}

void ziman_recovery_pop(void) {
    if (g_recovery_depth > 0) g_recovery_depth--;
}

/* Only points that fit in the table are real; a deeper push is ignored, so an
 * error there jumps to the deepest real one. */
static void jump_to_recovery(void) {
    int depth = g_recovery_depth > MAX_RECOVERY_DEPTH ? MAX_RECOVERY_DEPTH : g_recovery_depth;
    g_recovery_depth = depth - 1;
    longjmp(*g_recovery[depth - 1], 1);
}

void ziman_abort_quietly(void) {
    if (g_recovery_depth > 0) jump_to_recovery();
    exit(1);
}

static void jump_to_outermost(void) {
    if (g_recovery_depth > MAX_RECOVERY_DEPTH) g_recovery_depth = MAX_RECOVERY_DEPTH;
    g_recovery_depth = 0;
    longjmp(*g_recovery[0], 1);
}

void ziman_finish_errors(void) {
    if (g_error_count == 0) return;
    if (g_sink != NULL && g_recovery_depth > 0) jump_to_outermost();
    if (g_error_count > 1 && !g_json_errors) fprintf(stderr, "ziman: %d errors\n", g_error_count);
    exit(1);
}

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

static __thread ThreadBoundary *t_boundary = NULL;

void ziman_thread_boundary_enter(ThreadBoundary *b) {
    b->message = NULL;
    t_boundary = b;
}

void ziman_thread_boundary_leave(void) { t_boundary = NULL; }

void ziman_error(const char *phase, int line, int col, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *message = malloc((size_t)(n > 0 ? n : 0) + 1);
    vsnprintf(message, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);

    if (t_boundary != NULL && g_sink == NULL && strcmp(phase, "runtime") == 0) {
        ThreadBoundary *b = t_boundary;
        t_boundary = NULL;
        int in_other_file = g_error_file != NULL && (g_entry_file == NULL || strcmp(g_error_file, g_entry_file) != 0);
        size_t room = strlen(message) + (g_error_file != NULL ? strlen(g_error_file) : 0) + 96;
        char *text = malloc(room);
        snprintf(text, room, "runtime error at %d:%d%s%s: %s", line, col, in_other_file ? " in " : "", in_other_file ? g_error_file : "", message);
        free(message);
        b->message = text;
        longjmp(b->jb, 1);
    }
    if (g_sink != NULL) {
        g_sink(phase, g_error_file, line, col, g_error_context, message);
        g_error_count++;
        if (g_recovery_depth > 0) {
            if (g_error_count >= MAX_ERRORS) jump_to_outermost();
            jump_to_recovery();
        }
        exit(1);
    }
    int show_file = g_error_file != NULL && (g_entry_file == NULL || strcmp(g_error_file, g_entry_file) != 0);
    if (g_json_errors) {
        fprintf(stderr, "{\"severity\":\"error\",\"phase\":");
        print_json_string(stderr, phase);
        fputs(",\"file\":", stderr);
        if (g_error_file != NULL) print_json_string(stderr, g_error_file);
        else fputs("null", stderr);
        fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":", line, col);
        if (g_error_context != NULL) print_json_string(stderr, g_error_context);
        else fputs("null", stderr);
        fputs(",\"message\":", stderr);
        print_json_string(stderr, message);
        fputs("}\n", stderr);
    } else {
        fprintf(stderr, "ziman: %s error at %d:%d", phase, line, col);
        if (show_file) fprintf(stderr, " in %s", g_error_file);
        if (g_error_context != NULL) fprintf(stderr, " (in %s)", g_error_context);
        fprintf(stderr, ": %s\n", message);
    }
    g_error_count++;
    if (g_recovery_depth > 0 && g_error_count < MAX_ERRORS) jump_to_recovery();
    if (g_error_count >= MAX_ERRORS && g_recovery_depth > 0) {
        if (!g_json_errors) fprintf(stderr, "ziman: too many errors (%d); stopping\n", g_error_count);
    } else if (g_error_count > 1 && !g_json_errors) {
        fprintf(stderr, "ziman: %d errors\n", g_error_count);
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
