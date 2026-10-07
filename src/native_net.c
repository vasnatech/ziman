#define _POSIX_C_SOURCE 200809L
#include "native_net.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "native_os.h"

/* Blocking TCP behind lib/core/net.zm. A connection or listener is an int
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

/* `ssl` is non-NULL once start_tls has upgraded the connection; every
 * read/write/close then goes through it (see io_read/io_write below), so
 * read/read_exact/write/read_text/set_timeout work over TLS without the
 * callers -- or pg, or a future HTTP layer -- knowing. OpenSSL forbids
 * two threads touching one SSL object at once, so a TLS handle has a
 * lock taken around each operation (a plain socket doesn't need one:
 * the OS lets one thread read while another writes). */
typedef struct {
    int fd;
    SSL *ssl;
    SSL_CTX *server_ctx; /* non-NULL on a TLS listener: accept() completes a handshake with it */
    pthread_mutex_t tls_mu;
} NetHandle;

/* The handle Ziman holds is the NetHandle's address; g_live says which addresses are open
 * sockets. A handle that was closed, or forged, is not in it (an error, not a crash). Being in the
 * set does not keep the NetHandle alive: one whose Socket value nobody holds any more is
 * collected, and net_finalizer closes its descriptor -- so forgetting `net.close` no longer leaks
 * the descriptor for good (close it promptly anyway: a collection may be far off). */
static HandleSet g_live = HANDLE_SET_INIT;

static void release_handle(NetHandle *h) {
    if (h->server_ctx != NULL) {
        SSL_CTX_free(h->server_ctx);
        h->server_ctx = NULL;
    }
    if (h->ssl != NULL) {
        SSL_free(h->ssl);
        h->ssl = NULL;
    }
    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }
}

#ifdef ZIMAN_GC
static void net_finalizer(void *obj, void *client_data) {
    (void)client_data;
    NetHandle *h = obj;
    if (handleset_remove(&g_live, h)) release_handle(h);
}
#endif

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

static int64_t handle_register(int fd) {
    NetHandle *h = calloc(1, sizeof(NetHandle));
    h->fd = fd;
    pthread_mutex_init(&h->tls_mu, NULL);
    handleset_add(&g_live, h);
#ifdef ZIMAN_GC
    GC_REGISTER_FINALIZER_NO_ORDER(h, net_finalizer, NULL, NULL, NULL);
#endif
    return (int64_t)(intptr_t)h;
}

/* The handle for `id`, or NULL if it is unknown or closed. */
static NetHandle *handle_get(int64_t id) {
    NetHandle *h = (NetHandle *)(intptr_t)id;
    return handleset_has(&g_live, h) ? h : NULL;
}

static NetHandle *require_handle(int64_t id, const char *what, int line, int col) {
    NetHandle *h = handle_get(id);
    if (h == NULL) ziman_error("runtime", line, col, "net.%s: invalid or closed handle %lld", what, (long long)id);
    return h;
}

static int require_fd(int64_t id, const char *what, int line, int col) {
    return require_handle(id, what, line, col)->fd;
}

/* ---- I/O over a plain socket or, after start_tls, over TLS ---- */

typedef struct {
    int errno_val;   /* set for a plain-socket failure */
    int is_tls;      /* set for a TLS-level failure (bad record, alert, ...) */
    char message[256];
} IoErr;

static void tls_error_message(IoErr *e, SSL *ssl) {
    unsigned long code = ERR_get_error();
    char buf[200];
    if (code != 0) ERR_error_string_n(code, buf, sizeof(buf));
    else snprintf(buf, sizeof(buf), "TLS protocol error");
    long verify = ssl != NULL ? SSL_get_verify_result(ssl) : X509_V_OK;
    if (verify != X509_V_OK) {
        snprintf(e->message, sizeof(e->message), "%s (%s)", buf, X509_verify_cert_error_string(verify));
    } else {
        snprintf(e->message, sizeof(e->message), "%s", buf);
    }
    e->is_tls = 1;
}

