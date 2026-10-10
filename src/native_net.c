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
#include <sys/un.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "native_os.h"
#include "xalloc.h"

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
 * program should only close a socket from the thread that owns it.
 *
 * What that costs, exactly, since this is the one part of this module two
 * threads touch at once (docs/reviews/tasks.md A24). The descriptor is
 * read and cleared atomically, so there is no data race on the field and
 * exactly one thread ever hands it to close(2) -- the clear is an
 * exchange, and `handleset_remove` already decides which thread gets
 * there. A reader that arrives after the close sees -1 and is told the
 * socket is closed.
 *
 * What remains is not fixable by ordering, and is the reason the advice
 * above still stands: a reader that loaded the number *before* the close
 * still holds it afterwards, and if any thread opens anything in between,
 * the OS may hand that same number back -- so the read lands on an
 * unrelated socket, file or database connection rather than failing.
 * Closing that window needs the descriptor to outlive its last user: a
 * reference count on the handle, with close(2) deferred to the last
 * release, or a close deferred to the collector's finalizer (net_finalizer
 * already does exactly that for a handle nobody holds). Neither is here
 * because neither is free -- a refcount puts an atomic pair around every
 * read and write -- and because a server that closes from its accepting
 * thread, which is what `server.zm` does, never opens the window. */

/* `ssl` is non-NULL once start_tls has upgraded the connection; every
 * read/write/close then goes through it (see io_read/io_write below), so
 * read/read_exact/write/read_text/set_timeout work over TLS without the
 * callers -- or pg, or a future HTTP layer -- knowing. OpenSSL forbids
 * two threads touching one SSL object at once, so a TLS handle has a
 * lock taken around each operation (a plain socket doesn't need one:
 * the OS lets one thread read while another writes). */
