#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ast.h"
#include "corelib.h"
#include "error.h"
#include "format.h"
#include "loader.h"
#include "typecheck.h"
#include "xalloc.h"

/* ------------------------------------------------------------------- JSON */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal {
    JKind kind;
    double num;
    int boolean;
    char *str;
    struct JVal **items; /* array elements, or object values */
    char **keys;         /* object keys */
    int count;
} JVal;

static JVal *jnew(JKind k) {
    JVal *v = ziman_xcalloc(1, sizeof(JVal));
    v->kind = k;
    return v;
}

typedef struct { const char *p, *end; int failed; } JParser;

static void jskip(JParser *jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static void utf8_put(char *out, size_t *n, unsigned cp) {
    if (cp < 0x80) out[(*n)++] = (char)cp;
    else if (cp < 0x800) { out[(*n)++] = (char)(0xC0 | (cp >> 6)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (cp >> 12)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (cp >> 18)); out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *jparse_string(JParser *jp) {
    jp->p++; /* opening quote */
    size_t cap = (size_t)(jp->end - jp->p) + 1;
    char *out = ziman_xmalloc(cap);
    size_t n = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        if (*jp->p == '\\' && jp->p + 1 < jp->end) {
            jp->p++;
            switch (*jp->p) {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': out[n++] = '\r'; break;
                case 'b': out[n++] = '\b'; break;
                case 'f': out[n++] = '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (jp->p + 4 >= jp->end || !hex4(jp->p + 1, &cp)) { jp->failed = 1; return out; }
                    jp->p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && jp->p + 6 < jp->end && jp->p[1] == '\\' && jp->p[2] == 'u') {
                        unsigned lo;
                        if (hex4(jp->p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            jp->p += 6;
                        }
                    }
                    utf8_put(out, &n, cp);
                    break;
                }
                default: out[n++] = *jp->p; break;
            }
            jp->p++;
        } else {
            out[n++] = *jp->p++;
        }
    }
    if (jp->p >= jp->end) jp->failed = 1;
    else jp->p++; /* closing quote */
    out[n] = '\0';
    return out;
}

static JVal *jparse_value(JParser *jp, int depth);

static JVal *jparse_value(JParser *jp, int depth) {
    jskip(jp);
    if (jp->p >= jp->end || depth > 64) { jp->failed = 1; return jnew(J_NULL); }
    char c = *jp->p;
    if (c == '"') {
        JVal *v = jnew(J_STR);
        v->str = jparse_string(jp);
        return v;
    }
    if (c == '{' || c == '[') {
        int is_obj = c == '{';
        JVal *v = jnew(is_obj ? J_OBJ : J_ARR);
        jp->p++;
        jskip(jp);
        if (jp->p < jp->end && *jp->p == (is_obj ? '}' : ']')) { jp->p++; return v; }
        for (;;) {
            jskip(jp);
            char *key = NULL;
            if (is_obj) {
                if (jp->p >= jp->end || *jp->p != '"') { jp->failed = 1; return v; }
                key = jparse_string(jp);
                jskip(jp);
                if (jp->p >= jp->end || *jp->p != ':') { jp->failed = 1; return v; }
                jp->p++;
            }
            JVal *item = jparse_value(jp, depth + 1);
            v->items = ziman_xrealloc(v->items, (size_t)(v->count + 1) * sizeof(JVal *));
            if (is_obj) v->keys = ziman_xrealloc(v->keys, (size_t)(v->count + 1) * sizeof(char *));
            v->items[v->count] = item;
            if (is_obj) v->keys[v->count] = key;
            v->count++;
            jskip(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
            if (jp->p < jp->end && *jp->p == (is_obj ? '}' : ']')) { jp->p++; return v; }
            jp->failed = 1;
            return v;
        }
    }
    if (strncmp(jp->p, "true", 4) == 0 && jp->p + 4 <= jp->end) { jp->p += 4; JVal *v = jnew(J_BOOL); v->boolean = 1; return v; }
    if (strncmp(jp->p, "false", 5) == 0 && jp->p + 5 <= jp->end) { jp->p += 5; JVal *v = jnew(J_BOOL); return v; }
    if (strncmp(jp->p, "null", 4) == 0 && jp->p + 4 <= jp->end) { jp->p += 4; return jnew(J_NULL); }
    char *endp;
    double d = strtod(jp->p, &endp);
    if (endp == jp->p) { jp->failed = 1; return jnew(J_NULL); }
    jp->p = endp;
    JVal *v = jnew(J_NUM);
    v->num = d;
    return v;
}

static JVal *json_parse(const char *s, size_t len) {
    JParser jp = {s, s + len, 0};
    JVal *v = jparse_value(&jp, 0);
    return jp.failed ? NULL : v;
}

static JVal *jget(const JVal *o, const char *key) {
    if (o == NULL || o->kind != J_OBJ) return NULL;
    for (int i = 0; i < o->count; i++) if (strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

static const char *jstr(const JVal *v) { return v != NULL && v->kind == J_STR ? v->str : NULL; }
static int jint(const JVal *v, int fallback) { return v != NULL && v->kind == J_NUM ? (int)v->num : fallback; }

/* ------------------------------------------------------------ string builder */

typedef struct { char *s; size_t n, cap; } SB;

static void sb_grow(SB *b, size_t extra) {
    if (b->n + extra + 1 > b->cap) {
        b->cap = (b->n + extra + 1) * 2;
        b->s = ziman_xrealloc(b->s, b->cap);
    }
}

static void sb_put(SB *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void sb_puts(SB *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_printf(SB *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    sb_grow(b, (size_t)n);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    b->n += (size_t)n;
    va_end(ap);
}

static void sb_json_string(SB *b, const char *s) {
    sb_puts(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            default:
                if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
                else sb_put(b, (const char *)p, 1);
        }
    }
    sb_puts(b, "\"");
}

/* --------------------------------------------------------------- transport */

static void send_message(const char *body) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(stdout);
}

static void write_id(SB *b, const JVal *id) {
    if (id == NULL || id->kind == J_NULL) sb_puts(b, "null");
    else if (id->kind == J_NUM) sb_printf(b, "%.0f", id->num);
    else if (id->kind == J_STR) sb_json_string(b, id->str);
    else sb_puts(b, "null");
}

static void send_result(const JVal *id, const char *result_json) {
    SB b = {0};
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&b, id);
    sb_printf(&b, ",\"result\":%s}", result_json);
    send_message(b.s);
}

static void send_error(const JVal *id, int code, const char *message) {
    SB b = {0};
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&b, id);
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json_string(&b, message);
    sb_puts(&b, "}}");
    send_message(b.s);
}

static void send_notification(const char *method, const char *params_json) {
    SB b = {0};
    sb_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params_json);
    send_message(b.s);
}

/* Reads one message; NULL at end of input. */
static char *read_message(size_t *len) {
    long content_length = -1;
    char line[512];
    for (;;) {
        if (fgets(line, sizeof line, stdin) == NULL) return NULL;
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) content_length = atol(line + 15);
    }
    if (content_length < 0) return NULL;
    char *body = ziman_xmalloc((size_t)content_length + 1);
    size_t got = fread(body, 1, (size_t)content_length, stdin);
    body[got] = '\0';
    *len = got;
    return got == (size_t)content_length ? body : NULL;
}

/* -------------------------------------------------------------- documents */

typedef struct { char *uri, *path, *text; } Doc;
static Doc *g_docs = NULL;
static int g_ndocs = 0;

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) != 0) return ziman_xstrdup(uri);
    const char *p = uri + 7;
    char *out = ziman_xmalloc(strlen(p) + 1);
    size_t n = 0;
    for (; *p; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            out[n++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return out;
}

static char *path_to_uri(const char *path) {
    SB b = {0};
    sb_puts(&b, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p) != NULL) sb_put(&b, (const char *)p, 1);
        else sb_printf(&b, "%%%02X", *p);
    }
    return b.s;
}

static Doc *find_doc_by_uri(const char *uri) {
    for (int i = 0; i < g_ndocs; i++) if (strcmp(g_docs[i].uri, uri) == 0) return &g_docs[i];
    return NULL;
}

static Doc *find_doc_by_path(const char *path) {
    for (int i = 0; i < g_ndocs; i++) if (strcmp(g_docs[i].path, path) == 0) return &g_docs[i];
    return NULL;
}

static void set_doc(const char *uri, const char *text);
static void analyze(Doc *trigger);

/* The largest document this server will parse and type-check. There was no limit at all: the
 * server analysed whatever arrived, so a client could hand it an arbitrarily large buffer on
 * `didOpen` and pay for a full parse and typecheck of it on every keystroke afterwards
 * (docs/reviews/tasks.md A27). 4 MB is about 45x the largest file in this project
 * (`lib/http/server.zm`, 89 KB) and far past anything a person edits.
 *
 * Over the limit the text is still stored -- so hover, formatting and the rest answer rather than
 * behaving as though the file were empty, and the formatter's own ceiling refuses it there -- but
 * the analysis is skipped and one diagnostic says why. Skipping silently would be worse than
 * refusing: a file with no diagnostics looks like a file with no errors. */
#define LSP_MAX_DOCUMENT (4u * 1024u * 1024u)

/* Publishes a single diagnostic of our own for `d`, at the top of the file. */
static void send_one_diagnostic(const Doc *d, const char *message) {
    SB b = {0};
    sb_puts(&b, "{\"uri\":");
    sb_json_string(&b, d->uri);
    sb_puts(&b, ",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                "\"end\":{\"line\":0,\"character\":1}},\"severity\":1,\"source\":\"ziman\",\"message\":");
    sb_json_string(&b, message);
    sb_puts(&b, "}]}");
    send_notification("textDocument/publishDiagnostics", b.s);
    free(b.s);
}

