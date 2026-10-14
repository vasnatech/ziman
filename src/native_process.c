/* posix_spawn_file_actions_addchdir_np is a GNU extension, and the nogc build does not pass
 * -D_GNU_SOURCE the way CFLAGS does -- guarded rather than unconditional so the default build,
 * which does pass it, gets no redefinition warning. Same shape as src/stack.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "native_process.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "value.h"
#include "xalloc.h"

/* The `process` core module's natives: start another program, wait for it, signal it. See
 * lib/core/process.zm for the Ziman side.
 *
 * Why the language needs this at all: without it Ziman cannot write a test harness, a build script
 * or a deploy tool -- the things a project reaches for first after its own code works. It was found
 * missing by the reference backend's test migration, where 19 of 20 suites start the server as a
 * child process and exactly one does not (docs/reviews/tasks.md F54).
 *
 * Three decisions worth knowing before changing anything here.
 *
 * One: **output goes to a file, never a pipe.** `start` takes optional paths for the child's stdout
 * and stderr and redirects them with spawn file actions; with none given the child inherits this
 * process's. There is deliberately no pipe-reading API, because a pipe no one drains fills its
 * buffer and blocks the child -- a server under test logging to a pipe nobody reads deadlocks at a
 * few kilobytes, and the fix a caller reaches for (read after wait) is the deadlock. A file has no
 * such limit, is readable with `fs.read_text` after the fact, and survives the child's death, which
 * is exactly when a test wants to look at it.
 *
 * Two: **posix_spawn, not fork/exec.** The interpreter is multithreaded (`spawn`), and between a
 * fork and its exec only async-signal-safe calls are legal -- allocating an argv there is already
 * undefined. posix_spawn has no such window.
 *
 * Three: **the child is reaped exactly once, by `wait`.** The handle stays live after the child
 * exits so that `wait` can be called, return its status, and only then drop the handle; a second
 * `wait` on the same handle is a runtime error rather than a silent -1, since the status is gone
 * and inventing one would hide a double-wait bug. `release` exists for a caller that never waits:
 * it signals, reaps and drops in one step so a test's cleanup path cannot leak a zombie. */

typedef struct Child {
    pid_t pid;
    int reaped;      /* wait() or release() has already collected the child */
    int status;      /* the raw wait status, once reaped -- kept for a debugger, not read by any native */
    pthread_mutex_t mu;
} Child;

/* `HANDLE_SET_INIT`, like the five other HandleSets in the tree. Without it the 64 shard mutexes
 * are zero-initialised static storage rather than `PTHREAD_MUTEX_INITIALIZER`, and POSIX does not
 * say what locking such a mutex does -- this worked only because glibc's initialiser happens to be
 * all-zero for a default mutex, which is an implementation detail and not a promise (A45). */
static HandleSet g_live = HANDLE_SET_INIT;

static Type string_type(void) { return type_scalar(TY_STRING); }

