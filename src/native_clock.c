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
    int64_t parts[2] = {(int64_t)ts.tv_sec, (int64_t)ts.tv_nsec};
    return value_int_array(parts, 2);
}

/* clock.sleep_millis(ms): blocks the calling thread only. Retried on
 * EINTR so an interrupting signal can't cut it short. */
static Value native_clock_sleep_millis(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int64_t ms = args[0].as.i;
    if (ms > 0) {
        struct timespec req = {(time_t)(ms / 1000), (long)((ms % 1000) * 1000000L)};
        struct timespec rem;
        while (nanosleep(&req, &rem) != 0) req = rem;
    }
    return value_int(0); /* void return -- the caller never uses this */
}

void native_clock_register(void) {
    native_register("clock::sleep_millis", native_clock_sleep_millis);
    native_register("clock::now_raw", native_clock_now_raw);
}