/* set_doc, then analyse -- unless the document is past LSP_MAX_DOCUMENT, in which case say so
 * instead of silently not analysing it. Both didOpen and didChange go through here so the limit
 * cannot apply to one and not the other. */
static void set_doc_and_analyze(const char *uri, const char *text) {
    size_t len = strlen(text);
    set_doc(uri, text);
    Doc *d = find_doc_by_uri(uri);
    if (d == NULL) return;
    if (len > LSP_MAX_DOCUMENT) {
        char message[200];
        snprintf(message, sizeof message,
                 "this file is %zu MB; the language server analyses files up to %u MB, so there are "
                 "no diagnostics for it",
                 (len + 1048575) / 1048576, (unsigned)(LSP_MAX_DOCUMENT / 1048576u));
        send_one_diagnostic(d, message);
        return;
    }
    analyze(d);
}

static void set_doc(const char *uri, const char *text) {
    Doc *d = find_doc_by_uri(uri);
    if (d == NULL) {
        g_docs = ziman_xrealloc(g_docs, (size_t)(g_ndocs + 1) * sizeof(Doc));
        d = &g_docs[g_ndocs++];
        d->uri = ziman_xstrdup(uri);
        d->path = uri_to_path(uri);
    }
    d->text = ziman_xstrdup(text);
}

static void remove_doc(const char *uri) {
    for (int i = 0; i < g_ndocs; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) {
            g_docs[i] = g_docs[g_ndocs - 1];
            g_ndocs--;
            return;
        }
    }
}

/* Positions. Ziman's are 1-based line and byte column; LSP's are 0-based line and
 * UTF-16 character. */
static const char *line_ptr(const char *text, int line0) {
    const char *p = text;
    for (int i = 0; i < line0; i++) {
        const char *nl = strchr(p, '\n');
        if (nl == NULL) return NULL;
        p = nl + 1;
    }
    return p;
}

