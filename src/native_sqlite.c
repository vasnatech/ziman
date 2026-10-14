#define _POSIX_C_SOURCE 200809L
#include "native_sqlite.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "handles.h"
#include "native.h"
#include "xalloc.h"

/* The SQLite driver behind lib/db/sqlite.zm. Everything crossing the native
 * boundary is a primitive: a connection is an int handle (an index into
 * a table of individually heap-allocated DbConn structs, same pattern
 * MutableString uses in prelude.c), SQL is a string, and both bound
 * parameters and result cells travel as *tagged strings* -- one leading
 * tag character followed by the payload:
 *
 *   N            SQL NULL
 *   I<digits>    integer            (also how SQLite hands back a bool)
 *   F<%.17g>     float
 *   T<text>      text
 *   B<hex>       blob
 *   b0 / b1      bool (parameters only; bound as integer 0/1)
 *
 * lib/db/sqlite.zm encodes/decodes SqlValue to and from exactly this shape.
 * Deliberately not the typed interpreter Values directly: constructing a
 * user-declared enum (SqlValue) from C would need to know the loader's
 * qualified name for it, which a native function has no business
 * knowing.
 *
 * Every call on one connection takes that connection's own mutex, so
 * concurrent Ziman threads sharing a handle are safe statement by
 * statement. That does *not* make a multi-statement transaction safe to
 * share -- another thread's statements can interleave inside it; see
 * docs/DESIGN.md's "db" entry. */

typedef struct {
    sqlite3 *db;
    pthread_mutex_t mu;
} DbConn;

/* The handle Ziman holds is the DbConn's address; g_live says which addresses are open
 * connections (a closed or forged handle is an error, not a crash). Being in the set does not keep
 * the DbConn alive: one whose Connection value nobody holds any more is collected, and
 * conn_finalizer closes the database -- so forgetting `datasource.close` no longer leaks it for good
 * (close it promptly anyway: a collection may be far off, and a file database stays locked). */
static HandleSet g_live = HANDLE_SET_INIT;

/* Finalizes the deferred cursors of `c`. Called with `c->mu` held, from every function that takes
 * it; defined with the cursor code below, where the reason for it is written down. */
static void pending_drain(DbConn *c);

#ifdef ZIMAN_GC
static void conn_finalizer(void *obj, void *client_data) {
    (void)client_data;
    DbConn *c = obj;
    if (handleset_remove(&g_live, c)) {
        pthread_mutex_lock(&c->mu);
        pending_drain(c); /* a deferred cursor's statement, before the database it belongs to goes */
        if (c->db != NULL) sqlite3_close(c->db);
        c->db = NULL;
        pthread_mutex_unlock(&c->mu);
    }
}
#endif

static Type string_type(void) { return type_scalar(TY_STRING); }

static Value err_result(Type ok_type, const char *category, const char *message) {
    size_t n = strlen(category) + strlen(message) + 2;
    char *s = ziman_xmalloc(n);
    snprintf(s, n, "%s|%s", category, message);
    return value_result(type_result(ok_type, string_type()), result_obj_new(0, value_string(s)));
}

static Value ok_result(Type ok_type, Value v) {
    return value_result(type_result(ok_type, string_type()), result_obj_new(1, v));
}

static const char *category_for(sqlite3 *db) {
    return (sqlite3_extended_errcode(db) & 0xff) == SQLITE_CONSTRAINT ? "constraint" : "sql";
}

static DbConn *conn_get(int64_t handle, int line, int col) {
    DbConn *c = (DbConn *)(intptr_t)handle;
    if (!handleset_has(&g_live, c)) c = NULL;
    if (c == NULL) ziman_error("runtime", line, col, "db: invalid or closed connection handle %lld", (long long)handle);
    return c;
}

/* sqlite.open_raw(path) -> Result<int, string>. `path` is a SQLite filename
 * or ":memory:". Foreign keys are switched on for every connection --
 * SQLite leaves them off by default, unlike Postgres, and a repository
 * written against one should get the same referential integrity from
 * the other. */
