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

#include "ast.h"
#include "error.h"
#include "format.h"
#include "loader.h"
#include "typecheck.h"

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
    JVal *v = calloc(1, sizeof(JVal));
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
    char *out = malloc(cap);
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
            v->items = realloc(v->items, (size_t)(v->count + 1) * sizeof(JVal *));
            if (is_obj) v->keys = realloc(v->keys, (size_t)(v->count + 1) * sizeof(char *));
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
        b->s = realloc(b->s, b->cap);
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
    char *body = malloc((size_t)content_length + 1);
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
    if (strncmp(uri, "file://", 7) != 0) return strdup(uri);
    const char *p = uri + 7;
    char *out = malloc(strlen(p) + 1);
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

static void set_doc(const char *uri, const char *text) {
    Doc *d = find_doc_by_uri(uri);
    if (d == NULL) {
        g_docs = realloc(g_docs, (size_t)(g_ndocs + 1) * sizeof(Doc));
        d = &g_docs[g_ndocs++];
        d->uri = strdup(uri);
        d->path = uri_to_path(uri);
    }
    d->text = strdup(text);
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
    g_diags = realloc(g_diags, (size_t)(g_ndiags + 1) * sizeof(Diag));
    Diag *d = &g_diags[g_ndiags++];
    d->file = strdup(file != NULL ? file : g_current_entry != NULL ? g_current_entry : "");
    d->line = line;
    d->col = col;
    if (context != NULL) {
        SB b = {0};
        sb_printf(&b, "(in %s) %s", context, message);
        d->message = b.s;
    } else {
        d->message = strdup(message);
    }
}

static Module *g_module = NULL;
static char *g_module_entry = NULL;

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The program a file belongs to: a `main.zm` in its directory or a parent (up to six
 * levels), else the file itself. */
