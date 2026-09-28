#define _POSIX_C_SOURCE 200809L
#include "native_http.h"

#include <string.h>

#include "native.h"
#include "value.h"

/* RFC 7230's `token`: what a header name or a method may be made of. */
static int is_tchar(unsigned char c) {
    if (c >= '0' && c <= '9') return 1;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static Value native_http_is_token(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const unsigned char *s = (const unsigned char *)args[0].as.str.ptr;
    size_t n = (size_t)args[0].as.str.len;
    if (n == 0) return value_bool(0);
    for (size_t i = 0; i < n; i++) {
        if (!is_tchar(s[i])) return value_bool(0);
    }
    return value_bool(1);
}

/* A header value (or request target) may hold any byte but a control character other than a tab --
 * above all no CR or LF, which would end the header line and let the rest pass for another one. */
static Value native_http_is_field_value(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const unsigned char *s = (const unsigned char *)args[0].as.str.ptr;
    size_t n = (size_t)args[0].as.str.len;
    for (size_t i = 0; i < n; i++) {
        if ((s[i] < 0x20 && s[i] != '\t') || s[i] == 0x7f) return value_bool(0);
    }
    return value_bool(1);
}

void native_http_register(void) {
    native_register("http::is_token", native_http_is_token);
    native_register("http::is_field_value", native_http_is_field_value);
}