static int utf16_of_bytes(const char *line, int nbytes) {
    int units = 0;
    for (int i = 0; i < nbytes && line[i] != '\0' && line[i] != '\n'; i++) {
        unsigned char c = (unsigned char)line[i];
        if ((c & 0xC0) == 0x80) continue;
        units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

static int bytes_of_utf16(const char *line, int units) {
    int i = 0, u = 0;
    while (line[i] != '\0' && line[i] != '\n' && u < units) {
        unsigned char c = (unsigned char)line[i];
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        u += c >= 0xF0 ? 2 : 1;
        i += len;
    }
    return i;
}

static int is_ident_char(int c) { return isalnum(c) || c == '_'; }

/* ---------------------------------------------------------------- analysis */

typedef struct { char *file; int line, col; char *message; } Diag;
static Diag *g_diags = NULL;
static int g_ndiags = 0;
static const char *g_current_entry = NULL;

static void diag_sink(const char *phase, const char *file, int line, int col, const char *context, const char *message) {
    (void)phase;
    g_diags = ziman_xrealloc(g_diags, (size_t)(g_ndiags + 1) * sizeof(Diag));
    Diag *d = &g_diags[g_ndiags++];
    d->file = ziman_xstrdup(file != NULL ? file : g_current_entry != NULL ? g_current_entry : "");
    d->line = line;
    d->col = col;
    if (context != NULL) {
        SB b = {0};
        sb_printf(&b, "(in %s) %s", context, message);
        d->message = b.s;
    } else {
        d->message = ziman_xstrdup(message);
    }
}

static Module *g_module = NULL;
/* The directory the last analysis's `use` paths actually resolved against -- `entry`'s own
 * directory ordinarily, but the fallback analyze() runs when a file isn't reached by the
 * project's real entry passes its OWN, different root (see run_analysis's and
 * load_module_graph's own comments): canonical_module_path/module_file need that same root to
 * find a `use` path's file, never `entry`'s own directory, which for that fallback case is some
 * unrelated file's, not the project's. */
static char *g_module_root = NULL;
static char *g_module_root_dir(void) { return g_module_root; }

/* Every distinct entry this session has ever analyzed, each remembering its own graph --
 * not just the most recent one. A project with a real entry main.zm doesn't reach can have
 * several genuinely different "programs" in it (serve.zm, a batch script, a second demo
 * entry) -- opening first one file, then another, re-runs analyze() with whichever one was
 * opened last as the trigger, and a single shared `g_module` getting overwritten each time
 * would make navigation and symbols for the FIRST file disappear the moment the second is
 * even looked at (its own didOpen alone triggers this, before any edit), coming back only if
 * that exact file is revisited -- exactly the "mixed results" this is the fix for. Symbol
 * lookup (collect_symbols and the enum-variant walks) reads every cached graph's own
 * declarations together; `g_module` itself stays the single most-recently-analyzed one,
 * which is all graph_has_file (deciding whether THIS call's own fallback is needed) ever
 * needs. */
typedef struct { char *entry; Module *module; } CachedGraph;
static CachedGraph *g_graphs = NULL;
static int g_n_graphs = 0;

static void cache_graph(const char *entry, Module *module) {
    for (int i = 0; i < g_n_graphs; i++) {
        if (strcmp(g_graphs[i].entry, entry) == 0) { g_graphs[i].module = module; return; }
    }
    g_graphs = ziman_xrealloc(g_graphs, (size_t)(g_n_graphs + 1) * sizeof(CachedGraph));
    g_graphs[g_n_graphs].entry = ziman_xstrdup(entry);
    g_graphs[g_n_graphs].module = module;
    g_n_graphs++;
}

/* Calls `visit` for every enum declared in any cached graph; stops when it returns non-zero. */
typedef int (*EnumVisitor)(EnumDecl *ed, void *ctx);
static void for_each_cached_enum(EnumVisitor visit, void *ctx) {
    for (int gi = 0; gi < g_n_graphs; gi++) {
        Module *m = g_graphs[gi].module;
        if (m == NULL) continue;
        for (int i = 0; i < m->enums.count; i++) if (visit(m->enums.items[i], ctx)) return;
    }
}

typedef struct { const char *word; const char *file; int line, col; } FindVariantCtx;

static int visit_find_variant(EnumDecl *ed, void *ctx) {
    FindVariantCtx *vc = ctx;
    for (int v = 0; v < ed->variants.count; v++) {
        if (strcmp(ed->variants.items[v].name, vc->word) == 0 && ed->source_file != NULL) {
            vc->file = ed->source_file;
            vc->line = ed->variants.items[v].line > 0 ? ed->variants.items[v].line : ed->line;
            vc->col = ed->variants.items[v].line > 0 ? ed->variants.items[v].col : ed->col;
            return 1;
        }
    }
    return 0;
}

typedef struct { const char *qualifier; SB *b; int *first; } EnumVariantCompletionCtx;
static void add_completion(SB *b, int *first, const char *label, int kind, const char *detail);
static const char *bare_name(const char *qualified);

static int visit_complete_variants(EnumDecl *ed, void *ctx) {
    EnumVariantCompletionCtx *ec = ctx;
    if (strcmp(bare_name(ed->name), ec->qualifier) != 0) return 0;
    for (int v = 0; v < ed->variants.count; v++) add_completion(ec->b, ec->first, ed->variants.items[v].name, 20, NULL);
    return 0;
}

/* The first `n` bytes of `s`, NUL-terminated, through the same allocator as everything else here.
 * Deliberately not `strndup`: zgc.h redirects malloc/calloc/realloc/strdup to the collector but not
 * strndup, so dirname_copy below used to return a libc block on one branch and a collector block on
 * the other -- and the collector build's no-op `free` then leaked whichever was libc's, while a real
 * `free` of the other would have been worse (docs/reviews/tasks.md A17). */
static char *copy_prefix(const char *s, size_t n) {
    char *out = ziman_xmalloc(n + 1);
    if (out == NULL) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *dirname_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? copy_prefix(path, (size_t)(slash - path)) : ziman_xstrdup("");
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The program a file belongs to: a `main.zm` in its directory or a parent (up to six
 * levels), else the file itself. */
static char *find_entry(const char *path) {
    char *dir = ziman_xstrdup(path);
    for (int level = 0; level < 6; level++) {
        char *slash = strrchr(dir, '/');
        if (slash == NULL) break;
        *slash = '\0';
        SB b = {0};
        sb_printf(&b, "%s/main.zm", dir);
        if (file_exists(b.s) || find_doc_by_path(b.s) != NULL) return b.s;
    }
    return ziman_xstrdup(path);
}

static char **g_prev_diag_files = NULL;
static int g_prev_n = 0;

static void publish(const char *path, int has_doc) {
    Doc *d = find_doc_by_path(path);
    if (d == NULL || !has_doc) return;
    SB b = {0};
    sb_puts(&b, "{\"uri\":");
    sb_json_string(&b, d->uri);
    sb_puts(&b, ",\"diagnostics\":[");
    int first = 1;
    for (int i = 0; i < g_ndiags; i++) {
        if (strcmp(g_diags[i].file, path) != 0) continue;
        int line0 = g_diags[i].line > 0 ? g_diags[i].line - 1 : 0;
        const char *lp = line_ptr(d->text, line0);
        int col_bytes = g_diags[i].col > 0 ? g_diags[i].col - 1 : 0;
        int sc = 0, ec = 1;
        if (lp != NULL) {
            sc = utf16_of_bytes(lp, col_bytes);
            int end_bytes = col_bytes;
            if (is_ident_char((unsigned char)lp[col_bytes])) {
                while (is_ident_char((unsigned char)lp[end_bytes])) end_bytes++;
            } else if (lp[col_bytes] != '\0' && lp[col_bytes] != '\n') {
                end_bytes++;
            }
            ec = utf16_of_bytes(lp, end_bytes);
            if (ec <= sc) ec = sc + 1;
        } else {
            line0 = 0;
        }
        if (!first) sb_puts(&b, ",");
        first = 0;
        sb_printf(&b, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},\"severity\":1,\"source\":\"ziman\",\"message\":",
                  line0, sc, line0, ec);
        sb_json_string(&b, g_diags[i].message);
        sb_puts(&b, "}");
    }
    sb_puts(&b, "]}");
    send_notification("textDocument/publishDiagnostics", b.s);
}

/* Loads and type-checks the program rooted at `entry`, collecting its diagnostics into g_diags.
 * `root`, NULL unless the caller is the fallback below, is the project root `entry`'s own `use`
 * paths resolve against (see load_module_graph's own comment on why that is not always
 * `entry`'s own directory). */
static void run_analysis(char *entry, const char *root) {
    g_current_entry = entry;

    const char **paths = ziman_xmalloc((size_t)(g_ndocs + 1) * sizeof(char *));
    const char **texts = ziman_xmalloc((size_t)(g_ndocs + 1) * sizeof(char *));
    for (int i = 0; i < g_ndocs; i++) { paths[i] = g_docs[i].path; texts[i] = g_docs[i].text; }
    loader_set_overrides(paths, texts, g_ndocs);

    ziman_reset_errors();
    ziman_set_error_sink(diag_sink);
    typecheck_skip_main_check(1);
    g_ndiags = 0;

    jmp_buf jb;
    Module *volatile loaded = NULL;
    ziman_recovery_push(&jb);
    if (setjmp(jb) == 0) {
        loaded = load_module_graph(entry, root);
        g_module = loaded;
        cache_graph(entry, loaded);
        free(g_module_root);
        g_module_root = root != NULL ? ziman_xstrdup(root) : dirname_copy(entry);
        typecheck(loaded);
        ziman_recovery_pop();
    }
    ziman_set_error_sink(NULL);
    g_current_entry = NULL;
}

/* Whether the last analysis loaded a declaration from `path`. */
static int graph_has_file(const char *path) {
    if (g_module == NULL) return 0;
    for (int i = 0; i < g_module->functions.count; i++) {
        const char *f = g_module->functions.items[i]->source_file;
        if (f != NULL && strcmp(f, path) == 0) return 1;
    }
    for (int i = 0; i < g_module->structs.count; i++) {
        const char *f = g_module->structs.items[i]->source_file;
        if (f != NULL && strcmp(f, path) == 0) return 1;
    }
    for (int i = 0; i < g_module->enums.count; i++) {
        const char *f = g_module->enums.items[i]->source_file;
        if (f != NULL && strcmp(f, path) == 0) return 1;
    }
    for (int i = 0; i < g_module->vals.count; i++) {
        const char *f = g_module->vals.items[i]->source_file;
        if (f != NULL && strcmp(f, path) == 0) return 1;
    }
    return 0;
}

static void analyze(Doc *trigger) {
    char *entry = find_entry(trigger->path);
    run_analysis(entry, NULL);
    /* A file the nearest main.zm does not reach (a second entry point, a test) is still analyzed on its
     * own, so at least its own declarations (and anything it drags in) show up for hover/definition/etc.
     * -- but its `use` paths must resolve against the SAME project root `entry` already established
     * (that root's own directory when `entry` is a real main.zm, or `trigger`'s own when none exists
     * at all, exactly what `entry`'s directory already is either way), never `trigger`'s own directory:
     * a file nested under the project root (lib/db/datasource.zm two levels under src/main/ziman/,
     * say) writes its `use` paths relative to that root, not to its own, immediately-enclosing folder,
     * and resolving them against the latter is exactly as wrong as it would be for the real entry. */
    if (strcmp(entry, trigger->path) != 0 && !graph_has_file(trigger->path)) {
        char *root = dirname_copy(entry);
        run_analysis(trigger->path, root);
        free(root);
    }

    /* publish to every open document that has diagnostics now or had them before */
    char **files = ziman_xmalloc((size_t)(g_ndocs + g_prev_n + g_ndiags + 2) * sizeof(char *));
    int nfiles = 0;
    for (int i = 0; i < g_ndiags; i++) {
        int seen = 0;
        for (int k = 0; k < nfiles; k++) if (strcmp(files[k], g_diags[i].file) == 0) seen = 1;
        if (!seen) files[nfiles++] = g_diags[i].file;
    }
    { int seen = 0;
      for (int k = 0; k < nfiles; k++) if (strcmp(files[k], trigger->path) == 0) seen = 1;
      if (!seen) files[nfiles++] = trigger->path; }
    for (int i = 0; i < g_prev_n; i++) {
        int seen = 0;
        for (int k = 0; k < nfiles; k++) if (strcmp(files[k], g_prev_diag_files[i]) == 0) seen = 1;
        if (!seen) files[nfiles++] = g_prev_diag_files[i];
    }
    for (int i = 0; i < nfiles; i++) publish(files[i], 1);
    char **next = ziman_xmalloc((size_t)(nfiles + 1) * sizeof(char *));
    int n_current = 0;
    for (int i = 0; i < nfiles; i++)
        for (int k = 0; k < g_ndiags; k++)
            if (strcmp(files[i], g_diags[k].file) == 0) { next[n_current++] = ziman_xstrdup(files[i]); break; }
    g_prev_diag_files = next;
    g_prev_n = n_current;
}

/* ----------------------------------------------------------------- symbols */

static const char *bare_name(const char *qualified) {
    const char *p = strstr(qualified, "::");
    const char *last = qualified;
    while (p != NULL) { last = p + 2; p = strstr(last, "::"); }
    return last;
}

/* The last path segment of the module part of a qualified name ("models/user::greet" -> "user"). */
static char *module_segment(const char *qualified) {
    const char *sep = strstr(qualified, "::");
    if (sep == NULL) return ziman_xstrdup("");
    const char *start = qualified;
    for (const char *p = qualified; p < sep; p++) if (*p == '/') start = p + 1;
    size_t n = (size_t)(sep - start);
    char *out = ziman_xmalloc(n + 1);
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

static char *type_text(Type t) { return type_name(t); }

static char *bounds_text(int bound) {
    SB b = {0};
    int any = 0;
    if (bound & BOUND_EQ) { sb_puts(&b, "Eq"); any = 1; }
    if (bound & BOUND_HASH) { sb_puts(&b, any ? " + Hash" : "Hash"); any = 1; }
    if (bound & BOUND_ORD) { sb_puts(&b, any ? " + Ord" : "Ord"); }
    return b.s != NULL ? b.s : ziman_xstrdup("");
}

static void write_type_params(SB *b, const TypeParamList *tp) {
    if (tp->count == 0) return;
    sb_puts(b, "<");
    for (int i = 0; i < tp->count; i++) {
        if (i > 0) sb_puts(b, ", ");
        sb_puts(b, tp->items[i].name);
        if (tp->items[i].bound != 0) sb_printf(b, ": %s", bounds_text(tp->items[i].bound));
    }
    sb_puts(b, ">");
}

/* "pub "/"internal "/"" -- the modifier text a hover/signature rendering shows ahead of the
 * declaration kind, mirroring exactly what the source itself would have written (or omitted,
 * for the private default). */
static const char *visibility_prefix(Visibility v) {
    if (v == VIS_PUBLIC) return "pub ";
    if (v == VIS_INTERNAL) return "internal ";
    return "";
}

static char *fn_signature(const FunctionDecl *fn) {
    SB b = {0};
    sb_puts(&b, visibility_prefix(fn->visibility));
    if (fn->is_native) sb_puts(&b, "native ");
    sb_printf(&b, "fn %s", bare_name(fn->name));
    write_type_params(&b, &fn->type_params);
    sb_puts(&b, "(");
    for (int i = 0; i < fn->params.count; i++) {
        if (i > 0) sb_puts(&b, ", ");
        sb_printf(&b, "%s: %s%s", fn->params.items[i].name, fn->params.items[i].is_variadic ? "variadic " : "", type_text(fn->params.items[i].type));
    }
    sb_puts(&b, ")");
    if (fn->return_type.kind != TY_VOID) sb_printf(&b, " -> %s", type_text(fn->return_type));
    return b.s;
}

static char *struct_signature(const StructDecl *sd) {
    SB b = {0};
    sb_puts(&b, visibility_prefix(sd->visibility));
    sb_printf(&b, "struct %s", bare_name(sd->name));
    write_type_params(&b, &sd->type_params);
    sb_puts(&b, " {\n");
    for (int i = 0; i < sd->fields.count; i++) sb_printf(&b, "    %s: %s\n", sd->fields.items[i].name, type_text(sd->fields.items[i].type));
    sb_puts(&b, "}");
    return b.s;
}

static char *enum_signature(const EnumDecl *ed) {
    SB b = {0};
    sb_puts(&b, visibility_prefix(ed->visibility));
    sb_printf(&b, "enum %s", bare_name(ed->name));
    write_type_params(&b, &ed->type_params);
    sb_puts(&b, " {\n");
    for (int i = 0; i < ed->variants.count; i++) {
        sb_printf(&b, "    %s", ed->variants.items[i].name);
        if (ed->variants.items[i].has_payload) sb_printf(&b, "(%s)", type_text(ed->variants.items[i].payload_type));
        sb_puts(&b, "\n");
    }
    sb_puts(&b, "}");
    return b.s;
}

static char *val_signature(const GlobalValDecl *v) {
    SB b = {0};
    sb_puts(&b, visibility_prefix(v->visibility));
    sb_printf(&b, "val %s", bare_name(v->name));
    if (v->has_declared_type) sb_printf(&b, ": %s", type_text(v->type));
    return b.s;
}

/* Bundled core modules have no file of their own: their embedded source is written once to a cache directory so
 * an editor can open it (read-only) and jump into it. */
static char *core_file(const char *use_path) {
    static char *names[64];
    static char *files[64];
    static int n = 0;
    for (int i = 0; i < n; i++) if (strcmp(names[i], use_path) == 0) return files[i];
    const char *source = corelib_source_for(use_path);
    if (source == NULL || n == 64) return NULL;
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    SB dir = {0};
    if (xdg != NULL && xdg[0] != '\0') sb_printf(&dir, "%s/ziman", xdg);
    else if (home != NULL) sb_printf(&dir, "%s/.cache/ziman", home);
    else sb_printf(&dir, "/tmp/ziman-cache");
    mkdir(dir.s, 0755);
    sb_puts(&dir, "/core");
    mkdir(dir.s, 0755);
    SB path = {0};
    sb_printf(&path, "%s/%s.zm", dir.s, use_path);
    unlink(path.s);
    FILE *f = fopen(path.s, "w");
    if (f == NULL) return NULL;
    fputs(source, f);
    fclose(f);
    chmod(path.s, 0444);
    names[n] = ziman_xstrdup(use_path);
    files[n] = path.s;
    return files[n++];
}

/* `visible_outside`: whether a *different* file could ever reference this symbol -- true for
 * both `pub` and `internal` (VIS_PRIVATE only). Completion doesn't currently distinguish
 * `internal`'s narrower same-package reach from `pub`'s: an internal-only symbol from a
 * different package can still show up in a `module.` completion list here, a minor imprecision
 * in suggestions the compiler's own enforcement (loader.c's `visibility_allows`) doesn't share --
 * getting this exactly right would mean teaching the LSP the same same-package tracking the
 * compiler has, for a suggestion list's own sake, which the value doesn't clearly justify yet. */
typedef struct { const char *name; const char *file; int line, col; int kind; char *signature; int visible_outside; int core; int deprecated; } Sym;

/* The file a declaration lives in: its own, or for a bundled core module (`mime::parse`) the cached copy. */
static const char *sym_file(const char *name, const char *source_file, int *core) {
    *core = 0;
    if (source_file != NULL) return source_file;
    const char *sep = strstr(name, "::");
    if (sep == NULL) return NULL;
    char *mod = copy_prefix(name, (size_t)(sep - name));
    char *f = core_file(mod);
    free(mod);
    if (f != NULL) *core = 1;
    return f;
}

/* Every declaration the last analysis loaded (derived code excluded). */
/* The index of a symbol already in `out[0..n)` at this exact file+line -- the same declaration,
 * reached again through a different cached graph (see g_graphs's own comment): every graph
 * that happens to load a shared dependency (common.zm, say) carries its own copy of that
 * dependency's declarations, so without this a name used by several of a project's distinct
 * entries would otherwise show up once per entry that reaches it. -1 if there is none. */
static int already_has(const Sym *out, int n, const char *file, int line) {
    for (int i = 0; i < n; i++) if (out[i].line == line && strcmp(out[i].file, file) == 0) return i;
    return -1;
}

static Sym *collect_symbols(int *count) {
    int n = 0, cap = 64;
    Sym *out = ziman_xmalloc((size_t)cap * sizeof(Sym));
    /* The SAME declaration can carry a different `name` depending on which graph loaded it: bare
     * when the file loading it is itself that graph's entry (an entry is never a `use` target, so
     * it keeps its own bare names -- see load_module_graph's own comment), module-qualified
     * ("util::double") when some other graph reaches it as an ordinary dependency. A later
     * qualified copy replaces an earlier bare one at the same spot -- never the other way --
     * since matches_word's own qualifier check (module_part_equals) needs the "::" a bare name
     * doesn't have; dropping the duplicate instead would silently break every cross-file,
     * qualified reference to a symbol whose file also happens to be some *other* graph's entry
     * (lib/http/client.zm's own net.zm dependency, say, when net.zm is independently open too). */
#define ADD(nm, f, l, c, k, sig, pub, cr, dep) do { \
    int dup = already_has(out, n, f, l); \
    if (dup < 0) { \
        if (n == cap) { cap *= 2; out = ziman_xrealloc(out, (size_t)cap * sizeof(Sym)); } \
        out[n++] = (Sym){nm, f, l, c, k, sig, pub, cr, dep}; \
    } else if (strstr(nm, "::") != NULL && strstr(out[dup].name, "::") == NULL) { \
        out[dup] = (Sym){nm, f, l, c, k, sig, pub, cr, dep}; \
    } } while (0)
    for (int gi = 0; gi < g_n_graphs; gi++) {
        Module *m = g_graphs[gi].module;
        if (m == NULL) continue;
        int core;
        const char *file;
        for (int i = 0; i < m->functions.count; i++) {
            FunctionDecl *fn = m->functions.items[i];
            if (fn->derived_from != NULL || fn->name[0] == '$') continue;
            if ((file = sym_file(fn->name, fn->source_file, &core)) == NULL || fn->line <= 0) continue;
            ADD(fn->name, file, fn->line, fn->col, 12, fn_signature(fn), fn->visibility != VIS_PRIVATE, core, fn->deprecated);
        }
        for (int i = 0; i < m->structs.count; i++) {
            StructDecl *sd = m->structs.items[i];
            if ((file = sym_file(sd->name, sd->source_file, &core)) == NULL || sd->line <= 0) continue;
            ADD(sd->name, file, sd->line, sd->col, 23, struct_signature(sd), sd->visibility != VIS_PRIVATE, core, sd->deprecated);
        }
        for (int i = 0; i < m->enums.count; i++) {
            EnumDecl *ed = m->enums.items[i];
            if ((file = sym_file(ed->name, ed->source_file, &core)) == NULL || ed->line <= 0) continue;
            ADD(ed->name, file, ed->line, ed->col, 10, enum_signature(ed), ed->visibility != VIS_PRIVATE, core, ed->deprecated);
        }
        for (int i = 0; i < m->vals.count; i++) {
            GlobalValDecl *v = m->vals.items[i];
            if ((file = sym_file(v->name, v->source_file, &core)) == NULL || v->line <= 0) continue;
            ADD(v->name, file, v->line, v->col, 13, val_signature(v), v->visibility != VIS_PRIVATE, core, v->deprecated);
        }
    }
#undef ADD
    *count = n;
    return out;
}

/* ---------------------------------------------------------------- requests */

/* The word at a position, and the identifier before a '.' if it is `qualifier.word`. */
typedef struct { char word[128]; char qualifier[128]; int start_byte, end_byte; int after_dot; } Cursor;

static int cursor_at(const Doc *d, int line0, int character, Cursor *c) {
    memset(c, 0, sizeof *c);
    const char *lp = line_ptr(d->text, line0);
    if (lp == NULL) return 0;
    int col = bytes_of_utf16(lp, character);
    int s = col, e = col;
    while (s > 0 && is_ident_char((unsigned char)lp[s - 1])) s--;
    while (lp[e] != '\0' && lp[e] != '\n' && is_ident_char((unsigned char)lp[e])) e++;
    c->start_byte = s;
    c->end_byte = e;
    if ((size_t)(e - s) >= sizeof c->word) return 0;
    memcpy(c->word, lp + s, (size_t)(e - s));
    if (s > 0 && lp[s - 1] == '.') {
        c->after_dot = 1;
        int qe = s - 1, qs = qe;
        while (qs > 0 && is_ident_char((unsigned char)lp[qs - 1])) qs--;
        if ((size_t)(qe - qs) < sizeof c->qualifier) memcpy(c->qualifier, lp + qs, (size_t)(qe - qs));
    }
    return 1;
}

static void write_location(SB *b, const char *file, int line, int col) {
    Doc *open = find_doc_by_path(file);
    char *uri = open != NULL ? open->uri : path_to_uri(file);
    int line0 = line > 0 ? line - 1 : 0;
    int sc = col > 0 ? col - 1 : 0;
    sb_puts(b, "{\"uri\":");
    sb_json_string(b, uri);
    sb_printf(b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}", line0, sc, line0, sc + 2);
}

/* ---- `use` lines: `use path/to/mod[.Name] [as alias]` ---- */

typedef struct {
    char path[192], name[128], alias[128];
    int path_start, path_end, name_start, name_end, alias_start, alias_end; /* byte offsets in the line */
    int has_name, has_alias;
} UseLine;

static int parse_use_line(const char *lp, UseLine *u) {
    memset(u, 0, sizeof *u);
    int i = 0;
    while (lp[i] == ' ' || lp[i] == '\t') i++;
    if (strncmp(lp + i, "use", 3) != 0 || (lp[i + 3] != ' ' && lp[i + 3] != '\t')) return 0;
    i += 3;
    while (lp[i] == ' ' || lp[i] == '\t') i++;
    int st = i;
    while (is_ident_char((unsigned char)lp[i]) || lp[i] == '/') i++;
    if (i == st || (size_t)(i - st) >= sizeof u->path) return 0;
    memcpy(u->path, lp + st, (size_t)(i - st));
    u->path_start = st;
    u->path_end = i;
    if (lp[i] == '.' && is_ident_char((unsigned char)lp[i + 1])) {
        int ns = ++i;
        while (is_ident_char((unsigned char)lp[i])) i++;
        if ((size_t)(i - ns) >= sizeof u->name) return 0;
        memcpy(u->name, lp + ns, (size_t)(i - ns));
        u->name_start = ns;
        u->name_end = i;
        u->has_name = 1;
    }
    int j = i;
    while (lp[j] == ' ' || lp[j] == '\t') j++;
    if (j > i && strncmp(lp + j, "as", 2) == 0 && (lp[j + 2] == ' ' || lp[j + 2] == '\t')) {
        j += 2;
        while (lp[j] == ' ' || lp[j] == '\t') j++;
        int as = j;
        while (is_ident_char((unsigned char)lp[j])) j++;
        if (j > as && (size_t)(j - as) < sizeof u->alias) {
            memcpy(u->alias, lp + as, (size_t)(j - as));
            u->alias_start = as;
            u->alias_end = j;
            u->has_alias = 1;
        }
    }
    return 1;
}

static const char *last_segment(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* Calls `visit` for every `use` line of a document; stops when it returns non-zero. */
typedef int (*UseVisitor)(const UseLine *u, void *ctx);
static void for_each_use(const Doc *d, UseVisitor visit, void *ctx) {
    const char *p = d->text;
    while (*p != '\0') {
        UseLine u;
        if (parse_use_line(p, &u) && visit(&u, ctx)) return;
        const char *nl = strchr(p, '\n');
        if (nl == NULL) break;
        p = nl + 1;
    }
}

static int file_exists(const char *path);
static Doc *find_doc_by_path(const char *path);
static char *g_module_root_dir(void);

/* The directory a `use` path in `d` resolves against: the last analysis's own root when one has
 * run (which, unlike `d`'s own directory, is right even for a file the project's real entry
 * doesn't reach -- see g_module_root's own comment), else `d`'s own directory as the only
 * guess available before any analysis has run at all. */
static int use_base_dir(const Doc *d, const char **out_base) {
    const char *root = g_module_root_dir();
    if (root != NULL) { *out_base = root; return (int)strlen(root); }
    *out_base = d->path;
    const char *slash = strrchr(d->path, '/');
    return slash != NULL ? (int)(slash - d->path) : 0;
}

/* The module a `use` path names: a folder holds its own module under its own name, so `use a/b` with no a/b.zm
 * but an a/b/b.zm is the module a/b/b (the same rule the loader applies). Returns a copy of `path` otherwise. */
static char *canonical_module_path(const Doc *d, const char *path) {
    const char *base;
    int dirlen = use_base_dir(d, &base);
    SB direct = {0}, inner = {0};
    const char *last = strrchr(path, '/');
    last = last != NULL ? last + 1 : path;
    if (dirlen > 0) {
        sb_printf(&direct, "%.*s/%s.zm", dirlen, base, path);
        sb_printf(&inner, "%.*s/%s/%s.zm", dirlen, base, path, last);
    } else {
        sb_printf(&direct, "%s.zm", path);
        sb_printf(&inner, "%s/%s.zm", path, last);
    }
    int has_direct = file_exists(direct.s) || find_doc_by_path(direct.s) != NULL;
    int has_inner = file_exists(inner.s) || find_doc_by_path(inner.s) != NULL;
    if (!has_direct && has_inner) {
        SB out = {0};
        sb_printf(&out, "%s/%s", path, last);
        return out.s;
    }
    return ziman_xstrdup(path);
}

typedef struct { const char *word; UseLine found; int hit; int want_name; const Doc *doc; } UseQuery;

static int visit_by_local(const UseLine *u, void *ctx) {
    UseQuery *q = ctx;
    const char *local = q->want_name ? (u->has_name ? (u->has_alias ? u->alias : u->name) : NULL)
                                     : (u->has_name ? NULL : (u->has_alias ? u->alias : last_segment(u->path)));
    if (local != NULL && strcmp(local, q->word) == 0) { q->found = *u; q->hit = 1; return 1; }
    return 0;
}

/* The `use` line that gives `word` a meaning: a whole-module import whose qualifier is `word` (want_name 0),
 * or a per-name import whose local name is `word` (want_name 1). */
static int find_use(const Doc *d, const char *word, int want_name, UseLine *out) {
    UseQuery q;
    memset(&q, 0, sizeof q);
    q.word = word;
    q.want_name = want_name;
    for_each_use(d, visit_by_local, &q);
    if (q.hit) *out = q.found;
    return q.hit;
}

static int visit_has_import(const UseLine *u, void *ctx) {
    UseQuery *q = ctx; /* q->word is "path::Name" */
    char key[400];
    if (!u->has_name) return 0;
    char *canon = canonical_module_path(q->doc, u->path);
    snprintf(key, sizeof key, "%s::%s", canon, u->name);
    free(canon);
    if (strcmp(key, q->word) == 0) { q->hit = 1; return 1; }
    return 0;
}

/* The file of a module named by a `use` path: a project file next to the program's root, else a bundled module. */
static char *module_file(const Doc *d, const char *mod_path) {
    const char *base;
    int dirlen = use_base_dir(d, &base);
    char *canon = canonical_module_path(d, mod_path);
    SB b = {0};
    if (dirlen > 0) sb_printf(&b, "%.*s/%s.zm", dirlen, base, canon);
    else sb_printf(&b, "%s.zm", canon);
    if (file_exists(b.s) || find_doc_by_path(b.s) != NULL) return b.s;
    return core_file(mod_path);
}

static int module_part_equals(const char *qualified, const char *mod_path) {
    const char *sep = strstr(qualified, "::");
    return sep != NULL && strlen(mod_path) == (size_t)(sep - qualified) && strncmp(qualified, mod_path, (size_t)(sep - qualified)) == 0;
}

static int matches_word(const Doc *d, const Sym *s, const Cursor *c) {
    if (strcmp(bare_name(s->name), c->word) != 0) return 0;
    if (c->qualifier[0] != '\0') {
        UseLine u;
        if (find_use(d, c->qualifier, 0, &u)) {
            char *canon = canonical_module_path(d, u.path);
            int same = module_part_equals(s->name, canon);
            free(canon);
            return same;
        }
        char *seg = module_segment(s->name);
        int ok = strcmp(seg, c->qualifier) == 0;
        free(seg);
        return ok;
    }
    if (s->core) { /* a bundled module's names are bare only where a `use mod.Name` brought them in */
        UseQuery q;
        memset(&q, 0, sizeof q);
        q.word = s->name;
        q.doc = d;
        for_each_use(d, visit_has_import, &q);
        return q.hit;
    }
    return 1;
}

/* Declarations the word may name: the current file's first. */
static Sym *lookup(const Doc *d, const Cursor *c, Sym *syms, int nsyms, int *found) {
    Sym *best = NULL;
    *found = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nsyms; i++) {
            int same = strcmp(syms[i].file, d->path) == 0;
            if ((pass == 0) != same) continue;
            if (matches_word(d, &syms[i], c)) { if (best == NULL) best = &syms[i]; (*found)++; }
        }
        if (best != NULL) break;
    }
    return best;
}

static Sym *find_qualified(Sym *syms, int nsyms, const char *path, const char *name) {
    char key[400];
    snprintf(key, sizeof key, "%s::%s", path, name);
    for (int i = 0; i < nsyms; i++) if (strcmp(syms[i].name, key) == 0) return &syms[i];
    return NULL;
}

static void send_location(const JVal *id, const char *file, int line, int col) {
    SB b = {0};
    write_location(&b, file, line, col);
    send_result(id, b.s);
}

/* ---- local variables, found by reading the source above the cursor ---- */

static int word_at(const char *s, int i, const char *w, int wl) {
    if (strncmp(s + i, w, (size_t)wl) != 0) return 0;
    if (i > 0 && is_ident_char((unsigned char)s[i - 1])) return 0;
    return !is_ident_char((unsigned char)s[i + wl]);
}

static int skip_blanks(const char *s, int i) {
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

/* The last column (byte offset, < `upto`) at which the line declares `w`, or -1. Recognises `val w`, `var w`,
 * `for w in` / `for i, w in`, a `Variant(w) {` switch arm, and lambda parameters `fn(w) {`. */
static int line_declares(const char *ln, int len, const char *w, int upto) {
    int wl = (int)strlen(w), best = -1;
    for (int i = 0; i + wl <= len; i++) {
        if (i >= upto) break;
        if (ln[i] == '/' && ln[i + 1] == '/') break;
        if (ln[i] == '(' && i > 0 && is_ident_char((unsigned char)ln[i - 1])) { /* `Variant(w) {` or `Variant(w) = e` */
            int j = skip_blanks(ln, i + 1);
            if (word_at(ln, j, w, wl)) {
                int k = skip_blanks(ln, j + wl);
                if (ln[k] == ')') {
                    k = skip_blanks(ln, k + 1);
                    if ((ln[k] == '{' || (ln[k] == '=' && ln[k + 1] != '=')) && j < upto) best = j;
                }
            }
            continue;
        }
        if (i > 0 && is_ident_char((unsigned char)ln[i - 1])) continue;
        if ((strncmp(ln + i, "val", 3) == 0 || strncmp(ln + i, "var", 3) == 0) && (ln[i + 3] == ' ')) {
            int j = skip_blanks(ln, i + 3);
            if (word_at(ln, j, w, wl) && j < upto) best = j;
        } else if (strncmp(ln + i, "for", 3) == 0 && ln[i + 3] == ' ') {
            int j = skip_blanks(ln, i + 3);
            for (;;) {
                if (word_at(ln, j, w, wl) && j < upto) best = j;
                while (is_ident_char((unsigned char)ln[j])) j++;
                j = skip_blanks(ln, j);
                if (ln[j] != ',') break;
                j = skip_blanks(ln, j + 1);
            }
        } else if (strncmp(ln + i, "fn(", 3) == 0) { /* a lambda's parameters */
            int j = i + 3;
            while (j < len && ln[j] != ')') {
                j = skip_blanks(ln, j);
                if (word_at(ln, j, w, wl) && j < upto) best = j;
                while (j < len && ln[j] != ',' && ln[j] != ')') j++;
                if (ln[j] == ',') j++;
            }
        }
    }
    return best;
}

/* Parameters of a top-level `fn` header that starts at `ln` and may run over several lines. */
static int header_declares(const char *ln, const char *w, int *out_line_off, int *out_col) {
    int wl = (int)strlen(w), depth = 0, line_off = 0, started = 0;
    const char *p = ln;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        int len = end != NULL ? (int)(end - p) : (int)strlen(p);
        for (int i = 0; i < len; i++) {
            if (p[i] == '/' && p[i + 1] == '/') break;
            if (p[i] == '(') { depth++; started = 1; }
            else if (p[i] == ')') { depth--; if (started && depth == 0) return 0; }
            else if (started && depth == 1 && word_at(p, i, w, wl)) {
                int k = skip_blanks(p, i + wl);
                if (p[k] == ':') { *out_line_off = line_off; *out_col = i; return 1; }
            }
        }
        if (end == NULL) break;
        p = end + 1;
        line_off++;
    }
    return 0;
}

static int is_header_start(const char *ln) {
    return strncmp(ln, "fn ", 3) == 0 || strncmp(ln, "pub fn ", 7) == 0 || strncmp(ln, "native fn ", 10) == 0 ||
           strncmp(ln, "pub native fn ", 14) == 0;
}

/* Where the local variable, parameter or loop binder `w` used on line `line0` was declared. */
static int find_local(const Doc *d, int line0, int col_bytes, const char *w, int *oline, int *ocol) {
    for (int l = line0; l >= 0; l--) {
        const char *ln = line_ptr(d->text, l);
        if (ln == NULL) continue;
        const char *nl = strchr(ln, '\n');
        int len = nl != NULL ? (int)(nl - ln) : (int)strlen(ln);
        int c = line_declares(ln, len, w, l == line0 ? col_bytes : len);
        if (c >= 0) { *oline = l; *ocol = c; return 1; }
        if (len > 0 && ln[0] != ' ' && ln[0] != '\t' && ln[0] != '}' && ln[0] != '/') {
            int off, col;
            if (is_header_start(ln) && header_declares(ln, w, &off, &col)) { *oline = l + off; *ocol = col; return 1; }
            return 0; /* a top-level item: the enclosing function has been searched */
        }
    }
    return 0;
}

static void handle_definition(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    JVal *pos = jget(params, "position");
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    int line0 = jint(jget(pos, "line"), 0), character = jint(jget(pos, "character"), 0);
    if (d == NULL) {
        send_result(id, "null");
        return;
    }
    const char *lp = line_ptr(d->text, line0);
    int col = lp != NULL ? bytes_of_utf16(lp, character) : 0;

    /* on a `use` line: the module path goes to the module's file, a name or alias to that declaration */
    UseLine u;
    if (lp != NULL && parse_use_line(lp, &u)) {
        if (col >= u.path_start && col <= u.path_end) {
            char *file = module_file(d, u.path);
            if (file != NULL) { send_location(id, file, 1, 1); return; }
        } else if ((u.has_name && col >= u.name_start && col <= u.name_end) || (u.has_alias && col >= u.alias_start && col <= u.alias_end)) {
            int nsyms;
            Sym *syms = collect_symbols(&nsyms);
            char *canon = canonical_module_path(d, u.path);
            Sym *s = u.has_name ? find_qualified(syms, nsyms, canon, u.name) : NULL;
            if (s != NULL) { send_location(id, s->file, s->line, s->col); return; }
            char *file = module_file(d, u.path);
            if (file != NULL) { send_location(id, file, 1, 1); return; }
        }
        send_result(id, "null");
        return;
    }

    Cursor c;
    if (!cursor_at(d, line0, character, &c) || c.word[0] == '\0') {
        send_result(id, "null");
        return;
    }
    int nsyms, found;
    Sym *syms = collect_symbols(&nsyms);
    Sym *s = lookup(d, &c, syms, nsyms, &found);
    if (s != NULL) {
        send_location(id, s->file, s->line, s->col);
        return;
    }
    /* `mod.` where `mod` names an imported module: the module's file */
    UseLine use;
    if (!c.after_dot && lp != NULL && lp[c.end_byte] == '.' && find_use(d, c.word, 0, &use)) {
        char *file = module_file(d, use.path);
        if (file != NULL) { send_location(id, file, 1, 1); return; }
    }
    /* a name imported with `use mod.Name [as alias]` */
    if (c.qualifier[0] == '\0' && find_use(d, c.word, 1, &use)) {
        char *canon = canonical_module_path(d, use.path);
        Sym *imported = find_qualified(syms, nsyms, canon, use.name);
        if (imported != NULL) { send_location(id, imported->file, imported->line, imported->col); return; }
    }
    /* an enum variant (`Shape.Circle`): point at its enum */
    {
        FindVariantCtx vc = {c.word, NULL, 0, 0};
        for_each_cached_enum(visit_find_variant, &vc);
        if (vc.file != NULL) { send_location(id, vc.file, vc.line, vc.col); return; }
    }
    /* a local variable, parameter or loop binder */
    int ll, lc;
    if (!c.after_dot && find_local(d, line0, c.start_byte, c.word, &ll, &lc)) {
        SB b = {0};
        const char *lline = line_ptr(d->text, ll);
        sb_printf(&b, "{\"uri\":");
        sb_json_string(&b, d->uri);
        int sc = utf16_of_bytes(lline, lc);
        sb_printf(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}", ll, sc, ll,
                  sc + (int)strlen(c.word));
        send_result(id, b.s);
        return;
    }
    send_result(id, "null");
}