static char *find_entry(const char *path) {
    char *dir = strdup(path);
    for (int level = 0; level < 6; level++) {
        char *slash = strrchr(dir, '/');
        if (slash == NULL) break;
        *slash = '\0';
        SB b = {0};
        sb_printf(&b, "%s/main.zm", dir);
        if (file_exists(b.s) || find_doc_by_path(b.s) != NULL) return b.s;
    }
    return strdup(path);
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

static void analyze(Doc *trigger) {
    char *entry = find_entry(trigger->path);
    g_current_entry = entry;

    const char **paths = malloc((size_t)(g_ndocs + 1) * sizeof(char *));
    const char **texts = malloc((size_t)(g_ndocs + 1) * sizeof(char *));
    for (int i = 0; i < g_ndocs; i++) { paths[i] = g_docs[i].path; texts[i] = g_docs[i].text; }
    loader_set_overrides(paths, texts, g_ndocs);

    ziman_reset_errors();
    ziman_set_error_sink(diag_sink);
    ziman_set_entry_file(entry);
    typecheck_skip_main_check(1);
    g_ndiags = 0;

    jmp_buf jb;
    Module *volatile loaded = NULL;
    ziman_recovery_push(&jb);
    if (setjmp(jb) == 0) {
        loaded = load_module_graph(entry);
        g_module = loaded;
        g_module_entry = entry;
        typecheck(loaded);
        ziman_recovery_pop();
    }
    ziman_set_error_sink(NULL);
    g_current_entry = NULL;

    /* publish to every open document that has diagnostics now or had them before */
    char **files = malloc((size_t)(g_ndocs + g_prev_n + g_ndiags + 2) * sizeof(char *));
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
    char **next = malloc((size_t)(nfiles + 1) * sizeof(char *));
    int n_current = 0;
    for (int i = 0; i < nfiles; i++)
        for (int k = 0; k < g_ndiags; k++)
            if (strcmp(files[i], g_diags[k].file) == 0) { next[n_current++] = strdup(files[i]); break; }
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
    if (sep == NULL) return strdup("");
    const char *start = qualified;
    for (const char *p = qualified; p < sep; p++) if (*p == '/') start = p + 1;
    size_t n = (size_t)(sep - start);
    char *out = malloc(n + 1);
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
    return b.s != NULL ? b.s : strdup("");
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

static char *fn_signature(const FunctionDecl *fn) {
    SB b = {0};
    if (fn->is_public) sb_puts(&b, "pub ");
    if (fn->is_native) sb_puts(&b, "native ");
    sb_printf(&b, "fn %s", bare_name(fn->name));
    write_type_params(&b, &fn->type_params);
    sb_puts(&b, "(");
    for (int i = 0; i < fn->params.count; i++) {
        if (i > 0) sb_puts(&b, ", ");
        sb_printf(&b, "%s: %s%s", fn->params.items[i].name, type_text(fn->params.items[i].type), fn->params.items[i].is_variadic ? "..." : "");
    }
    sb_puts(&b, ")");
    if (fn->return_type.kind != TY_VOID) sb_printf(&b, " -> %s", type_text(fn->return_type));
    return b.s;
}

static char *struct_signature(const StructDecl *sd) {
    SB b = {0};
    if (sd->is_public) sb_puts(&b, "pub ");
    sb_printf(&b, "struct %s", bare_name(sd->name));
    write_type_params(&b, &sd->type_params);
    sb_puts(&b, " {\n");
    for (int i = 0; i < sd->fields.count; i++) sb_printf(&b, "    %s: %s\n", sd->fields.items[i].name, type_text(sd->fields.items[i].type));
    sb_puts(&b, "}");
    return b.s;
}

static char *enum_signature(const EnumDecl *ed) {
    SB b = {0};
    if (ed->is_public) sb_puts(&b, "pub ");
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
    if (v->is_public) sb_puts(&b, "pub ");
    sb_printf(&b, "val %s", bare_name(v->name));
    if (v->has_declared_type) sb_printf(&b, ": %s", type_text(v->type));
    return b.s;
}

typedef struct { const char *name; const char *file; int line, col; int kind; char *signature; int is_public; } Sym;

/* Every declaration the last analysis loaded (derived code excluded). */
static Sym *collect_symbols(int *count) {
    int n = 0, cap = 64;
    Sym *out = malloc((size_t)cap * sizeof(Sym));
#define ADD(nm, f, l, c, k, sig, pub) do { if (n == cap) { cap *= 2; out = realloc(out, (size_t)cap * sizeof(Sym)); } \
    out[n++] = (Sym){nm, f, l, c, k, sig, pub}; } while (0)
    if (g_module != NULL) {
        for (int i = 0; i < g_module->functions.count; i++) {
            FunctionDecl *fn = g_module->functions.items[i];
            if (fn->derived_from != NULL || fn->source_file == NULL || fn->name[0] == '$') continue;
            ADD(fn->name, fn->source_file, fn->line, fn->col, 12, fn_signature(fn), fn->is_public);
        }
        for (int i = 0; i < g_module->structs.count; i++) {
            StructDecl *sd = g_module->structs.items[i];
            if (sd->source_file == NULL) continue;
            ADD(sd->name, sd->source_file, sd->line, sd->col, 23, struct_signature(sd), sd->is_public);
        }
        for (int i = 0; i < g_module->enums.count; i++) {
            EnumDecl *ed = g_module->enums.items[i];
            if (ed->source_file == NULL) continue;
            ADD(ed->name, ed->source_file, ed->line, ed->col, 10, enum_signature(ed), ed->is_public);
        }
        for (int i = 0; i < g_module->vals.count; i++) {
            GlobalValDecl *v = g_module->vals.items[i];
            if (v->source_file == NULL) continue;
            ADD(v->name, v->source_file, v->line, v->col, 13, val_signature(v), v->is_public);
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

static int matches_word(const Sym *s, const Cursor *c) {
    if (strcmp(bare_name(s->name), c->word) != 0) return 0;
    if (c->qualifier[0] != '\0') {
        char *seg = module_segment(s->name);
        int ok = strcmp(seg, c->qualifier) == 0;
        return ok;
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
            if (matches_word(&syms[i], c)) { if (best == NULL) best = &syms[i]; (*found)++; }
        }
        if (best != NULL) break;
    }
    return best;
}

static void handle_definition(const JVal *id, const JVal *params) {
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
        /* an enum variant (`Shape.Circle`): point at its enum */
        for (int i = 0; i < g_module->enums.count && g_module != NULL; i++) {
            EnumDecl *ed = g_module->enums.items[i];
            for (int v = 0; v < ed->variants.count; v++) {
                if (strcmp(ed->variants.items[v].name, c.word) == 0 && ed->source_file != NULL) {
                    SB b = {0};
                    write_location(&b, ed->source_file, ed->variants.items[v].line > 0 ? ed->variants.items[v].line : ed->line,
                                   ed->variants.items[v].line > 0 ? ed->variants.items[v].col : ed->col);
                    send_result(id, b.s);
                    return;
                }
            }
        }
        send_result(id, "null");
        return;
    }
    SB b = {0};
    write_location(&b, s->file, s->line, s->col);
    send_result(id, b.s);
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

static const char *const KEYWORDS[] = {"fn", "val", "var", "if", "else", "while", "for", "in", "break", "continue", "spawn", "defer", "switch",
                                       "return", "true", "false", "as", "struct", "enum", "with", "pub", "use", "native", "Result", "Channel", "Map", "Set",
                                       "int", "float", "bool", "string", "bigint", "decimal", "byte"};
static const char *const BUILTINS[] = {"print", "printf", "format", "str", "len", "push", "insert", "set", "remove", "contains", "hash", "compare",
                                       "abs", "min", "max", "ok", "err", "is_ok", "unwrap", "unwrap_err", "channel_new", "send", "receive",
                                       "try_send", "try_receive", "channel_len", "concat", "substring", "split", "trim", "to_upper", "to_lower",
                                       "index_of", "replace", "starts_with", "ends_with", "parse_int", "parse_float", "join", "slice",
                                       "concat_arrays", "map_get", "map_keys", "map_values", "set_items", "string_to_bytes", "bytes_to_string", "bytes_concat", "sb_new", "sb_append", "sb_to_string", "ab_new", "ab_append", "ab_to_array"};

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
                if (strcmp(seg, c.qualifier) == 0 && syms[i].is_public) {
                    add_completion(&b, &first, bare_name(syms[i].name), syms[i].kind == 12 ? 3 : syms[i].kind == 23 ? 22 : syms[i].kind == 10 ? 13 : 6, syms[i].signature);
                }
            }
            if (g_module != NULL) {
                for (int i = 0; i < g_module->enums.count; i++) {
                    EnumDecl *ed = g_module->enums.items[i];
                    if (strcmp(bare_name(ed->name), c.qualifier) != 0) continue;
                    for (int v = 0; v < ed->variants.count; v++) add_completion(&b, &first, ed->variants.items[v].name, 20, NULL);
                }
            }
        } else {
            for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) add_completion(&b, &first, KEYWORDS[i], 14, NULL);
            for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) add_completion(&b, &first, BUILTINS[i], 3, "builtin");
            for (int i = 0; i < nsyms; i++) {
                if (strcmp(syms[i].file, d->path) != 0 && !syms[i].is_public) continue;
                if (strcmp(syms[i].file, d->path) != 0) continue; /* other modules are reached as `module.name` */
                add_completion(&b, &first, bare_name(syms[i].name), syms[i].kind == 12 ? 3 : syms[i].kind == 23 ? 22 : syms[i].kind == 10 ? 13 : 6, syms[i].signature);
            }
        }
    }
    sb_puts(&b, "]}");
    send_result(id, b.s);
}