static Value native_sqlite_open_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type int_type = type_scalar(TY_INT);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(value_cstr(args[0]), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        Value r = err_result(int_type, "connection", db != NULL ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db != NULL) sqlite3_close(db);
        return r;
    }
    /* Several connections to one database file (a pool) contend for the
     * single writer lock; wait up to five seconds for it instead of
     * failing at once with SQLITE_BUSY. */
    sqlite3_busy_timeout(db, 5000);
    char *errmsg = NULL;
    if (sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, &errmsg) != SQLITE_OK) {
        Value r = err_result(int_type, "connection", errmsg != NULL ? errmsg : "cannot enable foreign keys");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return r;
    }

    DbConn *c = ziman_xcalloc(1, sizeof(DbConn));
    c->db = db;
    pthread_mutex_init(&c->mu, NULL);

    handleset_add(&g_live, c);
#ifdef ZIMAN_GC
    GC_REGISTER_FINALIZER_NO_ORDER(c, conn_finalizer, NULL, NULL, NULL);
#endif
    int64_t handle = (int64_t)(intptr_t)c;
    return ok_result(int_type, value_int(handle));
}

static Value native_sqlite_close_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    DbConn *c = conn_get(args[0].as.i, line, col);
    pthread_mutex_lock(&c->mu);
    pending_drain(c); /* a deferred cursor's statement, before the database it belongs to goes */
    sqlite3_close(c->db);
    c->db = NULL;
    pthread_mutex_unlock(&c->mu);
    handleset_remove(&g_live, c);
    return value_int(0);
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Binds one tagged-string parameter (1-based index). Returns SQLite's rc,
 * or -1 for a malformed tag/payload (a bug in sqlite.zm's own encoder, not a
 * user-facing condition). */
static int bind_param(sqlite3_stmt *stmt, int index, const char *p) {
    switch (p[0]) {
        case 'N': return sqlite3_bind_null(stmt, index);
        case 'I': return sqlite3_bind_int64(stmt, index, strtoll(p + 1, NULL, 10));
        case 'b': return sqlite3_bind_int64(stmt, index, p[1] == '1' ? 1 : 0);
        case 'F': return sqlite3_bind_double(stmt, index, strtod(p + 1, NULL));
        case 'T': return sqlite3_bind_text(stmt, index, p + 1, -1, SQLITE_TRANSIENT);
        case 'B': {
            size_t hex_len = strlen(p + 1);
            if (hex_len % 2 != 0) return -1;
            size_t n = hex_len / 2;
            unsigned char *buf = ziman_xmalloc(n > 0 ? n : 1);
            for (size_t i = 0; i < n; i++) {
                int hi = hexval(p[1 + 2 * i]), lo = hexval(p[2 + 2 * i]);
                if (hi < 0 || lo < 0) { free(buf); return -1; }
                buf[i] = (unsigned char)(hi * 16 + lo);
            }
            int rc = sqlite3_bind_blob(stmt, index, buf, (int)n, SQLITE_TRANSIENT);
            free(buf);
            return rc;
        }
        default: return -1;
    }
}

static int bind_all(sqlite3_stmt *stmt, ArrayObj *params) {
    int expected = sqlite3_bind_parameter_count(stmt);
    if (expected != params->count) return -2;
    for (int i = 0; i < params->count; i++) {
        int rc = bind_param(stmt, i + 1, value_cstr(array_get(params, i)));
        if (rc != SQLITE_OK) return rc == -1 ? -1 : rc;
    }
    return SQLITE_OK;
}

static Value bind_error(Type ok_type, sqlite3 *db, int rc, int expected_hint, ArrayObj *params) {
    (void)expected_hint;
    if (rc == -2) {
        char msg[128];
        snprintf(msg, sizeof(msg), "statement expects a different number of parameters than the %d given", params->count);
        return err_result(ok_type, "sql", msg);
    }
    if (rc == -1) return err_result(ok_type, "sql", "malformed bound parameter");
    return err_result(ok_type, category_for(db), sqlite3_errmsg(db));
}

