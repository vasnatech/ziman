#define _POSIX_C_SOURCE 200809L
#include "native_csv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* csv.scan_cells(text, sep, typed) -> Result<[string], string>: the RFC 4180 scanner behind the
 * `csv` module. A flat array of strings, in reading order: each cell is a one-character tag
 * followed by its text, and each row is ended by a string that starts with byte 0x01 followed
 * by the line the row began on. The tag is 'q' for a quoted cell and 'u' for an unquoted one;
 * with `typed`, an unquoted cell that looks like something else is tagged 'n' (empty), 't' /
 * 'x' (true / false), 'i' (an integer) or 'd' (a float). Blank lines (one empty unquoted
 * cell) produce nothing. csv.zm turns this into rows. */

typedef struct {
    Value *items;
    size_t count, cap;
} ValueBuf;

static void vbuf_push(ValueBuf *b, Value v) {
    if (b->count == b->cap) {
        b->cap = b->cap == 0 ? 64 : b->cap * 2;
        b->items = realloc(b->items, b->cap * sizeof(Value));
    }
    b->items[b->count++] = v;
}

static Value tagged_cell(char tag, const char *text, size_t len) {
    char *s = malloc(len + 2);
    s[0] = tag;
    memcpy(s + 1, text, len);
    s[len + 1] = '\0';
    return value_string(s);
}

static int is_digit_c(char c) { return c >= '0' && c <= '9'; }

/* `42`, `-7`, `0`; not `007`, `+1`, `1_000`, `-`. */
static int looks_int(const char *s, size_t n) {
    size_t i = 0;
    if (n > 0 && s[0] == '-') i = 1;
    if (i >= n) return 0;
    if (s[i] == '0' && n > i + 1) return 0;
    for (; i < n; i++) if (!is_digit_c(s[i])) return 0;
    return 1;
}

/* `1.5`, `-0.25`, `2e3`, `1.5E-3`: an int-looking part, then a fraction and/or an exponent. */
static int looks_float(const char *s, size_t n) {
    size_t i = 0;
    if (n > 0 && s[0] == '-') i = 1;
    if (i >= n || !is_digit_c(s[i])) return 0;
    if (s[i] == '0' && n > i + 1 && is_digit_c(s[i + 1])) return 0;
    int seen_dot = 0, seen_exp = 0, seen_digit = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (is_digit_c(c)) seen_digit = 1;
        else if (c == '.' && !seen_dot && !seen_exp && seen_digit) { seen_dot = 1; seen_digit = 0; }
        else if ((c == 'e' || c == 'E') && !seen_exp && seen_digit) {
            seen_exp = 1; seen_digit = 0;
            if (i + 1 < n && (s[i + 1] == '+' || s[i + 1] == '-')) i++;
        } else return 0;
    }
    return seen_digit && (seen_dot || seen_exp);
}

/* The tag of an unquoted cell when `typed`: n null, t true, x false, i int, d float, else u. */
static char cell_tag(const char *text, size_t len, int quoted, int typed) {
    if (quoted) return 'q';
    if (!typed) return 'u';
    if (len == 0) return 'n';
    if (len == 4 && memcmp(text, "true", 4) == 0) return 't';
    if (len == 5 && memcmp(text, "false", 5) == 0) return 'x';
    if (looks_int(text, len)) return 'i';
    if (looks_float(text, len)) return 'd';
    return 'u';
}

static Value scan_error(Type result_type, int line, const char *message, int other_line) {
    char buf[160];
    if (other_line > 0) snprintf(buf, sizeof buf, "line %d: %s (opened on line %d)", line, message, other_line);
    else snprintf(buf, sizeof buf, "line %d: %s", line, message);
    return value_result(result_type, result_obj_new(0, value_string(strdup(buf))));
}

static Value native_csv_scan_cells(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type string_type = type_scalar(TY_STRING);
    Type result_type = type_result(type_array(string_type), string_type);
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    char sep = args[1].as.str.len > 0 ? args[1].as.str.ptr[0] : ',';
    int typed = args[2].as.b;

    ValueBuf out = {0}, row = {0};
    char *field = malloc(n + 1);
    size_t flen = 0;
    int quoted = 0, in_quotes = 0, started = 0;
    int line_no = 1, row_line = 1, quote_line = 0;

#define END_FIELD() do { vbuf_push(&row, tagged_cell(cell_tag(field, flen, quoted, typed), field, flen)); flen = 0; quoted = 0; started = 0; } while (0)

    for (size_t p = 0; p < n; p++) {
        char c = s[p];
        if (in_quotes) {
            if (c == '"') {
                if (p + 1 < n && s[p + 1] == '"') { field[flen++] = '"'; p++; }
                else in_quotes = 0;
            } else {
                if (c == '\n') line_no++;
                field[flen++] = c;
            }
            continue;
        }
        if (c == '"') {
            if (started) { free(field); return scan_error(result_type, line_no, "a quote inside an unquoted field", 0); }
            in_quotes = 1; quoted = 1; started = 1; quote_line = line_no;
        } else if (c == sep) {
            END_FIELD();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && p + 1 < n && s[p + 1] == '\n') p++;
            END_FIELD();
            int blank = row.count == 1 && row.items[0].as.str.len == 1 && (row.items[0].as.str.ptr[0] == 'u' || row.items[0].as.str.ptr[0] == 'n');
            if (!blank) {
                for (size_t i = 0; i < row.count; i++) vbuf_push(&out, row.items[i]);
                char marker[24];
                snprintf(marker, sizeof marker, "\x01%d", row_line);
                vbuf_push(&out, value_string(strdup(marker)));
            }
            row.count = 0;
            line_no++;
            row_line = line_no;
        } else {
            if (quoted) { free(field); return scan_error(result_type, line_no, "text after a closing quote", 0); }
            field[flen++] = c;
            started = 1;
        }
    }
    if (in_quotes) { free(field); return scan_error(result_type, line_no, "unterminated quoted field", quote_line); }
    if (started || quoted || flen > 0 || row.count > 0) {
        END_FIELD();
        int blank = row.count == 1 && row.items[0].as.str.len == 1 && (row.items[0].as.str.ptr[0] == 'u' || row.items[0].as.str.ptr[0] == 'n');
        if (!blank) {
            for (size_t i = 0; i < row.count; i++) vbuf_push(&out, row.items[i]);
            char marker[24];
            snprintf(marker, sizeof marker, "\x01%d", row_line);
            vbuf_push(&out, value_string(strdup(marker)));
        }
    }
#undef END_FIELD
    free(field);
    Value items = value_array(type_array(string_type), array_obj_new(out.items != NULL ? out.items : malloc(sizeof(Value)), (int)out.count));
    return value_result(result_type, result_obj_new(1, items));
}

void native_csv_register(void) {
    native_register("csv::scan_cells", native_csv_scan_cells);
}
