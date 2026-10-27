#include "derive.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "xalloc.h"

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
    cb->data = ziman_xmalloc(cb->cap);
    cb->data[0] = '\0';
    cb->len = 0;
}

/* The format attribute is not decoration: dropping a `<%s>` from one of these format strings while
 * leaving its argument behind produced code that passed a *type name* where a variable belonged,
 * and compiled silently. gcc checks the count and the types now. */
__attribute__((format(printf, 2, 3))) static void cb_appendf(CodeBuf *cb, const char *fmt, ...) {
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
        cb->data = ziman_xrealloc(cb->data, cb->cap);
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
    char *buf = ziman_xmalloc((size_t)needed + 1);
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
char *ziman_pascal_to_snake(const char *name);
static char *pascal_to_snake(const char *name) {
    size_t len = strlen(name);
    char *out = ziman_xmalloc(len * 2 + 1);
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

char *ziman_pascal_to_snake(const char *name) {
    return pascal_to_snake(name);
}

char *ziman_compare_function_name(const char *type_name) {
    const char *bare = type_name;
    for (const char *p = strstr(bare, "::"); p != NULL; p = strstr(bare, "::")) bare = p + 2;
    char *snake = pascal_to_snake(bare);
    size_t prefix_len = (size_t)(bare - type_name);
    char *out = ziman_xmalloc(prefix_len + strlen(snake) + strlen("_compare") + 1);
    memcpy(out, type_name, prefix_len);
    strcpy(out + prefix_len, snake);
    strcat(out, "_compare");
    return out;
}

/* "NetError" -> "net_error_message": where the `error_message` builtin looks for a type's
 * message function (docs/DESIGN.md's "A unified error_message builtin" entry). Same
 * PascalCase-to-snake_case rule as `ziman_compare_function_name`, no "flat" spelling -- unlike
 * `Ord`, this one has no pre-existing bundled types with an established alternate spelling to
 * stay compatible with. */
char *ziman_message_function_name(const char *type_name) {
    const char *bare = type_name;
    for (const char *p = strstr(bare, "::"); p != NULL; p = strstr(bare, "::")) bare = p + 2;
    char *snake = pascal_to_snake(bare);
    size_t prefix_len = (size_t)(bare - type_name);
    char *out = ziman_xmalloc(prefix_len + strlen(snake) + strlen("_message") + 1);
    memcpy(out, type_name, prefix_len);
    strcpy(out + prefix_len, snake);
    strcat(out, "_message");
    return out;
}

char *ziman_compare_function_name_flat(const char *type_name) {
    const char *bare = type_name;
    for (const char *p = strstr(bare, "::"); p != NULL; p = strstr(bare, "::")) bare = p + 2;
    size_t prefix_len = (size_t)(bare - type_name);
    size_t bare_len = strlen(bare);
    char *out = ziman_xmalloc(prefix_len + bare_len + strlen("_compare") + 1);
    memcpy(out, type_name, prefix_len);
    for (size_t i = 0; i < bare_len; i++) out[prefix_len + i] = (char)tolower((unsigned char)bare[i]);
    strcpy(out + prefix_len + bare_len, "_compare");
    return out;
}

/* "date_of_birth" -> "dateOfBirth" -- the JSON key a struct field maps
 * to. Fixed and unconfigurable; a documented shortcoming, not solved
 * here -- see docs/ROADMAP.md's derive entry. */
static char *snake_to_camel(const char *name) {
    size_t len = strlen(name);
    char *out = ziman_xmalloc(len + 1);
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

/* The bundled types whose converters are spelled short: `to_json(d)` / `to_localdate(j)` rather
 * than `local_date_to_json` / `local_date_from_json`. Answers the type's flat spelling (the one
 * `lib/time/time.zm` already uses for `localdate_of` and `localdate_compare`) or NULL for every
 * other type, which keeps derive's own `<snake>_to_json` convention.
 *
 * **Why only these seven, and why the `to` direction can be one name while the `from` direction
 * cannot.** `to_json` is told apart by its argument, so the seven are one overload set. Their
 * inverses all take a `Json`, so they cannot be -- each needs a name of its own, which is what the
 * flat type name supplies. Extending the short form to *derived* types is what this cannot do:
 * every `#[derive(Json)]` type would join one `to_json` set, and `OVL_MAX_CANDIDATES` is 32
 * against 100 derive(Json) sites in the reference backend alone, so the resolver would hand back
 * `find_function`'s first declaration instead of resolving. The short names are vocabulary for a
 * fixed, small set of library types; `<snake>_to_json` stays the rule for everything generated.
 *
 * The cost, stated: a module that declares its own `to_json` binds the name, `ensure_named_import`
 * leaves it alone (deliberately -- see its comment), and a derived field of a time type then
 * resolves to that function and reports a type error naming it. `local_date_to_json` could not
 * collide that way. Judged worth it for the call sites; `Uuid` keeps `uuid_to_json` because its
 * name has nothing to shorten. */
static const char *standard_flat_type_name(const char *type_name) {
    const char *dot = strrchr(type_name, '.');
    const char *bare = dot != NULL ? dot + 1 : type_name;
    const char *colons = strstr(bare, "::");
    if (colons != NULL) bare = colons + 2;
    static const struct { const char *type; const char *flat; } FLAT[] = {
        {"Instant", "instant"},         {"LocalDate", "localdate"},
        {"LocalTime", "localtime"},     {"LocalDateTime", "localdatetime"},
        {"Duration", "duration"},       {"ZoneOffset", "zoneoffset"},
        {"ZoneId", "zoneid"},
    };
    for (size_t i = 0; i < sizeof FLAT / sizeof FLAT[0]; i++) {
        if (strcmp(bare, FLAT[i].type) == 0) return FLAT[i].flat;
    }
    return NULL;
}

static void emit_to_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col);
static void emit_from_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col, const char *field_ctx);

/* A shape check whose failure message is known at codegen time (`"expected a string"`):
 * named with the JSON key when `field_ctx` is the struct field this check is for (the direct
 * field of the struct being decoded, never a recursive element/entry a level down, so an error
 * never names more than one field), plain otherwise. */
static void emit_type_check(CodeBuf *cb, const char *cond_expr, const char *field_ctx, const char *msg) {
    if (field_ctx != NULL) {
        cb_appendf(cb, "    if !%s { return err(\"%s: %s\") }\n", cond_expr, field_ctx, msg);
    } else {
        cb_appendf(cb, "    if !%s { return err(\"%s\") }\n", cond_expr, msg);
    }
}

/* A `try`-shaped call (`int_from_json(x)`, `<type>_from_json(x)`, a generic converter) whose
 * failure message is only known at run time: named the same way as emit_type_check, by catching
 * the `Err` and prefixing it (`concat`, the same device already used for an unknown enum tag)
 * rather than propagating it bare through `try`. */
static void emit_try_call(CodeBuf *cb, int *tmp, const char *dst_var, const char *call_expr, const char *field_ctx) {
    if (field_ctx == NULL) {
        cb_appendf(cb, "    val %s = try %s\n", dst_var, call_expr);
        return;
    }
    char *ok_v = gen_tmp(tmp, "ok");
    char *err_v = gen_tmp(tmp, "err");
    cb_appendf(cb, "    val %s = switch %s {\n", dst_var, call_expr);
    cb_appendf(cb, "        Ok(%s) { %s }\n", ok_v, ok_v);
    cb_appendf(cb, "        Err(%s) { return err(concat(\"%s: \", %s)) }\n", err_v, field_ctx, err_v);
    cb_appendf(cb, "    }\n");
}

/* ---- generic declarations ------------------------------------------------
 * `struct Page<T> { ... }` / `enum Either<L, R> { ... }` derive too. `params`
 * is the declaration's type parameter list (count 0 for an ordinary one).
 *
 * Ord: the generated function is itself generic with an `Ord` bound on every
 * parameter -- `pair_compare<A: Ord, B: Ord>(a: Pair<A, B>, b: Pair<A, B>)` --
 * so the `compare` calls in its body type-check.
 *
 * Json: a generic type can't know how to convert its `T`, so each type
 * parameter gets one *converter function value* passed in:
 *   page_to_json<T>(v: Page<T>, t_to_json: fn(T) -> Json) -> Json
 *   page_from_json<T>(j: Json, t_from_json: fn(Json) -> Result<T, string>)
 * A field of type `T` calls that converter. */

/* "<T, U>" (with `bound` after each name, e.g. ": Ord"), or "" for no params. */
static char *type_param_decl_text(TypeParamList *params, const char *bound) {
    if (params->count == 0) return fmt_str("%s", "");
    char *out = fmt_str("%s", "<");
    for (int i = 0; i < params->count; i++) {
        out = fmt_str("%s%s%s%s", out, i > 0 ? ", " : "", params->items[i].name, bound);
    }
    return fmt_str("%s>", out);
}

/* "Page" or "Page<T, U>" -- the declaration's own type as written in a signature. */
static char *decl_type_text(const char *name, TypeParamList *params) {
    if (params->count == 0) return fmt_str("%s", name);
    char *out = fmt_str("%s<", name);
    for (int i = 0; i < params->count; i++) {
        out = fmt_str("%s%s%s", out, i > 0 ? ", " : "", params->items[i].name);
    }
    return fmt_str("%s>", out);
}

/* The converter parameter for type parameter `T`: "t_to_json" / "t_from_json". */
static char *converter_param_name(const char *type_param, int is_to) {
    char *snake = pascal_to_snake(type_param);
    return fmt_str("%s_%s_json", snake, is_to ? "to" : "from");
}

/* ", t_to_json: fn(T) -> Json, u_to_json: fn(U) -> Json" appended to a parameter list. */
static char *converter_params_text(TypeParamList *params, int is_to) {
    char *out = fmt_str("%s", "");
    for (int i = 0; i < params->count; i++) {
        char *name = converter_param_name(params->items[i].name, is_to);
        if (is_to) out = fmt_str("%s, %s: fn(%s) -> Json", out, name, params->items[i].name);
        else out = fmt_str("%s, %s: fn(Json) -> Result<%s, string>", out, name, params->items[i].name);
    }
    return out;
}

/* The function value that converts one type argument of a generic field type
 * (`Page<Patient>`'s `Patient`): the enclosing declaration's own converter for a
 * type parameter, a `json` helper for a scalar, `<type>_to_json` for a plain
 * struct/enum. Anything else (an array, an Option) has no named converter to
 * pass. */
static char *converter_for_type_arg(Type arg, int is_to, int line, int col) {
    const char *dir = is_to ? "to" : "from";
    switch (arg.kind) {
        case TY_TYPEVAR: return converter_param_name(arg.type_var_name, is_to);
        case TY_INT: return fmt_str("int_%s_json", dir);
        case TY_FLOAT: return fmt_str("float_%s_json", dir);
        case TY_BIGINT: return fmt_str("bigint_%s_json", dir);
        case TY_DECIMAL: return fmt_str("decimal_%s_json", dir);
        case TY_BOOL: return fmt_str("bool_%s_json", dir);
        case TY_STRING: return fmt_str("string_%s_json", dir);
        case TY_STRUCT:
        case TY_ENUM: {
            if (arg.type_args.count == 0 && !is_option_type_here(arg)) {
                const char *name = arg.kind == TY_STRUCT ? arg.struct_name : arg.enum_name;
                char *snake = pascal_to_snake(name);
                return fmt_str("%s_%s_json", snake, dir);
            }
            break;
        }
        default: break;
    }
    ziman_error("syntax", line, col,
                "derive(Json) cannot build a converter for type argument '%s' -- use a scalar, a type parameter or a plain struct/enum there",
                type_name(arg));
    return NULL;
}

/* "<A, B>" from a concrete type's type arguments, for an explicit-type-argument call. */
static char *type_args_text(Type t) {
    char *out = fmt_str("%s", "<");
    for (int i = 0; i < t.type_args.count; i++) {
        out = fmt_str("%s%s%s", out, i > 0 ? ", " : "", type_name(t.type_args.items[i]));
    }
    return fmt_str("%s>", out);
}

/* A Map's key as a JSON object key: a string as is, an int through str(). Nothing else
 * has an obvious text form. */
static void check_json_map_key(Type key, int line, int col) {
    if (key.kind != TY_STRING && key.kind != TY_INT) {
        ziman_error("syntax", line, col, "derive(Json) supports Map keys of type string or int (they become JSON object keys), found '%s'",
                    type_name(key));
    }
}

/* Appends statements to `cb` that bind `dst_var` (a `val`, freshly
 * declared) to a `Json` value built from `src_expr` (an already-valid
 * Ziman expression of type `t`). Priority order, matching the design:
 * struct/enum types (other than Option) dispatch by naming convention
 * to their own <name>_to_json (hand-written or itself derived); scalars
 * map directly; Option<U>/[U] recurse into U. */
static void emit_to_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = int_to_json(%s)\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = float_to_json(%s)\n", dst_var, src_expr);
            return;
        case TY_BIGINT:
            cb_appendf(cb, "    val %s = bigint_to_json(%s)\n", dst_var, src_expr);
            return;
        case TY_DECIMAL:
            cb_appendf(cb, "    val %s = decimal_to_json(%s)\n", dst_var, src_expr);
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
            cb_appendf(cb, "    val %s = mutable_array_new<Json>()\n", ab);
            cb_appendf(cb, "    for %s in %s {\n", item, src_expr);
            emit_to_json(*t.elem, item, item_json, cb, tmp, line, col);
            cb_appendf(cb, "    push<Json>(%s, %s)\n", ab, item_json);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = Json.JArray(snapshot(%s))\n", dst_var, ab);
            return;
        }
        case TY_MAP: {
            check_json_map_key(*t.elem, line, col);
            char *k = gen_tmp(tmp, "key");
            char *v = gen_tmp(tmp, "entry");
            char *v_json = gen_tmp(tmp, "entry_json");
            char *acc = gen_tmp(tmp, "fields");
            cb_appendf(cb, "    val %s = mutable_map_new<string, Json>()\n", acc);
            cb_appendf(cb, "    for %s, %s in %s {\n", k, v, src_expr);
            emit_to_json(*t.elem_err, v, v_json, cb, tmp, line, col);
            cb_appendf(cb, "    set<string, Json>(%s, %s, %s)\n", acc, t.elem->kind == TY_INT ? fmt_str("str(%s)", k) : k, v_json);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = Json.JObject(snapshot(%s))\n", dst_var, acc);
            return;
        }
        case TY_SET: {
            char *item = gen_tmp(tmp, "item");
            char *item_json = gen_tmp(tmp, "item_json");
            char *ab = gen_tmp(tmp, "ab");
            cb_appendf(cb, "    val %s = mutable_array_new<Json>()\n", ab);
            cb_appendf(cb, "    for %s in %s {\n", item, src_expr);
            emit_to_json(*t.elem, item, item_json, cb, tmp, line, col);
            cb_appendf(cb, "    push<Json>(%s, %s)\n", ab, item_json);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = Json.JArray(snapshot(%s))\n", dst_var, ab);
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
                if (t.type_args.count > 0) {
                    /* A generic type: its own converters take one converter per type argument. */
                    char *convs = fmt_str("%s", "");
                    for (int i = 0; i < t.type_args.count; i++) {
                        convs = fmt_str("%s, %s", convs, converter_for_type_arg(t.type_args.items[i], 1, line, col));
                    }
                    cb_appendf(cb, "    val %s = %s_to_json%s(%s%s)\n", dst_var, fn, type_args_text(t), src_expr, convs);
                } else if (standard_flat_type_name(name) != NULL) {
                    cb_appendf(cb, "    val %s = to_json(%s)\n", dst_var, src_expr);
                } else {
                    cb_appendf(cb, "    val %s = %s_to_json(%s)\n", dst_var, fn, src_expr);
                }
                return;
            }
        case TY_TYPEVAR:
            cb_appendf(cb, "    val %s = %s(%s)\n", dst_var, converter_param_name(t.type_var_name, 1), src_expr);
            return;
        default:
            ziman_error("syntax", line, col, "derive(Json) does not support field type '%s' yet", type_name(t));
    }
}

