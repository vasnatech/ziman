#include "derive.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "lexer.h"
#include "parser.h"

/* ---------------------------------------------------------------------
 * A tiny growable buffer for the Ziman source text this pass generates.
 * The generated text is fed straight back through lex()/parse() (see
 * parse_and_splice below) rather than built as hand-constructed AST
 * nodes -- much easier to get right and to debug (the generated source
 * can just be printed and read) for something with as much shape as a
 * full function body.
 * --------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} CodeBuf;

static void cb_init(CodeBuf *cb) {
    cb->cap = 4096;
    cb->data = malloc(cb->cap);
    cb->data[0] = '\0';
    cb->len = 0;
}

static void cb_appendf(CodeBuf *cb, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int needed = vsnprintf(cb->data + cb->len, cb->cap - cb->len, fmt, ap);
        va_end(ap);
        if (needed < 0) ziman_error("internal", 0, 0, "derive: vsnprintf failed");
        if ((size_t)needed < cb->cap - cb->len) {
            cb->len += (size_t)needed;
            return;
        }
        size_t needed_cap = cb->len + (size_t)needed + 1;
        while (cb->cap < needed_cap) cb->cap *= 2;
        cb->data = realloc(cb->data, cb->cap);
    }
}

/* A one-off formatted string, used for building small expression
 * fragments (e.g. "v.first_name", "Option.unwrap_Some(x)") passed
 * around as `src_expr`/`dst_var` arguments -- never freed, matching
 * every other AST-adjacent allocation in this codebase. */
static char *fmt_str(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = malloc((size_t)needed + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    return buf;
}

/* ---------------------------------------------------------------------
 * Naming conventions.
 * --------------------------------------------------------------------- */

/* "ValidationError" -> "validation_error", "Patient" -> "patient",
 * "Uuid" -> "uuid". A fresh, standard, multi-word-aware convention for
 * the names this pass itself generates (<Type>_to_json/_from_json) --
 * deliberately its own convention, distinct from the older, simpler
 * "lowercase with no inserted underscores" shortcut lib/core/time.zm's
 * hand-written _to_string/_from_string pairs already use for their own
 * compound names (localdate_to_string, not local_date_to_string). Those
 * corelib types get their own hand-written _to_json/_from_json wrappers
 * (see lib/core/time.zm, lib/core/uuid.zm) named to match *this*
 * convention, so a struct field of type LocalDate correctly resolves to
 * `local_date_to_json`. See docs/DESIGN.md's "Attributes and derive"
 * entry. */
static char *pascal_to_snake(const char *name) {
    size_t len = strlen(name);
    char *out = malloc(len * 2 + 1);
    size_t oi = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isupper(c)) {
            int prev_lower_or_digit = i > 0 && (islower((unsigned char)name[i - 1]) || isdigit((unsigned char)name[i - 1]));
            int prev_upper_next_lower = i > 0 && isupper((unsigned char)name[i - 1]) && i + 1 < len && islower((unsigned char)name[i + 1]);
            if (i > 0 && (prev_lower_or_digit || prev_upper_next_lower)) out[oi++] = '_';
            out[oi++] = (char)tolower(c);
        } else {
            out[oi++] = name[i];
        }
    }
    out[oi] = '\0';
    return out;
}

/* "date_of_birth" -> "dateOfBirth" -- the JSON key a struct field maps
 * to. Fixed and unconfigurable; a documented shortcoming, not solved
 * here -- see docs/ROADMAP.md's derive entry. */
static char *snake_to_camel(const char *name) {
    size_t len = strlen(name);
    char *out = malloc(len + 1);
    size_t oi = 0;
    int capitalize_next = 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '_') { capitalize_next = 1; continue; }
        out[oi++] = capitalize_next ? (char)toupper((unsigned char)c) : c;
        capitalize_next = 0;
    }
    out[oi] = '\0';
    return out;
}

static char *gen_tmp(int *counter, const char *hint) {
    return fmt_str("__derive_%s_%d", hint, (*counter)++);
}

