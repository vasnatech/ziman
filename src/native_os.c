#define _POSIX_C_SOURCE 200809L
#include "native_os.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interpreter.h"
#include "native.h"
#include "value.h"
#include "xalloc.h"

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
    if (v == NULL) return value_result(result_type, result_obj_new(0, value_string(ziman_xstrdup(""))));
    return value_result(result_type, result_obj_new(1, value_string(ziman_xstrdup(v))));
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
    Value *items = ziman_xmalloc((size_t)(g_prog_argc > 0 ? g_prog_argc : 1) * sizeof(Value));
    for (int i = 0; i < g_prog_argc; i++) items[i] = value_string(ziman_xstrdup(g_prog_argv[i]));
    return value_array(type_array(type_scalar(TY_STRING)), array_obj_new(items, g_prog_argc));
}

/* os.cwd_raw() -> Result<string, string>: the current directory. */
static Value native_os_cwd(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    char buf[4096];
    if (getcwd(buf, sizeof buf) == NULL) {
        return value_result(type_result(str, str), result_obj_new(0, value_string(ziman_xstrdup(strerror(errno)))));
    }
    return value_result(type_result(str, str), result_obj_new(1, value_string(ziman_xstrdup(buf))));
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

/* os.collect_garbage(): run a collection and the finalizers it queues (sockets and connections
 * nobody references any more are closed). A no-op without the collector.
 *
 * Runs ziman_force_collect() (this thread's own Phase 3 GcHeap, docs/MEMORY.md) *before* Boehm's
 * own GC_gcollect(), not after or not at all: gc_heap_sweep zeroes a reclaimed object's own bytes
 * (src/gc_heap.c's own comment on why), which matters here specifically because this thread's
 * GcHeap arena is registered with Boehm as an additional scanned range (arena_create_scanned) --
 * without sweeping first, a disguised native-handle address sitting in an already-dead, not-yet-
 * reclaimed gc_heap object (e.g. net.Socket's own wrapper, once struct_obj_new started allocating
 * it there) would still look like a live reference to Boehm's own scan, keeping the underlying
 * resource open well past the point nothing in the program can still reach it -- an explicit
 * request to collect garbage should not leave that behind for Boehm to misread. */
static Value native_os_collect_garbage(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
#ifdef ZIMAN_GC
    ziman_force_collect();
    GC_gcollect();
    GC_invoke_finalizers();
#endif
    return value_int(0);
}

/* os.collector_enabled() -> bool: whether this build has the garbage collector. */
static Value native_os_collector_enabled(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
#ifdef ZIMAN_GC
    return value_bool(1);
#else
    return value_bool(0);
#endif
}

/* os.open_descriptors() -> int: how many file descriptors this process has open (Linux: the
 * entries of /proc/self/fd), or -1 where that cannot be counted. */
static Value native_os_open_descriptors(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) return value_int(-1);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') n++;
    closedir(d);
    return value_int(n - 1); /* not counting the descriptor opendir itself used */
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

/* `volatile sig_atomic_t` is what C11 7.14.1.1p5 guarantees a handler may write and ordinary code
 * may read, and that guarantee is why this was correct before. It is accessed through the atomic
 * builtins anyway, for a reason worth writing down rather than rediscovering: a thread sanitizer
 * cannot see that guarantee -- there is nothing in the program text that says this object is
 * shared, only a type whose meaning lives in the standard -- so it reported the handler's write
 * against `accept`'s read as a data race on three fixtures, and a tool that cries wolf on correct
 * code is a tool nobody runs (docs/reviews/tasks.md A24). Relaxed ordering: this is a flag whose
 * only transition is 0 -> 1 and nothing is published with it, and `sig_atomic_t` is lock-free, so
 * the handler stays signal-safe. */
static volatile sig_atomic_t g_shutdown_requested = 0;

int ziman_shutdown_requested(void) {
    return __atomic_load_n(&g_shutdown_requested, __ATOMIC_RELAXED);
}

static void on_shutdown_signal(int sig) {
    (void)sig;
    /* Saved and restored because a handler runs between any two instructions of the thread it
     * interrupts: anything it leaves in errno would be read there as the result of a call this
     * handler had nothing to do with. Nothing below sets errno today, which is exactly why the
     * save is here rather than in a comment saying it is not needed yet. */
    int saved_errno = errno;
    __atomic_store_n(&g_shutdown_requested, 1, __ATOMIC_RELAXED);
    errno = saved_errno;
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
    return value_bool(ziman_shutdown_requested()); /* the one reader of the flag; see it for why */
}


/* ---------------------------------------------------------------------
 * The machine and the process: CPUs, memory, disks, load, uptime. Linux reads /proc; where a
 * file is missing the function answers Err("not supported here: ..."), never a made-up number.
 * --------------------------------------------------------------------- */

static Value os_ok_int(int64_t n) {
    return value_result(type_result(type_scalar(TY_INT), type_scalar(TY_STRING)), result_obj_new(1, value_int(n)));
}

static Value os_err_int(const char *message) {
    return value_result(type_result(type_scalar(TY_INT), type_scalar(TY_STRING)), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

static Value os_ok_float(double x) {
    return value_result(type_result(type_scalar(TY_FLOAT), type_scalar(TY_STRING)), result_obj_new(1, value_float(x)));
}

static Value os_err_float(const char *message) {
    return value_result(type_result(type_scalar(TY_FLOAT), type_scalar(TY_STRING)), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

/* The whole of a small /proc file into `buf` (NUL-terminated); 0 when it cannot be read. */
static int os_read_small(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

/* The number after `key` (`VmRSS:`, `MemTotal:`) in a /proc text, times `scale`; -1 when the key is absent. */
static int64_t os_proc_value(const char *text, const char *key, int64_t scale) {
    const char *at = strstr(text, key);
    if (at == NULL) return -1;
    at += strlen(key);
    while (*at == ' ' || *at == '\t') at++;
    return (int64_t)strtoll(at, NULL, 10) * scale;
}

/* os.cpu_count() -> int: the logical CPUs online (at least 1). */
static Value native_os_cpu_count(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return value_int(n < 1 ? 1 : (int64_t)n);
}

/* os.hostname() -> string ("" when it cannot be read). */
static Value native_os_hostname(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return value_string(ziman_xstrdup(""));
    buf[sizeof buf - 1] = '\0';
    return value_string(ziman_xstrdup(buf));
}

/* os.os_name() -> string: `linux`, `macos`, ... (the kernel's name, lower case, `darwin` as `macos`). */
static Value native_os_os_name(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    struct utsname u;
    if (uname(&u) != 0) return value_string(ziman_xstrdup("unknown"));
    char name[64];
    size_t i = 0;
    for (; u.sysname[i] != '\0' && i < sizeof name - 1; i++) {
        char c = u.sysname[i];
        name[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    name[i] = '\0';
    if (strcmp(name, "darwin") == 0) return value_string(ziman_xstrdup("macos"));
    return value_string(ziman_xstrdup(name));
}

/* os.os_arch() -> string: `x86_64`, `aarch64`, ... */
static Value native_os_os_arch(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    struct utsname u;
    if (uname(&u) != 0) return value_string(ziman_xstrdup("unknown"));
    return value_string(ziman_xstrdup(u.machine));
}

/* os.load_average() -> Result<[float], string>: the 1, 5 and 15 minute load averages. */
static Value native_os_load_average(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type result_type = type_result(type_array(type_scalar(TY_FLOAT)), type_scalar(TY_STRING));
    char buf[256];
    double a = 0, b = 0, c = 0;
    if (!os_read_small("/proc/loadavg", buf, sizeof buf) || sscanf(buf, "%lf %lf %lf", &a, &b, &c) != 3) {
        return value_result(result_type, result_obj_new(0, value_string(ziman_xstrdup("not supported here: cannot read /proc/loadavg"))));
    }
    Value *items = ziman_xmalloc(3 * sizeof(Value));
    items[0] = value_float(a);
    items[1] = value_float(b);
    items[2] = value_float(c);
    return value_result(result_type, result_obj_new(1, value_array(type_array(type_scalar(TY_FLOAT)), array_obj_new(items, 3))));
}

/* os.memory_total() / os.memory_available() -> Result<int, string>: the machine's memory, in bytes. */
static Value os_meminfo(const char *key) {
    char buf[8192];
    if (!os_read_small("/proc/meminfo", buf, sizeof buf)) return os_err_int("not supported here: cannot read /proc/meminfo");
    int64_t v = os_proc_value(buf, key, 1024);
    return v < 0 ? os_err_int("not supported here: no such line in /proc/meminfo") : os_ok_int(v);
}

static Value native_os_memory_total(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return os_meminfo("MemTotal:");
}

static Value native_os_memory_available(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return os_meminfo("MemAvailable:");
}

/* os.process_memory() -> Result<int, string>: this process's resident memory, in bytes. */
static Value native_os_process_memory(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    char buf[8192];
    if (!os_read_small("/proc/self/status", buf, sizeof buf)) return os_err_int("not supported here: cannot read /proc/self/status");
    int64_t v = os_proc_value(buf, "VmRSS:", 1024);
    return v < 0 ? os_err_int("not supported here: no VmRSS in /proc/self/status") : os_ok_int(v);
}

/* os.thread_count() -> int: this process's threads (-1 where that cannot be read). */
static Value native_os_thread_count(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    char buf[8192];
    if (!os_read_small("/proc/self/status", buf, sizeof buf)) return value_int(-1);
    return value_int(os_proc_value(buf, "Threads:", 1));
}

/* os.disk_total / disk_free / disk_used(path) -> Result<int, string>: the filesystem `path` is on, in bytes. `free` is what an
 * ordinary user can still use, `used` is total minus what is free to anyone. */
static Value os_disk(Value *args, int which) {
    struct statvfs st;
    const char *path = value_cstr(args[0]);
    if (statvfs(path, &st) != 0) {
        char msg[1024];
        snprintf(msg, sizeof msg, "cannot read the disk of '%s': %s", path, strerror(errno));
        return os_err_int(msg);
    }
    int64_t unit = (int64_t)st.f_frsize;
    if (which == 0) return os_ok_int((int64_t)st.f_blocks * unit);
    if (which == 1) return os_ok_int((int64_t)st.f_bavail * unit);
    return os_ok_int(((int64_t)st.f_blocks - (int64_t)st.f_bfree) * unit);
}

static Value native_os_disk_total(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return os_disk(args, 0);
}

static Value native_os_disk_free(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return os_disk(args, 1);
}

static Value native_os_disk_used(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return os_disk(args, 2);
}

/* os.cpu_times_raw() -> Result<[int], string>: `[busy, total]` CPU ticks of all CPUs since boot (from /proc/stat). */
static Value native_os_cpu_times_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type result_type = type_result(type_array(type_scalar(TY_INT)), type_scalar(TY_STRING));
    char buf[4096];
    long long v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (!os_read_small("/proc/stat", buf, sizeof buf) ||
        sscanf(buf, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) {
        return value_result(result_type, result_obj_new(0, value_string(ziman_xstrdup("not supported here: cannot read /proc/stat"))));
    }
    long long total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    long long idle = v[3] + v[4]; /* idle and iowait */
    Value *items = ziman_xmalloc(2 * sizeof(Value));
    items[0] = value_int((int64_t)(total - idle));
    items[1] = value_int((int64_t)total);
    return value_result(result_type, result_obj_new(1, value_array(type_array(type_scalar(TY_INT)), array_obj_new(items, 2))));
}

/* os.process_cpu_seconds() -> Result<float, string>: the CPU time this process has used, all threads. */
static Value native_os_process_cpu_seconds(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return os_err_float("not supported here: no process CPU clock");
    return os_ok_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

/* os.uptime_seconds() -> Result<int, string>: how long the machine has been up. */
static Value native_os_uptime_seconds(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    char buf[256];
    double up = 0;
    if (!os_read_small("/proc/uptime", buf, sizeof buf) || sscanf(buf, "%lf", &up) != 1) return os_err_int("not supported here: cannot read /proc/uptime");
    return os_ok_int((int64_t)up);
}

/* os.process_uptime_seconds() -> Result<int, string>: how long this process has been running (start time from /proc/self/stat). */
static Value native_os_process_uptime_seconds(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    char buf[4096], up_buf[256];
    double up = 0;
    if (!os_read_small("/proc/uptime", up_buf, sizeof up_buf) || sscanf(up_buf, "%lf", &up) != 1 ||
        !os_read_small("/proc/self/stat", buf, sizeof buf)) {
        return os_err_int("not supported here: cannot read /proc");
    }
    const char *after = strrchr(buf, ')'); /* the command name may hold spaces; the fields follow its `)` */
    if (after == NULL) return os_err_int("not supported here: unexpected /proc/self/stat");
    after++;
    unsigned long long start_ticks = 0;
    /* the fields after `)` start at field 3 (state); the start time is field 22, the 20th of them */
    if (sscanf(after, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &start_ticks) != 1) {
        return os_err_int("not supported here: unexpected /proc/self/stat");
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz < 1) hz = 100;
    double running = up - (double)start_ticks / (double)hz;
    return os_ok_int(running < 0 ? 0 : (int64_t)running);
}

void native_os_register(void) {
    native_register("os::getenv_raw", native_os_getenv_raw);
    native_register("os::exit", native_os_exit);
    native_register("os::args", native_os_args);
    native_register("os::cwd_raw", native_os_cwd);
    native_register("os::pid", native_os_pid);
    native_register("os::process_uptime_seconds", native_os_process_uptime_seconds);
    native_register("os::uptime_seconds", native_os_uptime_seconds);
    native_register("os::process_cpu_seconds", native_os_process_cpu_seconds);
    native_register("os::cpu_times_raw", native_os_cpu_times_raw);
    native_register("os::disk_used", native_os_disk_used);
    native_register("os::disk_free", native_os_disk_free);
    native_register("os::disk_total", native_os_disk_total);
    native_register("os::thread_count", native_os_thread_count);
    native_register("os::process_memory", native_os_process_memory);
    native_register("os::memory_available", native_os_memory_available);
    native_register("os::memory_total", native_os_memory_total);
    native_register("os::load_average", native_os_load_average);
    native_register("os::os_arch", native_os_os_arch);
    native_register("os::os_name", native_os_os_name);
    native_register("os::hostname", native_os_hostname);
    native_register("os::cpu_count", native_os_cpu_count);
    native_register("os::collect_garbage", native_os_collect_garbage);
    native_register("os::open_descriptors", native_os_open_descriptors);
    native_register("os::collector_enabled", native_os_collector_enabled);
    native_register("os::set_env", native_os_set_env);
    native_register("os::write_stderr", native_os_write_stderr);
    native_register("os::install_shutdown_handler", native_os_install_shutdown_handler);
    native_register("os::shutdown_requested", native_os_shutdown_requested);
}