static void handle_hover(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    JVal *pos = jget(params, "position");
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    Cursor c;
    if (d == NULL || !cursor_at(d, jint(jget(pos, "line"), 0), jint(jget(pos, "character"), 0), &c) || c.word[0] == '\0') {
        send_result(id, "null");
        return;
    }
    int nsyms, found;
    Sym *syms = collect_symbols(&nsyms);
    Sym *s = lookup(d, &c, syms, nsyms, &found);
    if (s == NULL) {
        send_result(id, "null");
        return;
    }
    SB body = {0};
    sb_printf(&body, "```ziman\n%s\n```", s->signature);
    SB b = {0};
    sb_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    sb_json_string(&b, body.s);
    sb_puts(&b, "}}");
    send_result(id, b.s);
}

static void handle_document_symbol(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    SB b = {0};
    sb_puts(&b, "[");
    if (d != NULL) {
        int nsyms;
        Sym *syms = collect_symbols(&nsyms);
        int first = 1;
        for (int i = 0; i < nsyms; i++) {
            if (strcmp(syms[i].file, d->path) != 0) continue;
            if (!first) sb_puts(&b, ",");
            first = 0;
            sb_puts(&b, "{\"name\":");
            sb_json_string(&b, bare_name(syms[i].name));
            sb_printf(&b, ",\"kind\":%d,\"location\":", syms[i].kind);
            write_location(&b, syms[i].file, syms[i].line, syms[i].col);
            sb_puts(&b, "}");
        }
    }
    sb_puts(&b, "]");
    send_result(id, b.s);
}

