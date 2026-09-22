#define _POSIX_C_SOURCE 200809L
#include "native_random.h"

#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "native.h"
#include "value.h"

/* /dev/urandom rather than a platform syscall (getrandom() on Linux,
 * arc4random_buf() on macOS/BSD) -- works identically everywhere this
 * project already assumes a Unix-like OS (it already links pthread),
 * with the same plain fopen/fread/fclose shape native_io.c's own
 * read_file already uses, no #ifdef per platform. */
static Value native_random_bytes(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t n = args[0].as.i;
    if (n < 0) {
        ziman_error("runtime", line, col, "random_bytes: n must be non-negative, found %lld", (long long)n);
    }

    Value *items = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    if (n > 0) {
        unsigned char *buf = malloc((size_t)n);
        FILE *f = fopen("/dev/urandom", "rb");
        if (f == NULL) {
            ziman_error("runtime", line, col, "random_bytes: cannot open /dev/urandom");
        }
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        if (got != (size_t)n) {
            ziman_error("runtime", line, col, "random_bytes: short read from /dev/urandom (wanted %lld, got %zu)", (long long)n, got);
        }
        for (int64_t i = 0; i < n; i++) {
            items[i] = value_byte(buf[i]);
        }
        free(buf);
    }
    return value_array(type_array(type_scalar(TY_BYTE)), array_obj_new(items, (int)n));
}

void native_random_register(void) {
    native_register("random::random_bytes", native_random_bytes);
}