/* Reads up to n bytes. Returns the count (> 0), 0 if the peer closed the
 * connection cleanly, or -1 with *e filled in. */
static ssize_t io_read(NetHandle *h, void *buf, size_t n, IoErr *e) {
    memset(e, 0, sizeof(*e));
    if (h->ssl == NULL) {
        ssize_t r;
        do { r = recv(h->fd, buf, n, 0); } while (r < 0 && errno == EINTR);
        if (r < 0) e->errno_val = errno;
        return r;
    }
    pthread_mutex_lock(&h->tls_mu);
    ERR_clear_error();
    int r = SSL_read(h->ssl, buf, (int)n);
    if (r > 0) { pthread_mutex_unlock(&h->tls_mu); return r; }
    int code = SSL_get_error(h->ssl, r);
    ssize_t out = -1;
    if (code == SSL_ERROR_ZERO_RETURN) {
        out = 0;
    } else if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
        e->errno_val = EAGAIN; /* a receive timeout on the underlying socket */
    } else if (code == SSL_ERROR_SYSCALL) {
        if (errno == 0 || ERR_peek_error() == 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) e->errno_val = EAGAIN;
            else if (errno == 0) out = 0; /* peer closed without a close_notify */
            else e->errno_val = errno;
        } else {
            tls_error_message(e, h->ssl);
        }
    } else {
        tls_error_message(e, h->ssl);
    }
    pthread_mutex_unlock(&h->tls_mu);
    return out;
}

/* Writes all n bytes. Returns n, or -1 with *e filled in. */
static ssize_t io_write(NetHandle *h, const unsigned char *data, size_t n, IoErr *e) {
    memset(e, 0, sizeof(*e));
    size_t sent = 0;
    while (sent < n) {
        if (h->ssl == NULL) {
            ssize_t w;
            do { w = send(h->fd, data + sent, n - sent, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
            if (w < 0) { e->errno_val = errno; return -1; }
            sent += (size_t)w;
        } else {
            pthread_mutex_lock(&h->tls_mu);
            ERR_clear_error();
            int w = SSL_write(h->ssl, data + sent, (int)(n - sent));
            if (w > 0) {
                pthread_mutex_unlock(&h->tls_mu);
                sent += (size_t)w;
                continue;
            }
            int code = SSL_get_error(h->ssl, w);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) e->errno_val = EAGAIN;
            else if (code == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) e->errno_val = errno != 0 ? errno : EPIPE;
            else if (code == SSL_ERROR_ZERO_RETURN) e->errno_val = EPIPE;
            else tls_error_message(e, h->ssl);
            pthread_mutex_unlock(&h->tls_mu);
            return -1;
        }
    }
    return (ssize_t)n;
}

static Value io_error_result(Type ok_type, const IoErr *e) {
    if (e->is_tls) return err_result(ok_type, "tls", e->message);
    return errno_result(ok_type, e->errno_val != 0 ? e->errno_val : EIO);
}

/* Out of descriptors: sockets that were dropped without `net.close` are only released when the
 * collector next runs, which may be far off. Collect now and run their finalizers, then the caller
 * retries once. Returns 0 (nothing to retry) when there is no collector. */
static int reclaim_descriptors(void) {
#ifdef ZIMAN_GC
    GC_gcollect();
    GC_invoke_finalizers();
    return 1;
#else
    return 0;
#endif
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
        if (fd < 0 && (errno == EMFILE || errno == ENFILE) && reclaim_descriptors()) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        }
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
    return open_socket(value_cstr(args[0]), args[1].as.i, 0);
}

static Value native_net_listen_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return open_socket(value_cstr(args[0]), args[1].as.i, 1);
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

