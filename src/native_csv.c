#define _POSIX_C_SOURCE 200809L
#include "native_csv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* csv.scan_cells(text, sep) -> Result<[string], string>: the RFC 4180 scanner behind the `csv`
 * module. A flat array of strings, in reading order: each cell is a one-character tag ('q'
 * for a quoted cell, 'u' for an unquoted one) followed by its text, and each row is ended by
 * a string that starts with byte 0x01 followed by the line the row began on. Blank lines
 * (one empty unquoted cell) produce nothing. csv.zm turns this into rows. */

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

    ValueBuf out = {0}, row = {0};
    char *field = malloc(n + 1);
    size_t flen = 0;
    int quoted = 0, in_quotes = 0, started = 0;
    int line_no = 1, row_line = 1, quote_line = 0;

#define END_FIELD() do { vbuf_push(&row, tagged_cell(quoted ? 'q' : 'u', field, flen)); flen = 0; quoted = 0; started = 0; } while (0)

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
            int blank = row.count == 1 && row.items[0].as.str.len == 1 && row.items[0].as.str.ptr[0] == 'u';
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
        int blank = row.count == 1 && row.items[0].as.str.len == 1 && row.items[0].as.str.ptr[0] == 'u';
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
