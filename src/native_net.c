#define _POSIX_C_SOURCE 200809L
#include "native_net.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "error.h"
#include "native.h"

/* Blocking TCP behind corelib/net.zm. A connection or listener is an int
 * handle into a table of file descriptors (same pattern as StringBuilder
 * and the db driver) rather than the raw fd itself, so a Ziman program
 * can't pass an arbitrary number to close(2). Errors reach Ziman as
 * "category|message" strings, split into a NetError enum in net.zm.
 *
 * Blocking is deliberate: Ziman has real OS threads, so a blocked read
 * only blocks its own thread -- no event loop needed.
 *
 * Closing a handle another thread is blocked reading on is *allowed*
 * (close shuts the socket down first, which wakes that read with an
 * error), but the fd number is then free for the OS to reuse, so a
 * program should only close a socket from the thread that owns it. */

typedef struct {
    int fd;
} NetHandle;

static NetHandle **g_handles = NULL;
static int g_handle_count = 0;
static int g_handle_capacity = 0;
static pthread_mutex_t g_handle_mu = PTHREAD_MUTEX_INITIALIZER;

static Type string_type(void) { return type_scalar(TY_STRING); }
static Type byte_array_type(void) { return type_array(type_scalar(TY_BYTE)); }

static Value err_result(Type ok_type, const char *category, const char *message) {
    size_t n = strlen(category) + strlen(message) + 2;
    char *s = malloc(n);
    snprintf(s, n, "%s|%s", category, message);
    return value_result(type_result(ok_type, string_type()), result_obj_new(0, value_string(s)));
}

static Value ok_result(Type ok_type, Value v) {
    return value_result(type_result(ok_type, string_type()), result_obj_new(1, v));
}

/* Classifies an errno from a socket call into a net.zm error category. */
static Value errno_result(Type ok_type, int e) {
    const char *category = "io";
    if (e == ECONNREFUSED) category = "refused";
    else if (e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT) category = "timeout";
    else if (e == EPIPE || e == ECONNRESET || e == ENOTCONN || e == EBADF || e == ESHUTDOWN) category = "closed";
    return err_result(ok_type, category, strerror(e));
}

static int handle_register(int fd) {
    NetHandle *h = malloc(sizeof(NetHandle));
    h->fd = fd;
    pthread_mutex_lock(&g_handle_mu);
    if (g_handle_count == g_handle_capacity) {
        g_handle_capacity = g_handle_capacity == 0 ? 16 : g_handle_capacity * 2;
        g_handles = realloc(g_handles, sizeof(NetHandle *) * (size_t)g_handle_capacity);
    }
    int id = g_handle_count;
    g_handles[g_handle_count++] = h;
    pthread_mutex_unlock(&g_handle_mu);
    return id;
}

/* Returns the fd, or -1 for an unknown/closed handle (a Ziman-level bug
 * the caller turns into a fatal runtime error). */
static int handle_fd(int64_t id) {
    pthread_mutex_lock(&g_handle_mu);
    int fd = (id >= 0 && id < g_handle_count && g_handles[id] != NULL) ? g_handles[id]->fd : -1;
    pthread_mutex_unlock(&g_handle_mu);
    return fd;
}

static int require_fd(int64_t id, const char *what, int line, int col) {
    int fd = handle_fd(id);
    if (fd < 0) ziman_error("runtime", line, col, "net.%s: invalid or closed handle %lld", what, (long long)id);
    return fd;
}

/* Resolves host:port and returns a socket that either connected
 * (listener == 0) or is bound+listening (listener == 1), or a Result
 * error. */
static Value open_socket(const char *host, int64_t port, int listener) {
    Type int_type = type_scalar(TY_INT);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (listener) hints.ai_flags = AI_PASSIVE;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%lld", (long long)port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host[0] == '\0' ? NULL : host, port_str, &hints, &res);
    if (rc != 0) return err_result(int_type, "notfound", gai_strerror(rc));

    int last_errno = 0;
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        if (listener) {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 128) == 0) break;
        } else {
            int r;
            do { r = connect(fd, ai->ai_addr, ai->ai_addrlen); } while (r < 0 && errno == EINTR);
            if (r == 0) break;
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return errno_result(int_type, last_errno != 0 ? last_errno : EIO);
    return ok_result(int_type, value_int(handle_register(fd)));
}