/* The address at the other end of a connected socket, as text: "192.0.2.7" or "2001:db8::1", with no
 * port. Empty string when the socket is not connected or the family is one this does not know, which a
 * caller reads as "no address available" rather than an error -- there is nothing to retry.
 *
 * This is the transport peer and nothing else. It is deliberately *not* `X-Forwarded-For`: that header
 * is written by whoever is in front, and a client that is not behind a proxy can put anything in it, so
 * a server that read it by default would let any caller choose its own identity -- which is exactly
 * backwards for rate limiting, the reason this exists (B10). An application behind a proxy it trusts
 * has to say so itself and then read the header itself; the library will not guess. */
static Value native_net_peer_address_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    int fd = require_fd(args[0].as.i, "peer_address", line, col);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) return value_string(strdup(""));
    char text[INET6_ADDRSTRLEN];
    const void *addr = NULL;
    if (ss.ss_family == AF_INET) addr = &((struct sockaddr_in *)&ss)->sin_addr;
    else if (ss.ss_family == AF_INET6) addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    if (addr == NULL || inet_ntop(ss.ss_family, addr, text, sizeof text) == NULL) {
        return value_string(strdup(""));
    }
    return value_string(strdup(text));
}

static Value native_net_accept_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    int fd = require_fd(args[0].as.i, "accept", line, col);
    int c;
    /* Wait in short slices so a shutdown request (a signal can land on any
     * thread) is noticed instead of blocking in accept forever. */
    for (;;) {
        if (ziman_shutdown_requested()) return errno_result(int_type, EINTR);
        struct pollfd pfd = {fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 250);
        if (ready < 0 && errno != EINTR) return errno_result(int_type, errno);
        if (ready > 0) break;
    }
    do { c = accept(fd, NULL, NULL); } while (c < 0 && errno == EINTR);
    if (c < 0 && (errno == EMFILE || errno == ENFILE) && reclaim_descriptors()) c = accept(fd, NULL, NULL);
    if (c < 0) return errno_result(int_type, errno);
    SSL_CTX *server_ctx = require_handle(args[0].as.i, "accept", line, col)->server_ctx;
    if (server_ctx == NULL) return ok_result(int_type, value_int(handle_register(c)));

    /* A TLS listener: finish the handshake before handing the socket out, but never wait
     * long for a client that starts one and stalls. */
    struct timeval limit = {10, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
    ERR_clear_error();
    SSL *ssl = SSL_new(server_ctx);
    if (ssl == NULL) { close(c); IoErr e; tls_error_message(&e, NULL); return err_result(int_type, "tls", e.message); }
    SSL_set_fd(ssl, c);
    if (SSL_accept(ssl) != 1) {
        IoErr e;
        tls_error_message(&e, ssl);
        SSL_free(ssl);
        close(c);
        return err_result(int_type, "tls", e.message);
    }
    struct timeval none = {0, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    int64_t id = handle_register(c);
    handle_get(id)->ssl = ssl;
    return ok_result(int_type, value_int(id));
}

/* listen_tls_raw(host, port, cert_file, key_file): a listener whose accepted sockets are already
 * TLS (TLS 1.2 or later, the certificate chain from a PEM file and its private key). */
static Value native_net_listen_tls_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type int_type = type_scalar(TY_INT);
    ERR_clear_error();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) { IoErr e; tls_error_message(&e, NULL); return err_result(int_type, "tls", e.message); }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    const char *cert = value_cstr(args[2]);
    const char *key = value_cstr(args[3]);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 || SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        IoErr e;
        tls_error_message(&e, NULL);
        char msg[400];
        snprintf(msg, sizeof msg, "cannot use certificate '%s' and key '%s': %s", cert, key, e.message);
        SSL_CTX_free(ctx);
        return err_result(int_type, "tls", msg);
    }
    Value opened = open_socket(value_cstr(args[0]), args[1].as.i, 1);
    if (!opened.as.res->is_ok) { SSL_CTX_free(ctx); return opened; }
    NetHandle *h = handle_get(opened.as.res->value.as.i);
    h->server_ctx = ctx;
    return opened;
}