static void handle_formatting(const JVal *id, const JVal *params) {
    const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
    Doc *d = uri != NULL ? find_doc_by_uri(uri) : NULL;
    if (d == NULL) { send_result(id, "[]"); return; }
    char err[256];
    char *formatted = format_source(d->text, strlen(d->text), 120, err, sizeof err);
    if (formatted == NULL) { send_error(id, -32803, err); return; }
    if (strcmp(formatted, d->text) == 0) { send_result(id, "[]"); return; }
    int lines = 0;
    for (const char *p = d->text; *p; p++) if (*p == '\n') lines++;
    SB b = {0};
    sb_printf(&b, "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":%d,\"character\":0}},\"newText\":", lines + 1);
    sb_json_string(&b, formatted);
    sb_puts(&b, "}]");
    send_result(id, b.s);
}

static int g_shutdown = 0;

static void handle_message(const JVal *msg) {
    const char *method = jstr(jget(msg, "method"));
    JVal *id = jget(msg, "id");
    JVal *params = jget(msg, "params");
    if (method == NULL) return; /* a response to a request we never send */
    if (strcmp(method, "initialize") == 0) {
        send_result(id,
                    "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{}},"
                    "\"documentFormattingProvider\":true,\"hoverProvider\":true,\"definitionProvider\":true,"
                    "\"documentSymbolProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
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
            set_doc(uri, text);
            analyze(find_doc_by_uri(uri));
        }
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        const char *uri = jstr(jget(jget(params, "textDocument"), "uri"));
        JVal *changes = jget(params, "contentChanges");
        if (uri != NULL && changes != NULL && changes->kind == J_ARR && changes->count > 0) {
            const char *text = jstr(jget(changes->items[changes->count - 1], "text"));
            if (text != NULL) {
                set_doc(uri, text);
                analyze(find_doc_by_uri(uri));
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