/* Repository SQL is written with Postgres-style numbered placeholders
 * ($1, $2, ...). SQLite treats `$name` as a *named* parameter, numbered
 * by order of first appearance -- so `WHERE a = $2 AND b = $1` would bind
 * backwards. Rewrite `$N` to SQLite's own numbered form `?N`, leaving
 * anything inside a '...' string literal or "..." quoted identifier
 * alone. */
static char *rewrite_placeholders(const char *sql) {
    size_t n = strlen(sql);
    char *out = ziman_xmalloc(n + 1);
    size_t o = 0;
    char quote = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = sql[i];
        if (quote) {
            if (ch == quote) quote = 0;
            out[o++] = ch;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            out[o++] = ch;
        } else if (ch == '$' && i + 1 < n && sql[i + 1] >= '0' && sql[i + 1] <= '9') {
            out[o++] = '?';
        } else {
            out[o++] = ch;
        }
    }
    out[o] = '\0';
    return out;
}

/* db.execute_raw(handle, sql, params) -> Result<int, string>: runs every
 * statement in `sql` (a DDL script may hold several), returning the row
 * count SQLite reports for the last one. Parameters bind to the first
 * statement only, and are rejected alongside a multi-statement script. */
static Value native_sqlite_execute_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    DbConn *c = conn_get(args[0].as.i, line, col);
    const char *sql = rewrite_placeholders(value_cstr(args[1]));
    ArrayObj *params = args[2].as.arr;

    pthread_mutex_lock(&c->mu);
    pending_drain(c);
    int64_t changes = 0;
    int first = 1;
    Value result;
    for (;;) {
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        if (sqlite3_prepare_v2(c->db, sql, -1, &stmt, &tail) != SQLITE_OK) {
            result = err_result(int_type, category_for(c->db), sqlite3_errmsg(c->db));
            goto done;
        }
        if (stmt == NULL) { /* only whitespace/comments left */
            result = ok_result(int_type, value_int(changes));
            goto done;
        }
        if (first) {
            int rc = bind_all(stmt, params);
            if (rc != SQLITE_OK) {
                result = bind_error(int_type, c->db, rc, 0, params);
                sqlite3_finalize(stmt);
                goto done;
            }
        } else if (params->count > 0) {
            sqlite3_finalize(stmt);
            result = err_result(int_type, "sql", "bound parameters are not allowed with a multi-statement script");
            goto done;
        }
        int rc;
        do { rc = sqlite3_step(stmt); } while (rc == SQLITE_ROW);
        if (rc != SQLITE_DONE) {
            result = err_result(int_type, category_for(c->db), sqlite3_errmsg(c->db));
            sqlite3_finalize(stmt);
            goto done;
        }
        changes = sqlite3_changes(c->db);
        sqlite3_finalize(stmt);
        sql = tail;
        first = 0;
    }
done:
    pthread_mutex_unlock(&c->mu);
    return result;
}

typedef struct {
    Value *items;
    int count;
    int capacity;
} CellVec;

static void cellvec_push(CellVec *v, char *s) {
    if (v->count == v->capacity) {
        v->capacity = v->capacity == 0 ? 64 : v->capacity * 2;
        v->items = ziman_xrealloc(v->items, sizeof(Value) * (size_t)v->capacity);
    }
    v->items[v->count++] = value_string(s);
}

static char *prefixed(char tag, const char *payload, size_t len) {
    char *s = ziman_xmalloc(len + 2);
    s[0] = tag;
    memcpy(s + 1, payload, len);
    s[len + 1] = '\0';
    return s;
}

static char *encode_cell(sqlite3_stmt *stmt, int i) {
    switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, i));
            return prefixed('I', buf, strlen(buf));
        }
        case SQLITE_FLOAT: {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.17g", sqlite3_column_double(stmt, i));
            return prefixed('F', buf, strlen(buf));
        }
        case SQLITE_TEXT: {
            const unsigned char *t = sqlite3_column_text(stmt, i);
            return prefixed('T', (const char *)t, (size_t)sqlite3_column_bytes(stmt, i));
        }
        case SQLITE_BLOB: {
            const unsigned char *b = sqlite3_column_blob(stmt, i);
            int n = sqlite3_column_bytes(stmt, i);
            char *s = ziman_xmalloc((size_t)n * 2 + 2);
            s[0] = 'B';
            for (int k = 0; k < n; k++) snprintf(s + 1 + 2 * k, 3, "%02x", b[k]);
            s[1 + 2 * n] = '\0';
            return s;
        }
        default:
            return prefixed('N', "", 0);
    }
}

