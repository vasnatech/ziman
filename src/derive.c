#include "derive.h"
#include "generators.h"

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


/* ---------------------------------------------------------------------
 * Struct derive: <name>_to_json / <name>_from_json.
 * --------------------------------------------------------------------- */


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


/* ---------------------------------------------------------------------
 * Properties derive: <name>_from_properties / <name>_keys, binding a
 * struct to the keys under a prefix in a lib/core/properties.zm
 * Properties (field `idle_timeout_ms` <-> key `<prefix>.idle-timeout-ms`).
 * The generated code calls qualified `properties.*`, so the consumer
 * needs only `use properties`. Option<T> fields are optional keys;
 * every other field is required.
 * --------------------------------------------------------------------- */


/* ---------------------------------------------------------------------
 * Driver: lex+parse the generated snippet and splice its functions in.
 * --------------------------------------------------------------------- */

/* `line`/`col` are the declaration the code is generated for. Every token
 * is restamped with that position before parsing, so an error anywhere in
 * the generated code points at the user's own `struct`, and `label` (say,
 * "derive(Json) for 'Point'") is attached to the functions so the checker
 * can say what it was reading. */
int g_emit_derived = 0;

/* `target` is the one caller that does not want a *new* declaration: a `#[repository]` derived
 * query is already declared, by the author, with its own name, parameters, return type and
 * position -- all that is missing is the body. So the snippet's single function is read for its
 * body alone and the author's declaration keeps everything else, which is what makes a refusal
 * inside it land on the line the query was written at.
 *
 * Its vals are **appended** where every other caller's are prepended, and the reason is the same
 * ordering fact read the other way: a derived query's val calls `eql.translate(.., <entity>_meta)`,
 * and when the entity is declared in this very module that `_meta` is itself a prepended derived
 * val. Prepending here would put the query ahead of the metamodel it reads. Nothing hand-written
 * can refer to a query val -- it is private and named for a generated body -- so last is safe. */
