#define _POSIX_C_SOURCE 200809L
#include "native_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "native.h"
#include "native_os.h"
#include "value.h"
#include "xalloc.h"

/* io.stdin_read(size) -> Result<[byte], string>: up to `size` bytes from the
 * process's standard input, blocking until at least one arrives.
 *
 * **An empty Ok is end of input, and it is the only way to say so.** A native
 * cannot build an Option (a user-declared enum) itself, so the `None` that ends
 * the stream is `io.stdin`'s to make -- which is also why the never-empty-chunk
 * rule io.zm's header states is enforced there rather than here.
 *
 * **One read, not a loop that fills the buffer.** A pipe hands over what is
 * written as it is written, and a terminal a line at a time; filling `size`
 * first would make an interactive reader wait for 64 KiB of typing. So this is
 * read(2) once and whatever it gave, which is what every layer over it expects
 * of a chunk (`fs.chunks` is the exception, and says so: exactly `size` except
 * the last).
 *
 * EINTR is retried, because a signal is not an I/O failure -- unless a
 * cooperative shutdown has been requested, where retrying is a hang. There it
 * ends the stream, the same answer os.install_shutdown_handler() gets everywhere
 * else: stop producing, let the program drain. */
static Value native_io_stdin_read(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type bt = type_array(type_scalar(TY_BYTE));
    const Type *rt = type_ref(type_result(bt, type_scalar(TY_STRING)));
    int64_t size = args[0].as.i;
    if (size < 1 || size > (int64_t)1 << 30) {
        char msg[128];
        snprintf(msg, sizeof msg, "read size %lld is out of range (1 to 1073741824)", (long long)size);
        return value_result(rt, result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    uint8_t *buf = ziman_xmalloc((size_t)size);
    ssize_t got;
    for (;;) {
        got = read(STDIN_FILENO, buf, (size_t)size);
        if (got >= 0) break;
        if (errno != EINTR) break;
        if (ziman_shutdown_requested()) { got = 0; break; }
    }
    if (got < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot read standard input: %s", strerror(errno));
        free(buf);
        return value_result(rt, result_obj_new(0, value_string(ziman_xstrdup(msg))));
    }
    Value v = value_result(rt, result_obj_new(1, value_byte_array(buf, (int)got)));
    free(buf);
    return v;
}

void native_io_register(void) {
    native_register("io::stdin_read", native_io_stdin_read);
}
