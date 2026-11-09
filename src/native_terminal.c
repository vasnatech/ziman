/* The four natives behind lib/core/terminal.zm, and the one job C has to get right: **the
 * terminal's settings are put back however the program ends.**
 *
 * docs/design/terminal.md decision 3.1 is the reason this file is longer than four functions would
 * suggest. A program that dies in raw mode leaves the user's shell with no echo and no line
 * editing, and they have to type `reset` blind -- a failure worse than the crash that caused it. So
 * the original `termios` is restored from three places: an explicit `set_raw(false)`, an `atexit`
 * handler, and a signal handler for the signals that would otherwise kill the process before
 * `atexit` could run.
 *
 * Decision 3.2 is the reason it is not longer still: everything about *escape sequences* -- the
 * table, the ESC ambiguity, UTF-8 -- lives in Ziman, where `ziman test` can reach it with no
 * terminal attached. This file reads bytes and knows nothing about keys.
 *
 * Decision 3.3: `termios` and `TIOCGWINSZ` are POSIX, so Linux and macOS come together and nothing
 * here is guarded per-platform. Windows needs a different API entirely; it is an honest error in
 * the Ziman half rather than a `#ifdef` here. */
#define _POSIX_C_SOURCE 200809L
#include "native_terminal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "native.h"
#include "value.h"
#include "xalloc.h"

/* The settings as they were before the first `set_raw(true)`, and whether we hold them.
 *
 * `volatile sig_atomic_t` for the flag because the signal handler reads it, and a plain `int` read
 * in a handler is not guaranteed to see a complete value. The `struct termios` itself is not
 * sig_atomic and cannot be: it is written once, before the flag is set, and only read afterwards,
 * which is the ordering that makes the handler's read safe. */
static struct termios g_saved;
static volatile sig_atomic_t g_raw_active = 0;
static int g_atexit_installed = 0;

/* The signals whose default action is to terminate, and which therefore end the program without
 * `atexit` ever running. SIGINT is in the list even though raw mode turns ISIG off -- raw mode is
 * entered and left, and a SIGINT can arrive in the window where it is still generated. The crash
 * signals are here for the case decision 3.1 names outright: a program that *dies* in raw mode. */
static const int g_fatal_signals[] = {
    SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
};
#define FATAL_SIGNAL_COUNT ((int)(sizeof g_fatal_signals / sizeof g_fatal_signals[0]))

/* Whether we installed a handler for each one, so that leaving raw mode puts back exactly what was
 * there -- and so that a signal we did *not* claim is never touched. */
static int g_claimed[FATAL_SIGNAL_COUNT];
static struct sigaction g_previous[FATAL_SIGNAL_COUNT];

/* Async-signal-safe: `tcsetattr` is on POSIX's list, and nothing else is called. */
static void restore_now(void) {
    if (g_raw_active) {
        g_raw_active = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    }
}

static void on_fatal_signal(int sig) {
    int saved_errno = errno;
    restore_now();
    errno = saved_errno;
    /* Put back the default action and re-raise, so the process dies of what was actually wrong and
     * with the right status -- a handler that returned would resume a program that has, for
     * SIGSEGV, no defined state to resume into. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Claims a terminating signal **only if nobody else has.** A program that called
 * `os.install_shutdown_handler()` has its own plan for SIGINT and SIGTERM: it sets a flag, drains,
 * and returns from main, at which point `atexit` restores. Overwriting that handler here would
 * break the drain to fix a problem `atexit` already covers. */
static void claim_fatal_signals(void) {
    struct sigaction ours;
    memset(&ours, 0, sizeof ours);
    ours.sa_handler = on_fatal_signal;
    sigemptyset(&ours.sa_mask);
    ours.sa_flags = 0;
    for (int i = 0; i < FATAL_SIGNAL_COUNT; i++) {
        struct sigaction current;
        g_claimed[i] = 0;
        if (sigaction(g_fatal_signals[i], NULL, &current) != 0) continue;
        if (current.sa_handler != SIG_DFL) continue;
        if (sigaction(g_fatal_signals[i], &ours, &g_previous[i]) == 0) g_claimed[i] = 1;
    }
}

static void release_fatal_signals(void) {
    for (int i = 0; i < FATAL_SIGNAL_COUNT; i++) {
        if (!g_claimed[i]) continue;
        sigaction(g_fatal_signals[i], &g_previous[i], NULL);
        g_claimed[i] = 0;
    }
}

static const Type *result_of(Type ok_type) {
    return type_ref(type_result(ok_type, type_scalar(TY_STRING)));
}

static Value terminal_err(Type ok_type, const char *what) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s: %s", what, strerror(errno));
    return value_result(result_of(ok_type), result_obj_new(0, value_string(ziman_xstrdup(msg))));
}

/* terminal.is_tty() -> bool: whether standard input is a terminal.
 *
 * **First, because a program must be able to refuse.** A TUI whose stdin is a pipe should say so
 * rather than render a frame into a file; everything else here is undefined when this is false. */
static Value native_terminal_is_tty(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_bool(isatty(STDIN_FILENO) == 1);
}

/* terminal.size_raw() -> Result<[int], string>: `[rows, cols]`.
 *
 * An array rather than a struct because Ziman has no tuple type and a native cannot build a
 * user-declared struct; `terminal.size()` turns it into a `Size`, the same division `os.cpu_times`
 * already uses. */