typedef struct {
    /* Read and cleared with the __atomic builtins, never plainly: one thread can close a handle
     * while another is reading or accepting on it, which is how a server shuts down, and that made
     * this field a data race TSan reported on six fixtures (docs/reviews/tasks.md A24). The clear is
     * an exchange, so exactly one thread ever passes the descriptor to close(2). */
    int fd;
    /* Whether this handle has ever carried TLS. Set once, released, and never cleared -- it exists
     * so the plain-socket path can decide without taking `tls_mu`, which on that path would be a
     * lock per read and write (see A34 for what the one lock already on it costs). A handle whose
     * TLS has since been torn down keeps the flag, so a reader still takes the lock and finds
     * `ssl` NULL, which is the answer it should get. */
    int tls;
    /* How many threads are inside a call that is using this handle, and whether a close is waiting
     * for them. Both exist so a descriptor cannot be closed -- and its *number* reused by the OS --
     * while another thread still holds it: `accept_raw` in particular holds the number across an
     * unbounded series of 250 ms poll waits, and that window was demonstrable, not theoretical
     * (10 runs out of 10 produced ENOTSOCK from a pure-Ziman program; 6 out of 6 of another got
     * accept(2) to consume a connection meant for a different listener -- docs/reviews/tasks.md
     * A31). `refs` is incremented while `g_live`'s lock is held, so it cannot race the removal that
     * decides a close, and atomically, because the decrement does not take that lock; it is
     * decremented by whichever thread finishes last. */
    int refs;
    int closing;
    /* Touched only while holding `tls_mu`, never plainly. It used to be read here and freed in
     * release_handle with nothing between them, which is a use-after-free on an OpenSSL object --
     * worse than A24's recycled descriptor, because the memory is freed heap rather than a number.
     * It stayed invisible because `test-tls` is not in `make check`, so no TLS path has ever run
     * under a sanitizer (docs/reviews/tasks.md A32). */
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

#ifdef ZIMAN_GC
/* Defined below, beside the rest of the handle bookkeeping; declared here because net_finalizer has
 * to sit next to the allocation it finalizes and needs to retire one. Takes the handle as void * so
 * this does not have to move the NetHandle typedef above the finalizer. */
static int handle_retire_p(void *h);
#endif

/* Publishes an SSL on a handle: the pointer under `tls_mu`, then the flag with a release store, so
 * a thread that sees the flag set is guaranteed to see the pointer too. Until the flag is set, the
 * handle reads and writes as a plain socket, which is what it still is. */
static void install_ssl(NetHandle *h, SSL *ssl) {
    pthread_mutex_lock(&h->tls_mu);
    h->ssl = ssl;
    pthread_mutex_unlock(&h->tls_mu);
    __atomic_store_n(&h->tls, 1, __ATOMIC_RELEASE);
}

static void release_handle(NetHandle *h) {
    if (h->server_ctx != NULL) {
        SSL_CTX_free(h->server_ctx);
        h->server_ctx = NULL;
    }
    /* Under `tls_mu`, which every TLS read and write also holds: a thread inside SSL_read therefore
     * finishes before the object is freed, and one arriving afterwards finds NULL and is told the
     * socket is closed. The shutdown goes here too -- its callers used to read `h->ssl` plainly to
     * decide whether to send a close_notify, which was the same race one line earlier. */
    pthread_mutex_lock(&h->tls_mu);
    SSL *ssl = h->ssl;
    h->ssl = NULL;
    if (ssl != NULL) {
        SSL_shutdown(ssl); /* sends close_notify; the peer's reply isn't awaited */
        SSL_free(ssl);
    }
    pthread_mutex_unlock(&h->tls_mu);
    /* Take the descriptor out of the handle and close the copy: an exchange, so a second caller
     * racing here gets -1 and closes nothing, and every later reader sees -1 rather than a number
     * that is on its way to being closed. */
    int fd = __atomic_exchange_n(&h->fd, -1, __ATOMIC_ACQ_REL);
    if (fd >= 0) close(fd);
}

#ifdef ZIMAN_GC
static void net_finalizer(void *obj, void *client_data) {
    (void)client_data;
    handle_retire_p(obj);
}
#endif

static Type string_type(void) { return type_scalar(TY_STRING); }
static Type byte_array_type(void) { return type_array(type_scalar(TY_BYTE)); }

static Value err_result(Type ok_type, const char *category, const char *message) {
    size_t n = strlen(category) + strlen(message) + 2;
    char *s = ziman_xmalloc(n);
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
    NetHandle *h = ziman_xcalloc(1, sizeof(NetHandle));
    __atomic_store_n(&h->fd, fd, __ATOMIC_RELAXED); /* before handleset_add publishes `h`; see the field */

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

/* Called by handleset_has_then with `g_live`'s lock held: see handles.h. Atomic as well as locked,
 * and both for different reasons -- the lock is what stops it racing the `handleset_remove` that
 * decides a close, and the atomic is what stops it racing a *decrement*, which handle_unuse does
 * without the lock. Getting that wrong is a real race and not a sanitizer artifact: with a plain
 * `++` here, TSan reported it on four fixtures. */
static void ref_inc(void *p) { __atomic_fetch_add(&((NetHandle *)p)->refs, 1, __ATOMIC_ACQ_REL); }

/* The handle for `id`, with a reference held, or NULL if it is not live. The descriptor stays open
 * until the matching handle_unuse, so the number cannot be recycled under the caller -- which is
 * the whole point (A31). Every path out of a function that calls this must call handle_unuse: there
 * is deliberately no ziman_error between the two anywhere in this file, because a longjmp past the
 * release would leak the reference and the descriptor would never close, which is worse than the
 * bug being fixed. */
static NetHandle *handle_use(int64_t id) {
    NetHandle *h = (NetHandle *)(intptr_t)id;
    return handleset_has_then(&g_live, h, ref_inc) ? h : NULL;
}

/* The same for a handle already in hand: 1 if it was still live and a reference is now held. The
 * NetHandle itself cannot go away under a caller -- it is collector memory, rooted by the Ziman
 * Socket value that named it (see src/handles.c on what roots a handle) -- so this is only ever
 * about the descriptor *number*, which is what gets recycled. */
static int handle_acquire(NetHandle *h) {
    return handleset_has_then(&g_live, h, ref_inc);
}

/* Releases the handle for good once the last user is gone and a close is waiting. Safe to reach
 * from both sides of the race -- the closer, having seen `refs == 0`, and the last user, having
 * seen `closing` -- because release_handle is idempotent by construction: it takes the descriptor
 * with an atomic exchange (A24) and the SSL under `tls_mu` with a NULL check (A32), so whichever
 * arrives second does nothing. */
static void release_handle(NetHandle *h);

static void handle_unuse(NetHandle *h) {
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0 &&
        __atomic_load_n(&h->closing, __ATOMIC_ACQUIRE)) {
        release_handle(h);
    }
}

/* Takes the handle out of the live set -- after which no new reference can be taken -- and closes
 * it if nobody is using it right now. 1 if this caller was the one that removed it. */
static int handle_retire(NetHandle *h) {
    if (h == NULL || !handleset_remove(&g_live, h)) return 0;
    __atomic_store_n(&h->closing, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&h->refs, __ATOMIC_ACQUIRE) == 0) release_handle(h);
    return 1;
}