/* Mirror of emit_to_json: appends statements binding `dst_var` (a
 * `val`) to a Ziman value of type `t`, decoded from `src_expr` (an
 * already-valid Ziman expression of type `Json`). Every branch either
 * binds `dst_var` or returns an `err(...)`/propagates via `try` -- never
 * falls through -- so the emitted code always leaves `dst_var` usable
 * afterward. Lenient by construction: nothing here ever inspects a
 * JObject's *other* keys, so unrecognized extra fields are silently
 * ignored, matching the design's lenient-deserialization decision. */
static void emit_from_json(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col, const char *field_ctx) {
    switch (t.kind) {
        case TY_INT:
            emit_try_call(cb, tmp, dst_var, fmt_str("int_from_json(%s)", src_expr), field_ctx);
            return;
        case TY_FLOAT:
            emit_try_call(cb, tmp, dst_var, fmt_str("float_from_json(%s)", src_expr), field_ctx);
            return;
        case TY_BIGINT:
            emit_try_call(cb, tmp, dst_var, fmt_str("bigint_from_json(%s)", src_expr), field_ctx);
            return;
        case TY_DECIMAL:
            emit_try_call(cb, tmp, dst_var, fmt_str("decimal_from_json(%s)", src_expr), field_ctx);
            return;
        case TY_BOOL:
            emit_type_check(cb, fmt_str("Json.is_JBool(%s)", src_expr), field_ctx, "expected a boolean");
            cb_appendf(cb, "    val %s = Json.unwrap_JBool(%s)\n", dst_var, src_expr);
            return;
        case TY_STRING:
            emit_type_check(cb, fmt_str("Json.is_JString(%s)", src_expr), field_ctx, "expected a string");
            cb_appendf(cb, "    val %s = Json.unwrap_JString(%s)\n", dst_var, src_expr);
            return;
        case TY_ARRAY: {
            char *ab = gen_tmp(tmp, "ab");
            char *item = gen_tmp(tmp, "item");
            char *item_val = gen_tmp(tmp, "item_val");
            char *elem_type_name = type_name(*t.elem);
            emit_type_check(cb, fmt_str("Json.is_JArray(%s)", src_expr), field_ctx, "expected an array");
            cb_appendf(cb, "    val %s = mutable_array_new<%s>()\n", ab, elem_type_name);
            cb_appendf(cb, "    for %s in Json.unwrap_JArray(%s) {\n", item, src_expr);
            emit_from_json(*t.elem, item, item_val, cb, tmp, line, col, NULL);
            cb_appendf(cb, "    push<%s>(%s, %s)\n", elem_type_name, ab, item_val);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = snapshot(%s)\n", dst_var, ab);
            return;
        }
        case TY_MAP: {
            check_json_map_key(*t.elem, line, col);
            char *k = gen_tmp(tmp, "key");
            char *v = gen_tmp(tmp, "raw");
            char *entry = gen_tmp(tmp, "entry");
            char *key_n = gen_tmp(tmp, "key_n");
            char *bld = gen_tmp(tmp, "builder");
            char *kt = type_name(*t.elem);
            char *vt = type_name(*t.elem_err);
            emit_type_check(cb, fmt_str("Json.is_JObject(%s)", src_expr), field_ctx, "expected an object");
            cb_appendf(cb, "    val %s = mutable_map_new<%s, %s>()\n", bld, kt, vt);
            cb_appendf(cb, "    for %s, %s in Json.unwrap_JObject(%s) {\n", k, v, src_expr);
            emit_from_json(*t.elem_err, v, entry, cb, tmp, line, col, NULL);
            if (t.elem->kind == TY_INT) {
                cb_appendf(cb, "    val %s = try parse_int(%s)\n", key_n, k);
                cb_appendf(cb, "    set<%s, %s>(%s, %s, %s)\n", kt, vt, bld, key_n, entry);
            } else {
                cb_appendf(cb, "    set<%s, %s>(%s, %s, %s)\n", kt, vt, bld, k, entry);
            }
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = snapshot(%s)\n", dst_var, bld);
            return;
        }
        case TY_SET: {
            char *item = gen_tmp(tmp, "item");
            char *item_val = gen_tmp(tmp, "item_val");
            char *bld = gen_tmp(tmp, "builder");
            char *et = type_name(*t.elem);
            emit_type_check(cb, fmt_str("Json.is_JArray(%s)", src_expr), field_ctx, "expected an array");
            cb_appendf(cb, "    val %s = mutable_set_new<%s>()\n", bld, et);
            cb_appendf(cb, "    for %s in Json.unwrap_JArray(%s) {\n", item, src_expr);
            emit_from_json(*t.elem, item, item_val, cb, tmp, line, col, NULL);
            /* `add`, not `mutable_set_add`: F199 made that the word in both set worlds and left
             * the prefixed one `internal`, which only mattered once a non-`pub` prelude name
             * stopped being reachable from outside the prelude (2026-10-22) -- generated code is
             * attributed to the user's file, so it is outside. Emitting the internal name failed
             * `map_json_derive` the moment that rule went in, which is the third time this
             * generator has been the blast radius of a prelude rename.
             *
             * The prefixed name is not merely `internal` now but gone: `add`'s builder member
             * became a `native fn` later the same day and took its job, so there is no second
             * spelling left to emit by mistake. */
            cb_appendf(cb, "    add<%s>(%s, %s)\n", et, bld, item_val);
            cb_appendf(cb, "    }\n");
            cb_appendf(cb, "    val %s = snapshot(%s)\n", dst_var, bld);
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
                emit_from_json(elem, src_expr, inner, cb, tmp, line, col, NULL);
                cb_appendf(cb, "    %s = Option<%s>.Some(%s)\n", dst_var, elem_type_name, inner);
                cb_appendf(cb, "    }\n");
                return;
            } else {
                const char *name = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
                char *fn = pascal_to_snake(name);
                if (t.type_args.count > 0) {
                    char *convs = fmt_str("%s", "");
                    for (int i = 0; i < t.type_args.count; i++) {
                        convs = fmt_str("%s, %s", convs, converter_for_type_arg(t.type_args.items[i], 0, line, col));
                    }
                    emit_try_call(cb, tmp, dst_var, fmt_str("%s_from_json%s(%s%s)", fn, type_args_text(t), src_expr, convs), field_ctx);
                } else if (standard_flat_type_name(name) != NULL) {
                    emit_try_call(cb, tmp, dst_var, fmt_str("to_%s(%s)", standard_flat_type_name(name), src_expr), field_ctx);
                } else {
                    emit_try_call(cb, tmp, dst_var, fmt_str("%s_from_json(%s)", fn, src_expr), field_ctx);
                }
                return;
            }
        case TY_TYPEVAR:
            emit_try_call(cb, tmp, dst_var, fmt_str("%s(%s)", converter_param_name(t.type_var_name, 0), src_expr), field_ctx);
            return;
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
    cb_appendf(&cb, "pub fn %s_to_json%s(v: %s%s) -> Json {\n", fn_name, type_param_decl_text(&sd->type_params, ""),
               decl_type_text(sd->name, &sd->type_params), converter_params_text(&sd->type_params, 1));

    int tmp = 0;
    char **dst_vars = ziman_xmalloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    char **camel_keys = ziman_xmalloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *dst = fmt_str("field_%s_json", f->name);
        char *src = fmt_str("v.%s", f->name);
        emit_to_json(f->type, src, dst, &cb, &tmp, f->line, f->col);
        dst_vars[i] = dst;
        camel_keys[i] = snake_to_camel(f->name);
    }

    cb_appendf(&cb, "    return Json.JObject(Map<string, Json> {\n");
    for (int i = 0; i < sd->fields.count; i++) {
        cb_appendf(&cb, "        \"%s\": %s%s\n", camel_keys[i], dst_vars[i], i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    })\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

static char *generate_struct_from_json(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_from_json%s(j: Json%s) -> Result<%s, string> {\n", fn_name, type_param_decl_text(&sd->type_params, ""),
               converter_params_text(&sd->type_params, 0), decl_type_text(sd->name, &sd->type_params));
    cb_appendf(&cb, "    if !Json.is_JObject(j) { return err(\"expected a JSON object for %s\") }\n", sd->name);

    int tmp = 0;
    char **field_vars = ziman_xmalloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
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
            cb_appendf(&cb, "    val %s = json.get(j, \"%s\")\n", found, camel);
            cb_appendf(&cb, "    var %s = Option<%s>.None\n", dst, elem_type_name);
            cb_appendf(&cb, "    if Option.is_Some(%s) {\n", found);
            cb_appendf(&cb, "    val %s = Option.unwrap_Some(%s)\n", raw, found);
            cb_appendf(&cb, "    if !Json.is_JNull(%s) {\n", raw);
            emit_from_json(elem, raw, inner, &cb, &tmp, f->line, f->col, camel);
            cb_appendf(&cb, "    %s = Option<%s>.Some(%s)\n", dst, elem_type_name, inner);
            cb_appendf(&cb, "    }\n");
            cb_appendf(&cb, "    }\n");
        } else {
            char *raw = fmt_str("field_%s_raw", f->name);
            cb_appendf(&cb, "    val %s = try json.require(j, \"%s\")\n", raw, camel);
            emit_from_json(f->type, raw, dst, &cb, &tmp, f->line, f->col, camel);
        }
        field_vars[i] = dst;
    }

    cb_appendf(&cb, "    return %s {\n", decl_type_text(sd->name, &sd->type_params));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "        %s: %s%s\n", f->name, field_vars[i], i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");

    /* ---- the accumulating companion ----
     *
     * `<type>_from_json` stops at the first bad field, so a request with three wrong types reports one of
     * them. `<type>_from_json_all` reports all of them, as `Result<T, [string]>`.
     *
     * A companion rather than a change to the decoder above: that one keeps its signature, so nothing
     * breaks and nothing it already does gets slower, and a caller opts in by name.
     *
     * Each field is decoded inside a *lambda*, which is what makes this cheap to generate. The emitters
     * above end a failing field with `return err(...)`, and inside a lambda that returns from the lambda --
     * so the existing per-field emission is reused verbatim, including its "skip the rest of this field"
     * behaviour, with no second recording mode to keep in step with it. `field_ctx` is passed as NULL there
     * and the key is prefixed once here, or the message would name the field twice.
     *
     * The cost is one closure per field, paid only by callers of this function. Known limit: a nested
     * struct contributes one message, its own first, because its decoder is the single-error one. */
    cb_appendf(&cb, "pub fn %s_from_json_all%s(j: Json%s) -> Result<%s, [string]> {\n", fn_name,
               type_param_decl_text(&sd->type_params, ""), converter_params_text(&sd->type_params, 0),
               decl_type_text(sd->name, &sd->type_params));
    cb_appendf(&cb, "    if !Json.is_JObject(j) { return err([\"expected a JSON object for %s\"]) }\n", sd->name);
    cb_appendf(&cb, "    val problems = mutable_array_new<string>()\n");
    tmp = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *camel = snake_to_camel(f->name);
        char *dst = fmt_str("field_%s", f->name);
        char *raw = fmt_str("field_%s_raw", f->name);
        cb_appendf(&cb, "    val get_%s = fn() -> Result<%s, string> {\n", f->name, type_name(f->type));
        if (is_option_type_here(f->type)) {
            Type elem = f->type.type_args.items[0];
            char *elem_type_name = type_name(elem);
            char *found = fmt_str("field_%s_found", f->name);
            char *inner = fmt_str("field_%s_inner", f->name);
            cb_appendf(&cb, "    val %s = json.get(j, \"%s\")\n", found, camel);
            cb_appendf(&cb, "    var %s = Option<%s>.None\n", dst, elem_type_name);
            cb_appendf(&cb, "    if Option.is_Some(%s) {\n", found);
            cb_appendf(&cb, "    val %s = Option.unwrap_Some(%s)\n", raw, found);
            cb_appendf(&cb, "    if !Json.is_JNull(%s) {\n", raw);
            emit_from_json(elem, raw, inner, &cb, &tmp, f->line, f->col, NULL);
            cb_appendf(&cb, "    %s = Option<%s>.Some(%s)\n", dst, elem_type_name, inner);
            cb_appendf(&cb, "    }\n");
            cb_appendf(&cb, "    }\n");
        } else {
            cb_appendf(&cb, "    val %s = try json.require(j, \"%s\")\n", raw, camel);
            emit_from_json(f->type, raw, dst, &cb, &tmp, f->line, f->col, NULL);
        }
        cb_appendf(&cb, "    return ok(%s)\n", dst);
        cb_appendf(&cb, "    }\n");
        cb_appendf(&cb, "    val got_%s = get_%s()\n", f->name, f->name);
        cb_appendf(&cb, "    if Err(problem_%s) = got_%s { push(problems, concat(\"%s: \", problem_%s)) }\n",
                   f->name, f->name, camel, f->name);
    }
    cb_appendf(&cb, "    if len(problems) > 0 { return err(snapshot(problems)) }\n");
    cb_appendf(&cb, "    return ok(%s {\n", decl_type_text(sd->name, &sd->type_params));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "        %s: unwrap(got_%s)%s\n", f->name, f->name, i + 1 < sd->fields.count ? "," : "");
    }
    cb_appendf(&cb, "    })\n");
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
    cb_appendf(&cb, "pub fn %s_to_json%s(v: %s%s) -> Json {\n", fn_name, type_param_decl_text(&ed->type_params, ""),
               decl_type_text(ed->name, &ed->type_params), converter_params_text(&ed->type_params, 1));

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
            cb_appendf(&cb, "        return Json.JObject(Map<string, Json> { \"type\": Json.JString(\"%s\") })\n", variant->name);
        } else {
            char *unwrap_expr = fmt_str("%s.unwrap_%s(v)", ed->name, variant->name);
            char *dst = fmt_str("payload_json_%d", i);
            emit_to_json(variant->payload_type, unwrap_expr, dst, &cb, &tmp, variant->line, variant->col);
            cb_appendf(&cb, "        return Json.JObject(Map<string, Json> {\n");
            cb_appendf(&cb, "            \"type\": Json.JString(\"%s\"),\n", variant->name);
            cb_appendf(&cb, "            \"value\": %s\n", dst);
            cb_appendf(&cb, "        })\n");
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
    cb_appendf(&cb, "pub fn %s_from_json%s(j: Json%s) -> Result<%s, string> {\n", fn_name, type_param_decl_text(&ed->type_params, ""),
               converter_params_text(&ed->type_params, 0), decl_type_text(ed->name, &ed->type_params));
    cb_appendf(&cb, "    if !Json.is_JObject(j) { return err(\"expected a JSON object for %s\") }\n", ed->name);
    cb_appendf(&cb, "    val __derive_tag = try json.require_string(j, \"type\")\n");

    int tmp = 0;
    int n = ed->variants.count;
    for (int i = 0; i < n; i++) {
        EnumVariant *variant = &ed->variants.items[i];
        const char *kw = i == 0 ? "if" : "} else if";
        cb_appendf(&cb, "    %s __derive_tag == \"%s\" {\n", kw, variant->name);
        if (!variant->has_payload) {
            if (ed->type_params.count > 0) cb_appendf(&cb, "        return %s.%s\n", decl_type_text(ed->name, &ed->type_params), variant->name);
            else cb_appendf(&cb, "        return %s.%s\n", ed->name, variant->name);
        } else {
            cb_appendf(&cb, "        val __derive_payload_raw = try json.require(j, \"value\")\n");
            char *dst = fmt_str("payload_%d", i);
            emit_from_json(variant->payload_type, "__derive_payload_raw", dst, &cb, &tmp, variant->line, variant->col, NULL);
            cb_appendf(&cb, "        return %s.%s(%s)\n", decl_type_text(ed->name, &ed->type_params), variant->name, dst);
        }
    }
    cb_appendf(&cb, "    } else {\n");
    cb_appendf(&cb, "        return err(concat(\"unknown variant tag for %s: \", __derive_tag))\n", ed->name);
    cb_appendf(&cb, "    }\n");
    cb_appendf(&cb, "}\n\n");
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Entity derive: <name>_columns, the struct's field names in order.
 *
 * It emits only what the compiler already knows for certain. In
 * particular it does **not** guess the table name: of the sample
 * application's eighteen entities, three break every pluralisation rule
 * (`FamilyHistory` is stored in `family_history`, `MedicalHistory` in
 * `medical_histories`, `UserNotificationPreference` in
 * `user_notification_prefs`), and a wrong guess there would generate SQL
 * against a table that does not exist. The table name stays a hand-written
 * `val` beside the struct, which is one line and cannot drift silently
 * because the database refuses it.
 *
 * What *was* drifting silently is this list. Every entity carried its own
 * `pub val columns = ["id", "user_id", ...]` in `to_params` order, written
 * by hand beside the struct that already says it -- and `patient.zm`'s own
 * comment records the profile growing "from 8 columns to 24 in one commit",
 * while `service/crud.zm`'s E9 note is about a hand-numbered INSERT passing
 * 24 parameters to 8 holes. The order here is field declaration order, which
 * is exactly the order `<name>_to_params` pushes its values in, so the two
 * cannot disagree.
 * --------------------------------------------------------------------- */

/* `table` is the string the target was written with, or NULL for a bare `#[derive(Entity)]`.
 *
 * **With it, three hand-written lines per entity become none.** The North Star application wrote
 * the same table name twice -- once in a `pub val table` its services import and once in the
 * `eql.entity(..)` call beside it -- and the entity's own name a third time, which is the struct's
 * name spelled again. All three are mechanical given the table, so the table is the only thing
 * worth writing, and `#[derive(Entity("patients"))]` is where it goes.
 *
 * Without it the target still writes the column list alone, which is what it did before and what
 * a struct whose table is decided elsewhere still wants.
 *
 * **The entity name is the struct's name**, not derived from the table: `eql` matches what a query
 * writes (`FROM Patient p`) against it, and a query names the type a reader has in hand. Taking it
 * from the table would make `FROM patients p` the spelling, which is SQL leaking back through the
 * layer whose job is to hide it. */
static char *generate_struct_entity(StructDecl *sd, const char *table) {
    CodeBuf cb;
    cb_init(&cb);
    char *val_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub val %s_columns: [string] = [", val_name);
    for (int i = 0; i < sd->fields.count; i++) {
        cb_appendf(&cb, "%s\"%s\"", i > 0 ? ", " : "", sd->fields.items[i].name);
    }
    cb_appendf(&cb, "]\n\n");
    if (table != NULL) {
        cb_appendf(&cb, "pub val table: string = \"%s\"\n\n", table);
        /* The `#[pk]` fields, in field order. `keyed_entity` when there are any and `entity` when
         * there are none, so an entity that declares no key says so in the call rather than
         * carrying an empty list that reads like an oversight. */
        int keys = 0;
        for (int i = 0; i < sd->fields.count; i++) {
            if (sd->fields.items[i].is_pk) keys++;
        }
        if (keys == 0) {
            cb_appendf(&cb, "pub val %s_meta: eql.Entity = eql.entity(\"%s\", \"%s\", %s_columns)\n\n",
                       val_name, sd->name, table, val_name);
        } else {
            cb_appendf(&cb, "pub val %s_meta: eql.Entity = eql.keyed_entity(\"%s\", \"%s\", %s_columns, [",
                       val_name, sd->name, table, val_name);
            int written = 0;
            for (int i = 0; i < sd->fields.count; i++) {
                if (!sd->fields.items[i].is_pk) continue;
                cb_appendf(&cb, "%s\"%s\"", written++ > 0 ? ", " : "", sd->fields.items[i].name);
            }
            cb_appendf(&cb, "])\n\n");
        }
    }
    return cb.data;
}

/* ---------------------------------------------------------------------
 * Row derive: <name>_from_row / <name>_to_params, for mapping a struct to
 * and from one SQL result row (lib/db/db.zm's Row/SqlValue).
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

/* A struct or enum name with its module qualifier stripped. A declaration read out of an
 * already-loaded module carries the loader's self-qualified form (`uuid::Uuid`), which no generated
 * text may contain: `::` is the loader's internal separator and is not source syntax. Only `::` is
 * stripped -- a dotted `uuid.Uuid` is how a program writes a qualified type itself, and is left
 * alone. Returns `name` unchanged for everything a derive sees before loading, which is the usual
 * case and the reason this is a no-op for the Row and Json derives. */
static const char *bare_name_of(const char *name) {
    if (name == NULL) return NULL;
    const char *sep = strstr(name, "::");
    return sep != NULL ? sep + 2 : name;
}

static void emit_to_sql(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = SqlValue.Int(%s)\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = SqlValue.Float(%s)\n", dst_var, src_expr);
            return;
        case TY_DECIMAL:
            cb_appendf(cb, "    val %s = SqlValue.Text(str(%s))\n", dst_var, src_expr);
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
                const char *name = bare_name_of(t.kind == TY_STRUCT ? t.struct_name : t.enum_name);
                char *fn = pascal_to_snake(name);
                if (standard_flat_type_name(name) != NULL) {
                    cb_appendf(cb, "    val %s = to_sql(%s)\n", dst_var, src_expr);
                } else {
                    cb_appendf(cb, "    val %s = %s_to_sql(%s)\n", dst_var, fn, src_expr);
                }
                return;
            }
        default:
            ziman_error("syntax", line, col, "derive(Row) does not support field type '%s'", type_name(t));
    }
}