static const char *const KEYWORDS[] = {"fn", "val", "var", "if", "else", "while", "for", "in", "break", "continue", "spawn", "defer", "try", "switch",
                                       "return", "true", "false", "unit", "as", "struct", "enum", "with", "pub", "use", "native", "variadic", "Result", "Channel", "Map", "Set",
                                       "int", "float", "bool", "string", "bigint", "decimal", "byte", "unit"};
static const char *const BUILTINS[] = {"print", "printf", "format", "str", "len", "push", "insert", "set", "remove", "contains", "hash", "compare",
                                       "abs", "min", "max", "ok", "err", "is_ok", "unwrap", "unwrap_err", "is_Some", "is_None", "unwrap_Some", "channel_new", "send", "receive",
                                       "try_send", "try_receive", "channel_len", "channel_close", "receive_option", "receive_timeout", "select2", "concat", "substring", "split", "trim", "is_empty", "is_blank", "to_upper", "to_lower",
                                       "index_of", "replace", "starts_with", "ends_with", "parse_int", "parse_float", "join", "slice",
                                       "concat_arrays", "map_get", "map_get_or", "map_merge", "map_filter", "map_remove_all", "map_from_arrays", "set_union", "set_intersect", "set_difference", "set_is_subset", "set_from_array", "set_insert_all", "set_filter", "map_keys", "map_values", "set_items", "string_to_bytes", "bytes_to_string", "bytes_concat", "sb_new", "sb_append", "sb_to_string", "ab_new", "ab_append", "ab_to_array", "mapb_new", "mapb_set", "mapb_get", "mapb_remove", "mapb_contains", "mapb_len", "mapb_to_map", "setb_new", "setb_add", "setb_remove", "setb_contains", "setb_len", "setb_to_set", "snapshot"};

