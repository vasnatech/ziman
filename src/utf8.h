#ifndef ZIMAN_UTF8_H
#define ZIMAN_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* One strict UTF-8 decoder, shared rather than duplicated. It began as a static in prelude.c for
 * `bytes_to_string` and the character functions; the JSON serializer needs exactly the same
 * judgement at the other end of the pipe, and two validators that could disagree about what counts
 * as valid text is precisely the bug this is used to prevent. Header-only so neither file grows a
 * link dependency on the other.
 *
 * Decodes the sequence starting at s[pos] (pos < len), writing the code point to *cp and returning
 * its byte length (1-4) -- or 0 if it is invalid: truncated at the end of the buffer, a bad
 * continuation byte, an overlong encoding (the same value spelled with more bytes than necessary --
 * a classic filter-bypass trick if left unchecked), a surrogate half (U+D800 to U+DFFF, never a real
 * code point on its own), or beyond Unicode's own range (past U+10FFFF). */
static inline int ziman_utf8_decode_strict(const uint8_t *s, size_t len, size_t pos, uint32_t *cp) {
    uint8_t c = s[pos];
    if (c < 0x80) { *cp = c; return 1; }
    int n;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) { n = 2; min = 0x80; *cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; min = 0x800; *cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; min = 0x10000; *cp = c & 0x07; }
    else return 0; /* a stray continuation byte (10xxxxxx), or a leading byte UTF-8 never uses (0xF8-0xFF) */
    if (pos + (size_t)n > len) return 0;
    for (int i = 1; i < n; i++) {
        uint8_t cb = s[pos + (size_t)i];
        if ((cb & 0xC0) != 0x80) return 0;
        *cp = (*cp << 6) | (uint32_t)(cb & 0x3F);
    }
    if (*cp < min || *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF)) return 0;
    return n;
}

#endif
