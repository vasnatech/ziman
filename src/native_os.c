#define _POSIX_C_SOURCE 200809L
#include "native_os.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
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

static int g_prog_argc = 0;
static char **g_prog_argv = NULL;

void ziman_set_program_args(int count, char **args) {
    g_prog_argc = count;
    g_prog_argv = args;
}

/* os.args() -> [string]: the program's own arguments (those after `--`). */
static Value native_os_args(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Value *items = malloc((size_t)(g_prog_argc > 0 ? g_prog_argc : 1) * sizeof(Value));
    for (int i = 0; i < g_prog_argc; i++) items[i] = value_string(strdup(g_prog_argv[i]));
    return value_array(type_array(type_scalar(TY_STRING)), array_obj_new(items, g_prog_argc));
}

/* os.cwd_raw() -> Result<string, string>: the current directory. */
static Value native_os_cwd(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    char buf[4096];
    if (getcwd(buf, sizeof buf) == NULL) {
        return value_result(type_result(str, str), result_obj_new(0, value_string(strdup(strerror(errno)))));
    }
    return value_result(type_result(str, str), result_obj_new(1, value_string(strdup(buf))));
}

static Value native_os_pid(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_int((int64_t)getpid());
}

/* os.set_env(name, value) -> bool: sets a variable for this process and its children. */
static Value native_os_set_env(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(setenv(value_cstr(args[0]), value_cstr(args[1]), 1) == 0);
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
    native_register("os::args", native_os_args);
    native_register("os::cwd_raw", native_os_cwd);
    native_register("os::pid", native_os_pid);
    native_register("os::set_env", native_os_set_env);
    native_register("os::write_stderr", native_os_write_stderr);
    native_register("os::install_shutdown_handler", native_os_install_shutdown_handler);
    native_register("os::shutdown_requested", native_os_shutdown_requested);
}