#ifdef ZIMAN_GC
static int handle_retire_p(void *h) { return handle_retire((NetHandle *)h); }
#endif

static NetHandle *require_use(int64_t id, const char *what, int line, int col) {
    NetHandle *h = handle_use(id);
    if (h == NULL) ziman_error("runtime", line, col, "net.%s: invalid or closed handle %lld", what, (long long)id);
    return h;
}

static NetHandle *require_handle(int64_t id, const char *what, int line, int col) {
    NetHandle *h = handle_get(id);
    if (h == NULL) ziman_error("runtime", line, col, "net.%s: invalid or closed handle %lld", what, (long long)id);
    return h;
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
    if (!__atomic_load_n(&h->tls, __ATOMIC_ACQUIRE)) {
        /* A reference across the recv, not just an atomic load of the number: a blocking read can
         * sit here for as long as its timeout allows, and without the reference a close on another
         * thread frees the number for the OS to hand to something else mid-read (A31). A handle
         * already closed answers EBADF, which errno_result reports as the `closed` category -- the
         * same answer the kernel would give, decided here instead of discovered there. */
        if (!handle_acquire(h)) { e->errno_val = EBADF; return -1; }
        int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
        if (fd < 0) { handle_unuse(h); e->errno_val = EBADF; return -1; }
        ssize_t r;
        do { r = recv(fd, buf, n, 0); } while (r < 0 && errno == EINTR);
        if (r < 0) e->errno_val = errno;
        handle_unuse(h);
        return r;
    }
    pthread_mutex_lock(&h->tls_mu);
    /* Read once, under the lock, and used throughout: the handle's own field may be cleared by a
     * close the moment this lock is released. */
    SSL *ssl = h->ssl;
    if (ssl == NULL) { /* closed by another thread; see the field's own comment */
        pthread_mutex_unlock(&h->tls_mu);
        e->errno_val = EBADF;
        return -1;
    }
    ERR_clear_error();
    int r = SSL_read(ssl, buf, (int)n);
    if (r > 0) { pthread_mutex_unlock(&h->tls_mu); return r; }
    int code = SSL_get_error(ssl, r);
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
            tls_error_message(e, ssl);
        }
    } else {
        tls_error_message(e, ssl);
    }
    pthread_mutex_unlock(&h->tls_mu);
    return out;
}

