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

/* The UTF-8 encoding of `cp`, written to `out` (4 bytes are always enough), answering how many
 * bytes that took. The inverse of the decoder above, and here for the same stated reason: two
 * encoders that could disagree about what a code point looks like is the bug this file exists to
 * prevent. `native_json.c` had the only copy until 2026-10-23, when case mapping needed one too.
 *
 * `cp` must be a real code point -- the callers get theirs from this file's own decoder or from
 * `towupper_l`, so none of them can produce a surrogate or anything past U+10FFFF. */
static inline int ziman_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- byte offsets <-> character indices -------------------------------------------------------
 *
 * The regex engine matches bytes and reports byte offsets; everything above it in Ziman counts
 * characters (prelude.c's character-indexing block). These two convert at that boundary.
 *
 * Both are **lenient**: an invalid sequence counts as one character and advances one byte, where
 * prelude.c's equivalents raise. That is deliberate -- a caller here is converting an offset the
 * matcher already produced from text it already walked, and a unit conversion is the wrong place to
 * re-litigate whether the text is valid UTF-8. The strict judgement belongs at the boundary where
 * bytes become a string (`bytes_to_string`), which is where it is. */

/* The character index at byte offset `bo`, which should be a character boundary. */
static inline int64_t ziman_utf8_char_index(const uint8_t *s, size_t len, size_t bo) {
    if (bo > len) bo = len;
    int64_t ci = 0;
    size_t pos = 0;
    while (pos < bo) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        pos += n > 0 ? (size_t)n : 1;
        ci++;
    }
    return ci;
}

/* The byte offset where character `ci` begins; a `ci` past the end answers `len`, so an exclusive
 * bound converts without a special case. */
static inline size_t ziman_utf8_byte_offset(const uint8_t *s, size_t len, int64_t ci) {
    size_t pos = 0;
    for (int64_t seen = 0; seen < ci && pos < len; seen++) {
        uint32_t cp;
        int n = ziman_utf8_decode_strict(s, len, pos, &cp);
        pos += n > 0 ? (size_t)n : 1;
    }
    return pos;
}

/* How many characters are in `len` bytes. */
static inline int64_t ziman_utf8_count(const uint8_t *s, size_t len) {
    return ziman_utf8_char_index(s, len, len);
}

#endif