/* db.query_raw(handle, sql, params) -> Result<[string], string>. The
 * result is one flat array: [column_count, name_0 .. name_{n-1}, then
 * row-major tagged cells]. sqlite.zm splits it back into Rows. */
static Value native_sqlite_query_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = type_array(string_type());
    DbConn *c = conn_get(args[0].as.i, line, col);
    ArrayObj *params = args[2].as.arr;

    pthread_mutex_lock(&c->mu);
    pending_drain(c);
    Value result;
    sqlite3_stmt *stmt = NULL;
    char *sql = rewrite_placeholders(value_cstr(args[1]));
    if (sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL) != SQLITE_OK || stmt == NULL) {
        result = err_result(arr_type, category_for(c->db), stmt == NULL && sqlite3_errcode(c->db) == SQLITE_OK ? "empty statement" : sqlite3_errmsg(c->db));
        goto done;
    }
    int rc = bind_all(stmt, params);
    if (rc != SQLITE_OK) {
        result = bind_error(arr_type, c->db, rc, 0, params);
        sqlite3_finalize(stmt);
        goto done;
    }

    CellVec cells = {0};
    int ncols = sqlite3_column_count(stmt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", ncols);
    cellvec_push(&cells, ziman_xstrdup(buf));
    for (int i = 0; i < ncols; i++) cellvec_push(&cells, ziman_xstrdup(sqlite3_column_name(stmt, i)));
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (int i = 0; i < ncols; i++) cellvec_push(&cells, encode_cell(stmt, i));
    }
    if (rc != SQLITE_DONE) {
        result = err_result(arr_type, category_for(c->db), sqlite3_errmsg(c->db));
        sqlite3_finalize(stmt);
        goto done;
    }
    sqlite3_finalize(stmt);
    result = ok_result(arr_type, value_array(arr_type, array_obj_new(cells.items, cells.count)));
done:
    pthread_mutex_unlock(&c->mu);
    return result;
}


/* ---- cursors ------------------------------------------------------------------------------
 * datasource.rows walks a result one row at a time instead of building the whole array: `cursor_open_raw`
 * prepares and binds a statement, `cursor_next_raw` steps it, `cursor_close_raw` finalizes it. A
 * cursor is an address in the same pointer-as-handle scheme as a connection, and a cursor that is
 * dropped unfinished is finalized when it is collected. It holds its connection, so close cursors
 * before closing the connection. */
typedef struct DbCursor DbCursor;
struct DbCursor {
    DbConn *conn;
    sqlite3_stmt *stmt;
    int ncols;
    int done;
    DbCursor *pending_next; /* see g_pending */
};

static HandleSet g_cursors = HANDLE_SET_INIT;

/* Cursors whose finalizer could not take their connection's lock, waiting for a caller that holds
 * it legitimately to finalize their statement (A42).
 *
 * The collector runs finalizers inside the allocation that triggered the collection, on whichever
 * thread was allocating (`src/main.c`'s GC_INIT path). So `cursor_finalizer` fires while that same
 * thread may be inside a `c->mu` critical section, and taking the mutex there is a thread waiting
 * for itself. Reproduced: an abandoned cursor's finalizer blocking in `cursor_finalize` beneath
 * `GC_generic_malloc_many` beneath `native_sqlite_cursor_open_raw`, which holds the lock -- the
 * `DbCursor` allocation of a *new* cursor collecting an *old* one on the same connection.
 *
 * `src/handles.c` solves its own version of this by releasing its lock around the one allocation
 * it makes. That does not transfer here. In a `make gc` build `src/zgc.h` redirects `malloc`,
 * `calloc`, `realloc` and `strdup` to the collector, so *every* allocation under `c->mu` is a
 * collection point -- the placeholder rewrite, each encoded cell, the row vector's growth, the
 * error string -- and "never allocate while holding c->mu" would be a rule the row-building loop
 * cannot keep and that every future edit would have to re-prove.
 *
 * So the finalizer never blocks instead: it tries the lock, and hands the cursor to this list if
 * it cannot have it. A finalizer runs inside a collection and may not allocate, so the list is
 * intrusive through `pending_next` and pushed with a CAS. The head is a file-scope static, which
 * the collector scans as a root, so a cursor parked here stays alive until it is drained. */