static Value native_net_connect_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return open_socket(args[0].as.s, args[1].as.i, 0);
}

static Value native_net_listen_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return open_socket(args[0].as.s, args[1].as.i, 1);
}

static Value native_net_listener_port_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    int fd = require_fd(args[0].as.i, "listener_port", line, col);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return value_int(-1);
    if (ss.ss_family == AF_INET) return value_int(ntohs(((struct sockaddr_in *)&ss)->sin_port));
    if (ss.ss_family == AF_INET6) return value_int(ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    return value_int(-1);
}

static Value native_net_accept_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    int fd = require_fd(args[0].as.i, "accept", line, col);
    int c;
    do { c = accept(fd, NULL, NULL); } while (c < 0 && errno == EINTR);
    if (c < 0) return errno_result(int_type, errno);
    return ok_result(int_type, value_int(handle_register(c)));
}

/* Reads up to `max` bytes (at least one -- blocks until something is
 * available). 0 bytes from recv means the peer closed cleanly, reported
 * as the "closed" category rather than an empty array, so a caller can't
 * forget to check for EOF. */
static Value native_net_read_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = byte_array_type();
    int fd = require_fd(args[0].as.i, "read", line, col);
    int64_t max = args[1].as.i;
    if (max <= 0) return err_result(arr_type, "io", "read: max must be positive");
    unsigned char *buf = malloc((size_t)max);
    ssize_t n;
    do { n = recv(fd, buf, (size_t)max, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) { int e = errno; free(buf); return errno_result(arr_type, e); }
    if (n == 0) { free(buf); return err_result(arr_type, "closed", "connection closed by peer"); }
    Value *items = malloc(sizeof(Value) * (size_t)n);
    for (ssize_t i = 0; i < n; i++) items[i] = value_byte(buf[i]);
    free(buf);
    return ok_result(arr_type, value_array(arr_type, array_obj_new(items, (int)n)));
}

static Value native_net_read_exact_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = byte_array_type();
    int fd = require_fd(args[0].as.i, "read_exact", line, col);
    int64_t want = args[1].as.i;
    if (want < 0) return err_result(arr_type, "io", "read_exact: count must not be negative");
    unsigned char *buf = malloc((size_t)want + 1);
    int64_t got = 0;
    while (got < want) {
        ssize_t n;
        do { n = recv(fd, buf + got, (size_t)(want - got), 0); } while (n < 0 && errno == EINTR);
        if (n < 0) { int e = errno; free(buf); return errno_result(arr_type, e); }
        if (n == 0) { free(buf); return err_result(arr_type, "closed", "connection closed before the requested bytes arrived"); }
        got += n;
    }
    Value *items = malloc(sizeof(Value) * (size_t)(want > 0 ? want : 1));
    for (int64_t i = 0; i < want; i++) items[i] = value_byte(buf[i]);
    free(buf);
    return ok_result(arr_type, value_array(arr_type, array_obj_new(items, (int)want)));
}

static Value send_all(int fd, const unsigned char *data, size_t n) {
    Type int_type = type_scalar(TY_INT);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w;
        do { w = send(fd, data + sent, n - sent, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
        if (w < 0) return errno_result(int_type, errno);
        sent += (size_t)w;
    }
    return ok_result(int_type, value_int((int64_t)n));
}

static Value native_net_write_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    int fd = require_fd(args[0].as.i, "write", line, col);
    ArrayObj *arr = args[1].as.arr;
    unsigned char *buf = malloc((size_t)(arr->count > 0 ? arr->count : 1));
    for (int i = 0; i < arr->count; i++) buf[i] = (unsigned char)arr->items[i].as.by;
    Value r = send_all(fd, buf, (size_t)arr->count);
    free(buf);
    return r;
}