/* ---------------------------------------------------------------------
 * Per-field/per-payload conversion codegen, shared by structs and enums.
 * Runs on a freshly *parsed* Type -- before the loader's struct-vs-enum
 * reclassification pass, so a real Option<T> is still TY_STRUCT-shaped
 * at this point (see ast.h's Type comment on why); is_option_type_here
 * checks the name directly rather than the kind so it works either way.
 * --------------------------------------------------------------------- */

static int is_option_type_here(Type t) {
    if (t.kind != TY_STRUCT && t.kind != TY_ENUM) return 0;
    const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
    return name != NULL && strcmp(name, "Option") == 0 && t.type_args.count == 1;
}

static void emit_to_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col);
static void emit_from_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col);

/* Appends statements to `cb` that bind `dst_var` (a `val`, freshly
 * declared) to a `Json` value built from `src_expr` (an already-valid
 * Ziman expression of type `t`). Priority order, matching the design:
 * struct/enum types (other than Option) dispatch by naming convention
 * to their own <name>_to_json (hand-written or itself derived); scalars
 * map directly; Option<U>/[U] recurse into U. */
static void emit_to_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = Json.JNumber(%s as float)\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = Json.JNumber(%s)\n", dst_var, src_expr);
            return;
        case TY_BOOL:
            cb_appendf(cb, "    val %s = Json.JBool(%s)\n", dst_var, src_expr);
            return;
        case TY_STRING:
            cb_appendf(cb, "    val %s = Json.JString(%s)\n", dst_var, src_expr);
            return;
        case TY_ARRAY: {
            char *item = gen_tmp(tmp, "item");
            char *item_json = gen_tmp(tmp, "item_json");
            char *ab = gen_tmp(tmp, "ab");
            cb_appendf(cb, "    val %s = ab_new<Json>()\n", ab);
            cb_appendf(cb, "    for %s in %s {\n", item, src_expr);
            emit_to_json(*t.elem, item, item_json, cb, tmp, line, col);
            cb_appendf(cb, "    ab_append<Json>(%s, %s)\n", ab, item_json);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = Json.JArray(ab_to_array<Json>(%s))\n", dst_var, ab);
            return;
        }
        case TY_STRUCT:
        case TY_ENUM:
            if (is_option_type_here(t)) {
                Type elem = t.type_args.items[0];
                char *inner = gen_tmp(tmp, "opt_inner");
                char *unwrap_expr = fmt_str("Option.unwrap_Some(%s)", src_expr);
                cb_appendf(cb, "    var %s = Json.JNull\n", dst_var);
                cb_appendf(cb, "    if Option.is_Some(%s) {\n", src_expr);
                emit_to_json(elem, unwrap_expr, inner, cb, tmp, line, col);
                cb_appendf(cb, "    %s = %s\n", dst_var, inner);
                cb_appendf(cb, "    }\n");
                return;
            } else {
                const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
                char *fn = pascal_to_snake(name);
                cb_appendf(cb, "    val %s = %s_to_json(%s)\n", dst_var, fn, src_expr);
                return;
            }
        default:
            ziman_error("syntax", line, col, "derive(Json) does not support field type '%s' yet", type_name(t));
    }
}

/* Mirror of emit_to_json: appends statements binding `dst_var` (a
 * `val`) to a Ziman value of type `t`, decoded from `src_expr` (an
 * already-valid Ziman expression of type `Json`). Every branch either
 * binds `dst_var` or returns an `err(...)`/propagates via `?` -- never
 * falls through -- so the emitted code always leaves `dst_var` usable
 * afterward. Lenient by construction: nothing here ever inspects a
 * JObject's *other* keys, so unrecognized extra fields are silently
 * ignored, matching the design's lenient-deserialization decision. */