static Value err_result(Type ok_type, const char *message) {
    return value_result(type_result(ok_type, string_type()), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

static Value err_resultf(Type ok_type, const char *what, int e) {
    char buf[320];
    snprintf(buf, sizeof buf, "%s: %s", what, strerror(e));
    return err_result(ok_type, buf);
}

static Value ok_result(Type ok_type, Value v) {
    return value_result(type_result(ok_type, string_type()), result_obj_new(1, v));
}

static Child *child_get(int64_t handle, int line, int col) {
    Child *c = (Child *)(intptr_t)handle;
    if (!handleset_has(&g_live, c)) c = NULL;
    if (c == NULL) {
        /* The one message for both causes, because this is where a double `wait` lands: `wait`
         * reaps and drops the handle, so a second call cannot reach any check inside `wait`
         * itself. An earlier version of this file had that check anyway and it was dead code --
         * found by calling `wait` twice rather than by reading it. */
        ziman_error("runtime", line, col,
                    "process: handle %lld is not a running process -- either it was never one, or 'wait' or "
                    "'release' has already collected it (each reaps the child and ends the handle, so keep the "
                    "exit code 'wait' returned rather than asking twice)",
                    (long long)handle);
    }
    return c;
}

/* `environ` plus the caller's overrides. An override replaces a variable of the same name rather
 * than appending a second one (which the child's libc would resolve inconsistently), and an empty
 * value is kept -- `PROFILE_ACTIVE=""` is a meaningful setting, not a request to unset. */
static char **build_env(ArrayObj *overrides, int *out_count) {
    extern char **environ;
    int base = 0;
    while (environ[base] != NULL) base++;
    int extra = overrides == NULL ? 0 : overrides->count;
    char **env = ziman_xcalloc((size_t)(base + extra + 1), sizeof(char *));
    int n = 0;
    for (int i = 0; i < base; i++) {
        const char *entry = environ[i];
        const char *eq = strchr(entry, '=');
        size_t name_len = eq == NULL ? strlen(entry) : (size_t)(eq - entry);
        int overridden = 0;
        for (int j = 0; j < extra; j++) {
            const char *o = value_cstr(overrides->boxed_items[j]);
            const char *oeq = strchr(o, '=');
            size_t olen = oeq == NULL ? strlen(o) : (size_t)(oeq - o);
            if (olen == name_len && strncmp(o, entry, name_len) == 0) { overridden = 1; break; }
        }
        if (!overridden) env[n++] = ziman_xstrdup(entry);
    }
    for (int j = 0; j < extra; j++) {
        env[n++] = ziman_xstrdup(value_cstr(overrides->boxed_items[j]));
    }
    env[n] = NULL;
    *out_count = n;
    return env;
}

static void free_env(char **env) {
    for (int i = 0; env[i] != NULL; i++) free(env[i]);
    free(env);
}

/* process::start_raw(program, args, env, stdout_path, stderr_path, cwd) -> Result<int, string>
 *
 * `args` are the arguments *after* the program name; argv[0] is the program itself, as every shell
 * and exec does it, so a caller never has to repeat the name. `env` entries are "NAME=value"
 * strings, applied over this process's own environment. An empty string for a path or for `cwd`
 * means "leave it alone", which keeps the native's arity fixed and the Option-handling in Ziman
 * where it reads better. */
static Value native_process_start(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type int_type = type_scalar(TY_INT);
    const char *program = value_cstr(args[0]);
    ArrayObj *argv_in = args[1].as.arr;
    ArrayObj *env_in = args[2].as.arr;
    const char *out_path = value_cstr(args[3]);
    const char *err_path = value_cstr(args[4]);
    const char *cwd = value_cstr(args[5]);

    int extra = argv_in == NULL ? 0 : argv_in->count;
    char **argv = ziman_xcalloc((size_t)(extra + 2), sizeof(char *));
    argv[0] = ziman_xstrdup(program);
    for (int i = 0; i < extra; i++) {
        argv[i + 1] = ziman_xstrdup(value_cstr(argv_in->boxed_items[i]));
    }
    argv[extra + 1] = NULL;

    int env_count = 0;
    char **env = build_env(env_in, &env_count);

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) {
        for (int i = 0; argv[i] != NULL; i++) free(argv[i]);
        free(argv);
        free_env(env);
        return err_resultf(int_type, "cannot prepare to start the process", rc);
    }
    if (cwd[0] != '\0') {
        rc = posix_spawn_file_actions_addchdir_np(&fa, cwd);
    }
    if (rc == 0 && out_path[0] != '\0') {
        rc = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }
    if (rc == 0 && err_path[0] != '\0') {
        rc = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }

    pid_t pid = 0;
    if (rc == 0) {
        rc = posix_spawnp(&pid, program, &fa, NULL, argv, env);
    }
    posix_spawn_file_actions_destroy(&fa);
    for (int i = 0; argv[i] != NULL; i++) free(argv[i]);
    free(argv);
    free_env(env);
    if (rc != 0) {
        char what[320];
        snprintf(what, sizeof what, "cannot start '%s'", program);
        return err_resultf(int_type, what, rc);
    }

    Child *c = ziman_xcalloc(1, sizeof(Child));
    c->pid = pid;
    pthread_mutex_init(&c->mu, NULL);
    handleset_add(&g_live, c);
    return ok_result(int_type, value_int((int64_t)(intptr_t)c));
}

/* process::wait_raw(handle) -> Result<int, string>: blocks, reaps, and answers the exit code, or
 * 128 + the signal number for a child a signal ended -- the convention a shell reports, so
 * `terminate` then `wait` reads as 143 rather than as something a caller has to decode. */
static Value native_process_wait(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    Child *c = child_get(args[0].as.i, line, col);
    pthread_mutex_lock(&c->mu);
    int status = 0;
    pid_t got = 0;
    do {
        got = waitpid(c->pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        int e = errno;
        pthread_mutex_unlock(&c->mu);
        return err_resultf(int_type, "cannot wait for the process", e);
    }
    c->reaped = 1;
    c->status = status;
    pthread_mutex_unlock(&c->mu);
    handleset_remove(&g_live, c);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return ok_result(int_type, value_int(code));
}

static Value signal_child(Value *args, int sig, int line, int col) {
    Type unit_type = type_scalar(TY_UNIT);
    Child *c = child_get(args[0].as.i, line, col);
    pthread_mutex_lock(&c->mu);
    int already = c->reaped;
    pid_t pid = c->pid;
    pthread_mutex_unlock(&c->mu);
    if (already) {
        return ok_result(unit_type, value_unit()); /* nothing to signal; not an error */
    }
    if (kill(pid, sig) != 0 && errno != ESRCH) {
        return err_resultf(unit_type, "cannot signal the process", errno);
    }
    return ok_result(unit_type, value_unit());
}

static Value native_process_terminate(Value *args, int argc, int line, int col) {
    (void)argc;
    return signal_child(args, SIGTERM, line, col);
}

static Value native_process_kill(Value *args, int argc, int line, int col) {
    (void)argc;
    return signal_child(args, SIGKILL, line, col);
}

/* SIGINT, which is a different thing from SIGTERM to a program that installs a shutdown handler --
 * the reference backend's own lifecycle test asserts on both, and found this missing. Three named
 * signals rather than a raw number: `interrupt`, `terminate`, `kill` cover what a caller actually
 * wants, and a number would be both less readable and less portable. */
static Value native_process_interrupt(Value *args, int argc, int line, int col) {
    (void)argc;
    return signal_child(args, SIGINT, line, col);
}

/* process::wait_for_raw(handle, millis) -> Result<int, string>: the exit code, or -1 when the child
 * is still running after the budget. Polling rather than a signal handler or waitid's timeout
 * because this has to work from any thread of a multithreaded interpreter without installing
 * anything process-wide, and a 10 ms tick is far below the granularity any caller of this cares
 * about.
 *
 * It exists because `wait` alone makes a hung child hang its caller for good: a test that should
 * report "the server did not exit" would instead never finish, which is the worst way for a test
 * to fail. Only a run that *did* exit reaps and ends the handle, so a caller can escalate to
 * `kill` and `wait` after a timeout. */
static Value native_process_wait_for(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    Child *c = child_get(args[0].as.i, line, col);
    int64_t budget = args[1].as.i;
    if (budget < 0) budget = 0;
    pthread_mutex_lock(&c->mu);
    int64_t waited = 0;
    int status = 0;
    pid_t got = 0;
    for (;;) {
        do {
            got = waitpid(c->pid, &status, WNOHANG);
        } while (got < 0 && errno == EINTR);
        if (got != 0) break;            /* exited, or an error worth reporting */
        if (waited >= budget) {
            pthread_mutex_unlock(&c->mu);
            return ok_result(int_type, value_int(-1));
        }
        struct timespec tick = {0, 10 * 1000 * 1000};
        nanosleep(&tick, NULL);
        waited += 10;
    }
    if (got < 0) {
        int e = errno;
        pthread_mutex_unlock(&c->mu);
        return err_resultf(int_type, "cannot wait for the process", e);
    }
    c->reaped = 1;
    c->status = status;
    pthread_mutex_unlock(&c->mu);
    handleset_remove(&g_live, c);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return ok_result(int_type, value_int(code));
}

/* process::release_raw(handle) -> unit: SIGKILL, reap, drop the handle, ignoring every failure.
 * For a cleanup path that must not leave a zombie behind and has nothing useful to do if it
 * cannot -- a test whose assertions already failed, say. */
static Value native_process_release(Value *args, int argc, int line, int col) {
    (void)argc;
    Child *c = child_get(args[0].as.i, line, col);
    pthread_mutex_lock(&c->mu);
    if (!c->reaped) {
        kill(c->pid, SIGKILL);
        int status = 0;
        pid_t got = 0;
        do {
            got = waitpid(c->pid, &status, 0);
        } while (got < 0 && errno == EINTR);
        c->reaped = 1;
        c->status = status;
    }
    pthread_mutex_unlock(&c->mu);
    handleset_remove(&g_live, c);
    return value_unit();
}

/* process::pid_raw(handle) -> int, for a message or a log line. */
static Value native_process_pid(Value *args, int argc, int line, int col) {
    (void)argc;
    Child *c = child_get(args[0].as.i, line, col);
    return value_int((int64_t)c->pid);
}

void native_process_register(void) {
    native_register("process::start_raw", native_process_start);
    native_register("process::wait_raw", native_process_wait);
    native_register("process::terminate_raw", native_process_terminate);
    native_register("process::kill_raw", native_process_kill);
    native_register("process::interrupt_raw", native_process_interrupt);
    native_register("process::wait_for_raw", native_process_wait_for);
    native_register("process::release_raw", native_process_release);
    native_register("process::pid_raw", native_process_pid);
}