static DbCursor *g_pending;

static void pending_push(DbCursor *cur) {
    DbCursor *head = __atomic_load_n(&g_pending, __ATOMIC_RELAXED);
    do {
        cur->pending_next = head;
    } while (!__atomic_compare_exchange_n(&g_pending, &head, cur, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* Finalizes every deferred cursor belonging to `c` and puts the others back. Called with `c->mu`
 * held, and allocation-free, so it is safe anywhere inside a critical section. */
static void pending_drain(DbConn *c) {
    if (__atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == NULL) return;
    DbCursor *taken = __atomic_exchange_n(&g_pending, NULL, __ATOMIC_ACQ_REL);
    DbCursor *keep = NULL;
    while (taken != NULL) {
        DbCursor *next = taken->pending_next;
        if (taken->conn == c) {
            if (taken->stmt != NULL) {
                sqlite3_finalize(taken->stmt);
                taken->stmt = NULL;
            }
            taken->done = 1;
        } else {
            taken->pending_next = keep;
            keep = taken;
        }
        taken = next;
    }
    while (keep != NULL) {
        DbCursor *next = keep->pending_next;
        pending_push(keep);
        keep = next;
    }
}

/* The statement half, with `cur->conn->mu` already held. */
static void cursor_finalize_locked(DbCursor *cur) {
    if (cur->stmt != NULL) {
        sqlite3_finalize(cur->stmt);
        cur->stmt = NULL;
    }
    cur->done = 1;
}

static void cursor_finalize(DbCursor *cur) {
    if (cur->stmt == NULL) {
        cur->done = 1;
        return;
    }
    pthread_mutex_lock(&cur->conn->mu);
    cursor_finalize_locked(cur);
    pthread_mutex_unlock(&cur->conn->mu);
}

#ifdef ZIMAN_GC
static void cursor_finalizer(void *obj, void *client_data) {
    (void)client_data;
    DbCursor *cur = obj;
    if (!handleset_remove(&g_cursors, cur)) return;
    if (cur->stmt == NULL) {
        cur->done = 1;
        return;
    }
    /* Never `pthread_mutex_lock` here -- see g_pending. */
    if (pthread_mutex_trylock(&cur->conn->mu) == 0) {
        cursor_finalize_locked(cur);
        pthread_mutex_unlock(&cur->conn->mu);
    } else {
        pending_push(cur);
    }
}
#endif

static DbCursor *cursor_get(int64_t handle, int line, int col) {
    DbCursor *cur = (DbCursor *)(intptr_t)handle;
    if (!handleset_has(&g_cursors, cur)) ziman_error("runtime", line, col, "db: invalid or closed cursor handle %lld", (long long)handle);
    return cur;
}

/* sqlite.cursor_open_raw(handle, sql, params) -> Result<int, string> */
static Value native_sqlite_cursor_open_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type int_type = type_scalar(TY_INT);
    DbConn *c = conn_get(args[0].as.i, line, col);
    ArrayObj *params = args[2].as.arr;
    pthread_mutex_lock(&c->mu);
    pending_drain(c);
    Value result;
    sqlite3_stmt *stmt = NULL;
    char *sql = rewrite_placeholders(value_cstr(args[1]));
    if (sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL) != SQLITE_OK || stmt == NULL) {
        result = err_result(int_type, category_for(c->db), stmt == NULL && sqlite3_errcode(c->db) == SQLITE_OK ? "empty statement" : sqlite3_errmsg(c->db));
        goto done;
    }
    int rc = bind_all(stmt, params);
    if (rc != SQLITE_OK) {
        result = bind_error(int_type, c->db, rc, 0, params);
        sqlite3_finalize(stmt);
        goto done;
    }
    DbCursor *cur = ziman_xcalloc(1, sizeof(DbCursor));
    cur->conn = c;
    cur->stmt = stmt;
    cur->ncols = sqlite3_column_count(stmt);
    handleset_add(&g_cursors, cur);
#ifdef ZIMAN_GC
    GC_REGISTER_FINALIZER_NO_ORDER(cur, cursor_finalizer, NULL, NULL, NULL);
#endif
    result = ok_result(int_type, value_int((int64_t)(intptr_t)cur));
done:
    pthread_mutex_unlock(&c->mu);
    return result;
}

/* sqlite.cursor_columns_raw(cursor) -> [string] */
static Value native_sqlite_cursor_columns_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    DbCursor *cur = cursor_get(args[0].as.i, line, col);
    CellVec names = {0};
    if (cur->stmt != NULL) {
        for (int i = 0; i < cur->ncols; i++) cellvec_push(&names, ziman_xstrdup(sqlite3_column_name(cur->stmt, i)));
    }
    return value_array(type_array(string_type()), array_obj_new(names.items, names.count));
}