static void emit_from_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    if !Json.is_JNumber(%s) { return err(\"expected a number\") }\n", src_expr);
            cb_appendf(cb, "    val %s = Json.unwrap_JNumber(%s) as int\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    if !Json.is_JNumber(%s) { return err(\"expected a number\") }\n", src_expr);
            cb_appendf(cb, "    val %s = Json.unwrap_JNumber(%s)\n", dst_var, src_expr);
            return;
        case TY_BOOL:
            cb_appendf(cb, "    if !Json.is_JBool(%s) { return err(\"expected a boolean\") }\n", src_expr);
            cb_appendf(cb, "    val %s = Json.unwrap_JBool(%s)\n", dst_var, src_expr);
            return;
        case TY_STRING:
            cb_appendf(cb, "    if !Json.is_JString(%s) { return err(\"expected a string\") }\n", src_expr);
            cb_appendf(cb, "    val %s = Json.unwrap_JString(%s)\n", dst_var, src_expr);
            return;
        case TY_ARRAY: {
            char *ab = gen_tmp(tmp, "ab");
            char *item = gen_tmp(tmp, "item");
            char *item_val = gen_tmp(tmp, "item_val");
            char *elem_type_name = type_name(*t.elem);
            cb_appendf(cb, "    if !Json.is_JArray(%s) { return err(\"expected an array\") }\n", src_expr);
            cb_appendf(cb, "    val %s = ab_new<%s>()\n", ab, elem_type_name);
            cb_appendf(cb, "    for %s in Json.unwrap_JArray(%s) {\n", item, src_expr);
            emit_from_json(*t.elem, item, item_val, cb, tmp, line, col);
            cb_appendf(cb, "    ab_append<%s>(%s, %s)\n", elem_type_name, ab, item_val);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = ab_to_array<%s>(%s)\n", dst_var, elem_type_name, ab);
            return;
        }
        case TY_STRUCT:
        case TY_ENUM:
            if (is_option_type_here(t)) {
                Type elem = t.type_args.items[0];
                char *elem_type_name = type_name(elem);
                char *inner = gen_tmp(tmp, "opt_inner");
                cb_appendf(cb, "    var %s = Option<%s>.None\n", dst_var, elem_type_name);
                cb_appendf(cb, "    if !Json.is_JNull(%s) {\n", src_expr);
                emit_from_json(elem, src_expr, inner, cb, tmp, line, col);
                cb_appendf(cb, "    %s = Option<%s>.Some(%s)\n", dst_var, elem_type_name, inner);
                cb_appendf(cb, "    }\n");
                return;
            } else {
                const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
                char *fn = pascal_to_snake(name);
                cb_appendf(cb, "    val %s = %s_from_json(%s)?\n", dst_var, fn, src_expr);
                return;
            }
        default:
            ziman_error("syntax", line, col, "derive(Json) does not support field type '%s' yet", type_name(t));
    }
}

/* ---------------------------------------------------------------------
 * Struct derive: <name>_to_json / <name>_from_json.
 * --------------------------------------------------------------------- */