static void add_completion(SB *b, int *first, const char *label, int kind, const char *detail) {
    if (!*first) sb_puts(b, ",");
    *first = 0;
    sb_puts(b, "{\"label\":");
    sb_json_string(b, label);
    sb_printf(b, ",\"kind\":%d", kind);
    if (detail != NULL) {
        sb_puts(b, ",\"detail\":");
        sb_json_string(b, detail);
    }
    sb_puts(b, "}");
}

static void handle_completion(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    JVal *pos = jget(params, "position");
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    SB b = {0};
    sb_puts(&b, "{\"isIncomplete\":false,\"items\":[");
    int first = 1;
    Cursor c;
    if (d != NULL && cursor_at(d, jint(jget(pos, "line"), 0), jint(jget(pos, "character"), 0), &c)) {
        int nsyms;
        Sym *syms = collect_symbols(&nsyms);
        if (c.after_dot && c.qualifier[0] != '\0') {
            /* `module.` -> its public declarations; `Enum.` -> its variants */
            for (int i = 0; i < nsyms; i++) {
                char *seg = module_segment(syms[i].name);
                if (strcmp(seg, c.qualifier) == 0 && syms[i].visible_outside) {
                    add_completion(&b, &first, bare_name(syms[i].name), syms[i].kind == 12 ? 3 : syms[i].kind == 23 ? 22 : syms[i].kind == 10 ? 13 : 6, syms[i].signature);
                }
            }
            {
                EnumVariantCompletionCtx ec = {c.qualifier, &b, &first};
                for_each_cached_enum(visit_complete_variants, &ec);
            }
        } else {
            for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) add_completion(&b, &first, KEYWORDS[i], 14, NULL);
            for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) add_completion(&b, &first, BUILTINS[i], 3, "builtin");
            for (int i = 0; i < nsyms; i++) {
                if (strcmp(syms[i].file, d->path) != 0 && !syms[i].visible_outside) continue;
                if (strcmp(syms[i].file, d->path) != 0) continue; /* other modules are reached as `module.name` */
                add_completion(&b, &first, bare_name(syms[i].name), syms[i].kind == 12 ? 3 : syms[i].kind == 23 ? 22 : syms[i].kind == 10 ? 13 : 6, syms[i].signature);
            }
        }
    }
    sb_puts(&b, "]}");
    send_result(id, b.s);
}

