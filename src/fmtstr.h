#ifndef ZIMAN_FMTSTR_H
#define ZIMAN_FMTSTR_H

#include <stddef.h>

/* The template language of `format`, `printf` and `format_dynamic`, read in one place so the type checker (which
 * counts and types the placeholders of a literal) and the runtime (which fills them) can never disagree:
 *
 *   {}       the next argument, displayed as `print` displays it
 *   {:.N}    the next argument, a float or a decimal, rounded to N places (0 to 100) and written with exactly N
 *   {{  }}   a literal `{` or `}`
 *
 * A template is a run of pieces: literal text, or a placeholder. */
typedef struct {
    int placeholder;    /* 0: literal text (`text`, `len`); 1: a placeholder */
    const char *text;
    size_t len;
    int precision;      /* a placeholder's N, or -1 for a plain `{}` */
} FmtPiece;

enum { FMT_MAX_PRECISION = 100 };

/* Reads the piece at `*pos` and advances it: 1 with the piece in `out`, 0 at the end of the template, -1 with a
 * message in `err` for a malformed template (a stray `{` or `}`, a bad precision). A `{{` or `}}` is a one-character
 * literal piece. */
int fmt_next(const char *fmt, size_t n, size_t *pos, FmtPiece *out, char *err, size_t err_size);

#endif
