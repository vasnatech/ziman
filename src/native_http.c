#define _POSIX_C_SOURCE 200809L
#include "native_http.h"

#include <stdlib.h>
#include <string.h>
#ifdef __GLIBC__
/* <malloc.h> declares malloc/calloc/realloc/free, and src/zgc.h is force-included into every
 * translation unit and has already redefined all four to route through Boehm -- so the header has to
 * be read with those macros suspended, or its own declarations expand into nonsense. Suspended rather
 * than undefined, so the rest of this file still allocates through the collector like everything else.
 * The alternative was to declare `mallopt` by hand and hardcode glibc's M_ARENA_MAX (-8), which works
 * but puts a number from another project's ABI in our source. */
#pragma push_macro("malloc")
#pragma push_macro("calloc")
#pragma push_macro("realloc")
#pragma push_macro("free")
#undef malloc
#undef calloc
#undef realloc
#undef free
#include <malloc.h>
#pragma pop_macro("free")
#pragma pop_macro("realloc")
#pragma pop_macro("calloc")
#pragma pop_macro("malloc")
#endif

#include "native.h"
#include "value.h"

/* RFC 7230's `token`: what a header name or a method may be made of. */
static int is_tchar(unsigned char c) {
    if (c >= '0' && c <= '9') return 1;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* `server::bound_malloc_arenas(n)`: caps how many malloc arenas glibc will create, called once by
 * `server.serve`. Lives in this file because it serves lib/http, which `server` belongs to.
 *
 * Why a server wants this (docs/reviews/tasks.md B9, measured 2026-10-06 and again 2026-10-07). glibc
 * gives each thread its own malloc arena and keeps what that thread allocated in it. The server is
 * thread-per-connection, and a password hash asks OpenSSL for a ~19 MiB Argon2id buffer, so every
 * connection that ever authenticated left 19 MiB of free-but-retained memory behind: 48 registrations
 * at 16 concurrent grew resident memory by 307 MB and never gave it back.
 *
 * Capping it at 1 from `serve` brings that run to 136 MB -- a 56% reduction, repeatable to within a
 * few MB. Setting `MALLOC_ARENA_MAX=1` in the environment instead reaches 98 MB, and the gap is not a
 * bug: glibc reads its variable at the first allocation of the process, while a `mallopt` from `serve`
 * can only bound arenas created *after* it runs, and three threads are already alive by then. So the
 * default here is the part a program can do for itself, and an operator who wants the floor still has
 * the environment variable -- which is also why this must not override it.
 *
 * Why it costs nothing to do, which is not obvious. Ziman's own allocation does not go through glibc
 * at all -- zgc.h redirects malloc to GC_MALLOC and Boehm takes its memory from mmap -- so the arenas
 * mostly hold what third-party C libraries allocate, which is exactly the buffer being wasted.
 * Measured at every concurrency rather than assumed: an authenticated read was 3.8% slower at 16
 * clients, 1.7% *faster* at 32 and 7% faster at 64; registrations and an allocation-heavy eight-thread
 * program with no server in it were both within noise.
 *
 * Only when the environment has not already spoken. `MALLOC_ARENA_MAX` is glibc's own variable, so an
 * operator who set it has made this decision explicitly and a default must not quietly override it.
 * Returns whether it took effect, which `serve` ignores -- there is nothing a server could usefully do
 * about a failure, and on a platform without glibc there is no such thing to fail. */
static Value native_server_bound_malloc_arenas(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    if (getenv("MALLOC_ARENA_MAX") != NULL) return value_bool(0);
#ifdef __GLIBC__
    long n = (long)args[0].as.i;
    if (n < 1) return value_bool(0);
    return value_bool(mallopt(M_ARENA_MAX, (int)n) == 1);
#else
    (void)args;
    return value_bool(0);
#endif
}

static Value native_http_is_token(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const unsigned char *s = (const unsigned char *)args[0].as.str.ptr;
    size_t n = (size_t)args[0].as.str.len;
    if (n == 0) return value_bool(0);
    for (size_t i = 0; i < n; i++) {
        if (!is_tchar(s[i])) return value_bool(0);
    }
    return value_bool(1);
}

/* A header value (or request target) may hold any byte but a control character other than a tab --
 * above all no CR or LF, which would end the header line and let the rest pass for another one. */
static Value native_http_is_field_value(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const unsigned char *s = (const unsigned char *)args[0].as.str.ptr;
    size_t n = (size_t)args[0].as.str.len;
    for (size_t i = 0; i < n; i++) {
        if ((s[i] < 0x20 && s[i] != '\t') || s[i] == 0x7f) return value_bool(0);
    }
    return value_bool(1);
}

void native_http_register(void) {
    native_register("server::bound_malloc_arenas", native_server_bound_malloc_arenas);
    native_register("http::is_token", native_http_is_token);
    native_register("http::is_field_value", native_http_is_field_value);
}