/* The largest formatted document this server will answer with; see handle_formatting. */
#define LSP_MAX_FORMAT_REPLY (8u * 1024u * 1024u)

static void handle_formatting(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    if (d == NULL) { send_result(id, "[]"); return; }
    char err[256];
    char *formatted = format_source(d->text, strlen(d->text), 120, 0, err, sizeof err);
    if (formatted == NULL) { send_error(id, -32803, err); return; }
    if (strcmp(formatted, d->text) == 0) { send_result(id, "[]"); return; }
    /* A reply is built in memory and then escaped into JSON beside itself, so an answer this big
     * costs several times its own size in a process the editor keeps open all day. The formatter's
     * own nesting limit (format.c's FORMAT_MAX_NESTING) is what stops a small document from
     * producing a huge answer; this is the backstop for a document that really is enormous, and it
     * refuses rather than swallowing the memory (docs/reviews/tasks.md A18). */
    size_t formatted_len = strlen(formatted);
    if (formatted_len > LSP_MAX_FORMAT_REPLY) {
        char too_big[192];
        snprintf(too_big, sizeof too_big,
                 "formatting this document would return %zu MB, past the %zu MB this server will send",
                 (formatted_len + 1048575) / 1048576, (size_t)LSP_MAX_FORMAT_REPLY / 1048576);
        free(formatted);
        send_error(id, -32803, too_big);
        return;
    }
    int lines = 0;
    for (const char *p = d->text; *p; p++) if (*p == '\n') lines++;
    SB b = {0};
    sb_printf(&b, "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":%d,\"character\":0}},\"newText\":", lines + 1);
    sb_json_string(&b, formatted);
    sb_puts(&b, "}]");
    send_result(id, b.s);
}

static int g_shutdown = 0;

static int contains_ignore_case(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay != '\0'; hay++) if (strncasecmp(hay, needle, n) == 0) return 1;
    return n == 0;
}

/* Ctrl+T: every declaration whose name contains the query, the program's own before the bundled library's. */
static void handle_workspace_symbol(const JVal *id, const JVal *params) {
    const char *query = jstr(jget(params, "query"));
    if (query == NULL) query = "";
    int nsyms;
    Sym *syms = collect_symbols(&nsyms);
    SB b = {0};
    sb_puts(&b, "[");
    int first = 1, emitted = 0;
    for (int pass = 0; pass < 2 && emitted < 200; pass++) {
        for (int i = 0; i < nsyms && emitted < 200; i++) {
            if (syms[i].core != pass) continue;
            if (!contains_ignore_case(bare_name(syms[i].name), query)) continue;
            if (!first) sb_puts(&b, ",");
            first = 0;
            emitted++;
            sb_puts(&b, "{\"name\":");
            sb_json_string(&b, bare_name(syms[i].name));
            sb_printf(&b, ",\"kind\":%d,\"containerName\":", syms[i].kind);
            char *seg = module_segment(syms[i].name);
            sb_json_string(&b, seg);
            free(seg);
            sb_puts(&b, ",\"location\":");
            write_location(&b, syms[i].file, syms[i].line, syms[i].col);
            sb_puts(&b, "}");
        }
    }
    sb_puts(&b, "]");
    send_result(id, b.s);
}

/* Every other place the word under the cursor appears in the file (outside `//` comments). */
/* A name followed by `:` whose nearest preceding non-space character is `{`, `,` or `(` is a
 * *label* rather than a use: a struct literal's field (`Holder { x: 9 }`), a struct declaration's
 * field (`x: int`), or a named argument (`f(a: 1)`). Each of those names a field or a parameter,
 * which is a different thing from a local that happens to share the spelling. The scan crosses
 * newlines on purpose, since a field declaration's `{` is usually on the line above.
 *
 * The declaration itself is checked for separately by the caller, so a parameter's own
 * `(count: int` is not mistaken for a label and dropped from its own highlight list. */
static int looks_like_label(const char *text, size_t word_start, size_t word_end) {
    size_t i = word_end;
    while (text[i] == ' ' || text[i] == '\t') i++;
    if (text[i] != ':') return 0;
    size_t j = word_start;
    while (j > 0) {
        char ch = text[j - 1];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { j--; continue; }
        return ch == '{' || ch == ',' || ch == '(';
    }
    return 0;
}

/* Whether the name at `word_start` is introduced by a `val` or `var` immediately before it, so the
 * occurrence *declares* a binding rather than using one.
 *
 * This is needed for a shadow's own declaration, which is the one case resolution cannot settle: at
 * the position of `val x = 2` the inner `x` is not in scope yet, so the name there still resolves to
 * the outer binding and looks like a use of it. A `val` in front says otherwise.
 *
 * Covers `val` and `var` only. A nested `for` binder or a lambda parameter of the same name can
 * still come back as a use; those are other spellings of the same situation and are recorded in
 * docs/reviews/tasks.md F77 rather than guessed at here. */
static char declaring_keyword(const char *text, size_t word_start);

static int declares_here(const char *text, size_t word_start) {
    return declaring_keyword(text, word_start) != 0;
}

/* Which keyword introduces the name at `word_start`: 'l' for `val`, 'r' for `var`, 0 for neither.
 * The one fact that decides `readonly` against `mutable`, and the reason semantic tokens can do
 * what no regex grammar can -- the keyword is at the declaration, and the modifier belongs on
 * every *use*. */
static char declaring_keyword(const char *text, size_t word_start) {
    size_t j = word_start;
    while (j > 0 && (text[j - 1] == ' ' || text[j - 1] == '\t')) j--;
    if (j < 3) return 0;
    int boundary = (j == 3) || !isalnum((unsigned char)text[j - 4]);
    if (!boundary) return 0;
    if (strncmp(text + j - 3, "val", 3) == 0) return 'l';
    if (strncmp(text + j - 3, "var", 3) == 0) return 'r';
    return 0;
}

/* The nearest preceding non-space character, or 0 at the start of the document. */
static char char_before(const char *text, size_t at) {
    while (at > 0) {
        char ch = text[at - 1];
        if (ch != ' ' && ch != '\t') return ch;
        at--;
    }
    return 0;
}

static void handle_document_highlight(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    JVal *pos = jget(params, "position");
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    Cursor c;
    if (d == NULL || !cursor_at(d, jint(jget(pos, "line"), 0), jint(jget(pos, "character"), 0), &c) || c.word[0] == '\0') {
        send_result(id, "[]");
        return;
    }
    int wl = (int)strlen(c.word);
    int line0 = jint(jget(pos, "line"), 0);

    /* What the cursor actually names, rather than how it is spelled. A local resolves to its own
     * declaration, and an occurrence elsewhere is a use of *this* binding only when it resolves to
     * the same declaration -- which is exactly what tells a shadowing `val x` in an inner block
     * apart from the `x` outside it. Before this, the reply was every word that matched the text,
     * so an editor rendered a field declaration, a field label, `p.x` and both sides of a shadow
     * as "other uses of this symbol" (docs/reviews/tasks.md F77).
     *
     * A name that is not a local -- a function, a type, a module-level val -- still answers by
     * spelling, because resolving every occurrence of a global would mean doing the type checker's
     * work here. What it no longer does is match inside a string or after a dot. */
    int dl = -1, dc = -1;
    int is_local = !c.after_dot && find_local(d, line0, c.start_byte, c.word, &dl, &dc);
    /* The cursor may be sitting *on* the declaration, where there is nothing earlier to resolve to
     * and `find_local` correctly fails. Taking that as "not a local" was the first version's bug:
     * it switched resolution off for the one request where someone most often asks, with the
     * cursor on `val x`, so a shadow still came back as a use. If any later occurrence resolves
     * back to this exact position, this position is the declaration. */
    if (!is_local && !c.after_dot) {
        int line_n = 0;
        for (const char *q = d->text; *q != '\0';) {
            const char *nl = strchr(q, '\n');
            int len = nl != NULL ? (int)(nl - q) : (int)strlen(q);
            for (int i = 0; i + wl <= len && !is_local; i++) {
                if (!word_at(q, i, c.word, wl)) continue;
                int ol = -1, oc = -1;
                if (find_local(d, line_n, i, c.word, &ol, &oc) && ol == line0 && oc == c.start_byte) {
                    dl = line0;
                    dc = c.start_byte;
                    is_local = 1;
                }
            }
            if (nl == NULL || is_local) break;
            q = nl + 1;
            line_n++;
        }
    }

    SB b = {0};
    sb_puts(&b, "[");
    int first = 1, line = 0;
    const char *p = d->text;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        int len = nl != NULL ? (int)(nl - p) : (int)strlen(p);
        int in_string = 0;
        for (int i = 0; i + wl <= len; i++) {
            if (!in_string && p[i] == '/' && p[i + 1] == '/') break;
            if (p[i] == '"' && (i == 0 || p[i - 1] != '\\')) in_string = !in_string;
            if (in_string) continue;
            if (!word_at(p, i, c.word, wl)) continue;
            size_t abs_start = (size_t)(p - d->text) + (size_t)i;
            int is_decl = is_local && line == dl && i == dc;
            if (!is_decl) {
                /* `p.x` names a field of `p`, not this binding. */
                if (!c.after_dot && char_before(d->text, abs_start) == '.') continue;
                if (looks_like_label(d->text, abs_start, abs_start + (size_t)wl)) continue;
                /* A `val`/`var` here declares a different binding of the same name. */
                if (is_local && declares_here(d->text, abs_start)) continue;
                if (is_local) {
                    int ol = -1, oc = -1;
                    if (!find_local(d, line, i, c.word, &ol, &oc) || ol != dl || oc != dc) continue;
                }
            }
            if (!first) sb_puts(&b, ",");
            first = 0;
            int sc = utf16_of_bytes(p, i);
            sb_printf(&b, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}", line, sc, line,
                      sc + utf16_of_bytes(p + i, wl));
        }
        if (nl == NULL) break;
        p = nl + 1;
        line++;
    }
    sb_puts(&b, "]");
    send_result(id, b.s);
}