/* Reads up to `max` bytes (at least one -- blocks until something is
 * available). 0 bytes means the peer closed cleanly, reported as the
 * "closed" category rather than an empty array, so a caller can't forget
 * to check for EOF. */
static Value native_net_read_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = byte_array_type();
    NetHandle *h = require_handle(args[0].as.i, "read", line, col);
    int64_t max = args[1].as.i;
    if (max <= 0) return err_result(arr_type, "io", "read: max must be positive");
    unsigned char *buf = malloc((size_t)max);
    IoErr e;
    ssize_t n = io_read(h, buf, (size_t)max, &e);
    if (n < 0) { free(buf); return io_error_result(arr_type, &e); }
    if (n == 0) { free(buf); return err_result(arr_type, "closed", "connection closed by peer"); }
    Value bytes = value_byte_array(buf, (size_t)n);
    free(buf);
    return ok_result(arr_type, bytes);
}

static Value native_net_read_exact_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = byte_array_type();
    NetHandle *h = require_handle(args[0].as.i, "read_exact", line, col);
    int64_t want = args[1].as.i;
    if (want < 0) return err_result(arr_type, "io", "read_exact: count must not be negative");
    unsigned char *buf = zalloc_atomic((size_t)want + 1);
    int64_t got = 0;
    while (got < want) {
        IoErr e;
        ssize_t n = io_read(h, buf + got, (size_t)(want - got), &e);
        if (n < 0) return io_error_result(arr_type, &e);
        if (n == 0) return err_result(arr_type, "closed", "connection closed before the requested bytes arrived");
        got += n;
    }
    return ok_result(arr_type, value_array(arr_type, array_obj_new_bytes(buf, (int)want)));
}

static Value send_all(NetHandle *h, const unsigned char *data, size_t n) {
    Type int_type = type_scalar(TY_INT);
    IoErr e;
    if (io_write(h, data, n, &e) < 0) return io_error_result(int_type, &e);
    return ok_result(int_type, value_int((int64_t)n));
}

static Value native_net_write_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    NetHandle *h = require_handle(args[0].as.i, "write", line, col);
    ArrayObj *arr = args[1].as.arr;
    if (arr->bytes != NULL) return send_all(h, arr->bytes, (size_t)arr->count);
    uint8_t *buf = array_copy_bytes(arr, NULL);
    Value r = send_all(h, buf, (size_t)arr->count);
    free(buf);
    return r;
}

/* Text variants: a Ziman string is NUL-terminated, so text mode cannot
 * carry a 0 byte. A read that receives one reports it rather than
 * silently truncating; use read/write ([byte]) for binary data. */
static Value native_net_write_text_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    NetHandle *h = require_handle(args[0].as.i, "write_text", line, col);
    return send_all(h, (const unsigned char *)args[1].as.str.ptr, args[1].as.str.len);
}