static void emit_from_sql(Type t, const char *src_expr, const char *dst_var, CodeBuf *cb, int *tmp, int line, int col) {
    switch (t.kind) {
        case TY_INT:
            cb_appendf(cb, "    val %s = try sql_as_int(%s)\n", dst_var, src_expr);
            return;
        case TY_FLOAT:
            cb_appendf(cb, "    val %s = try sql_as_float(%s)\n", dst_var, src_expr);
            return;
        case TY_DECIMAL:
            cb_appendf(cb, "    val %s = try sql_as_decimal(%s)\n", dst_var, src_expr);
            return;
        case TY_BOOL:
            cb_appendf(cb, "    val %s = try sql_as_bool(%s)\n", dst_var, src_expr);
            return;
        case TY_STRING:
            cb_appendf(cb, "    val %s = try sql_as_text(%s)\n", dst_var, src_expr);
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
                if (standard_flat_type_name(name) != NULL) {
                    cb_appendf(cb, "    val %s = try to_%s(%s)\n", dst_var, standard_flat_type_name(name), src_expr);
                } else {
                    cb_appendf(cb, "    val %s = try %s_from_sql(%s)\n", dst_var, fn, src_expr);
                }
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
    char **field_vars = ziman_xmalloc(sizeof(char *) * (size_t)(sd->fields.count > 0 ? sd->fields.count : 1));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *raw = fmt_str("field_%s_raw", f->name);
        char *dst = fmt_str("field_%s", f->name);
        cb_appendf(&cb, "    val %s = try row_get(row, \"%s\")\n", raw, f->name);
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
    cb_appendf(&cb, "    val __derive_params = mutable_array_new<SqlValue>()\n");
    int tmp = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        char *src = fmt_str("v.%s", f->name);
        char *dst = fmt_str("field_%s_sql", f->name);
        emit_to_sql(f->type, src, dst, &cb, &tmp, f->line, f->col);
        cb_appendf(&cb, "    push<SqlValue>(__derive_params, %s)\n", dst);
    }
    cb_appendf(&cb, "    return snapshot(__derive_params)\n");
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
    char *out = ziman_xmalloc(n + 1);
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
            /* **`get_%s`, not `entry_%s`** (2026-10-24): `properties` folded the two families
             * into one name per type, told apart by whether an `Entry` or a `Properties` plus a
             * key is given -- which is exactly what the two emit sites here already pass. */
            cb_appendf(&cb, "        val value_%s = try properties.get_%s(Option.unwrap_Some(found_%s))\n", f->name, g, f->name);
            cb_appendf(&cb, "        field_%s = Option<%s>.Some(value_%s)\n    }\n", f->name, g, f->name);
        } else {
            const char *g = properties_getter_suffix(f->type, f->line, f->col);
            cb_appendf(&cb, "    val field_%s = try properties.get_%s(props, properties.key(prefix, \"%s\"))\n", f->name, g, kebab);
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
    /* A derive may emit a module-level `val` as well as functions -- `Entity`'s column list is a
     * value, computed once, not a function rebuilding its array per call. Only functions were
     * carried over before, so such a `val` parsed cleanly and then vanished, and the first thing
     * that referenced it failed with "use of undefined variable".
     *
     * **Prepended, not appended.** Module-level vals are initialised in list order, so a
     * hand-written `pub val columns = patient_columns` beside the struct would be evaluated
     * before a derived val pushed onto the end -- and fail, with an error pointing at whatever
     * file was being checked. A derived val's initialiser is a literal, referencing no other val,
     * so putting them all first is safe and makes the derived name available to every hand-written
     * val whatever order the file is in. */
    if (snippet->vals.count > 0) {
        GlobalValList combined;
        memset(&combined, 0, sizeof combined);
        for (int i = 0; i < snippet->vals.count; i++) {
            global_val_list_push(&combined, snippet->vals.items[i]);
        }
        for (int i = 0; i < module->vals.count; i++) {
            global_val_list_push(&combined, module->vals.items[i]);
        }
        module->vals = combined;
    }
    /* `tokens`/`snippet`/`source` are deliberately never freed -- AST
     * nodes in this codebase are never freed at all (see ast.h), and a
     * spliced-in FunctionDecl's own Tokens were already fully copied out
     * by dup_token_text at parse time, so nothing depends on `source`
     * staying alive past this point either; leaving it allocated is
     * simplest and matches the rest of the codebase's policy. */
}