/* sqlite.cursor_next_raw(cursor) -> Result<[string], string>: the next row's tagged cells, or an empty
 * array when the rows are used up (the statement is then finalized). After an error the cursor is done. */
static Value native_sqlite_cursor_next_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    Type arr_type = type_array(string_type());
    DbCursor *cur = cursor_get(args[0].as.i, line, col);
    if (cur->done || cur->stmt == NULL) return ok_result(arr_type, value_array(arr_type, array_obj_new(NULL, 0)));
    DbConn *c = cur->conn;
    pthread_mutex_lock(&c->mu);
    pending_drain(c);
    Value result;
    if (c->db == NULL) {
        cur->done = 1;
        result = err_result(arr_type, "connection", "the connection was closed while a cursor was open");
        goto done;
    }
    int rc = sqlite3_step(cur->stmt);
    if (rc == SQLITE_ROW) {
        CellVec cells = {0};
        for (int i = 0; i < cur->ncols; i++) cellvec_push(&cells, encode_cell(cur->stmt, i));
        result = ok_result(arr_type, value_array(arr_type, array_obj_new(cells.items, cells.count)));
        goto done;
    }
    cur->done = 1;
    if (rc == SQLITE_DONE) {
        result = ok_result(arr_type, value_array(arr_type, array_obj_new(NULL, 0)));
    } else {
        result = err_result(arr_type, category_for(c->db), sqlite3_errmsg(c->db));
    }
    sqlite3_finalize(cur->stmt);
    cur->stmt = NULL;
done:
    pthread_mutex_unlock(&c->mu);
    return result;
}

static Value native_sqlite_cursor_close_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    DbCursor *cur = (DbCursor *)(intptr_t)args[0].as.i;
    if (handleset_remove(&g_cursors, cur)) cursor_finalize(cur);
    return value_int(0);
}

/* db.float_to_text(f) -> string: exact (%.17g) text form of a float, for
 * encoding a bound parameter. Ziman's own str(float) uses %g (six
 * significant digits), which would silently lose precision on a value
 * headed for a database. */
static Value native_sqlite_float_to_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.17g", args[0].as.f);
    return value_string(ziman_xstrdup(buf));
}

void native_sqlite_register(void) {
    native_register("sqlite::float_to_text", native_sqlite_float_to_text);
    native_register("pg::float_to_text", native_sqlite_float_to_text);
    native_register("sqlite::open_raw", native_sqlite_open_raw);
    native_register("sqlite::close_raw", native_sqlite_close_raw);
    native_register("sqlite::execute_raw", native_sqlite_execute_raw);
    native_register("sqlite::query_raw", native_sqlite_query_raw);
    native_register("sqlite::cursor_open_raw", native_sqlite_cursor_open_raw);
    native_register("sqlite::cursor_columns_raw", native_sqlite_cursor_columns_raw);
    native_register("sqlite::cursor_next_raw", native_sqlite_cursor_next_raw);
    native_register("sqlite::cursor_close_raw", native_sqlite_cursor_close_raw);
}