/* Writes all n bytes. Returns n, or -1 with *e filled in. */
static ssize_t io_write(NetHandle *h, const unsigned char *data, size_t n, IoErr *e) {
    memset(e, 0, sizeof(*e));
    size_t sent = 0;
    while (sent < n) {
        if (!__atomic_load_n(&h->tls, __ATOMIC_ACQUIRE)) {
            ssize_t w;
            if (!handle_acquire(h)) { e->errno_val = EBADF; return -1; } /* see io_read */
            int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
            if (fd < 0) { handle_unuse(h); e->errno_val = EBADF; return -1; }
            do { w = send(fd, data + sent, n - sent, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
            int failed = w < 0;
            int saved = errno;
            handle_unuse(h);
            if (failed) { e->errno_val = saved; return -1; }
            sent += (size_t)w;
        } else {
            pthread_mutex_lock(&h->tls_mu);
            SSL *ssl = h->ssl; /* once, under the lock -- see io_read */
            if (ssl == NULL) {
                pthread_mutex_unlock(&h->tls_mu);
                e->errno_val = EBADF;
                return -1;
            }
            ERR_clear_error();
            int w = SSL_write(ssl, data + sent, (int)(n - sent));
            if (w > 0) {
                pthread_mutex_unlock(&h->tls_mu);
                sent += (size_t)w;
                continue;
            }
            int code = SSL_get_error(ssl, w);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) e->errno_val = EAGAIN;
            else if (code == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) e->errno_val = errno != 0 ? errno : EPIPE;
            else if (code == SSL_ERROR_ZERO_RETURN) e->errno_val = EPIPE;
            else tls_error_message(e, ssl);
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

/* A Unix-domain socket, which `open_socket` above cannot reach: getaddrinfo resolves host and port
 * and there is neither here, only a filesystem path (ROADMAP.md's `net` follow-ups -- local
 * Postgres usually listens on one, which is the concrete consumer).
 *
 * Two things a caller has to know, and both are reported rather than papered over. The path has a
 * hard limit well under PATH_MAX -- `sun_path` is 108 bytes on Linux including its terminator -- so
 * a long path is refused with a message naming the limit instead of being silently truncated, which
 * is the classic way to end up bound to a path you did not ask for. And `bind` fails if the path
 * already exists, even when it is a leftover from a process that died: this does *not* unlink it
 * first, because a stale socket and a live one look identical from here and unlinking the wrong one
 * would take down a running server. The error says what to do. */
static Value open_unix_socket(const char *path, int listener) {
    Type int_type = type_scalar(TY_INT);
    struct sockaddr_un addr;
    size_t len = strlen(path);
    if (len == 0) return err_result(int_type, "io", "a Unix-domain socket path cannot be empty");
    if (len >= sizeof addr.sun_path) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "this Unix-domain socket path is %zu bytes; the limit is %zu, which is the operating system's, "
                 "not this language's", len, sizeof addr.sun_path - 1);
        return err_result(int_type, "io", msg);
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 && (errno == EMFILE || errno == ENFILE) && reclaim_descriptors()) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
    }
    if (fd < 0) return errno_result(int_type, errno);

    int ok;
    if (listener) {
        ok = bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) == 0 && listen(fd, 128) == 0;
        if (!ok && errno == EADDRINUSE) {
            int saved = errno;
            close(fd);
            (void)saved;
            char msg[320];
            snprintf(msg, sizeof msg,
                     "'%s' already exists, so it cannot be bound -- remove it with fs.remove if it is a socket "
                     "left behind by a process that has gone, and note that a live listener's path looks exactly "
                     "the same from here, which is why this is not removed for you", path);
            return err_result(int_type, "io", msg);
        }
    } else {
        int r;
        do { r = connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr); } while (r < 0 && errno == EINTR);
        ok = r == 0;
    }
    if (!ok) {
        int saved = errno;
        close(fd);
        return errno_result(int_type, saved);
    }
    return ok_result(int_type, value_int(handle_register(fd)));
}

static Value native_net_connect_unix_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return open_unix_socket(value_cstr(args[0]), 0);
}

static Value native_net_listen_unix_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return open_unix_socket(value_cstr(args[0]), 1);
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
    NetHandle *h = require_use(args[0].as.i, "listener_port", line, col);
    int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int64_t port = -1;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) == 0) {
        if (ss.ss_family == AF_INET) port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        else if (ss.ss_family == AF_INET6) port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    handle_unuse(h);
    return value_int(port);
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
    NetHandle *h = require_use(args[0].as.i, "peer_address", line, col);
    int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    char text[INET6_ADDRSTRLEN];
    const char *out = "";
    if (getpeername(fd, (struct sockaddr *)&ss, &len) == 0) {
        const void *addr = NULL;
        if (ss.ss_family == AF_INET) addr = &((struct sockaddr_in *)&ss)->sin_addr;
        else if (ss.ss_family == AF_INET6) addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
        if (addr != NULL && inet_ntop(ss.ss_family, addr, text, sizeof text) != NULL) out = text;
    }
    handle_unuse(h);
    return value_string(ziman_xstrdup(out));
}