/* ---------------------------------------------------------------------
 * Ord derive: <name>_compare(a, b) -> int. A struct compares field by field in
 * declaration order (the first difference decides); an enum compares by
 * variant position first, then by payload when both are the same variant.
 * Each field/payload is compared with the `compare` builtin, so it must be an
 * Ord type itself -- the checker reports one that isn't.
 * --------------------------------------------------------------------- */

static char *generate_struct_ord(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(sd->name);
    cb_appendf(&cb, "pub fn %s_compare%s(a: %s, b: %s) -> int {\n", fn_name, type_param_decl_text(&sd->type_params, ": Ord"),
               decl_type_text(sd->name, &sd->type_params), decl_type_text(sd->name, &sd->type_params));
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        cb_appendf(&cb, "    val c_%s = compare(a.%s, b.%s)\n", f->name, f->name, f->name);
        cb_appendf(&cb, "    if c_%s != 0 {\n        return c_%s\n    }\n", f->name, f->name);
    }
    cb_appendf(&cb, "    return 0\n}\n\n");
    return cb.data;
}

static char *generate_enum_ord(EnumDecl *ed) {
    CodeBuf cb;
    cb_init(&cb);
    char *fn_name = pascal_to_snake(ed->name);
    cb_appendf(&cb, "pub fn %s_compare%s(a: %s, b: %s) -> int {\n", fn_name, type_param_decl_text(&ed->type_params, ": Ord"),
               decl_type_text(ed->name, &ed->type_params), decl_type_text(ed->name, &ed->type_params));
    cb_appendf(&cb, "    var ra = 0\n    var rb = 0\n");
    for (int i = 0; i + 1 < ed->variants.count; i++) {
        cb_appendf(&cb, "    if %s.is_%s(a) {\n        ra = %d\n    }\n", ed->name, ed->variants.items[i].name, i);
        cb_appendf(&cb, "    if %s.is_%s(b) {\n        rb = %d\n    }\n", ed->name, ed->variants.items[i].name, i);
    }
    cb_appendf(&cb, "    if ra == 0 && !%s.is_%s(a) {\n        ra = %d\n    }\n", ed->name, ed->variants.items[0].name, ed->variants.count - 1);
    cb_appendf(&cb, "    if rb == 0 && !%s.is_%s(b) {\n        rb = %d\n    }\n", ed->name, ed->variants.items[0].name, ed->variants.count - 1);
    cb_appendf(&cb, "    if ra < rb {\n        return -1\n    }\n    if ra > rb {\n        return 1\n    }\n");
    for (int i = 0; i < ed->variants.count; i++) {
        if (!ed->variants.items[i].has_payload) continue;
        cb_appendf(&cb, "    if %s.is_%s(a) {\n        return compare(%s.unwrap_%s(a), %s.unwrap_%s(b))\n    }\n",
                   ed->name, ed->variants.items[i].name, ed->name, ed->variants.items[i].name, ed->name, ed->variants.items[i].name);
    }
    cb_appendf(&cb, "    return 0\n}\n\n");
    return cb.data;
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

/* The standard types a derive can convert without the consuming module importing their converters: the module each
 * lives in and the snake_case stem of its `<stem>_to_json` / `_from_json` / `_to_sql` / `_from_sql` functions. */
static const struct { const char *type; const char *module; const char *stem; } STANDARD_TYPES[] = {
    {"Uuid", "uuid", "uuid"},
    {"Instant", "time", "instant"},
    {"LocalDate", "time", "local_date"},
    {"LocalTime", "time", "local_time"},
    {"LocalDateTime", "time", "local_date_time"},
    {"Duration", "time", "duration"},
    {"ZoneOffset", "time", "zone_offset"},
    {"ZoneId", "time", "zone_id"},
};

static void ensure_named_import(Module *module, const char *path, const char *name, int line, int col);

/* Imports the converters of every standard type found inside `t` (through arrays, Options, maps and type arguments),
 * so a struct with an `Instant` or `Uuid` field derives without the file listing `use time.instant_to_sql` and the
 * rest. A name the module already binds (its own function, or an import it wrote) is left alone. */
/* Whether `t` is a handle -- a `#[thread_bound]` struct. Two tests, and both are needed: derive
 * expansion runs *before* the prelude's declarations are merged into the module, so looking up
 * `MutableArray` here finds nothing (a user's own `#[thread_bound]` struct is already there and
 * does). The named list covers the built-ins; the lookup covers everything written in Ziman,
 * including a handle someone defines themselves. */
static int derive_type_is_handle(Module *module, Type t) {
    if (t.kind != TY_STRUCT || t.struct_name == NULL) return 0;
    const char *dot = strrchr(t.struct_name, '.');
    const char *bare = dot != NULL ? dot + 1 : t.struct_name;
    static const char *const BUILT_IN_HANDLES[] = {"MutableArray", "MutableMap", "MutableSet", "MutableString",
                                                   "Mutable",      "Iter",       NULL};
    for (size_t i = 0; BUILT_IN_HANDLES[i] != NULL; i++) {
        if (strcmp(bare, BUILT_IN_HANDLES[i]) == 0) return 1;
    }
    for (int i = 0; i < module->structs.count; i++) {
        if (strcmp(module->structs.items[i]->name, bare) == 0) return module->structs.items[i]->thread_bound;
    }
    return 0;
}

/* Serialising a handle is meaningless: whatever is written cannot decode back to the same mutable
 * object, because the thing that made it mutable is an index into a native table belonging to one
 * thread. Refused *by name* rather than left to fail incidentally -- which is what used to happen,
 * with a message that read "call to undefined function 'mutable_array_to_json' ... or write
 * 'mutable_array_to_json' by hand and import it with 'use'", i.e. an invitation to do the one thing
 * that cannot work. Recursive, so a handle inside an Option, an array or a type argument is caught
 * where a field-level check alone would miss it. */
static void refuse_handle_field(Module *module, const char *target, Type t, int line, int col) {
    if (derive_type_is_handle(module, t)) {
        ziman_error("syntax", line, col,
                    "derive(%s) cannot convert '%s': it is a thread-bound handle, and nothing written out could "
                    "decode back to the same mutable object. Hand over the finished value and convert that instead "
                    "-- snapshot(x) for a mutable collection, get(c) for a Mutable",
                    target, type_name(t));
    }
    if (t.elem != NULL) refuse_handle_field(module, target, *t.elem, line, col);
    if (t.elem_err != NULL) refuse_handle_field(module, target, *t.elem_err, line, col);
    for (int i = 0; i < t.type_args.count; i++) refuse_handle_field(module, target, t.type_args.items[i], line, col);
}

static void import_field_converters(Module *module, const char *suffix, Type t, int line, int col) {
    if (t.kind == TY_STRUCT || t.kind == TY_ENUM) {
        /* `bare_name_of` first, for the fourth place the loader's `::` leaked: a field type read
         * out of an already-loaded module is `uuid::Uuid`, which matched no STANDARD_TYPES entry,
         * so `#![repository(E)]` over an entity with a `Uuid` key generated `uuid_to_sql(id)` and
         * imported nothing. It went unseen because the one repository using the annotation imported
         * `uuid_to_sql` by hand for its own raw SQL; the gap appeared the moment that SQL went
         * away. The Row and Json derives never saw it -- they read a struct in their own module,
         * whose field types are not self-qualified yet. */
        const char *full = bare_name_of(t.kind == TY_STRUCT ? t.struct_name : t.enum_name);
        if (full == NULL) return;
        const char *dot = strrchr(full, '.');
        const char *bare = dot != NULL ? dot + 1 : full;
        for (size_t i = 0; i < sizeof STANDARD_TYPES / sizeof STANDARD_TYPES[0]; i++) {
            if (strcmp(bare, STANDARD_TYPES[i].type) != 0) continue;
            /* The names must be exactly the ones the emitters write -- see `standard_flat_type_name`
             * for which types are spelled short and why. Getting these two out of step leaves a
             * generated call with no import, reported against the deriving declaration. */
            const char *flat = standard_flat_type_name(bare);
            char *to = flat != NULL ? fmt_str("to_%s", suffix) : fmt_str("%s_to_%s", STANDARD_TYPES[i].stem, suffix);
            char *from = flat != NULL ? fmt_str("to_%s", flat) : fmt_str("%s_from_%s", STANDARD_TYPES[i].stem, suffix);
            ensure_named_import(module, STANDARD_TYPES[i].module, to, line, col);
            ensure_named_import(module, STANDARD_TYPES[i].module, from, line, col);
        }
    }
    if (t.elem != NULL) import_field_converters(module, suffix, *t.elem, line, col);
    if (t.elem_err != NULL) import_field_converters(module, suffix, *t.elem_err, line, col);
    for (int i = 0; i < t.type_args.count; i++) import_field_converters(module, suffix, t.type_args.items[i], line, col);
}

static void import_for_derive(Module *module, const char *target, int line, int col) {
    if (strcmp(target, "Json") == 0) {
        /* The scalar converters keep their `<type>_to_json` names -- that is the derive naming
         * convention, looked up by name, and `string_from_json` could not become `to_string`
         * without changing it for every derived type (see the 2026-10-24 time entry). They are
         * imported by name as before.
         *
         * The three tree helpers lost their prefix with the rest of `json`'s vocabulary, and
         * `get`, `require` and `require_string` are names the prelude also has. They are reached
         * **qualified** for that reason: a per-name import replaces the bare name for the whole
         * file, so `use json.get` in a file that derives would have shadowed the prelude's `get`
         * there -- silently, and without the author asking for it. A module import cannot. */
        static const char *const names[] = {"Json",
                                            "int_to_json", "int_from_json", "float_to_json", "float_from_json", "bigint_to_json", "bigint_from_json", "decimal_to_json", "decimal_from_json",
                                            "bool_to_json", "bool_from_json", "string_to_json", "string_from_json"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) ensure_named_import(module, "json", names[i], line, col);
        ensure_module_import(module, "json", line, col);
    } else if (strcmp(target, "Row") == 0) {
        static const char *const names[] = {"SqlValue", "DbError", "Row", "row_get", "sql_as_text", "sql_as_int", "sql_as_bool", "sql_as_float", "sql_as_decimal"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) ensure_named_import(module, "db", names[i], line, col);
    } else if (strcmp(target, "Properties") == 0) {
        ensure_module_import(module, "properties", line, col);
    }
}

/* ---------------------------------------------------------------------
 * `#![repository(Entity)]`: the CRUD every repository writes the same way.
 *
 * The functions below are the ones whose body is decided entirely by the
 * entity -- its table, its column list and the `#[pk]` fields that address
 * one of its rows. A repository's *domain* queries are not here and are not
 * generated: "this patient's vital signs, newest first, page 3" is a fact
 * about the application, not about the entity, and a generator that tried to
 * guess it from a function name would be a naming convention pretending to
 * be a feature.
 *
 * **A name the module already defines wins, and nothing is emitted for it.**
 * That rule is what makes the attribute safe to add to a repository that has
 * already been written: `repository/vital_sign.zm` scopes its `update` and
 * `delete` by the patient as well as the id -- the defence that makes another
 * patient's row "not found" rather than moved -- and a generated keyed
 * version replacing it would silently remove a guarantee. The cost is that
 * the file does not say which of its functions are generated. `--emit-derived`
 * prints them, and names each one it skipped for a hand-written version;
 * `--api` on the module names the attribute, since reading one file in
 * isolation cannot reach the entity and so cannot list the signatures.
 * --------------------------------------------------------------------- */

/* The type as the generated signature must spell it: see `bare_name_of`. The import ensured beside
 * it is what puts that bare name in scope. */
static const char *bare_type_text(Type t) {
    const char *name = NULL;
    if (t.kind == TY_STRUCT) name = t.struct_name;
    if (t.kind == TY_ENUM) name = t.enum_name;
    if (name == NULL) return type_name(t);
    return bare_name_of(name);
}

static int module_defines_function(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *f = module->functions.items[i];
        if (f->name != NULL && strcmp(f->name, name) == 0) return 1;
    }
    return 0;
}