/* Text variants: a Ziman string is NUL-terminated, so text mode cannot
 * carry a 0 byte. A read that receives one reports it rather than
 * silently truncating; use read/write ([byte]) for binary data. */
static Value native_net_write_text_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    int fd = require_fd(args[0].as.i, "write_text", line, col);
    return send_all(fd, (const unsigned char *)args[1].as.s, strlen(args[1].as.s));
}

static Value native_net_read_text_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type str_type = string_type();
    int fd = require_fd(args[0].as.i, "read_text", line, col);
    int64_t max = args[1].as.i;
    if (max <= 0) return err_result(str_type, "io", "read_text: max must be positive");
    char *buf = malloc((size_t)max + 1);
    ssize_t n;
    do { n = recv(fd, buf, (size_t)max, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) { int e = errno; free(buf); return errno_result(str_type, e); }
    if (n == 0) { free(buf); return err_result(str_type, "closed", "connection closed by peer"); }
    buf[n] = '\0';
    if (memchr(buf, '\0', (size_t)n) != NULL && strlen(buf) != (size_t)n) {
        free(buf);
        return err_result(str_type, "io", "read_text: received a NUL byte; use read for binary data");
    }
    return ok_result(str_type, value_string(buf));
}

static Value native_net_set_timeout_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    int fd = require_fd(args[0].as.i, "set_timeout", line, col);
    int64_t ms = args[1].as.i;
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return errno_result(int_type, errno);
    }
    return ok_result(int_type, value_int(0));
}

static Value native_net_close_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int64_t id = args[0].as.i;
    pthread_mutex_lock(&g_handle_mu);
    NetHandle *h = (id >= 0 && id < g_handle_count) ? g_handles[id] : NULL;
    if (h != NULL) g_handles[id] = NULL;
    pthread_mutex_unlock(&g_handle_mu);
    if (h != NULL) {
        shutdown(h->fd, SHUT_RDWR); /* wakes a thread blocked reading this socket */
        close(h->fd);
        free(h);
    }
    return value_int(0);
}

/* bytes.string_to_bytes / bytes.bytes_to_string -- in this file only
 * because they are the other half of what a socket carries. */
static Value native_bytes_string_to_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type arr_type = byte_array_type();
    const char *s = args[0].as.s;
    size_t n = strlen(s);
    Value *items = malloc(sizeof(Value) * (n > 0 ? n : 1));
    for (size_t i = 0; i < n; i++) items[i] = value_byte((unsigned char)s[i]);
    return value_array(arr_type, array_obj_new(items, (int)n));
}

static Value native_bytes_bytes_to_string(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str_type = string_type();
    ArrayObj *arr = args[0].as.arr;
    char *s = malloc((size_t)arr->count + 1);
    for (int i = 0; i < arr->count; i++) {
        int b = arr->items[i].as.by;
        if (b == 0) {
            free(s);
            return value_result(type_result(str_type, str_type), result_obj_new(0, value_string(strdup("bytes contain a NUL byte and cannot be a string"))));
        }
        s[i] = (char)b;
    }
    s[arr->count] = '\0';
    return value_result(type_result(str_type, str_type), result_obj_new(1, value_string(s)));
}

void native_net_register(void) {
    signal(SIGPIPE, SIG_IGN);
    native_register("net::connect_raw", native_net_connect_raw);
    native_register("net::listen_raw", native_net_listen_raw);
    native_register("net::listener_port_raw", native_net_listener_port_raw);
    native_register("net::accept_raw", native_net_accept_raw);
    native_register("net::read_raw", native_net_read_raw);
    native_register("net::read_exact_raw", native_net_read_exact_raw);
    native_register("net::write_raw", native_net_write_raw);
    native_register("net::read_text_raw", native_net_read_text_raw);
    native_register("net::write_text_raw", native_net_write_text_raw);
    native_register("net::set_timeout_raw", native_net_set_timeout_raw);
    native_register("net::close_raw", native_net_close_raw);
    native_register("bytes::string_to_bytes", native_bytes_string_to_bytes);
    native_register("bytes::bytes_to_string", native_bytes_bytes_to_string);
}