static Value native_net_accept_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    /* One reference for the whole wait, which is the point of A31: this function holds the
     * descriptor number across an unbounded series of 250 ms poll slices, so without a reference a
     * close on another thread can free the number and the OS can hand it to something else before
     * the accept(2) below runs -- demonstrably, 10 runs out of 10. */
    NetHandle *h = require_use(args[0].as.i, "accept", line, col);
    int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
    int c;
    /* Wait in short slices so a shutdown request (a signal can land on any
     * thread) is noticed instead of blocking in accept forever. */
    for (;;) {
        if (ziman_shutdown_requested()) { handle_unuse(h); return errno_result(int_type, EINTR); }
        struct pollfd pfd = {fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 250);
        if (ready < 0 && errno != EINTR) { int se = errno; handle_unuse(h); return errno_result(int_type, se); }
        if (ready > 0) break;
    }
    do { c = accept(fd, NULL, NULL); } while (c < 0 && errno == EINTR);
    if (c < 0 && (errno == EMFILE || errno == ENFILE) && reclaim_descriptors()) c = accept(fd, NULL, NULL);
    if (c < 0) {
        /* `closed` rather than whatever errno the kernel chose, when the listener was closed by
         * this program while we were waiting: accept(2) on a shut-down socket answers EINVAL, which
         * errno_result would otherwise report as the `io` category -- true but unhelpful, since the
         * caller is a server whose shutdown just worked. Only this handle's own flag is consulted,
         * so a genuine EINVAL from anything else still reports as itself. */
        int se = errno;
        int closing = __atomic_load_n(&h->closing, __ATOMIC_ACQUIRE);
        handle_unuse(h);
        if (closing) return err_result(int_type, "closed", "the listener was closed");
        return errno_result(int_type, se);
    }
    /* Read through the reference already held rather than looking the handle up again. The second
     * lookup this replaces could fail -- the listener having been closed meanwhile -- and raise an
     * error with `c` already accepted and not yet registered, leaking that descriptor for the life
     * of the process; it is where the A31 reproduction landed, 6 times out of 6 (A33). */
    SSL_CTX *server_ctx = h->server_ctx;
    handle_unuse(h);
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
    install_ssl(handle_get(id), ssl);
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
    unsigned char *buf = ziman_xmalloc((size_t)max);
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
    char *buf = ziman_xmalloc((size_t)max + 1);
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
    NetHandle *h = require_use(args[0].as.i, "set_timeout", line, col);
    int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
    int64_t ms = args[1].as.i;
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    int failed = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
                 setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0;
    int saved_errno = errno;
    handle_unuse(h);
    if (failed) return errno_result(int_type, saved_errno);
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
    /* The set-once flag, not the pointer: a connection that had TLS and has since been closed is
     * still not a plain socket, and reading the pointer here would be the same race again. */
    if (__atomic_load_n(&h->tls, __ATOMIC_ACQUIRE)) return err_result(int_type, "tls", "connection is already using TLS");

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
    /* A reference from here to the end of the handshake. `SSL_set_fd` copies the descriptor number
     * into OpenSSL's BIO and `SSL_connect` then blocks on it for up to the socket's timeout, so
     * this is the third place that holds a number across a wait -- and the only one where the
     * number is held by a library rather than by this file (A31). */
    if (!handle_acquire(h)) { SSL_free(ssl); return err_result(int_type, "closed", "connection closed"); }
    SSL_set_fd(ssl, __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE));
    if (!host_is_ip) SSL_set_tlsext_host_name(ssl, host);
    if (verify >= 2) {
        int ok = host_is_ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) : SSL_set1_host(ssl, host);
        if (!ok) { handle_unuse(h); SSL_free(ssl); return err_result(int_type, "tls", "cannot set the expected host name"); }
    }

    int r = SSL_connect(ssl);
    handle_unuse(h);
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
    install_ssl(h, ssl);
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
    if (e->h != NULL) {
        /* Shut down first, retire second, and in that order for a reason: `shutdown` is what wakes
         * a thread blocked reading this socket, and unlike `close` it does not free the descriptor
         * *number*, so it is safe to call while another thread still holds one. Retiring first
         * would leave a blocked reader holding a reference with nothing to wake it. */
        int fd = __atomic_load_n(&e->h->fd, __ATOMIC_ACQUIRE);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        handle_retire(e->h);
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
    g_idle[slot].key = ziman_xstrdup(key);
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
    if (h != NULL) {
        /* shutdown then retire; see idle_drop for why that order */
        int fd = __atomic_load_n(&h->fd, __ATOMIC_ACQUIRE);
        if (fd >= 0) shutdown(fd, SHUT_RDWR); /* wakes a thread blocked reading this socket */
        handle_retire(h);
    }
    return value_int(0);
}

void native_net_register(void) {
    signal(SIGPIPE, SIG_IGN);
    native_register("net::connect_raw", native_net_connect_raw);
    native_register("net::listen_raw", native_net_listen_raw);
    native_register("net::connect_unix_raw", native_net_connect_unix_raw);
    native_register("net::listen_unix_raw", native_net_listen_unix_raw);
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
