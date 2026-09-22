#define _POSIX_C_SOURCE 200809L
#include "native_clock.h"

#include <stdlib.h>
#include <time.h>

#include "error.h"
#include "native.h"
#include "value.h"

/* One clock_gettime() call, both fields read atomically relative to
 * each other -- reading seconds and nanoseconds as two separate native
 * calls from Ziman would risk the clock ticking over between them,
 * a real (if narrow) correctness gap this avoids entirely. */
static Value native_clock_now_raw(Value *args, int argc, int line, int col) {
    (void)args;
    (void)argc;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ziman_error("runtime", line, col, "clock.now_raw: clock_gettime failed");
    }
    Value *items = malloc(2 * sizeof(Value));
    items[0] = value_int((int64_t)ts.tv_sec);
    items[1] = value_int((int64_t)ts.tv_nsec);
    return value_array(type_array(type_scalar(TY_INT)), array_obj_new(items, 2));
}

void native_clock_register(void) {
    native_register("clock::now_raw", native_clock_now_raw);
}
