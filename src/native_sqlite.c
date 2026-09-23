#define _POSIX_C_SOURCE 200809L
#include "native_sqlite.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* The SQLite driver behind lib/core/sqlite.zm. Everything crossing the native
 * boundary is a primitive: a connection is an int handle (an index into
 * a table of individually heap-allocated DbConn structs, same pattern
 * StringBuilder uses in prelude.c), SQL is a string, and both bound
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
 * lib/core/sqlite.zm encodes/decodes SqlValue to and from exactly this shape.
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

static DbConn **g_conns = NULL;
static int g_conn_count = 0;
static int g_conn_capacity = 0;
static pthread_mutex_t g_table_mu = PTHREAD_MUTEX_INITIALIZER;

static Type string_type(void) { return type_scalar(TY_STRING); }

static Value err_result(Type ok_type, const char *category, const char *message) {
    size_t n = strlen(category) + strlen(message) + 2;
    char *s = malloc(n);
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
    pthread_mutex_lock(&g_table_mu);
    DbConn *c = (handle >= 0 && handle < g_conn_count) ? g_conns[handle] : NULL;
    pthread_mutex_unlock(&g_table_mu);
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
    int rc = sqlite3_open_v2(args[0].as.s, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
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

    DbConn *c = calloc(1, sizeof(DbConn));
    c->db = db;
    pthread_mutex_init(&c->mu, NULL);

    pthread_mutex_lock(&g_table_mu);
    if (g_conn_count == g_conn_capacity) {
        g_conn_capacity = g_conn_capacity == 0 ? 8 : g_conn_capacity * 2;
        g_conns = realloc(g_conns, sizeof(DbConn *) * (size_t)g_conn_capacity);
    }
    int handle = g_conn_count;
    g_conns[g_conn_count++] = c;
    pthread_mutex_unlock(&g_table_mu);
    return ok_result(int_type, value_int(handle));
}

static Value native_sqlite_close_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    DbConn *c = conn_get(args[0].as.i, line, col);
    pthread_mutex_lock(&c->mu);
    sqlite3_close(c->db);
    c->db = NULL;
    pthread_mutex_unlock(&c->mu);
    pthread_mutex_lock(&g_table_mu);
    g_conns[args[0].as.i] = NULL;
    pthread_mutex_unlock(&g_table_mu);
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
            unsigned char *buf = malloc(n > 0 ? n : 1);
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
        int rc = bind_param(stmt, i + 1, params->items[i].as.s);
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
    char *out = malloc(n + 1);
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
    const char *sql = rewrite_placeholders(args[1].as.s);
    ArrayObj *params = args[2].as.arr;

    pthread_mutex_lock(&c->mu);
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
        v->items = realloc(v->items, sizeof(Value) * (size_t)v->capacity);
    }
    v->items[v->count++] = value_string(s);
}

static char *prefixed(char tag, const char *payload, size_t len) {
    char *s = malloc(len + 2);
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
            char *s = malloc((size_t)n * 2 + 2);
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
    Value result;
    sqlite3_stmt *stmt = NULL;
    char *sql = rewrite_placeholders(args[1].as.s);
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
    cellvec_push(&cells, strdup(buf));
    for (int i = 0; i < ncols; i++) cellvec_push(&cells, strdup(sqlite3_column_name(stmt, i)));
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

/* db.float_to_text(f) -> string: exact (%.17g) text form of a float, for
 * encoding a bound parameter. Ziman's own str(float) uses %g (six
 * significant digits), which would silently lose precision on a value
 * headed for a database. */
static Value native_sqlite_float_to_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.17g", args[0].as.f);
    return value_string(strdup(buf));
}

void native_sqlite_register(void) {
    native_register("sqlite::float_to_text", native_sqlite_float_to_text);
    native_register("pg::float_to_text", native_sqlite_float_to_text);
    native_register("sqlite::open_raw", native_sqlite_open_raw);
    native_register("sqlite::close_raw", native_sqlite_close_raw);
    native_register("sqlite::execute_raw", native_sqlite_execute_raw);
    native_register("sqlite::query_raw", native_sqlite_query_raw);
}