/* Appends `fn` to `cb` unless the module writes its own, and says which it skipped when asked. */
static void emit_unless_written(Module *module, CodeBuf *cb, const char *name, char *text) {
    if (module_defines_function(module, name)) {
        if (g_emit_derived) printf("// repository: '%s' is written by hand, so none is generated\n", name);
        return;
    }
    cb_appendf(cb, "%s", text);
}

void generate_repository_functions(Module *module, StructDecl *sd, const char *entity_module) {
    /* The name as the attribute wrote it, not `sd->name`: a struct from an already-loaded module
     * carries its self-qualified name (`entity/vital_sign::VitalSign`), and the generated text is
     * source, where the bare name is what resolves. */
    const char *entity = module->repository_entity;
    char *snake = pascal_to_snake(entity);
    int line = module->repository_line;
    int col = module->repository_col;

    int key_count = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        if (sd->fields.items[i].is_pk) key_count++;
    }
    if (key_count == 0) {
        ziman_error("syntax", line, col,
                    "#![repository(%s)] needs '%s' to mark at least one field #[pk] -- a repository's "
                    "find, update and delete each address one row, and without a key there is nothing "
                    "that addresses one",
                    entity, entity);
        return;
    }
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        if (f->is_pk && is_option_type_here(f->type)) {
            ziman_error("syntax", line, col,
                        "#![repository(%s)] cannot use '%s' as a key column: it is an Option, and a key "
                        "that may be absent does not address a row",
                        entity, f->name);
            return;
        }
    }

    /* Everything the generated bodies name. A file that wrote only the attribute has no reason to
     * have imported any of it, which is the point of the attribute. */
    ensure_module_import(module, "eql", line, col);
    ensure_module_import(module, "datasource", line, col);
    ensure_named_import(module, "db", "DbError", line, col);
    ensure_named_import(module, "db", "SqlValue", line, col);
    /* `NULL` means the entity is declared in this same file, where its own names are already in
     * scope and importing them from itself would be a cycle. */
    if (entity_module != NULL) {
        ensure_named_import(module, entity_module, entity, line, col);
        ensure_named_import(module, entity_module, fmt_str("%s_meta", snake), line, col);
        ensure_named_import(module, entity_module, fmt_str("%s_to_params", snake), line, col);
        ensure_named_import(module, entity_module, fmt_str("%s_from_row", snake), line, col);
    }
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        if (!f->is_pk) continue;
        import_field_converters(module, "sql", f->type, line, col);
        /* The key's own type has to be nameable in the signature, which is a different import from
         * its converter: `delete(conn, id: Uuid)` needs `Uuid` as well as `to_sql`. */
        if (f->type.kind == TY_STRUCT || f->type.kind == TY_ENUM) {
            const char *tn = f->type.kind == TY_STRUCT ? f->type.struct_name : f->type.enum_name;
            if (tn != NULL) {
                const char *bare = bare_type_text(f->type);
                for (size_t s = 0; s < sizeof STANDARD_TYPES / sizeof STANDARD_TYPES[0]; s++) {
                    if (strcmp(bare, STANDARD_TYPES[s].type) == 0) {
                        ensure_named_import(module, STANDARD_TYPES[s].module, STANDARD_TYPES[s].type, line, col);
                    }
                }
            }
        }
    }

    /* `, id: Uuid` for each key field, and the block that turns them into a `[SqlValue]`. The
     * conversion is `emit_to_sql`, the same one `<name>_to_params` is built from, so a key's value
     * reaches SQL by exactly the route its column does. */
    CodeBuf params;
    cb_init(&params);
    CodeBuf keys;
    cb_init(&keys);
    CodeBuf key_array;
    cb_init(&key_array);
    int tmp = 0;
    int written = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        if (!f->is_pk) continue;
        cb_appendf(&params, ", %s: %s", f->name, bare_type_text(f->type));
        char *dst = gen_tmp(&tmp, "key");
        emit_to_sql(f->type, f->name, dst, &keys, &tmp, line, col);
        cb_appendf(&key_array, "%s%s", written++ > 0 ? ", " : "", dst);
    }
    char *key_params = params.data;
    char *key_block = fmt_str("%s    val keys: [SqlValue] = [%s]\n", keys.data, key_array.data);

    CodeBuf cb;
    cb_init(&cb);
    emit_unless_written(module, &cb, "insert", fmt_str(
        "pub fn insert(conn: datasource.Connection, a: %s) -> Result<%s, DbError> {\n"
        "    return eql.insert_one<%s>(conn, %s_meta, %s_to_params(a), %s_from_row)\n"
        "}\n\n",
        entity, entity, entity, snake, snake, snake));
    emit_unless_written(module, &cb, "update", fmt_str(
        "pub fn update(conn: datasource.Connection, a: %s) -> Result<%s, DbError> {\n"
        "    val unscoped: [string] = []\n"
        "    return eql.update_one<%s>(conn, %s_meta, %s_to_params(a), unscoped, %s_from_row)\n"
        "}\n\n",
        entity, entity, entity, snake, snake, snake));
    emit_unless_written(module, &cb, "find", fmt_str(
        "pub fn find(conn: datasource.Connection%s) -> Result<Option<%s>, DbError> {\n"
        "%s"
        "    return eql.find_maybe<%s>(conn, %s_meta, keys, %s_from_row)\n"
        "}\n\n",
        key_params, entity, key_block, entity, snake, snake));
    emit_unless_written(module, &cb, "find_or_error", fmt_str(
        "pub fn find_or_error(conn: datasource.Connection%s) -> Result<%s, DbError> {\n"
        "%s"
        "    return eql.find_by_key<%s>(conn, %s_meta, keys, %s_from_row)\n"
        "}\n\n",
        key_params, entity, key_block, entity, snake, snake));
    emit_unless_written(module, &cb, "exists", fmt_str(
        "pub fn exists(conn: datasource.Connection%s) -> Result<bool, DbError> {\n"
        "%s"
        "    return eql.exists(conn, %s_meta, keys)\n"
        "}\n\n",
        key_params, key_block, snake));
    emit_unless_written(module, &cb, "delete", fmt_str(
        "pub fn delete(conn: datasource.Connection%s) -> Result<unit, DbError> {\n"
        "%s"
        "    val unscoped: [eql.Binding] = []\n"
        "    return eql.delete_one(conn, %s_meta, keys, unscoped)\n"
        "}\n\n",
        key_params, key_block, snake));
    emit_unless_written(module, &cb, "find_all", fmt_str(
        "pub fn find_all(conn: datasource.Connection) -> Result<[%s], DbError> {\n"
        "    return eql.find_all<%s>(conn, %s_meta, %s_from_row)\n"
        "}\n\n",
        entity, entity, snake, snake));
    emit_unless_written(module, &cb, "count_all", fmt_str(
        "pub fn count_all(conn: datasource.Connection) -> Result<int, DbError> {\n"
        "    return eql.count_all(conn, %s_meta)\n"
        "}\n\n",
        snake));
    emit_unless_written(module, &cb, "delete_all", fmt_str(
        "pub fn delete_all(conn: datasource.Connection) -> Result<int, DbError> {\n"
        "    return eql.delete_all(conn, %s_meta)\n"
        "}\n\n",
        snake));

    if (cb.data == NULL || cb.data[0] == '\0') return;
    parse_and_splice(module, cb.data, line, col, fmt_str("repository(%s)", entity));
}