static char *generate_struct_to_json(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_to_json(v: %s) -> Json {\n", fn_name, sd->name);

    int tmp = 0;
    char **dst_vars = malloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    char **camel_keys = malloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *dst = fmt_str("field_%s_json", f->name);
        char *src = fmt_str("v.%s", f->name);
        emit_to_json(f->type, src, dst, &cb, &tmp, f->line, f->col);
        dst_vars[i] = dst;
        camel_keys[i] = snake_to_camel(f->name);
    }

    cb_appendf(&cb, "    return Json.JObject([\n");
    for (int i = 0; i < sd->fields.count; i++) {
        cb_appendf(&cb, "        JField { key: \"%s\", value: %s }%s\n", camel_keys[i], dst_vars[i], i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    ])\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

static char *generate_struct_from_json(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_from_json(j: Json) -> Result<%s, string> {\n", fn_name, sd->name);
    cb_appendf(&cb, "    if !Json.is_JObject(j) { return err(\"expected a JSON object for %s\") }\n", sd->name);

    int tmp = 0;
    char **field_vars = malloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *camel = snake_to_camel(f->name);
        char *dst = fmt_str("field_%s", f->name);
        if (is_option_type_here(f->type)) {
            /* Tolerant: an absent key *or* an explicit JSON null both
             * decode to None -- mirrors the exact hand-written pattern
             * health-record-backend-ziman's own request.zm used for its
             * `middleName` field before this mechanism existed. */
            Type elem = f->type.type_args.items[0];
            char *elem_type_name = type_name(elem);
            char *found = fmt_str("field_%s_found", f->name);
            char *raw = fmt_str("field_%s_raw", f->name);
            char *inner = fmt_str("field_%s_inner", f->name);
            cb_appendf(&cb, "    val %s = json_object_get(j, \"%s\")\n", found, camel);
            cb_appendf(&cb, "    var %s = Option<%s>.None\n", dst, elem_type_name);
            cb_appendf(&cb, "    if Option.is_Some(%s) {\n", found);
            cb_appendf(&cb, "    val %s = Option.unwrap_Some(%s)\n", raw, found);
            cb_appendf(&cb, "    if !Json.is_JNull(%s) {\n", raw);
            emit_from_json(elem, raw, inner, &cb, &tmp, f->line, f->col);
            cb_appendf(&cb, "    %s = Option<%s>.Some(%s)\n", dst, elem_type_name, inner);
            cb_appendf(&cb, "    }\n");
            cb_appendf(&cb, "    }\n");
        } else {
            char *raw = fmt_str("field_%s_raw", f->name);
            cb_appendf(&cb, "    val %s = json_require_field(j, \"%s\")?\n", raw, camel);
            emit_from_json(f->type, raw, dst, &cb, &tmp, f->line, f->col);
        }
        field_vars[i] = dst;
    }

    cb_appendf(&cb, "    return %s {\n", sd->name);
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "        %s: %s%s\n", f->name, field_vars[i], i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Enum derive: externally tagged -- {"type": "<Variant>"} for a unit
 * variant, {"type": "<Variant>", "value": ...} for a payload variant.
 * Option<T> never reaches here (it's excluded from this tagging by
 * emit_to_json/emit_from_json's own dedicated Option handling, which
 * intercepts it as a *field's* type before generic enum dispatch would
 * ever apply); this generator only ever runs for an ordinary
 * user-declared enum with #[derive(Json)] on it directly.
 * --------------------------------------------------------------------- */

static char *generate_enum_to_json(EnumDecl *ed) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(ed->name);
    cb_appendf(&cb, "pub fn %s_to_json(v: %s) -> Json {\n", fn_name, ed->name);

    int tmp = 0;
    int n = ed->variants.count;
    for (int i = 0; i < n; i++) {
        EnumVariant *variant = &ed->variants.items[i];
        int is_last = (i == n - 1);
        if (n == 1) {
            /* the only variant -- unconditional, no need to check */
        } else if (i == 0) {
            cb_appendf(&cb, "    if %s.is_%s(v) {\n", ed->name, variant->name);
        } else if (!is_last) {
            cb_appendf(&cb, "    } else if %s.is_%s(v) {\n", ed->name, variant->name);
        } else {
            cb_appendf(&cb, "    } else {\n");
        }
        if (!variant->has_payload) {
            cb_appendf(&cb, "        return Json.JObject([JField { key: \"type\", value: Json.JString(\"%s\") }])\n", variant->name);
        } else {
            char *unwrap_expr = fmt_str("%s.unwrap_%s(v)", ed->name, variant->name);
            char *dst = fmt_str("payload_json_%d", i);
            emit_to_json(variant->payload_type, unwrap_expr, dst, &cb, &tmp, variant->line, variant->col);
            cb_appendf(&cb, "        return Json.JObject([\n");
            cb_appendf(&cb, "            JField { key: \"type\", value: Json.JString(\"%s\") },\n", variant->name);
            cb_appendf(&cb, "            JField { key: \"value\", value: %s }\n", dst);
            cb_appendf(&cb, "        ])\n");
        }
    }
    if (n > 1) cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

static char *generate_enum_from_json(EnumDecl *ed) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(ed->name);
    cb_appendf(&cb, "pub fn %s_from_json(j: Json) -> Result<%s, string> {\n", fn_name, ed->name);
    cb_appendf(&cb, "    if !Json.is_JObject(j) { return err(\"expected a JSON object for %s\") }\n", ed->name);
    cb_appendf(&cb, "    val __derive_tag = json_require_string_field(j, \"type\")?\n");

    int tmp = 0;
    int n = ed->variants.count;
    for (int i = 0; i < n; i++) {
        EnumVariant *variant = &ed->variants.items[i];
        const char *kw = i == 0 ? "if" : "} else if";
        cb_appendf(&cb, "    %s __derive_tag == \"%s\" {\n", kw, variant->name);
        if (!variant->has_payload) {
            cb_appendf(&cb, "        return %s.%s\n", ed->name, variant->name);
        } else {
            cb_appendf(&cb, "        val __derive_payload_raw = json_require_field(j, \"value\")?\n");
            char *dst = fmt_str("payload_%d", i);
            emit_from_json(variant->payload_type, "__derive_payload_raw", dst, &cb, &tmp, variant->line, variant->col);
            cb_appendf(&cb, "        return %s.%s(%s)\n", ed->name, variant->name, dst);
        }
    }
    cb_appendf(&cb, "    } else {\n");
    cb_appendf(&cb, "        return err(concat(\"unknown variant tag for %s: \", __derive_tag))\n", ed->name);
    cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Row derive: <name>_from_row / <name>_to_params, for mapping a struct to
 * and from one SQL result row (lib/db/sql.zm's Row/SqlValue).
 *
 * Column names are the struct's own field names, as written -- already
 * snake_case, which is also how SQL columns are conventionally named, so
 * unlike Json's camelCase keys there is no transform here. Field
 * conversion: int/float/bool/string map to the matching SqlValue; a
 * struct/enum-kind field dispatches by naming convention to
 * <name>_to_sql/<name>_from_sql (hand-written -- lib/core/uuid.zm and
 * lib/core/time.zm provide them for their domain types, all carried as
 * Text); Option<U> is NULL-or-value. Arrays, nested tables and the
 * remaining scalar kinds are compile-time errors, not silently wrong.
 * --------------------------------------------------------------------- */

static void emit_to_sql(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = SqlValue.Int(%s)\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = SqlValue.Float(%s)\n", dst_var, src_expr);
            return;
        case TY_BOOL:
            cb_appendf(cb, "    val %s = SqlValue.Bool(%s)\n", dst_var, src_expr);
            return;
        case TY_STRING:
            cb_appendf(cb, "    val %s = SqlValue.Text(%s)\n", dst_var, src_expr);
            return;
        case TY_STRUCT:
        case TY_ENUM:
            if (is_option_type_here(t)) {
                Type elem = t.type_args.items[0];
                char *inner = gen_tmp(tmp, "opt_inner");
                char *unwrap_expr = fmt_str("Option.unwrap_Some(%s)", src_expr);
                cb_appendf(cb, "    var %s = SqlValue.Null\n", dst_var);
                cb_appendf(cb, "    if Option.is_Some(%s) {\n", src_expr);
                emit_to_sql(elem, unwrap_expr, inner, cb, tmp, line, col);
                cb_appendf(cb, "    %s = %s\n", dst_var, inner);
                cb_appendf(cb, "    }\n");
                return;
            } else {
                const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
                char *fn = pascal_to_snake(name);
                cb_appendf(cb, "    val %s = %s_to_sql(%s)\n", dst_var, fn, src_expr);
                return;
            }
        default:
            ziman_error("syntax", line, col, "derive(Row) does not support field type '%s'", type_name(t));
    }
}

static void emit_from_sql(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = sql_as_int(%s)?\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = sql_as_float(%s)?\n", dst_var, src_expr);
            return;
        case TY_BOOL:
            cb_appendf(cb, "    val %s = sql_as_bool(%s)?\n", dst_var, src_expr);
            return;
        case TY_STRING:
            cb_appendf(cb, "    val %s = sql_as_text(%s)?\n", dst_var, src_expr);
            return;
        case TY_STRUCT:
        case TY_ENUM:
            if (is_option_type_here(t)) {
                Type elem = t.type_args.items[0];
                char *elem_type_name = type_name(elem);
                char *inner = gen_tmp(tmp, "opt_inner");
                cb_appendf(cb, "    var %s = Option<%s>.None\n", dst_var, elem_type_name);
                cb_appendf(cb, "    if !SqlValue.is_Null(%s) {\n", src_expr);
                emit_from_sql(elem, src_expr, inner, cb, tmp, line, col);
                cb_appendf(cb, "    %s = Option<%s>.Some(%s)\n", dst_var, elem_type_name, inner);
                cb_appendf(cb, "    }\n");
                return;
            } else {
                const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
                char *fn = pascal_to_snake(name);
                cb_appendf(cb, "    val %s = %s_from_sql(%s)?\n", dst_var, fn, src_expr);
                return;
            }
        default:
            ziman_error("syntax", line, col, "derive(Row) does not support field type '%s'", type_name(t));
    }
}

static char *generate_struct_from_row(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_from_row(row: Row) -> Result<%s, DbError> {\n", fn_name, sd->name);
    int tmp = 0;
    char **field_vars = malloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *raw = fmt_str("field_%s_raw", f->name);
        char *dst = fmt_str("field_%s", f->name);
        cb_appendf(&cb, "    val %s = row_get(row, \"%s\")?\n", raw, f->name);
        emit_from_sql(f->type, raw, dst, &cb, &tmp, f->line, f->col);
        field_vars[i] = dst;
    }
    cb_appendf(&cb, "    return %s {\n", sd->name);
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "        %s: %s%s\n", f->name, field_vars[i], i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

/* The parameters come back in field declaration order -- the order a
 * hand-written `INSERT INTO t (col1, col2, ...) VALUES ($1, $2, ...)`
 * lists its columns in, since repository SQL is written by hand. */
static char *generate_struct_to_params(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_to_params(v: %s) -> [SqlValue] {\n", fn_name, sd->name);
    cb_appendf(&cb, "    val __derive_params = ab_new<SqlValue>()\n");
    int tmp = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *src = fmt_str("v.%s", f->name);
        char *dst = fmt_str("field_%s_sql", f->name);
        emit_to_sql(f->type, src, dst, &cb, &tmp, f->line, f->col);
        cb_appendf(&cb, "    ab_append<SqlValue>(__derive_params, %s)\n", dst);
    }
    cb_appendf(&cb, "    return ab_to_array<SqlValue>(__derive_params)\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Properties derive: <name>_from_properties / <name>_keys, binding a
 * struct to the keys under a prefix in a lib/core/properties.zm
 * Properties (field `idle_timeout_ms` <-> key `<prefix>.idle-timeout-ms`).
 * The generated code calls qualified `properties.*`, so the consumer
 * needs only `use properties`. Option<T> fields are optional keys;
 * every other field is required.
 * --------------------------------------------------------------------- */

static char *snake_to_kebab(const char *name) {
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    memcpy(out, name, n + 1);
    for (char *c = out; *c; c++) if (*c == '_') *c = '-';
    return out;
}

static const char *properties_getter_suffix(Type t, int line, int col) {
    switch (t.kind) {
        case TY_INT: return "int";
        case TY_FLOAT: return "float";
        case TY_BOOL: return "bool";
        case TY_STRING: return "string";
        default:
            ziman_error("syntax", line, col, "derive(Properties) supports only int, float, bool, string and Option of those, not '%s'", type_name(t));
    }
    return NULL;
}

static char *generate_struct_properties(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_from_properties(props: properties.Properties, prefix: string) -> Result<%s, string> {\n", fn_name, sd->name);
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *kebab = snake_to_kebab(f->name);
        if (is_option_type_here(f->type)) {
            const char *g = properties_getter_suffix(f->type.type_args.items[0], f->line, f->col);
            cb_appendf(&cb, "    var field_%s = Option<%s>.None\n", f->name, g);
            cb_appendf(&cb, "    val found_%s = properties.lookup(props, properties.key(prefix, \"%s\"))\n", f->name, kebab);
            cb_appendf(&cb, "    if Option.is_Some(found_%s) {\n", f->name);
            cb_appendf(&cb, "        val value_%s = properties.entry_%s(Option.unwrap_Some(found_%s))?\n", f->name, g, f->name);
            cb_appendf(&cb, "        field_%s = Option<%s>.Some(value_%s)\n    }\n", f->name, g, f->name);
        } else {
            const char *g = properties_getter_suffix(f->type, f->line, f->col);
            cb_appendf(&cb, "    val field_%s = properties.get_%s(props, properties.key(prefix, \"%s\"))?\n", f->name, g, kebab);
        }
    }
    cb_appendf(&cb, "    return %s {\n", sd->name);
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "        %s: field_%s%s\n", f->name, f->name, i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    }\n}\n\n");
    cb_appendf(&cb, "pub fn %s_keys(prefix: string) -> [string] {\n    return [", fn_name);
    for (int i = 0; i < sd->fields.count; i++) {
        char *kebab = snake_to_kebab(sd->fields.items[i].name);
        cb_appendf(&cb, "%sproperties.key(prefix, \"%s\")", i ? ", " : "", kebab);
    }
    cb_appendf(&cb, "]\n}\n\n");
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Driver: lex+parse the generated snippet and splice its functions in.
 * --------------------------------------------------------------------- */

/* `line`/`col` are the declaration the code is generated for. Every token
 * is restamped with that position before parsing, so an error anywhere in
 * the generated code points at the user's own `struct`, and `label` (say,
 * "derive(Json) for 'Point'") is attached to the functions so the checker
 * can say what it was reading. */
int g_emit_derived = 0;

static void parse_and_splice(Module *module, char *source, int line, int col, const char *label) {
    if (g_emit_derived) printf("// %s\n%s", label, source);
    ziman_set_error_context(label);
    TokenList tokens = lex(source);
    for (int i = 0; i < tokens.count; i++) {
        tokens.items[i].line = line;
        tokens.items[i].col = col;
    }
    Module *snippet = parse(tokens);
    ziman_set_error_context(NULL);
    for (int i = 0; i < snippet->functions.count; i++) {
        snippet->functions.items[i]->derived_from = label;
        function_list_push(&module->functions, snippet->functions.items[i]);
    }
    /* `tokens`/`snippet`/`source` are deliberately never freed -- AST
     * nodes in this codebase are never freed at all (see ast.h), and a
     * spliced-in FunctionDecl's own Tokens were already fully copied out
     * by dup_token_text at parse time, so nothing depends on `source`
     * staying alive past this point either; leaving it allocated is
     * simplest and matches the rest of the codebase's policy. */
}

/* ---------------------------------------------------------------------
 * Imports the generated code needs. The generator knows exactly which
 * names it emitted, so the user shouldn't have to spell five `use` lines
 * for one `#[derive(Json)]` -- and forgetting one gave an unhelpful error.
 * A name is added only when the module doesn't already bind it (an
 * explicit `use`, or a declaration of the same name), so an explicit
 * import still works and a user's own `Row` is left alone.
 * --------------------------------------------------------------------- */

static int module_binds_name(Module *module, const char *name) {
    for (int i = 0; i < module->uses.count; i++) {
        UseDecl *u = &module->uses.items[i];
        if (u->local_name != NULL && strcmp(u->local_name, name) == 0) return 1;
    }
    for (int i = 0; i < module->functions.count; i++) if (strcmp(module->functions.items[i]->name, name) == 0) return 1;
    for (int i = 0; i < module->structs.count; i++) if (strcmp(module->structs.items[i]->name, name) == 0) return 1;
    for (int i = 0; i < module->enums.count; i++) if (strcmp(module->enums.items[i]->name, name) == 0) return 1;
    return 0;
}

static void ensure_named_import(Module *module, const char *path, const char *name, int line, int col) {
    if (module_binds_name(module, name)) return;
    UseDecl u;
    memset(&u, 0, sizeof u);
    u.path = fmt_str("%s", path);
    u.imported_name = fmt_str("%s", name);
    u.local_name = fmt_str("%s", name);
    u.line = line;
    u.col = col;
    use_list_push(&module->uses, u);
}

static void ensure_module_import(Module *module, const char *path, int line, int col) {
    for (int i = 0; i < module->uses.count; i++) {
        UseDecl *u = &module->uses.items[i];
        if (u->qualifier != NULL && strcmp(u->path, path) == 0 && strcmp(u->qualifier, path) == 0) return;
    }
    UseDecl u;
    memset(&u, 0, sizeof u);
    u.path = fmt_str("%s", path);
    u.qualifier = fmt_str("%s", path);
    u.line = line;
    u.col = col;
    use_list_push(&module->uses, u);
}

static void import_for_derive(Module *module, const char *target, int line, int col) {
    if (strcmp(target, "Json") == 0) {
        static const char *const names[] = {"Json", "JField", "json_object_get", "json_require_field", "json_require_string_field"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) ensure_named_import(module, "json", names[i], line, col);
    } else if (strcmp(target, "Row") == 0) {
        static const char *const names[] = {"SqlValue", "DbError", "Row", "row_get", "sql_as_text", "sql_as_int", "sql_as_bool", "sql_as_float"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) ensure_named_import(module, "sql", names[i], line, col);
    } else if (strcmp(target, "Properties") == 0) {
        ensure_module_import(module, "properties", line, col);
    }
}

void generate_derived_functions(Module *module) {
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        for (int d = 0; d < sd->derive_count; d++) {
            int is_json = strcmp(sd->derives[d], "Json") == 0;
            int is_row = strcmp(sd->derives[d], "Row") == 0;
            int is_props = strcmp(sd->derives[d], "Properties") == 0;
            if (!is_json && !is_row && !is_props) {
                ziman_error("syntax", sd->line, sd->col, "unknown derive target '%s' on struct '%s'", sd->derives[d], sd->name);
            }
            if (sd->type_params.count > 0) {
                ziman_error("syntax", sd->line, sd->col, "derive(%s) does not support a generic struct yet ('%s')", sd->derives[d], sd->name);
            }
            const char *label = fmt_str("derive(%s) for '%s'", sd->derives[d], sd->name);
            import_for_derive(module, sd->derives[d], sd->line, sd->col);
            if (is_props) {
                parse_and_splice(module, generate_struct_properties(sd), sd->line, sd->col, label);
            } else if (is_json) {
                parse_and_splice(module, generate_struct_to_json(sd), sd->line, sd->col, label);
                parse_and_splice(module, generate_struct_from_json(sd), sd->line, sd->col, label);
            } else {
                parse_and_splice(module, generate_struct_from_row(sd), sd->line, sd->col, label);
                parse_and_splice(module, generate_struct_to_params(sd), sd->line, sd->col, label);
            }
        }
    }
    for (int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        for (int d = 0; d < ed->derive_count; d++) {
            if (strcmp(ed->derives[d], "Row") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Row) does not support an enum ('%s') -- a SQL row is a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Properties") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Properties) does not support an enum ('%s') -- configuration binds to a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Json") != 0) {
                ziman_error("syntax", ed->line, ed->col, "unknown derive target '%s' on enum '%s'", ed->derives[d], ed->name);
            }
            if (ed->type_params.count > 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Json) does not support a generic enum yet ('%s')", ed->name);
            }
            const char *label = fmt_str("derive(%s) for '%s'", ed->derives[d], ed->name);
            import_for_derive(module, ed->derives[d], ed->line, ed->col);
            parse_and_splice(module, generate_enum_to_json(ed), ed->line, ed->col, label);
            parse_and_splice(module, generate_enum_from_json(ed), ed->line, ed->col, label);
        }
    }
}
