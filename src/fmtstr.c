#define _POSIX_C_SOURCE 200809L
#include "fmtstr.h"

#include <stdio.h>

int fmt_next(const char *fmt, size_t n, size_t *pos, FmtPiece *out, char *err, size_t err_size) {
    size_t p = *pos;
    if (p >= n) return 0;
    out->placeholder = 0;
    out->text = fmt + p;
    out->len = 1;
    out->precision = -1;
    if (fmt[p] == '{') {
        if (p + 1 < n && fmt[p + 1] == '{') {
            *pos = p + 2;
            return 1;
        }
        if (p + 1 < n && fmt[p + 1] == '}') {
            out->placeholder = 1;
            *pos = p + 2;
            return 1;
        }
        if (p + 2 < n && fmt[p + 1] == ':' && fmt[p + 2] == '.') {
            size_t q = p + 3;
            int precision = 0, digits = 0;
            while (q < n && fmt[q] >= '0' && fmt[q] <= '9') {
                if (precision <= FMT_MAX_PRECISION) precision = precision * 10 + (fmt[q] - '0');
                digits++;
                q++;
            }
            if (digits == 0 || q >= n || fmt[q] != '}') {
                snprintf(err, err_size, "malformed placeholder in format string (expected '{:.N}' with a number of decimal places N, e.g. '{:.2}')");
                return -1;
            }
            if (precision > FMT_MAX_PRECISION) {
                snprintf(err, err_size, "precision in format string is too large (at most %d decimal places)", (int)FMT_MAX_PRECISION);
                return -1;
            }
            out->placeholder = 1;
            out->precision = precision;
            *pos = q + 1;
            return 1;
        }
        snprintf(err, err_size, "malformed placeholder in format string (expected '{}' or '{:.N}', found stray '{'; write '{{' for a literal one)");
        return -1;
    }
    if (fmt[p] == '}') {
        if (p + 1 < n && fmt[p + 1] == '}') {
            *pos = p + 2;
            return 1;
        }
        snprintf(err, err_size, "unmatched '}' in format string (write '}}' for a literal one)");
        return -1;
    }
    size_t q = p;
    while (q < n && fmt[q] != '{' && fmt[q] != '}') q++;
    out->len = q - p;
    *pos = q;
    return 1;
}