void generate_derived_functions(Module *module) {
    for (volatile int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        for (volatile int d = 0; d < sd->derive_count; d++) {
        jmp_buf recover;
        ziman_recovery_push(&recover);
        if (setjmp(recover) != 0) {
            ziman_set_error_context(NULL);
            continue;
        }
            int is_json = strcmp(sd->derives[d], "Json") == 0;
            int is_row = strcmp(sd->derives[d], "Row") == 0;
            int is_props = strcmp(sd->derives[d], "Properties") == 0;
            int is_ord = strcmp(sd->derives[d], "Ord") == 0;
            int is_entity = strcmp(sd->derives[d], "Entity") == 0;
            if (!is_json && !is_row && !is_props && !is_ord && !is_entity) {
                ziman_error("syntax", sd->line, sd->col, "unknown derive target '%s' on struct '%s'", sd->derives[d], sd->name);
            }
            if (sd->type_params.count > 0 && (is_row || is_props || is_entity)) {
                ziman_error("syntax", sd->line, sd->col, "derive(%s) does not support a generic struct ('%s') -- only Json and Ord do", sd->derives[d], sd->name);
            }
            const char *label = fmt_str("derive(%s) for '%s'", sd->derives[d], sd->name);
            import_for_derive(module, sd->derives[d], sd->line, sd->col);
            if (is_json || is_row || is_props) {
                for (int fi = 0; fi < sd->fields.count; fi++) {
                    refuse_handle_field(module, sd->derives[d], sd->fields.items[fi].type, sd->line, sd->col);
                }
            }
            if (is_json || is_row) {
                for (int fi = 0; fi < sd->fields.count; fi++) {
                    import_field_converters(module, is_json ? "json" : "sql", sd->fields.items[fi].type, sd->line, sd->col);
                }
            }
            if (is_entity) {
                if (sd->fields.count == 0) {
                    ziman_error("syntax", sd->line, sd->col, "derive(Entity) needs at least one field to be a column ('%s' has none)", sd->name);
                }
                /* An entity is something read back out of a row, and the only thing that reads
                 * one is `<name>_from_row`. Deriving the column list without it leaves half a
                 * metamodel: a query layer could name the columns and could not build the value. */
                int has_row = 0;
                for (int other = 0; other < sd->derive_count; other++) {
                    if (strcmp(sd->derives[other], "Row") == 0) has_row = 1;
                }
                if (!has_row) {
                    ziman_error("syntax", sd->line, sd->col,
                                "derive(Entity) on '%s' needs derive(Row) too -- an entity is read from a row, and `%s_from_row` is what reads it",
                                sd->name, pascal_to_snake(sd->name));
                }
                /* The generated `eql.entity(..)` call needs the module, and a file that wrote
                 * only the attribute has no reason to have imported it. `ensure_module_import` is
                 * what the standard-type conversions already do for `time` and `uuid`. */
                if (sd->derive_args != NULL && sd->derive_args[d] != NULL) {
                    ensure_module_import(module, "eql", sd->line, sd->col);
                }
                parse_and_splice(module,
                                 generate_struct_entity(sd, sd->derive_args != NULL ? sd->derive_args[d] : NULL),
                                 sd->line, sd->col, label);
            } else if (is_ord) {
                if (sd->fields.count == 0) {
                    ziman_error("syntax", sd->line, sd->col, "derive(Ord) needs at least one field to compare ('%s' has none)", sd->name);
                }
                parse_and_splice(module, generate_struct_ord(sd), sd->line, sd->col, label);
            } else if (is_props) {
                parse_and_splice(module, generate_struct_properties(sd), sd->line, sd->col, label);
            } else if (is_json) {
                parse_and_splice(module, generate_struct_to_json(sd), sd->line, sd->col, label);
                parse_and_splice(module, generate_struct_from_json(sd), sd->line, sd->col, label);
            } else {
                parse_and_splice(module, generate_struct_from_row(sd), sd->line, sd->col, label);
                parse_and_splice(module, generate_struct_to_params(sd), sd->line, sd->col, label);
            }
                ziman_recovery_pop();
}
    }
    for (volatile int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        for (volatile int d = 0; d < ed->derive_count; d++) {
        jmp_buf recover;
        ziman_recovery_push(&recover);
        if (setjmp(recover) != 0) {
            ziman_set_error_context(NULL);
            continue;
        }
            if (strcmp(ed->derives[d], "Entity") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Entity) does not support an enum ('%s') -- an entity is a struct, one field per column", ed->name);
            }
            if (strcmp(ed->derives[d], "Row") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Row) does not support an enum ('%s') -- a SQL row is a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Properties") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Properties) does not support an enum ('%s') -- configuration binds to a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Ord") == 0) {
                const char *ord_label = fmt_str("derive(Ord) for '%s'", ed->name);
                parse_and_splice(module, generate_enum_ord(ed), ed->line, ed->col, ord_label);
                ziman_recovery_pop();
                continue;
            }
            if (strcmp(ed->derives[d], "Json") != 0) {
                ziman_error("syntax", ed->line, ed->col, "unknown derive target '%s' on enum '%s'", ed->derives[d], ed->name);
            }
            const char *label = fmt_str("derive(%s) for '%s'", ed->derives[d], ed->name);
            import_for_derive(module, ed->derives[d], ed->line, ed->col);
            for (int vi = 0; vi < ed->variants.count; vi++) {
                if (ed->variants.items[vi].has_payload) {
                    refuse_handle_field(module, "Json", ed->variants.items[vi].payload_type, ed->line, ed->col);
                    import_field_converters(module, "json", ed->variants.items[vi].payload_type, ed->line, ed->col);
                }
            }
            parse_and_splice(module, generate_enum_to_json(ed), ed->line, ed->col, label);
            parse_and_splice(module, generate_enum_from_json(ed), ed->line, ed->col, label);
                ziman_recovery_pop();
}
    }
}