static Value native_net_read_text_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type str_type = string_type();
    NetHandle *h = require_handle(args[0].as.i, "read_text", line, col);
    int64_t max = args[1].as.i;
    if (max <= 0) return err_result(str_type, "io", "read_text: max must be positive");
    char *buf = malloc((size_t)max + 1);
    IoErr e;
    ssize_t n = io_read(h, buf, (size_t)max, &e);
    if (n < 0) { free(buf); return io_error_result(str_type, &e); }
    if (n == 0) { free(buf); return err_result(str_type, "closed", "connection closed by peer"); }
    buf[n] = '\0';
    if (strlen(buf) != (size_t)n) {
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

/* net.start_tls_raw(handle, host, verify, ca_file) -> Result<int, string>:
 * upgrades an already-connected socket to TLS in place (a client
 * handshake). `verify`: 0 = do not verify the certificate at all
 * (encryption only -- open to a man in the middle), 1 = verify the chain
 * against the trusted CAs, 2 = the chain *and* that the certificate is
 * valid for `host` (a DNS name, or an IP address literal). `ca_file`, if
 * non-empty, names a PEM file of trusted CAs used instead of the system
 * store. Minimum protocol version is TLS 1.2; the server name is sent
 * for SNI unless `host` is an IP literal. */
static Value native_net_start_tls_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    NetHandle *h = require_handle(args[0].as.i, "start_tls", line, col);
    const char *host = value_cstr(args[1]);
    int64_t verify = args[2].as.i;
    const char *ca_file = value_cstr(args[3]);
    if (h->ssl != NULL) return err_result(int_type, "tls", "connection is already using TLS");

    unsigned char addr[sizeof(struct in6_addr)];
    int host_is_ip = inet_pton(AF_INET, host, addr) == 1 || inet_pton(AF_INET6, host, addr) == 1;

    ERR_clear_error();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) { IoErr e; tls_error_message(&e, NULL); return err_result(int_type, "tls", e.message); }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (verify > 0) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        int ok = ca_file[0] != '\0' ? SSL_CTX_load_verify_locations(ctx, ca_file, NULL) : SSL_CTX_set_default_verify_paths(ctx);
        if (!ok) {
            IoErr e;
            tls_error_message(&e, NULL);
            char msg[300];
            snprintf(msg, sizeof(msg), "cannot load trusted certificates%s%s: %s", ca_file[0] != '\0' ? " from " : "", ca_file, e.message);
            SSL_CTX_free(ctx);
            return err_result(int_type, "tls", msg);
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx); /* the SSL holds its own reference */
    if (ssl == NULL) { IoErr e; tls_error_message(&e, NULL); return err_result(int_type, "tls", e.message); }
    SSL_set_fd(ssl, h->fd);
    if (!host_is_ip) SSL_set_tlsext_host_name(ssl, host);
    if (verify >= 2) {
        int ok = host_is_ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) : SSL_set1_host(ssl, host);
        if (!ok) { SSL_free(ssl); return err_result(int_type, "tls", "cannot set the expected host name"); }
    }

    int r = SSL_connect(ssl);
    if (r != 1) {
        IoErr e;
        int code = SSL_get_error(ssl, r);
        if ((code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) || (code == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            SSL_free(ssl);
            return err_result(int_type, "timeout", "TLS handshake timed out");
        }
        if (code == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
            SSL_free(ssl);
            return err_result(int_type, "closed", "connection closed during the TLS handshake");
        }
        tls_error_message(&e, ssl);
        SSL_free(ssl);
        return err_result(int_type, "tls", e.message);
    }
    h->ssl = ssl;
    return ok_result(int_type, value_int(0));
}

/* ---- an idle-connection cache, for HTTP keep-alive ------------------------------------------
 * A few open sockets parked by key ("https://host:443") so the next request to the same place can
 * reuse one instead of connecting again. The cache holds them strongly (so they are not
 * collected while parked) and drops any that have been idle longer than IDLE_SECONDS. The peer may
 * still have closed a parked socket; the caller (httpclient) retries on a fresh connection when
 * that shows. */
enum { IDLE_MAX = 32, IDLE_SECONDS = 30 };
typedef struct {
    char *key;
    NetHandle *h;
    time_t since;
} IdleEntry;
static IdleEntry g_idle[IDLE_MAX];
static pthread_mutex_t g_idle_mu = PTHREAD_MUTEX_INITIALIZER;

static void idle_drop(IdleEntry *e) {
    if (e->h != NULL && handleset_remove(&g_live, e->h)) {
        if (e->h->ssl != NULL) SSL_shutdown(e->h->ssl);
        if (e->h->fd >= 0) shutdown(e->h->fd, SHUT_RDWR);
        release_handle(e->h);
    }
    e->h = NULL;
    e->key = NULL;
}