static void parse_and_splice_into(Module *module, char *source, int line, int col, const char *label,
                                  FunctionDecl *target) {
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
        if (target != NULL) {
            target->body = snippet->functions.items[i]->body;
            target->derived_from = label;
            continue;
        }
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
    if (snippet->vals.count > 0 && target != NULL) {
        for (int i = 0; i < snippet->vals.count; i++) {
            global_val_list_push(&module->vals, snippet->vals.items[i]);
        }
    } else if (snippet->vals.count > 0) {
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

static void parse_and_splice(Module *module, char *source, int line, int col, const char *label) {
    parse_and_splice_into(module, source, line, col, label, NULL);
}

/* ---------------------------------------------------------------------
 * Ord derive: <name>_compare(a, b) -> int. A struct compares field by field in
 * declaration order (the first difference decides); an enum compares by
 * variant position first, then by payload when both are the same variant.
 * Each field/payload is compared with the `compare` builtin, so it must be an
 * Ord type itself -- the checker reports one that isn't.
 * --------------------------------------------------------------------- */


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
    /* **The key is what the nine generated functions need, so `crud: false` does not need one.**
     * Each of them addresses one row by it; a module that generates none of them has only derived
     * queries, whose predicates come from their own names and may never mention the key. */
    if (key_count == 0 && !module->repository_no_crud) {
        ziman_error("syntax", line, col,
                    "#![repository(%s)] needs '%s' to mark at least one field #[pk] -- a repository's "
                    "find, update and delete each address one row, and without a key there is nothing "
                    "that addresses one (a module that wants the entity named without those nine says "
                    "'crud: false')",
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

    /* **`crud: false` stops here, having done the imports and not the emitting.** The imports are
     * what every derived query in the module needs too -- `eql`, `datasource`, `DbError`,
     * `SqlValue`, the entity and its `_meta`/`_from_row` -- and they are this function's work
     * whether or not the nine are wanted. Everything below builds the nine. */
    if (module->repository_no_crud) {
        if (g_emit_derived) printf("// repository(%s): 'crud: false', so none of the nine is generated\n", entity);
        return;
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

/* ---------------------------------------------------------------------
 * Derived queries: `#[repository]` on a bodyless `pub fn`, whose **name is
 * its query**.
 *
 * `find_by_id_and_patient_id(conn, id, patient_id) -> Result<VitalSign, DbError>` is read as a
 * verb, a predicate over the entity's own columns, and a cardinality taken from the return type,
 * and the body that answers it is written here. The full design, including the counted evidence
 * that six name shapes cover 69 of the sample backend's 144 repository functions, is in
 * docs/design/derived-queries.md.
 *
 * **Two things set this apart from Spring Data's version, and both are checks rather than
 * features.** Parameters bind *by name*: `find_by_id_and_patient_id(patient_id, id)` -- two
 * transposed `Uuid`s -- is a compile error here and answers the wrong rows forever there. And a
 * name with two readings is refused rather than resolved by precedence, which is why the search
 * below enumerates every parse instead of committing to the longest match at each step.
 *
 * **Every refusal is reported at the function's own line, before any SQL exists**, from the
 * entity's `StructDecl` and the signature as written. The alternative -- letting the generated
 * `eql.translate` fail at module initialisation -- was measured on a real mistake during the
 * `eql` adoption and arrived with three frames of library code and not one line of the repository
 * that caused it.
 * --------------------------------------------------------------------- */

typedef enum { QV_FIND, QV_COUNT, QV_EXISTS, QV_DELETE } QueryVerb;

static const struct { const char *word; QueryVerb verb; } QUERY_VERBS[] = {
    {"find", QV_FIND}, {"count", QV_COUNT}, {"exists", QV_EXISTS}, {"delete", QV_DELETE},
};

typedef enum { QO_EQ, QO_NOT, QO_IN, QO_BETWEEN, QO_IS_NULL, QO_IS_NOT_NULL, QO_IGNORE_CASE } TermOp;

/* The operator suffix a term may carry. Longest first, so that the *greedy* walk the refusal
 * messages use reads `_is_not_null` rather than stopping at a column plus `_is_not`; the search
 * itself tries all of them at every position, because choosing is what produces the surprises
 * this exists to avoid. The empty suffix -- a plain equality -- is last for the same reason. */
static const struct { const char *suffix; TermOp op; } TERM_OPS[] = {
    {"_is_not_null", QO_IS_NOT_NULL},
    {"_ignore_case", QO_IGNORE_CASE},
    {"_is_null", QO_IS_NULL},
    {"_between", QO_BETWEEN},
    {"_not", QO_NOT},
    {"_in", QO_IN},
    {"", QO_EQ},
};

static const struct { const char *suffix; int descending; } ORDER_DIRS[] = {
    {"_desc", 1}, {"_asc", 0}, {"", 0},
};

#define BY_SEP "_by_"
#define AND_SEP "_and_"
#define ORDER_SEP "_order_by_"

typedef struct { const char *column; TermOp op; } QTerm;
typedef struct { const char *column; int descending; } QOrder;

/* One reading of a name: the predicate's terms in order, then the ordering's columns in order.
 * Both grow rather than sitting in a fixed array -- docs/design/derived-queries.md §8 decided
 * against a cap on term count, and a buffer limit would be one by another name. */
typedef struct {
    QTerm *terms;
    int term_count, term_cap;
    QOrder *orders;
    int order_count, order_cap;
} QShape;

static void qshape_push_term(QShape *s, const char *column, TermOp op) {
    if (s->term_count == s->term_cap) {
        s->term_cap = s->term_cap == 0 ? 4 : s->term_cap * 2;
        s->terms = ziman_xrealloc(s->terms, sizeof(QTerm) * (size_t)s->term_cap);
    }
    s->terms[s->term_count].column = column;
    s->terms[s->term_count++].op = op;
}

static void qshape_push_order(QShape *s, const char *column, int descending) {
    if (s->order_count == s->order_cap) {
        s->order_cap = s->order_cap == 0 ? 4 : s->order_cap * 2;
        s->orders = ziman_xrealloc(s->orders, sizeof(QOrder) * (size_t)s->order_cap);
    }
    s->orders[s->order_count].column = column;
    s->orders[s->order_count++].descending = descending;
}

static QShape qshape_copy(const QShape *s) {
    QShape out = {0};
    for (int i = 0; i < s->term_count; i++) qshape_push_term(&out, s->terms[i].column, s->terms[i].op);
    for (int i = 0; i < s->order_count; i++) qshape_push_order(&out, s->orders[i].column, s->orders[i].descending);
    return out;
}

/* The enumeration of a name's readings. `total` counts every one; `found` keeps the first two,
 * which is all a refusal needs to show that there is more than one. */
typedef struct {
    StructDecl *sd;
    QShape cur;
    QShape found[2];
    int stored;
    int total;
} QSearch;

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void qsearch_record(QSearch *q) {
    if (q->stored < 2) q->found[q->stored++] = qshape_copy(&q->cur);
    q->total++;
}

static void enum_orders(QSearch *q, const char *text) {
    for (int i = 0; i < q->sd->fields.count; i++) {
        const char *col = q->sd->fields.items[i].name;
        if (!starts_with(text, col)) continue;
        const char *rest = text + strlen(col);
        for (size_t d = 0; d < sizeof ORDER_DIRS / sizeof ORDER_DIRS[0]; d++) {
            if (!starts_with(rest, ORDER_DIRS[d].suffix)) continue;
            const char *after = rest + strlen(ORDER_DIRS[d].suffix);
            if (*after != '\0' && !starts_with(after, AND_SEP)) continue;
            qshape_push_order(&q->cur, col, ORDER_DIRS[d].descending);
            if (*after == '\0') {
                qsearch_record(q);
            } else {
                enum_orders(q, after + strlen(AND_SEP));
            }
            q->cur.order_count--;
        }
    }
}

static void enum_terms(QSearch *q, const char *text) {
    for (int i = 0; i < q->sd->fields.count; i++) {
        const char *col = q->sd->fields.items[i].name;
        if (!starts_with(text, col)) continue;
        const char *rest = text + strlen(col);
        for (size_t o = 0; o < sizeof TERM_OPS / sizeof TERM_OPS[0]; o++) {
            if (!starts_with(rest, TERM_OPS[o].suffix)) continue;
            const char *after = rest + strlen(TERM_OPS[o].suffix);
            int at_end = *after == '\0';
            int another = starts_with(after, AND_SEP);
            int ordered = starts_with(after, ORDER_SEP);
            if (!at_end && !another && !ordered) continue;
            qshape_push_term(&q->cur, col, TERM_OPS[o].op);
            if (at_end) {
                qsearch_record(q);
            } else if (ordered) {
                enum_orders(q, after + strlen(ORDER_SEP));
            } else {
                enum_terms(q, after + strlen(AND_SEP));
            }
            q->cur.term_count--;
        }
    }
}

/* `find_by_id_and_patient_id` -> QV_FIND and `_by_id_and_patient_id`. The word has to end at a
 * `_` or at the end of the name, so `finder_by_x` is not a `find`. */
static int split_verb(const char *name, QueryVerb *verb, const char **after) {
    for (size_t i = 0; i < sizeof QUERY_VERBS / sizeof QUERY_VERBS[0]; i++) {
        size_t n = strlen(QUERY_VERBS[i].word);
        if (strncmp(name, QUERY_VERBS[i].word, n) != 0) continue;
        if (name[n] != '\0' && name[n] != '_') continue;
        *verb = QUERY_VERBS[i].verb;
        *after = name + n;
        return 1;
    }
    return 0;
}

static const char *longest_column(StructDecl *sd, const char *text) {
    const char *best = NULL;
    for (int i = 0; i < sd->fields.count; i++) {
        const char *col = sd->fields.items[i].name;
        if (!starts_with(text, col)) continue;
        if (best == NULL || strlen(col) > strlen(best)) best = col;
    }
    return best;
}

static char *entity_field_list(StructDecl *sd) {
    CodeBuf cb;
    cb_init(&cb);
    for (int i = 0; i < sd->fields.count; i++) {
        cb_appendf(&cb, "%s%s", i > 0 ? ", " : "", sd->fields.items[i].name);
    }
    return cb.data;
}

static const char *op_word(TermOp op) {
    switch (op) {
        case QO_EQ: return "";
        case QO_NOT: return " not";
        case QO_IN: return " in";
        case QO_BETWEEN: return " between";
        case QO_IS_NULL: return " is null";
        case QO_IS_NOT_NULL: return " is not null";
        case QO_IGNORE_CASE: return " ignore case";
    }
    return "";
}

/* One reading, written out the way a reader can compare two of them: "'name' and 'type'". */
static char *render_shape(const QShape *s) {
    CodeBuf cb;
    cb_init(&cb);
    if (s->term_count == 0) cb_appendf(&cb, "no predicate");
    for (int i = 0; i < s->term_count; i++) {
        cb_appendf(&cb, "%s'%s'%s", i > 0 ? " and " : "", s->terms[i].column, op_word(s->terms[i].op));
    }
    for (int i = 0; i < s->order_count; i++) {
        cb_appendf(&cb, "%s'%s'%s", i == 0 ? ", ordered by " : " then ", s->orders[i].column,
                   s->orders[i].descending ? " descending" : "");
    }
    return cb.data;
}

/* Why a name could not be read at all. The search above answers "no parse" and nothing more, so
 * this walks the same text greedily -- longest column, then longest operator -- and reports the
 * first position where nothing fits. A greedy walk that reached the end would itself have been a
 * parse, so exactly one of these always fires. `ordering` starts the walk in the `_order_by_`
 * grammar, and the walk switches into it on its own when the predicate runs into one. */
static _Noreturn void refuse_unreadable(FunctionDecl *fn, StructDecl *sd, const char *entity,
                                        const char *text, int ordering) {
    const char *at = text;
    int read = 0;
    for (;;) {
        const char *part = ordering ? "ordering" : "predicate";
        const char *col = longest_column(sd, at);
        if (col == NULL) {
            if (read == 0) {
                ziman_error("type", fn->line, fn->col,
                            "nothing in '%s' names a field of %s -- a derived query's %s is made of the "
                            "entity's own field names (it has: %s)",
                            at, entity, part, entity_field_list(sd));
            }
            ziman_error("type", fn->line, fn->col,
                        "nothing in '%s' names a field of %s, and the %s needs one after '%s' (it has: %s)",
                        at, entity, part, AND_SEP, entity_field_list(sd));
        }
        at += strlen(col);
        if (ordering) {
            for (size_t d = 0; d < sizeof ORDER_DIRS / sizeof ORDER_DIRS[0]; d++) {
                if (starts_with(at, ORDER_DIRS[d].suffix)) { at += strlen(ORDER_DIRS[d].suffix); break; }
            }
        } else {
            for (size_t o = 0; o < sizeof TERM_OPS / sizeof TERM_OPS[0]; o++) {
                if (starts_with(at, TERM_OPS[o].suffix)) { at += strlen(TERM_OPS[o].suffix); break; }
            }
        }
        read++;
        if (starts_with(at, AND_SEP)) { at += strlen(AND_SEP); continue; }
        if (!ordering && starts_with(at, ORDER_SEP)) {
            at += strlen(ORDER_SEP);
            ordering = 1;
            read = 0;
            continue;
        }
        ziman_error("type", fn->line, fn->col,
                    "'%s' is left over after reading '%s' -- a derived query's %s is one field name per "
                    "term, joined by '%s' (%s has: %s)",
                    at, col, part, AND_SEP, entity, entity_field_list(sd));
    }
}

/* ------------------------------------------------- the signature, checked against the name */

typedef enum { RK_ENTITY, RK_OPTION_ENTITY, RK_LIST_ENTITY, RK_INT, RK_BOOL, RK_UNIT, RK_OTHER } ReturnKind;

/* A type name with both qualifiers stripped: the loader's own `uuid::Uuid` and the source's
 * `uuid.Uuid` both answer `Uuid`. The entity's fields come from an already-loaded module and the
 * signature's parameters are as the author wrote them, so a comparison between the two sees both
 * spellings of the same type. */
static const char *plain_type_name_of(const char *name) {
    if (name == NULL) return NULL;
    const char *sep = strstr(name, "::");
    const char *bare = sep != NULL ? sep + 2 : name;
    const char *dot = strrchr(bare, '.');
    return dot != NULL ? dot + 1 : bare;
}

static int type_is_named(Type t, const char *name) {
    const char *n = t.kind == TY_STRUCT ? t.struct_name : t.kind == TY_ENUM ? t.enum_name : NULL;
    return n != NULL && strcmp(plain_type_name_of(n), name) == 0;
}

static int same_column_type(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    switch (a.kind) {
        case TY_ARRAY:
            return a.elem != NULL && b.elem != NULL && same_column_type(*a.elem, *b.elem);
        case TY_STRUCT:
        case TY_ENUM: {
            const char *an = a.kind == TY_STRUCT ? a.struct_name : a.enum_name;
            const char *bn = b.kind == TY_STRUCT ? b.struct_name : b.enum_name;
            if (an == NULL || bn == NULL) return an == bn;
            if (strcmp(plain_type_name_of(an), plain_type_name_of(bn)) != 0) return 0;
            if (a.type_args.count != b.type_args.count) return 0;
            for (int i = 0; i < a.type_args.count; i++) {
                if (!same_column_type(a.type_args.items[i], b.type_args.items[i])) return 0;
            }
            return 1;
        }
        default:
            return 1;
    }
}

static ReturnKind classify_return(Type ret, const char *entity) {
    if (ret.kind != TY_RESULT || ret.elem == NULL) return RK_OTHER;
    Type ok = *ret.elem;
    if (type_is_named(ok, entity)) return RK_ENTITY;
    if (is_option_type_here(ok) && type_is_named(ok.type_args.items[0], entity)) return RK_OPTION_ENTITY;
    if (ok.kind == TY_ARRAY && ok.elem != NULL && type_is_named(*ok.elem, entity)) return RK_LIST_ENTITY;
    if (ok.kind == TY_INT) return RK_INT;
    if (ok.kind == TY_BOOL) return RK_BOOL;
    if (ok.kind == TY_UNIT) return RK_UNIT;
    return RK_OTHER;
}

/* **What a term asks of the parameter list.** An `is_null` term asks for nothing, a `between` for
 * two, an `in` for an array; everything else for one parameter of the column's own type.
 *
 * An `Option` column asks for its *inner* type, not for an `Option`: `find_by_note(conn, note:
 * string)` reads rows whose nullable `note` is that text, and the None side of such a column is
 * what `_is_null` is for. A parameter of `Option<string>` would bind NULL on None, and `col =
 * NULL` matches no row in any engine -- a query that silently answers nothing. */
typedef struct {
    const char *name;
    const char *column; /* the entity field the term named, which is not `name` for a `between` */
    Type type;
    int is_list;
} Expected;

static Type column_param_type(Param *f) {
    return is_option_type_here(f->type) ? f->type.type_args.items[0] : f->type;
}

static Param *entity_field(StructDecl *sd, const char *name) {
    for (int i = 0; i < sd->fields.count; i++) {
        if (strcmp(sd->fields.items[i].name, name) == 0) return &sd->fields.items[i];
    }
    return NULL;
}

static Type *type_box(Type t) {
    Type *p = ziman_xmalloc(sizeof(Type));
    *p = t;
    return p;
}

static Expected *expected_params(StructDecl *sd, const QShape *shape, int *count_out) {
    Expected *xs = ziman_xmalloc(sizeof(Expected) * (size_t)(shape->term_count * 2 + 1));
    int n = 0;
    for (int i = 0; i < shape->term_count; i++) {
        Param *f = entity_field(sd, shape->terms[i].column);
        Type base = column_param_type(f);
        switch (shape->terms[i].op) {
            case QO_IS_NULL:
            case QO_IS_NOT_NULL:
                break;
            case QO_BETWEEN:
                xs[n].name = fmt_str("%s_from", f->name);
                xs[n].column = f->name;
                xs[n].type = base;
                xs[n++].is_list = 0;
                xs[n].name = fmt_str("%s_to", f->name);
                xs[n].column = f->name;
                xs[n].type = base;
                xs[n++].is_list = 0;
                break;
            case QO_IN: {
                Type arr = {0};
                arr.kind = TY_ARRAY;
                arr.elem = type_box(base);
                xs[n].name = f->name;
                xs[n].column = f->name;
                xs[n].type = arr;
                xs[n++].is_list = 1;
                break;
            }
            default:
                xs[n].name = f->name;
                xs[n].column = f->name;
                xs[n].type = base;
                xs[n++].is_list = 0;
                break;
        }
    }
    *count_out = n;
    return xs;
}

/* The signature a name asks for, written as a reader would: `(conn, id: Uuid, patient_id: Uuid)`. */
static char *render_expected_signature(FunctionDecl *fn, const Expected *xs, int n) {
    CodeBuf cb;
    cb_init(&cb);
    /* The connection's own name when the signature has one, and `conn` when it does not -- this
     * renders the signature a name *asks for*, and the parameter the author put first is only its
     * connection when it is typed like one. */
    int has_conn = fn->params.count > 0 && type_is_named(fn->params.items[0].type, "Connection");
    cb_appendf(&cb, "(%s: datasource.Connection", has_conn ? fn->params.items[0].name : "conn");
    for (int i = 0; i < n; i++) cb_appendf(&cb, ", %s: %s", xs[i].name, type_name(xs[i].type));
    cb_appendf(&cb, ")");
    return cb.data;
}

/* **Parameters bind by name, which is the whole reason this exists rather than a copy of Spring
 * Data.** There, `findByIdAndPatientId(patientId, id)` compiles, boots, and answers another
 * patient's rows forever; here the mismatch is a compile error naming both sides. The type is
 * checked against the entity's field as well, which catches the other half -- a `string`
 * parameter for a `Uuid` column. */
static void check_query_params(FunctionDecl *fn, const char *entity, const Expected *xs, int n) {
    if (fn->params.count == 0 || !type_is_named(fn->params.items[0].type, "Connection")) {
        ziman_error("type", fn->line, fn->col,
                    "a derived query's first parameter is the connection it runs on "
                    "(conn: datasource.Connection), and the rest are its predicate's terms: %s",
                    render_expected_signature(fn, xs, n));
    }
    int given = fn->params.count - 1;
    if (given != n) {
        ziman_error("type", fn->line, fn->col,
                    "the name asks for %d parameter%s after the connection and the signature has %d -- %s",
                    n, n == 1 ? "" : "s", given, render_expected_signature(fn, xs, n));
    }
    for (int i = 0; i < n; i++) {
        Param *p = &fn->params.items[i + 1];
        if (strcmp(p->name, xs[i].name) != 0) {
            ziman_error("type", fn->line, fn->col,
                        "parameter %d is named '%s' and the name's term %d is '%s' -- a derived query binds "
                        "its parameters by name, not by position, so two transposed arguments are this error "
                        "rather than the wrong rows: %s",
                        i + 1, p->name, i + 1, xs[i].name, render_expected_signature(fn, xs, n));
        }
        if (!same_column_type(p->type, xs[i].type)) {
            /* `column` rather than `name`, because a `between` term's two parameters are
             * `<column>_from` and `<column>_to` and neither is a field of anything. */
            ziman_error("type", fn->line, fn->col,
                        "parameter '%s' is %s, and the name's term reads %s's field '%s', so it must be %s",
                        p->name, type_name(p->type), entity, xs[i].column, type_name(xs[i].type));
        }
    }
}

/* ------------------------------------------------- the SQL, and the body that runs it */

static char *build_query_sql(const char *entity, QueryVerb verb, const QShape *shape) {
    CodeBuf cb;
    cb_init(&cb);
    switch (verb) {
        case QV_FIND: cb_appendf(&cb, "SELECT e FROM %s e", entity); break;
        case QV_COUNT:
        case QV_EXISTS: cb_appendf(&cb, "SELECT count(*) AS n FROM %s e", entity); break;
        case QV_DELETE: cb_appendf(&cb, "DELETE FROM %s e", entity); break;
    }
    for (int i = 0; i < shape->term_count; i++) {
        const char *col = shape->terms[i].column;
        cb_appendf(&cb, "%s", i == 0 ? " WHERE " : " AND ");
        switch (shape->terms[i].op) {
            case QO_EQ: cb_appendf(&cb, "e.%s = :%s", col, col); break;
            case QO_NOT: cb_appendf(&cb, "e.%s <> :%s", col, col); break;
            case QO_IN: cb_appendf(&cb, "e.%s IN :%s", col, col); break;
            case QO_BETWEEN: cb_appendf(&cb, "e.%s BETWEEN :%s_from AND :%s_to", col, col, col); break;
            case QO_IS_NULL: cb_appendf(&cb, "e.%s IS NULL", col); break;
            case QO_IS_NOT_NULL: cb_appendf(&cb, "e.%s IS NOT NULL", col); break;
            /* `upper` on both sides rather than a collation: a collation is per-column and per
             * engine, and this has to mean the same thing on memdb, SQLite, Postgres and MySQL. */
            case QO_IGNORE_CASE: cb_appendf(&cb, "upper(e.%s) = upper(:%s)", col, col); break;
        }
    }
    for (int i = 0; i < shape->order_count; i++) {
        cb_appendf(&cb, "%se.%s%s", i == 0 ? " ORDER BY " : ", ", shape->orders[i].column,
                   shape->orders[i].descending ? " DESC" : "");
    }
    return cb.data;
}

/* The key's value as a person reads it, for the `NotFound` message -- `uuid.format(id)` for a
 * `Uuid`, `str(n)` for a number, the string itself for a string. NULL for every other type, and
 * then the message carries the label alone rather than a rendering nobody asked for: `format`
 * prints a struct as its fields (`uuid::Uuid { bytes: [17, 17, ..] }`), which in a 404 body would
 * be worse than saying nothing. */
static char *key_text_expr(Module *module, Type t, const char *expr, int line, int col) {
    switch (t.kind) {
        case TY_STRING:
            return fmt_str("%s", expr);
        case TY_INT:
        case TY_FLOAT:
        case TY_BOOL:
        case TY_BIGINT:
        case TY_DECIMAL:
            return fmt_str("str(%s)", expr);
        case TY_STRUCT:
        case TY_ENUM:
            if (type_is_named(t, "Uuid")) {
                ensure_module_import(module, "uuid", line, col);
                return fmt_str("uuid.format(%s)", expr);
            }
            return NULL;
        default:
            return NULL;
    }
}

/* The message a derived query's `NotFound` carries: `"Medication not found: <id>"` when the
 * predicate addresses the entity's key and the key renders, `"Medication not found"` when it does
 * not. The label is `#![repository(E, label: "..")]`'s, or the entity's own name when none was
 * given -- see ast.h's `repository_label` for why a generator cannot work it out.
 *
 * The shape is `<label> not found: <id>` because that is what the application this was adopted in
 * already wrote, by hand, in 31 places, and its own comment says why the id is in it: a 404 in a
 * log with no id cannot be matched to a request. */
static char *not_found_expr(Module *module, StructDecl *sd, const QShape *shape, FunctionDecl *fn) {
    const char *label = module->repository_label != NULL ? module->repository_label : module->repository_entity;
    int keys = 0;
    for (int i = 0; i < sd->fields.count; i++) {
        if (sd->fields.items[i].is_pk) keys++;
    }
    if (keys == 1) {
        for (int i = 0; i < shape->term_count; i++) {
            if (shape->terms[i].op != QO_EQ) continue;
            Param *f = entity_field(sd, shape->terms[i].column);
            if (f == NULL || !f->is_pk) continue;
            char *text = key_text_expr(module, column_param_type(f), f->name, fn->line, fn->col);
            if (text == NULL) break;
            return fmt_str("DbError.NotFound(concat(\"%s not found: \", %s))", label, text);
        }
    }
    return fmt_str("DbError.NotFound(\"%s not found\")", label);
}

/* The `[eql.Binding]` the body hands to `eql`, built from the parameters the name asked for.
 * Each value goes through `emit_to_sql` -- the same conversion `<name>_to_params` is built from --
 * so a parameter reaches SQL by exactly the route its column does. */
static void emit_bindings(CodeBuf *cb, const Expected *xs, int n, int *tmp, int line, int col) {
    for (int i = 0; i < n; i++) {
        if (!xs[i].is_list) {
            emit_to_sql(xs[i].type, xs[i].name, fmt_str("__value_%s", xs[i].name), cb, tmp, line, col);
            continue;
        }
        cb_appendf(cb, "    val __values_%s = mutable_array_new<SqlValue>()\n", xs[i].name);
        cb_appendf(cb, "    for __item_%s in %s {\n", xs[i].name, xs[i].name);
        emit_to_sql(*xs[i].type.elem, fmt_str("__item_%s", xs[i].name), fmt_str("__item_%s_sql", xs[i].name), cb, tmp, line, col);
        cb_appendf(cb, "    push<SqlValue>(__values_%s, __item_%s_sql)\n", xs[i].name, xs[i].name);
        cb_appendf(cb, "    }\n");
    }
    cb_appendf(cb, "    val __bindings: [eql.Binding] = [");
    for (int i = 0; i < n; i++) {
        if (xs[i].is_list) {
            cb_appendf(cb, "%seql.bind_list(\"%s\", snapshot(__values_%s))", i > 0 ? ", " : "", xs[i].name, xs[i].name);
        } else {
            cb_appendf(cb, "%seql.bind(\"%s\", __value_%s)", i > 0 ? ", " : "", xs[i].name, xs[i].name);
        }
    }
    cb_appendf(cb, "]\n");
}

static const char *verb_returns(QueryVerb verb) {
    switch (verb) {
        case QV_FIND:
            return "Result<E, DbError> (one row, NotFound when there is none), "
                   "Result<Option<E>, DbError> (one row, absence being the answer) or "
                   "Result<[E], DbError> (every matching row)";
        case QV_COUNT: return "Result<int, DbError>";
        case QV_EXISTS: return "Result<bool, DbError>";
        case QV_DELETE:
            return "Result<unit, DbError> (the one row this addresses, NotFound when none matched) or "
                   "Result<int, DbError> (every matching row, answering how many)";
    }
    return "";
}

/* The text a Ziman generator answered for this declaration.
 *
 * **There is no C fallback left for any target.** Each of the five used to have a C generator
 * beside it, kept so a build whose bridge could not run still compiled, and held to the same
 * bytes by a fixture. Once every target was ported the C became unreachable -- `derive_json`
 * was the last, and with it in `GENERATOR_MODULES` no `ziman_generate_in_ziman` call can answer
 * NULL any more. Unreachable code that no fixture can exercise is exactly what the C was there
 * to avoid becoming, so it is gone and a NULL is now what it actually means: the compiler's own
 * corelib did not load. */
/* The string a target was written with -- `#[derive(Entity("patients"))]` -- or NULL for one
 * written bare. `derive_args` is parallel to `derives`, and may be absent entirely. */
static const char *derive_arg(const StructDecl *sd, int d) {
    return sd->derive_args != NULL ? sd->derive_args[d] : NULL;
}

static char *generated_in_ziman(const char *target, StructDecl *sd, EnumDecl *ed, const char *arg, int line, int col) {
    char *text = ziman_generate_in_ziman(target, sd, ed, arg, line, col);
    if (text == NULL) {
        ziman_error("module", line, col,
                    "derive(%s) is generated by lib/core/derive_%s.zm, which did not load",
                    target, pascal_to_snake(target));
    }
    return text;
}

/* One `#[repository]` function: its name read against the entity, its signature checked against
 * the name, and then the query val and the body it was declared without. */
static void generate_one_derived_query(Module *module, StructDecl *sd, FunctionDecl *fn) {
    const char *entity = module->repository_entity;
    char *snake = pascal_to_snake(entity);
    const char *label = fmt_str("derived query '%s'", fn->name);
    ziman_set_error_context(label);

    if (fn->type_params.count > 0) {
        ziman_error("type", fn->line, fn->col,
                    "a derived query is a query over one entity, so it has no type parameters of its own");
    }

    QueryVerb verb;
    const char *after;
    if (!split_verb(fn->name, &verb, &after)) {
        ziman_error("type", fn->line, fn->col,
                    "a derived query's name begins with 'find', 'count', 'exists' or 'delete', and '%s' "
                    "begins with none of them",
                    fn->name);
    }
    if (*after != '\0' && !starts_with(after, BY_SEP) && !starts_with(after, ORDER_SEP)) {
        if (starts_with(after, "_all_by_") || strcmp(after, "_all") == 0) {
            ziman_error("type", fn->line, fn->col,
                        "there is no '_all' in a derived query's name: how many rows it answers comes from "
                        "its return type and nowhere else, so this is '%s%s' answering a [%s]",
                        QUERY_VERBS[verb].word, after + strlen("_all"), entity);
        }
        ziman_error("type", fn->line, fn->col,
                    "after '%s' a derived query's name has '%s<predicate>', '%s<ordering>' or nothing, and "
                    "'%s' is none of them -- a name about the application rather than about %s (a 'current' "
                    "or a 'recent') is written out by hand, and a hand-written body wins",
                    QUERY_VERBS[verb].word, BY_SEP, ORDER_SEP, after, entity);
    }

    QSearch q = {0};
    q.sd = sd;
    int only_ordering = starts_with(after, ORDER_SEP);
    const char *grammar_text = *after == '\0' ? after : after + strlen(only_ordering ? ORDER_SEP : BY_SEP);
    if (*after == '\0') {
        qsearch_record(&q);
    } else if (only_ordering) {
        enum_orders(&q, grammar_text);
    } else {
        enum_terms(&q, grammar_text);
    }
    if (q.total == 0) refuse_unreadable(fn, sd, entity, grammar_text, only_ordering);
    if (q.total > 1) {
        /* **Two readings is a refusal, not a preference** (docs/design/derived-queries.md §4.1).
         * No column in the sample backend's 36 entities contains '_and_', '_in', '_not',
         * '_between', '_asc' or '_desc', so this cannot fire there -- but a column named
         * `name_and_type` makes `find_by_name_and_type` genuinely two-ways readable, and guessing
         * is how the resolution of the thing this is modelled on surprises people. */
        ziman_error("type", fn->line, fn->col,
                    "'%s' reads %d ways against %s and this will not choose between them: %s, or %s. "
                    "Rename the function, or write the query out (a hand-written body wins)",
                    fn->name, q.total, entity, render_shape(&q.found[0]), render_shape(&q.found[1]));
    }
    QShape *shape = &q.found[0];
    if (shape->order_count > 0 && verb != QV_FIND) {
        ziman_error("type", fn->line, fn->col,
                    "'%s' orders its rows and then does not answer any of them -- an ordering belongs to a "
                    "'find'",
                    fn->name);
    }

    if (fn->return_type.kind != TY_RESULT || fn->return_type.elem_err == NULL ||
        !type_is_named(*fn->return_type.elem_err, "DbError")) {
        ziman_error("type", fn->line, fn->col,
                    "a derived query answers a Result<.., DbError>, and '%s' answers %s -- '%s' pairs with %s",
                    fn->name, type_name(fn->return_type), QUERY_VERBS[verb].word, verb_returns(verb));
    }
    ReturnKind rk = classify_return(fn->return_type, entity);
    int paired = (verb == QV_FIND && (rk == RK_ENTITY || rk == RK_OPTION_ENTITY || rk == RK_LIST_ENTITY)) ||
                 (verb == QV_COUNT && rk == RK_INT) || (verb == QV_EXISTS && rk == RK_BOOL) ||
                 (verb == QV_DELETE && (rk == RK_UNIT || rk == RK_INT));
    if (!paired) {
        ziman_error("type", fn->line, fn->col,
                    "'%s' answers %s, which '%s' does not pair with -- it pairs with %s",
                    fn->name, type_name(fn->return_type), QUERY_VERBS[verb].word, verb_returns(verb));
    }

    int expected_count = 0;
    Expected *expected = expected_params(sd, shape, &expected_count);
    check_query_params(fn, entity, expected, expected_count);

    /* Everything the generated body names. The entity's own imports are the repository
     * attribute's doing (it runs first, over the same struct); what is left is a converter for a
     * column this query binds and the keyed CRUD never touched. */
    ensure_module_import(module, "eql", fn->line, fn->col);
    ensure_module_import(module, "datasource", fn->line, fn->col);
    ensure_named_import(module, "db", "DbError", fn->line, fn->col);
    ensure_named_import(module, "db", "SqlValue", fn->line, fn->col);
    for (int i = 0; i < shape->term_count; i++) {
        Param *f = entity_field(sd, shape->terms[i].column);
        import_field_converters(module, "sql", f->type, fn->line, fn->col);
    }

    /* **One private `Translated` val per derived function, named for it** (§8.3). Translating
     * costs 140-300 microseconds against 300-3,000 for a round trip, so a repository translates
     * once when it loads -- and a query the generator emitted badly then fails at startup rather
     * than on the first request that happens to read that column. */
    char *val_name = fmt_str("%s_query", fn->name);
    for (int i = 0; i < module->vals.count; i++) {
        if (strcmp(module->vals.items[i]->name, val_name) != 0) continue;
        ziman_error("type", module->vals.items[i]->line, module->vals.items[i]->col,
                    "'%s' is the name of the query val the derived query '%s' generates -- rename one of "
                    "the two",
                    val_name, fn->name);
    }

    char *sql = build_query_sql(entity, verb, shape);
    const char *conn = fn->params.items[0].name;

    CodeBuf cb;
    cb_init(&cb);
    cb_appendf(&cb, "val %s: eql.Translated = unwrap(eql.translate(eql.registry([%s_meta]), \"%s\"))\n\n",
               val_name, snake, sql);
    /* **The snippet repeats the author's own signature**, although only the body is taken out of
     * it (see `parse_and_splice_into`). It costs nothing and it is what makes `--emit-derived`
     * output a function a reader can compare with the declaration, rather than a bare block under
     * a placeholder name. The types are rendered from the signature as parsed, which is still the
     * source's own spelling: self-qualification runs after generation. */
    cb_appendf(&cb, "fn %s(", fn->name);
    for (int i = 0; i < fn->params.count; i++) {
        cb_appendf(&cb, "%s%s: %s", i > 0 ? ", " : "", fn->params.items[i].name, type_name(fn->params.items[i].type));
    }
    cb_appendf(&cb, ") -> %s {\n", type_name(fn->return_type));
    int tmp = 0;
    emit_bindings(&cb, expected, expected_count, &tmp, fn->line, fn->col);
    switch (rk) {
        case RK_LIST_ENTITY:
            cb_appendf(&cb, "    return eql.list<%s>(%s, %s, __bindings, %s_from_row)\n", entity, conn, val_name, snake);
            break;
        case RK_OPTION_ENTITY:
            cb_appendf(&cb, "    return eql.one<%s>(%s, %s, __bindings, %s_from_row)\n", entity, conn, val_name, snake);
            break;
        case RK_ENTITY:
            cb_appendf(&cb, "    val __found = try eql.one<%s>(%s, %s, __bindings, %s_from_row)\n", entity, conn, val_name, snake);
            cb_appendf(&cb, "    if Some(__row) = __found {\n        return __row\n    }\n");
            cb_appendf(&cb, "    return err(%s)\n", not_found_expr(module, sd, shape, fn));
            break;
        case RK_INT:
            if (verb == QV_COUNT) {
                cb_appendf(&cb, "    return eql.count(%s, %s, __bindings)\n", conn, val_name);
            } else {
                cb_appendf(&cb, "    return eql.execute(%s, %s, __bindings)\n", conn, val_name);
            }
            break;
        case RK_BOOL:
            cb_appendf(&cb, "    val __matching = try eql.count(%s, %s, __bindings)\n", conn, val_name);
            cb_appendf(&cb, "    return __matching > 0\n");
            break;
        case RK_UNIT:
            cb_appendf(&cb, "    val __deleted = try eql.execute(%s, %s, __bindings)\n", conn, val_name);
            cb_appendf(&cb, "    if __deleted == 0 {\n");
            cb_appendf(&cb, "        return err(%s)\n", not_found_expr(module, sd, shape, fn));
            cb_appendf(&cb, "    }\n    return unit\n");
            break;
        case RK_OTHER:
            break;
    }
    cb_appendf(&cb, "}\n");

    parse_and_splice_into(module, cb.data, fn->line, fn->col, label, fn);
    ziman_set_error_context(NULL);
}

void generate_derived_queries(Module *module, StructDecl *sd) {
    for (volatile int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        if (!fn->derived_query) continue;
        jmp_buf recover;
        ziman_recovery_push(&recover);
        if (setjmp(recover) != 0) {
            ziman_set_error_context(NULL);
            continue;
        }
        if (sd == NULL) {
            ziman_error("type", fn->line, fn->col,
                        "#[repository] fills a function's body from the module's entity, and this module "
                        "declares none -- a repository says which entity it is over with the module "
                        "attribute '#![repository(Entity)]', before its first declaration");
        }
        generate_one_derived_query(module, sd, fn);
        ziman_recovery_pop();
    }
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
            /* `Reflect` prints the reified declaration and generates nothing -- the test
             * instrument for this boundary, see lib/core/derive_reflect.zm. It takes no part in
             * the restrictions below because its whole job is to accept whatever arrives:
             * a generic struct, a field type another target refuses, an enum. */
            int is_reflect = strcmp(sd->derives[d], "Reflect") == 0;
            if (is_reflect) {
                parse_and_splice(module, generated_in_ziman("Reflect", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, fmt_str("derive(Reflect) for '%s'", sd->name));
                ziman_recovery_pop();
                continue;
            }
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
                                 generated_in_ziman("Entity", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_ord) {
                if (sd->fields.count == 0) {
                    ziman_error("syntax", sd->line, sd->col, "derive(Ord) needs at least one field to compare ('%s' has none)", sd->name);
                }
                /* **The first derive answered by a generator written in Ziman** (F225), and
                 * still held to the bytes the C emitted by `tests/lang/derive_ord_in_ziman`. */
                parse_and_splice(module, generated_in_ziman("Ord", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_props) {
                parse_and_splice(module, generated_in_ziman("Properties", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_json) {
                parse_and_splice(module, generated_in_ziman("Json", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else {
                /* One call for both functions, and `tests/lang/derive_row_in_ziman` still holds
                 * them to the bytes the C pair emitted. */
                parse_and_splice(module, generated_in_ziman("Row", sd, NULL, derive_arg(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
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
            if (strcmp(ed->derives[d], "Reflect") == 0) {
                parse_and_splice(module, generated_in_ziman("Reflect", NULL, ed, NULL, ed->line, ed->col),
                                 ed->line, ed->col, fmt_str("derive(Reflect) for '%s'", ed->name));
                ziman_recovery_pop();
                continue;
            }
            if (strcmp(ed->derives[d], "Properties") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Properties) does not support an enum ('%s') -- configuration binds to a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Ord") == 0) {
                const char *ord_label = fmt_str("derive(Ord) for '%s'", ed->name);
                parse_and_splice(module, generated_in_ziman("Ord", NULL, ed, NULL, ed->line, ed->col),
                                 ed->line, ed->col, ord_label);
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
            parse_and_splice(module, generated_in_ziman("Json", NULL, ed, NULL, ed->line, ed->col),
                             ed->line, ed->col, label);
                ziman_recovery_pop();
}
    }
}
