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

    uint8_t *bytes = zalloc_atomic(n > 0 ? (size_t)n : 1);
    if (n > 0) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f == NULL) {
            ziman_error("runtime", line, col, "random_bytes: cannot open /dev/urandom");
        }
        size_t got = fread(bytes, 1, (size_t)n, f);
        fclose(f);
        if (got != (size_t)n) {
            ziman_error("runtime", line, col, "random_bytes: short read from /dev/urandom (wanted %lld, got %zu)", (long long)n, got);
        }
    }
    return value_array(type_ref(type_array(type_scalar(TY_BYTE))), array_obj_new_bytes(bytes, (int)n));
}

void native_random_register(void) {
    native_register("random::bytes", native_random_bytes);
}