static Value native_net_idle_put_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    NetHandle *h = handle_get(args[1].as.i);
    if (h == NULL) return value_bool(0);
    const char *key = value_cstr(args[0]);
    time_t now = time(NULL);
    pthread_mutex_lock(&g_idle_mu);
    int slot = -1;
    for (int i = 0; i < IDLE_MAX; i++) {
        if (g_idle[i].h != NULL && now - g_idle[i].since > IDLE_SECONDS) idle_drop(&g_idle[i]);
        if (g_idle[i].h == NULL && slot < 0) slot = i;
    }
    if (slot < 0) { /* full: replace the one parked longest */
        slot = 0;
        for (int i = 1; i < IDLE_MAX; i++) if (g_idle[i].since < g_idle[slot].since) slot = i;
        idle_drop(&g_idle[slot]);
    }
    g_idle[slot].key = strdup(key);
    g_idle[slot].h = h;
    g_idle[slot].since = now;
    pthread_mutex_unlock(&g_idle_mu);
    return value_bool(1);
}

static Value native_net_idle_take_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *key = value_cstr(args[0]);
    time_t now = time(NULL);
    int64_t found = -1;
    pthread_mutex_lock(&g_idle_mu);
    int best = -1;
    for (int i = 0; i < IDLE_MAX; i++) {
        if (g_idle[i].h == NULL) continue;
        if (now - g_idle[i].since > IDLE_SECONDS) { idle_drop(&g_idle[i]); continue; }
        if (strcmp(g_idle[i].key, key) == 0 && (best < 0 || g_idle[i].since >= g_idle[best].since)) best = i;
    }
    if (best >= 0) {
        found = (int64_t)(intptr_t)g_idle[best].h;
        g_idle[best].h = NULL;
        g_idle[best].key = NULL;
    }
    pthread_mutex_unlock(&g_idle_mu);
    return value_int(found);
}

static Value native_net_idle_clear_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    pthread_mutex_lock(&g_idle_mu);
    for (int i = 0; i < IDLE_MAX; i++) if (g_idle[i].h != NULL) idle_drop(&g_idle[i]);
    pthread_mutex_unlock(&g_idle_mu);
    return value_int(0);
}

static Value native_net_close_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    NetHandle *h = (NetHandle *)(intptr_t)args[0].as.i;
    if (h != NULL && handleset_remove(&g_live, h)) {
        if (h->ssl != NULL) SSL_shutdown(h->ssl); /* sends close_notify; the peer's reply isn't awaited */
        if (h->fd >= 0) shutdown(h->fd, SHUT_RDWR); /* wakes a thread blocked reading this socket */
        release_handle(h);
    }
    return value_int(0);
}

void native_net_register(void) {
    signal(SIGPIPE, SIG_IGN);
    native_register("net::connect_raw", native_net_connect_raw);
    native_register("net::listen_raw", native_net_listen_raw);
    native_register("net::listener_port_raw", native_net_listener_port_raw);
    native_register("net::peer_address_raw", native_net_peer_address_raw);
    native_register("net::accept_raw", native_net_accept_raw);
    native_register("net::idle_put_raw", native_net_idle_put_raw);
    native_register("net::idle_take_raw", native_net_idle_take_raw);
    native_register("net::idle_clear_raw", native_net_idle_clear_raw);
    native_register("net::listen_tls_raw", native_net_listen_tls_raw);
    native_register("net::read_raw", native_net_read_raw);
    native_register("net::read_exact_raw", native_net_read_exact_raw);
    native_register("net::write_raw", native_net_write_raw);
    native_register("net::read_text_raw", native_net_read_text_raw);
    native_register("net::write_text_raw", native_net_write_text_raw);
    native_register("net::set_timeout_raw", native_net_set_timeout_raw);
    native_register("net::start_tls_raw", native_net_start_tls_raw);
    native_register("net::close_raw", native_net_close_raw);
}