static Value native_terminal_size_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type ok = type_array(type_scalar(TY_INT));
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 && ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
        return terminal_err(ok, "cannot read the terminal size");
    }
    /* A terminal that answers zero is answering "I do not know" -- a pipe, or a pty that has not
     * been sized yet. Reported rather than passed on as a window of no rows, which a renderer
     * would divide by. */
    if (ws.ws_row == 0 || ws.ws_col == 0) {
        return value_result(result_of(ok), result_obj_new(0,
            value_string(ziman_xstrdup("the terminal did not report a size (not a terminal?)"))));
    }
    int64_t dims[2] = { (int64_t)ws.ws_row, (int64_t)ws.ws_col };
    return value_result(result_of(ok), result_obj_new(1, value_int_array(dims, 2)));
}

/* terminal.set_raw(on) -> Result<unit, string>: one key at a time, no echo, no signal keys.
 *
 * `cfmakeraw`'s settings, written out rather than called, because the two bits a reader will ask
 * about are the two that are not obvious: VMIN 0 and VTIME 0 make a read answer immediately with
 * whatever is there, which is what `read_raw`'s timeout needs -- the waiting is `poll`'s job, not
 * the driver's. Entering twice is not an error and does not re-save: the saved settings must stay
 * the ones from before the *first* enter. */
static Value native_terminal_set_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
    int on = args[0].as.b;
    if (!on) {
        if (!g_raw_active) {
            return value_result(result_of(ok), result_obj_new(1, value_unit()));
        }
        g_raw_active = 0;
        release_fatal_signals();
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved) != 0) {
            return terminal_err(ok, "cannot restore the terminal");
        }
        return value_result(result_of(ok), result_obj_new(1, value_unit()));
    }
    if (g_raw_active) {
        return value_result(result_of(ok), result_obj_new(1, value_unit()));
    }
    if (!isatty(STDIN_FILENO)) {
        return value_result(result_of(ok), result_obj_new(0,
            value_string(ziman_xstrdup("standard input is not a terminal, so there is no raw mode to enter"))));
    }
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return terminal_err(ok, "cannot read the terminal settings");
    }
    struct termios raw = g_saved;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    /* The order matters: the handlers and `atexit` must be in place *before* the mode changes, or a
     * signal in the window between would find nothing to restore and nothing to restore it. */
    if (!g_atexit_installed) {
        if (atexit(restore_now) == 0) g_atexit_installed = 1;
    }
    claim_fatal_signals();
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        release_fatal_signals();
        return terminal_err(ok, "cannot put the terminal into raw mode");
    }
    g_raw_active = 1;
    return value_result(result_of(ok), result_obj_new(1, value_unit()));
}

/* terminal.read_raw(timeout_ms) -> Result<[byte], string>: **one byte**, or none if the timeout
 * expired first. A negative timeout waits indefinitely.
 *
 * **One byte, not a bufferful**, and that is a decision rather than a shortcut. §5 of the design
 * gives the public reader as `read_key(timeout_ms) -> Result<Option<Key>, string>` -- no reader
 * object, no hidden module buffer -- and that signature is only honest if a read never returns
 * bytes the decoder did not ask for. Reading a bufferful would leave the tail of a burst somewhere,
 * and "somewhere" is module-level mutable state in a module that otherwise has none.
 *
 * It costs nothing a terminal notices: the continuation bytes of a multi-byte character and the
 * rest of an escape sequence are already in the driver's buffer by the time the decoder asks, so
 * those reads do not wait. A thousand-character paste is a thousand syscalls, and bracketed paste
 * is a non-goal (§8).
 *
 * An empty answer is the timeout, which is not an error: `Err` is for a read that failed. EOF is
 * also empty -- a terminal does not give one, and a caller that wants to tell them apart is a
 * caller reading a pipe, which is `io.stdin()`'s job (§9). */
static Value native_terminal_read_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_array(type_scalar(TY_BYTE));
    int64_t timeout_ms = args[0].as.i;
    if (timeout_ms > 2147483647) timeout_ms = 2147483647;
    struct pollfd p;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    for (;;) {
        int ready = poll(&p, 1, timeout_ms < 0 ? -1 : (int)timeout_ms);
        if (ready > 0) break;
        if (ready == 0) {
            uint8_t none[1] = {0};
            return value_result(result_of(ok), result_obj_new(1, value_byte_array(none, 0)));
        }
        /* EINTR is not an I/O failure, and retrying is right here in a way it is not for an
         * indefinite wait: the timeout is the caller's deadline, and a render loop passes a few
         * tens of milliseconds. A signal that means the program should stop will have been seen by
         * its own handler. */
        if (errno != EINTR) return terminal_err(ok, "cannot poll the terminal");
    }
    uint8_t one[1];
    for (;;) {
        ssize_t got = read(STDIN_FILENO, one, 1);
        if (got == 1) return value_result(result_of(ok), result_obj_new(1, value_byte_array(one, 1)));
        if (got == 0) return value_result(result_of(ok), result_obj_new(1, value_byte_array(one, 0)));
        if (errno != EINTR) return terminal_err(ok, "cannot read the terminal");
    }
}

void native_terminal_register(void) {
    native_register("terminal::is_tty", native_terminal_is_tty);
    native_register("terminal::size_raw", native_terminal_size_raw);
    native_register("terminal::set_raw", native_terminal_set_raw);
    native_register("terminal::read_raw", native_terminal_read_raw);
}