/* ---------------------------------------------------------------- semantic tokens
 *
 * What a TextMate grammar structurally cannot do. A regex sees spelling; these see what a name
 * *refers to*, which is the only way to mark a `var`'s **uses** -- and the use site is where a
 * one-character `val`/`var` typo does its damage, which docs/DESIGN.md asked a future tool to make
 * visible and the grammar could not (docs/reviews/tasks.md F76, F78).
 *
 * This is a first cut over identifiers only. Keywords, strings, numbers and attributes are left to
 * the grammar, which already does them well and which an editor layers underneath these: a token
 * emitted here refines what is already coloured rather than replacing it. What it emits:
 *
 *   variable   a local, with `mutable` on a `var` and `readonly` on a `val`, at the declaration
 *              and at every use, plus `declaration` on the binding site itself
 *   parameter  a local whose declaration is introduced by neither keyword, always `readonly`
 *   function   a resolved function, `defaultLibrary` when it comes from a bundled module
 *   struct     a resolved struct, same
 *   enum       a resolved enum, same
 *
 * and `deprecated` on any of the resolved kinds whose declaration carries `#[deprecated]`, which
 * gives the compiler's own warning a place in the editor.
 *
 * Deliberately deferred, from the style report's own ordering: `namespace`, `property`,
 * `enumMember`, `typeParameter`, and the non-standard `threadBound` and `optional`. Each needs a
 * resolution this file does not have yet (a field's owning struct, a variant's enum, a type
 * parameter's scope), and shipping the ones that close a standing finding first is worth more than
 * a legend that is complete and half-populated. */
#define TOK_VARIABLE 0
#define TOK_PARAMETER 1
#define TOK_FUNCTION 2
#define TOK_STRUCT 3
#define TOK_ENUM 4

#define MOD_DECLARATION 1
#define MOD_READONLY 2
#define MOD_MUTABLE 4
#define MOD_DEPRECATED 8
#define MOD_DEFAULT_LIBRARY 16

/* The legend, in the order the bit and index constants above assume. `mutable` is not in the LSP's
 * standard modifier set and Ziman names it, which the protocol allows and which is the point:
 * nothing standard says "this name refers to something you may reassign". */
#define SEMANTIC_LEGEND \
    "{\"tokenTypes\":[\"variable\",\"parameter\",\"function\",\"struct\",\"enum\"]," \
    "\"tokenModifiers\":[\"declaration\",\"readonly\",\"mutable\",\"deprecated\",\"defaultLibrary\"]}"

static void handle_semantic_tokens(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    if (d == NULL) {
        send_result(id, "{\"data\":[]}");
        return;
    }
    int nsyms;
    Sym *syms = collect_symbols(&nsyms);

    SB b = {0};
    sb_puts(&b, "{\"data\":[");
    int first = 1, line = 0, prev_line = 0, prev_col = 0;
    const char *p = d->text;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        int len = nl != NULL ? (int)(nl - p) : (int)strlen(p);
        int in_string = 0;
        for (int i = 0; i < len; i++) {
            if (!in_string && p[i] == '/' && p[i + 1] == '/') break;
            if (p[i] == '"' && (i == 0 || p[i - 1] != '\\')) { in_string = !in_string; continue; }
            if (in_string) continue;
            if (!(isalpha((unsigned char)p[i]) || p[i] == '_')) continue;
            /* not mid-identifier */
            if (i > 0 && (isalnum((unsigned char)p[i - 1]) || p[i - 1] == '_')) continue;
            int j = i;
            while (j < len && (isalnum((unsigned char)p[j]) || p[j] == '_')) j++;
            int wl = j - i;
            if (wl >= 128) { i = j - 1; continue; }
            char word[128];
            memcpy(word, p + i, (size_t)wl);
            word[wl] = '\0';

            int type = -1, mods = 0;
            size_t abs_start = (size_t)(p - d->text) + (size_t)i;
            int dline = -1, dcol = -1;
            if (find_local(d, line, i, word, &dline, &dcol) || declares_here(d->text, abs_start)) {
                /* A local: its own declaration says whether it may be reassigned, and that answer
                 * belongs on every use, not only where the keyword is. */
                size_t decl_at = abs_start;
                if (dline >= 0) {
                    const char *dl_ptr = line_ptr(d->text, dline);
                    if (dl_ptr != NULL) decl_at = (size_t)(dl_ptr - d->text) + (size_t)dcol;
                }
                char kw = declaring_keyword(d->text, decl_at);
                type = kw == 0 ? TOK_PARAMETER : TOK_VARIABLE;
                mods |= kw == 'r' ? MOD_MUTABLE : MOD_READONLY;
                if (dline < 0 || (line == dline && i == dcol)) mods |= MOD_DECLARATION;
            } else {
                Cursor c;
                if (!cursor_at(d, line, utf16_of_bytes(p, i), &c) || c.word[0] == '\0') { i = j - 1; continue; }
                int found;
                Sym *s = lookup(d, &c, syms, nsyms, &found);
                if (s == NULL) { i = j - 1; continue; }
                if (s->kind == 12) type = TOK_FUNCTION;
                else if (s->kind == 23) type = TOK_STRUCT;
                else if (s->kind == 10) type = TOK_ENUM;
                else type = TOK_VARIABLE, mods |= MOD_READONLY; /* a module-level val */
                if (s->core) mods |= MOD_DEFAULT_LIBRARY;
                if (s->deprecated) mods |= MOD_DEPRECATED;
            }
            if (type >= 0) {
                int col16 = utf16_of_bytes(p, i);
                int dl = line - prev_line;
                int dc = dl == 0 ? col16 - prev_col : col16;
                if (!first) sb_puts(&b, ",");
                first = 0;
                sb_printf(&b, "%d,%d,%d,%d,%d", dl, dc, utf16_of_bytes(p + i, wl), type, mods);
                prev_line = line;
                prev_col = col16;
            }
            i = j - 1;
        }
        if (nl == NULL) break;
        p = nl + 1;
        line++;
    }
    sb_puts(&b, "]}");
    send_result(id, b.s);
}

static void handle_message(const JVal *msg) {
    const char *method = jstr(jget(msg, "method"));
    JVal *id = jget(msg, "id");
    JVal *params = jget(msg, "params");
    if (method == NULL) return; /* a response to a request we never send */
    if (strcmp(method, "initialize") == 0) {
        send_result(id,
                    "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{}},"
                    "\"documentFormattingProvider\":true,\"hoverProvider\":true,\"definitionProvider\":true,"
                    "\"documentSymbolProvider\":true,\"workspaceSymbolProvider\":true,\"documentHighlightProvider\":true,"
                    "\"semanticTokensProvider\":{\"legend\":" SEMANTIC_LEGEND ",\"full\":true},"
                    "\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
                    "\"serverInfo\":{\"name\":\"ziman\"}}");
    } else if (strcmp(method, "shutdown") == 0) {
        g_shutdown = 1;
        send_result(id, "null");
    } else if (strcmp(method, "exit") == 0) {
        exit(g_shutdown ? 0 : 1);
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        JVal *td = jget(params, "textDocument");
        const char *uri = jstr(jget(td, "uri")), *text = jstr(jget(td, "text"));
        if (uri != NULL && text != NULL) {
            set_doc_and_analyze(uri, text);
        }
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
        JVal *changes = jget(params, "contentChanges");
        if (uri != NULL && changes != NULL && changes->kind == J_ARR && changes->count > 0) {
            const char *text = jstr(jget(changes->items[changes->count - 1], "text"));
            if (text != NULL) {
                set_doc_and_analyze(uri, text);
            }
        }
    } else if (strcmp(method, "textDocument/didSave") == 0) {
        const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
        Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
        if (d != NULL) analyze(d);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
        if (uri != NULL) {
            SB b = {0};
            sb_puts(&b, "{\"uri\":");
            sb_json_string(&b, uri);
            sb_puts(&b, ",\"diagnostics\":[]}");
            send_notification("textDocument/publishDiagnostics", b.s);
            remove_doc(uri);
        }
    } else if (strcmp(method, "textDocument/formatting") == 0) {
        handle_formatting(id, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(id, params);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbol(id, params);
    } else if (strcmp(method, "workspace/symbol") == 0) {
        handle_workspace_symbol(id, params);
    } else if (strcmp(method, "textDocument/documentHighlight") == 0) {
        handle_document_highlight(id, params);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        handle_semantic_tokens(id, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id, params);
    } else if (id != NULL) {
        send_error(id, -32601, "method not found");
    }
}

int lsp_main(void) {
    for (;;) {
        size_t len = 0;
        char *body = read_message(&len);
        if (body == NULL) return g_shutdown ? 0 : 1;
        JVal *msg = json_parse(body, len);
        if (msg == NULL || msg->kind != J_OBJ) {
            fprintf(stderr, "ziman --lsp: ignoring a malformed message\n");
            continue;
        }
        handle_message(msg);
    }
}
