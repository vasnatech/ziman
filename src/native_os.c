#define _POSIX_C_SOURCE 200809L
#include "native_os.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "value.h"

/* os.getenv_raw(name) -> Result<string, string>: Ok(value) if the variable
 * is set (an empty value included -- set-but-empty is not the same as
 * unset), Err("") if it isn't. Ziman wraps this into an Option<string>;
 * a native can't build an Option (a user-declared enum) itself, since it
 * would need the loader's qualified name for it. */
static Value native_os_getenv_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    Type result_type = type_result(str, str);
    const char *v = getenv(value_cstr(args[0]));
    if (v == NULL) return value_result(result_type, result_obj_new(0, value_string(strdup(""))));
    return value_result(result_type, result_obj_new(1, value_string(strdup(v))));
}

/* os.exit(code): flush stdout/stderr and leave the process with `code`. */
static Value native_os_exit(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    fflush(stdout);
    fflush(stderr);
    exit((int)args[0].as.i);
    return value_int(0);
}

/* os.write_stderr(text): text to stderr, unbuffered, no newline added. */
static Value native_os_write_stderr(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    fwrite(args[0].as.str.ptr, 1, args[0].as.str.len, stderr);
    fflush(stderr);
    return value_int(0);
}

static volatile sig_atomic_t g_shutdown_requested = 0;

int ziman_shutdown_requested(void) {
    return g_shutdown_requested;
}

static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

/* os.install_shutdown_handler(): SIGINT and SIGTERM set a flag instead of
 * killing the process. No SA_RESTART, but nothing relies on EINTR either:
 * the signal can land on any thread, so blocked calls that need to notice it
 * (accept) poll the flag on a short timeout instead. */
static Value native_os_install_shutdown_handler(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    return value_int(0);
}

static Value native_os_shutdown_requested(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_bool(g_shutdown_requested);
}

void native_os_register(void) {
    native_register("os::getenv_raw", native_os_getenv_raw);
    native_register("os::exit", native_os_exit);
    native_register("os::write_stderr", native_os_write_stderr);
    native_register("os::install_shutdown_handler", native_os_install_shutdown_handler);
    native_register("os::shutdown_requested", native_os_shutdown_requested);
}
