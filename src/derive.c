#include "derive.h"
#include "generators.h"
#include "prelude.h"

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
 * **No code is generated in this file any more.** The growable buffer that lived here existed
 * because every generator was a string builder (`declaration -> char *`); the last of them is
 * `lib/db/derive_repository.zm` as of F229, and what this file does now is find the declaration
 * a generator needs, ensure the imports its text names, and splice what comes back. `fmt_str`
 * below survives because messages and a handful of import names are still formatted here.
 * --------------------------------------------------------------------- */

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
 * `local_date_to_json`. See docs/design/journal/'s "Attributes and derive"
 * entry. */
char *ziman_pascal_to_snake(const char *name);
/* The prelude's `snake_case`, which is where this algorithm lives now: it was written twice, here
 * and in `meta.zm`, agreeing by inspection. See `ziman_snake_case` in src/prelude.c. */
static char *pascal_to_snake(const char *name) {
    return ziman_snake_case(name);
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
 * message function (docs/design/journal/'s "A unified error_message builtin" entry). Same
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
/* **Still here with its only reader now being the import rule.** The emitter that shared this
 * table is gone -- `derive_row`'s `flat_names` is the Ziman copy, and the generated text comes
 * from there -- but `import_field_converters` below has to import exactly the names that text
 * calls, and those two have to agree. That agreement is the one thing in this file that is
 * duplicated in Ziman on purpose, and the reason it is survivable is that a disagreement is a
 * missing import, which is an error at the deriving declaration rather than a wrong answer. */
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

/* `target` is the one caller that does not want a *new* declaration: a `#[derive(Repository)]` derived
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



static const char *field_attr_arg(const FieldAttr *a, const char *name) {
    for (int i = 0; i < a->arg_count; i++) {
        if (strcmp(a->args[i].name, name) == 0) return a->args[i].value;
    }
    return NULL;
}

/* Whether `text` is a whole number as the `int` kind means it, so `#[min("8")]` is refused rather
 * than read as the string "8". A leading `-` is allowed: `#[min(-1)]` is a sensible bound. */
static int is_whole_number(const char *text) {
    if (text == NULL || *text == '\0') return 0;
    const char *at = text;
    if (*at == '-') at++;
    if (*at == '\0') return 0;
    for (; *at != '\0'; at++) {
        if (*at < '0' || *at > '9') return 0;
    }
    return 1;
}

/* The spec for `name`, or NULL when the attribute is not one a field may carry. */

/* The spec list the current module's check is running against -- set by
 * `ziman_check_field_attributes` for the duration of its pass. A file-static rather than a
 * parameter because `check_field_attribute` is reached through two layers of message building
 * that have no business carrying it. */
static const ZimanAttrSpec *g_specs = NULL;
static int g_spec_count = 0;

/* Which declaration a spec is for. NULL means a field: the only position attributes had before
 * 2026-10-31, so a generator that does not say means what it has always meant. */
static int spec_is_for(const ZimanAttrSpec *spec, const char *position) {
    const char *on = spec->on != NULL ? spec->on : "field";
    return strcmp(on, position) == 0;
}

static const char *field_attr_spec(const char *name, const char *position) {
    int n = g_spec_count;
    const ZimanAttrSpec *specs = g_specs;
    for (int i = 0; i < n; i++) {
        if (strcmp(specs[i].name, name) == 0 && spec_is_for(&specs[i], position)) return specs[i].args;
    }
    return NULL;
}

/* A spec as a message reads it: `min:int?,max:int?` becomes `min: <number>, max: <number>`. The
 * `?` is dropped, because "it takes" is a list of what is accepted rather than of what is
 * required, and the one that *is* required says so in its own refusal. */
static char *field_attr_spec_text(const char *spec) {
    char *out = ziman_xmalloc(strlen(spec) * 4 + 2);
    out[0] = '\0';
    const char *at = spec;
    int first = 1;
    while (*at != '\0') {
        const char *comma = strchr(at, ',');
        size_t len = comma != NULL ? (size_t)(comma - at) : strlen(at);
        const char *colon = memchr(at, ':', len);
        if (colon != NULL) {
            if (!first) strcat(out, ", ");
            first = 0;
            size_t name_len = (size_t)(colon - at);
            strncat(out, at, name_len);
            const char *kind = colon + 1;
            strcat(out, strncmp(kind, "int", 3) == 0 ? ": <number>"
                      : strncmp(kind, "name", 4) == 0 ? ": <name>"
                      : strncmp(kind, "bool", 4) == 0 ? ": true|false"
                      : ": \"<text>\"");
        }
        at = comma != NULL ? comma + 1 : at + len;
    }
    return out;
}

/* The names a field may carry, in the order the generators declare them, for the refusal. */
static char *field_attr_names(const char *position) {
    int count = g_spec_count;
    const ZimanAttrSpec *specs = g_specs;
    size_t n = 1;
    for (int i = 0; i < count; i++) n += strlen(specs[i].name) + 2;
    char *out = ziman_xmalloc(n);
    out[0] = '\0';
    int shown = 0;
    for (int i = 0; i < count; i++) {
        if (!spec_is_for(&specs[i], position)) continue;
        if (shown++ > 0) strcat(out, ", ");
        strcat(out, specs[i].name);
    }
    if (shown == 0) out[0] = '\0';   /* the caller words the empty case; see check_field_attribute */
    return out;
}

/* Checks one attribute against its spec: the name exists, every argument it was given is one the
 * spec names and of the right kind, and every argument the spec requires was given. */
static void check_field_attribute(const FieldAttr *a, const char *position) {
    const char *spec = field_attr_spec(a->name, position);
    if (spec == NULL) {
        /* Reported against the position it was written in, which is the whole point of the spec
         * carrying one: `#[not_blank]` on a function and `#[route]` on a field are each a name
         * that exists, in a place it does not. */
        const char *where = strcmp(position, "field") == 0 ? "struct field" : position;
        char *known = field_attr_names(position);
        if (*known == '\0') {
            /* Nothing is declared for this position at all, so "takes one of: " has nothing to
             * follow it. The useful thing to say is which generator would have to declare it. */
            ziman_error("syntax", a->line, a->col,
                        "unknown %s attribute '%s' -- no generator this module imports declares an "
                        "attribute a %s may carry, so add the `use` for the generator that does",
                        position, a->name, where);
        }
        ziman_error("syntax", a->line, a->col,
                    "unknown %s attribute '%s' -- a %s takes one of: %s",
                    position, a->name, where, known);
        return;
    }
    /* Every argument given is named by the spec, and is of the kind it says. */
    for (int i = 0; i < a->arg_count; i++) {
        const FieldAttrArg *given = &a->args[i];
        char wanted[64];
        snprintf(wanted, sizeof wanted, "%s:", given->name);
        const char *at = strstr(spec, wanted);
        int at_start = at != NULL && (at == spec || *(at - 1) == ',');
        if (!at_start) {
            if (*spec == '\0') {
                ziman_error("syntax", given->line, given->col,
                            "'#[%s]' takes no arguments", a->name);
            } else if (strcmp(given->name, "value") == 0) {
                /* Written positionally -- `#[column("x")]` -- where this attribute names its
                 * arguments. Saying "no argument 'value'" would be true and unhelpful. */
                ziman_error("syntax", given->line, given->col,
                            "'#[%s]' takes named arguments: %s", a->name, field_attr_spec_text(spec));
            } else {
                ziman_error("syntax", given->line, given->col,
                            "'#[%s]' has no argument '%s' -- it takes: %s", a->name, given->name,
                            field_attr_spec_text(spec));
            }
            continue;
        }
        /* The spec's kind against the kind it was *written* as, not against its text: see
         * `FieldAttrArg.kind`. */
        const char *spec_kind = at + strlen(wanted);
        const char *wanted_kind = strncmp(spec_kind, "int", 3) == 0 ? "int"
                                : strncmp(spec_kind, "name", 4) == 0 ? "name"
                                : strncmp(spec_kind, "bool", 4) == 0 ? "bool"
                                : "string";
        if (strcmp(given->kind, wanted_kind) != 0) {
            const char *as_written = strcmp(given->kind, "string") == 0 ? "a string"
                                   : strcmp(given->kind, "int") == 0 ? "a number"
                                   : strcmp(given->kind, "bool") == 0 ? "true or false"
                                   : "a name";
            const char *needed = strcmp(wanted_kind, "int") == 0 ? "a whole number"
                               : strcmp(wanted_kind, "bool") == 0 ? "true or false"
                               : strcmp(wanted_kind, "name") == 0
                                     ? "a name -- the module's own val, which the generated code reads"
                                     : "a string in double quotes";
            ziman_error("syntax", given->line, given->col,
                        "'#[%s]'s '%s' is %s and has to be %s", a->name, given->name, as_written, needed);
        }
        if (strcmp(wanted_kind, "int") == 0 && !is_whole_number(given->value)) {
            ziman_error("syntax", given->line, given->col,
                        "'#[%s]'s '%s' is a whole number", a->name, given->name);
        }
        if (strcmp(wanted_kind, "string") == 0 && given->value[0] == '\0') {
            ziman_error("syntax", given->line, given->col,
                        "'#[%s(%s: \"\")]' names nothing", a->name, given->name);
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(a->args[j].name, given->name) == 0) {
                ziman_error("syntax", given->line, given->col,
                            "'#[%s]' has two '%s' arguments", a->name, given->name);
            }
        }
    }
    /* Every argument the spec requires -- one without a `?` -- was given. */
    const char *at = spec;
    while (*at != '\0') {
        const char *comma = strchr(at, ',');
        size_t len = comma != NULL ? (size_t)(comma - at) : strlen(at);
        int optional = len > 0 && at[len - 1] == '?';
        const char *colon = memchr(at, ':', len);
        if (colon != NULL && !optional) {
            char name[64];
            size_t n = (size_t)(colon - at);
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, at, n);
            name[n] = '\0';
            if (field_attr_arg(a, name) == NULL) {
                ziman_error("syntax", a->line, a->col,
                            "'#[%s]' needs its '%s' argument -- it takes: %s", a->name, name,
                            field_attr_spec_text(spec));
            }
        }
        at = comma != NULL ? comma + 1 : at + len;
    }
}

/* Every field attribute in the module, against what the generators say exists.
 *
 * **This used to happen while parsing**, against a table in `src/parser.c`. It cannot any more: a
 * generator declares the attributes it owns, and no generator has loaded while a file is being
 * parsed. So it is a pass of its own, run once per module from `generate_derived_functions`, off
 * the `line`/`col` every `FieldAttr` carries -- a refusal still lands on the attribute itself.
 *
 * **The guard matters.** Collecting the specs loads the generator program, so a module with no
 * field attribute at all must not ask: the overwhelming majority of modules have none, and making
 * every one of them load five thousand lines of Ziman to learn that would be a real cost for
 * nothing. A module that *does* carry one is deriving something in practice and would load them
 * anyway.
 *
 * The parser still sees every field, which is the property the table was in C for: a misspelling
 * on a struct that derives nothing is refused here, because this walks the module's structs rather
 * than its derives. */
void ziman_check_field_attributes(Module *module) {
    int any = 0;
    for (int i = 0; i < module->structs.count && !any; i++) {
        for (int f = 0; f < module->structs.items[i]->fields.count; f++) {
            if (module->structs.items[i]->fields.items[f].attrs.count > 0) {
                any = 1;
                break;
            }
        }
    }
    /* **A function's attributes are checked by the same pass**, since 2026-10-31: the question is
     * identical -- does any generator this module imports declare this name -- and answering it in
     * two places would be two answers. Without this a misspelled `#[rout(..)]` on a function would
     * be accepted and silently read by nobody, which is exactly the hole F259 closed for every
     * other position. */
    for (int i = 0; i < module->functions.count && !any; i++) {
        if (module->functions.items[i]->attrs.count > 0) any = 1;
    }
    if (!any) return;
    int count = 0;
    /* **Per module** (§5a): the bundled generators' attributes plus the ones declared by every
     * generator this file imports, so `#[csv_column(name:)]` means something here exactly when
     * `derive/csv` is in the `use` list and nothing anywhere else. */
    const ZimanAttrSpec *specs = ziman_module_attr_specs(module, &count);
    /* No generators means no answer about what exists, and refusing every attribute would be a
     * worse lie than accepting them: the derive machinery reports its own failure to load. */
    if (specs == NULL || count == 0) return;
    g_specs = specs;
    g_spec_count = count;
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        for (int f = 0; f < sd->fields.count; f++) {
            const FieldAttrList *attrs = &sd->fields.items[f].attrs;
            for (int a = 0; a < attrs->count; a++) check_field_attribute(&attrs->items[a], "field");
        }
    }
    for (int i = 0; i < module->functions.count; i++) {
        const FieldAttrList *attrs = &module->functions.items[i]->attrs;
        for (int a = 0; a < attrs->count; a++) check_field_attribute(&attrs->items[a], "function");
    }
    g_specs = NULL;
    g_spec_count = 0;
}

static int module_binds_name(Module *module, const char *name) {
    for (int i = 0; i < module->uses.count; i++) {
        UseDecl *u = &module->uses.items[i];
        if (u->local_name != NULL && strcmp(u->local_name, name) == 0) return 1;
    }
    /* **Vals as well** (2026-10-29): this decides whether `ensure_named_import` adds an import, and
     * a hand-written `pub val <snake>_meta` was invisible to it -- so a repository over a
     * hand-written metamodel in another file would have had an import added beside the val. */
    for (int i = 0; i < module->vals.count; i++) {
        if (module->vals.items[i]->name != NULL && strcmp(module->vals.items[i]->name, name) == 0) return 1;
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
         * so `#![derive(Repository(E))]` over an entity with a `Uuid` key generated `uuid_to_sql(id)` and
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
    } else if (strcmp(target, "Validate") == 0) {
        /* Reached **qualified** -- `validate.not_blank`, `validate.Violation` -- because `email`,
         * `first`, `matches` and `positive` are names an application is entitled to have of its
         * own, and a per-name import would take them over for the whole file without the author
         * asking. The same reasoning `Json`'s tree helpers are imported by module for. */
        ensure_module_import(module, "validate", line, col);
    }
}

/* ---------------------------------------------------------------------
 * `#![derive(Repository(Entity))]`: the CRUD every repository writes the same way.
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

/* The module's context around its entity, which is both entry points' input. Assembled here
 * rather than in `generators.c` because every field of it is this feature's: a `ZimanTarget` is
 * what `#![derive(Repository(..))]` means, and the bridge only reifies it. */
static ZimanTarget target_for(Module *module, StructDecl *sd) {
    const char **defined = ziman_xmalloc(sizeof(char *) * (size_t)(module->functions.count > 0 ? module->functions.count : 1));
    int defined_count = 0;
    for (int i = 0; i < module->functions.count; i++) {
        if (module->functions.items[i]->name != NULL) defined[defined_count++] = module->functions.items[i]->name;
    }
    ZimanTarget t;
    t.entity = sd;
    t.label = module->repository_label;
    t.crud = !module->repository_no_crud;
    t.defined = defined;
    t.defined_count = defined_count;
    return t;
}

/* **A module generator of a project's own** (user-generators.md §5c), run from the loader once the
 * module's imports are resolved -- which is what makes the target name resolvable at all.
 *
 * `Repository` does not come through here: its input is a `meta.Target`, entity-shaped, and
 * `generate_repository_functions` below assembles it. That asymmetry is the one §5c predicted and
 * accepted; what it buys is that nothing else has to pretend to have an entity.
 *
 * The functions are handed over **as the module declares them**, each carrying its own `attrs`, so
 * a generator reads `#[route(..)]` off them rather than being told about routes by the compiler.
 * The compiler's whole part is: collect, call, splice. */
void generate_project_module(Module *module) {
    if (module->module_derive_target == NULL) return;
    if (strcmp(module->module_derive_target, "Repository") == 0) return;
    int line = module->module_derive_line;
    int col = module->module_derive_col;

    const char **defined = ziman_xmalloc(sizeof(char *) * (size_t)(module->functions.count > 0 ? module->functions.count : 1));
    int defined_count = 0;
    FunctionDecl **fns = ziman_xmalloc(sizeof(FunctionDecl *) * (size_t)(module->functions.count > 0 ? module->functions.count : 1));
    int fn_count = 0;
    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        if (fn->name == NULL) continue;
        /* **Not a derived function.** Generation appends to this same list, so a second pass over a
         * module would hand a generator its own output -- and `defined` exists precisely so a
         * generator can skip what is already written. */
        if (fn->derived_from != NULL) continue;
        defined[defined_count++] = fn->name;
        fns[fn_count++] = fn;
    }

    ZimanModuleTarget mt;
    mt.args = module->module_derive_args != NULL ? module->module_derive_args->args : NULL;
    mt.arg_count = module->module_derive_args != NULL ? module->module_derive_args->arg_count : 0;
    mt.functions = fns;
    mt.function_count = fn_count;
    mt.defined = defined;
    mt.defined_count = defined_count;

    char *text = ziman_generate_module_for(module, module->module_derive_target, &mt, line, col);
    if (text == NULL) {
        ziman_error("module", line, col,
                    "'#![derive(%s)]' names a module generator, and no module in this file's `use` "
                    "list exports a 'generate_module' for it -- a module generator is a module "
                    "carrying '#![generator(%s)]' with a 'pub fn generate_module(m: meta.ModuleTarget) "
                    "-> string'",
                    module->module_derive_target, module->module_derive_target);
        return;
    }
    /* Nothing to add is a legitimate answer -- a module whose functions carry no attribute the
     * generator reads. */
    if (text[0] == '\0') return;
    parse_and_splice(module, text, line, col, fmt_str("derive(%s) for this module", module->module_derive_target));
}

/* The nine, as `lib/db/derive_repository.zm` wrote them.
 *
 * **Everything above this function is still C and that is the division**: the generator answers
 * *text*, and the module it is spliced into is the compiler's own data structure -- the imports
 * the generated bodies name, the position a refusal is reported at, and the splice itself cannot
 * be done by a function whose only output is a string. The five declaration derives are split
 * exactly the same way (`import_for_derive` is C beside every one of them), which is the argument
 * for a second generator *kind* rather than a second mechanism.
 *
 * The two refusals that used to be here -- an entity with no `#[pk]`, and an `Option` key -- are
 * the generator's now: both are facts about the entity, which is what a `meta.Target` carries.
 * They reach a reader through the same path a derive's refusal does. */
void generate_repository_functions(Module *module, StructDecl *sd, const char *entity_module) {
    /* The name as the attribute wrote it, not `sd->name`: a struct from an already-loaded module
     * carries its self-qualified name (`entity/vital_sign::VitalSign`), and the generated text is
     * source, where the bare name is what resolves. */
    const char *entity = module->repository_entity;
    char *snake = pascal_to_snake(entity);
    int line = module->repository_line;
    int col = module->repository_col;

    ZimanTarget target = target_for(module, sd);

    /* **Before the imports, because a refusal belongs before them.** An entity with no key is
     * refused here exactly as it was when this function did the refusing, rather than after a
     * file has been given four imports it turns out not to need. */
    char *text = ziman_generate_module_in_ziman(&target, line, col);
    if (text == NULL) {
        ziman_error("module", line, col,
                    "#![derive(Repository(%s))] is generated by lib/db/derive_repository.zm, which did not load",
                    entity);
        return;
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
    /* **Every field, not just the `#[pk]` ones.** This loop used to skip a field whose `is_pk` was
     * unset, which made `#[pk]` the fourth annotation the compiler knew the meaning of -- and for
     * nothing it could not get structurally: which fields are keys is `meta.keys_of`'s answer, in
     * the generator, and C was duplicating that reading to decide imports. Importing a converter
     * for every field type is a superset of what the keys need, costs an unused `use` at worst
     * (`ensure_named_import` skips a name the module already binds), and leaves C with no opinion
     * about the attribute. The derived-query loop below was already written this way. */
    for (int i = 0; i < sd->fields.count; i++) {
        Param *f = &sd->fields.items[i];
        import_field_converters(module, "sql", f->type, line, col);
        /* A key's own type has to be nameable in the signature, which is a different import from
         * its converter: `delete(conn, id: Uuid)` needs `Uuid` as well as `to_sql`. */
        if (f->type.kind == TY_STRUCT || f->type.kind == TY_ENUM) {
            const char *tn = f->type.kind == TY_STRUCT ? f->type.struct_name : f->type.enum_name;
            if (tn != NULL) {
                const char *bare = bare_name_of(tn);
                for (size_t st = 0; st < sizeof STANDARD_TYPES / sizeof STANDARD_TYPES[0]; st++) {
                    if (strcmp(bare, STANDARD_TYPES[st].type) == 0) {
                        ensure_named_import(module, STANDARD_TYPES[st].module, STANDARD_TYPES[st].type, line, col);
                    }
                }
            }
        }
    }

    /* **`crud: false` has had its imports and generates nothing.** They are what every derived
     * query in the module needs too -- `eql`, `datasource`, `DbError`, `SqlValue`, the entity and
     * its `_meta`/`_from_row` -- and they are this function's work whether or not the nine are
     * wanted, which is why the generator can answer no text at all for one. */
    if (module->repository_no_crud && g_emit_derived) {
        printf("// repository(%s): 'crud: false', so none of the nine is generated\n", entity);
    }
    if (text[0] == '\0') return;
    parse_and_splice(module, text, line, col, fmt_str("repository(%s)", entity));
}

/* ---------------------------------------------------------------------
 * Derived queries: `#[derive(Repository)]` on a bodyless `pub fn`, whose **name is
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

/* Whether `t` is the named struct or enum, with both qualifiers stripped: the loader's own
 * `uuid::Uuid` and a source's `uuid.Uuid` both answer `Uuid`. The only reader left in this file is
 * the `uuid` import rule; the comparisons the derived queries used to make are the generator's
 * now, where `meta.bare_name` does the same job. */
static int type_is_named_here(Type t, const char *name) {
    const char *n = t.kind == TY_STRUCT ? t.struct_name : t.kind == TY_ENUM ? t.enum_name : NULL;
    if (n == NULL) return 0;
    const char *sep = strstr(n, "::");
    const char *bare = sep != NULL ? sep + 2 : n;
    const char *dot = strrchr(bare, '.');
    return strcmp(dot != NULL ? dot + 1 : bare, name) == 0;
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
/* A target's arguments, or NULL for one written bare. `derive_args` is parallel to `derives` and
 * may be absent entirely; each entry is the target as a `FieldAttr`, read by the same
 * `parse_attr_args` a field attribute goes through (docs/design/user-generators.md §5b). */
static const FieldAttr *derive_attr(const StructDecl *sd, int d) {
    if (sd == NULL || sd->derive_args == NULL) return NULL;
    return &sd->derive_args[d];
}

/* How many arguments a target carries. A bare `#[derive(Json)]` carries none. */
static int derive_arg_count(const StructDecl *sd, int d) {
    const FieldAttr *a = derive_attr(sd, d);
    return a != NULL ? a->arg_count : 0;
}

static char *generated_in_ziman(Module *module, const char *target, StructDecl *sd, EnumDecl *ed, const FieldAttr *attr, int line, int col) {
    char *text = ziman_generate_in_ziman(module, target, sd, ed, attr, line, col);
    if (text == NULL) {
        ziman_error("module", line, col,
                    "derive(%s) is generated by lib/core/derive_%s.zm, which did not load",
                    target, pascal_to_snake(target));
    }
    return text;
}

/* One `#[derive(Repository)]` function: its name read against the entity, its signature checked against
 * the name, and then the query val and the body it was declared without. */
/* One `#[derive(Repository)]` function: `lib/db/derive_repository.zm` reads its name against the
 * entity, checks its signature against the name, and answers the query val and the body it was
 * declared without.
 *
 * **What is left here is what a string cannot do.** The imports the body names; the one name
 * collision that is a fact about the *module* rather than about the entity; and the splice, which
 * takes the body out of the generated function and leaves the author's declaration otherwise
 * untouched. The reading of the name -- the verb, the operator suffixes, every parse of an
 * ambiguous name, the greedy walk that says where an unreadable one stopped making sense, the SQL
 * and the bindings -- is all in the generator now, which is about 700 lines of C gone.
 *
 * The one refusal that stayed is the type-parameter one: it is a fact about the declaration's own
 * shape, needs no entity, and `meta.Function` deliberately does not carry type parameters,
 * because carrying them would be a field for the sake of one check (docs/design/meta.md §12). */
static void generate_one_derived_query(Module *module, StructDecl *sd, FunctionDecl *fn,
                                       const ZimanTarget *target) {
    const char *entity = module->repository_entity;
    const char *label = fmt_str("derived query '%s'", fn->name);
    ziman_set_error_context(label);

    if (fn->type_params.count > 0) {
        ziman_error("type", fn->line, fn->col,
                    "a derived query is a query over one entity, so it has no type parameters of its own");
    }

    /* **One private `Translated` val per derived function, named for it**, so a name of that shape
     * already in the module is a collision the author has to resolve. `module->vals` is the
     * module's, not the entity's, which is why this is not the generator's question. */
    char *val_name = fmt_str("%s_query", fn->name);
    for (int i = 0; i < module->vals.count; i++) {
        if (strcmp(module->vals.items[i]->name, val_name) != 0) continue;
        ziman_error("type", module->vals.items[i]->line, module->vals.items[i]->col,
                    "'%s' is the name of the query val the derived query '%s' generates -- rename one of "
                    "the two",
                    val_name, fn->name);
    }

    char *text = ziman_generate_query_in_ziman(target, fn, fn->line, fn->col);
    if (text == NULL) {
        ziman_error("module", fn->line, fn->col,
                    "#[derive(Repository)] on '%s' is generated by lib/db/derive_repository.zm, which did not load",
                    fn->name);
        return;
    }

    /* Everything the generated body names. The entity's own imports are the repository
     * attribute's doing (it runs first, over the same struct); what is left is a converter for a
     * column this query binds and the keyed CRUD never touched, and `uuid` for a `NotFound`
     * message that renders a key.
     *
     * **Imported per entity field rather than per bound column**, because which columns a query
     * binds is now the generator's reading of the name and no longer known here. The superset is
     * every field, which is what an entity's columns are anyway: an unused `use` costs a load and
     * nothing else, and the alternative was either to keep the name grammar in C for the sake of
     * an import list or to let a generator dictate imports, which is a wider grant than text. */
    ensure_module_import(module, "eql", fn->line, fn->col);
    ensure_module_import(module, "datasource", fn->line, fn->col);
    ensure_named_import(module, "db", "DbError", fn->line, fn->col);
    ensure_named_import(module, "db", "SqlValue", fn->line, fn->col);
    for (int i = 0; i < sd->fields.count; i++) {
        import_field_converters(module, "sql", sd->fields.items[i].type, fn->line, fn->col);
        /* `uuid.format(id)` is what a `NotFound` message carries for a `Uuid` key, and that call
         * needs the module rather than the type. **Any `Uuid` field, not any `Uuid` key**: the
         * comment here used to say that keying on `#[pk]` was "as close as a structural rule gets",
         * and a type is closer -- it is a superset, and it does not ask C what an attribute means. */
        Type kt = sd->fields.items[i].type;
        if (is_option_type_here(kt)) kt = kt.type_args.items[0];
        if (type_is_named_here(kt, "Uuid")) ensure_module_import(module, "uuid", fn->line, fn->col);
    }
    (void)entity;

    parse_and_splice_into(module, text, fn->line, fn->col, label, fn);
    ziman_set_error_context(NULL);
}
void generate_derived_queries(Module *module, StructDecl *sd) {
    /* Built once for the module and handed to every call: `defined` is the module's function list
     * and does not change while the bodies are filled in, since a derived query's declaration is
     * already there and only its body is missing. */
    ZimanTarget target = sd != NULL ? target_for(module, sd) : (ZimanTarget){0};
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
                        "#[derive(Repository)] fills a function's body from the module's entity, and this module "
                        "declares none -- a repository says which entity it is over with the module "
                        "attribute '#![derive(Repository(Entity))]', before its first declaration");
        }
        generate_one_derived_query(module, sd, fn, &target);
        ziman_recovery_pop();
    }
}

void generate_derived_functions(Module *module) {
    ziman_check_field_attributes(module);
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
            int is_validate = strcmp(sd->derives[d], "Validate") == 0;
            /* `Reflect` prints the reified declaration and generates nothing -- the test
             * instrument for this boundary, see lib/core/derive_reflect.zm. It takes no part in
             * the restrictions below because its whole job is to accept whatever arrives:
             * a generic struct, a field type another target refuses, an enum. */
            int is_reflect = strcmp(sd->derives[d], "Reflect") == 0;
            if (is_reflect) {
                parse_and_splice(module, generated_in_ziman(module, "Reflect", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, fmt_str("derive(Reflect) for '%s'", sd->name));
                ziman_recovery_pop();
                continue;
            }
            /* **A target this module imports a generator for** (§3 of
             * docs/design/user-generators.md). The compiler knows six names and has no opinion
             * about a seventh: a module carrying `#![generator(Csv)]` in the `use` list is what
             * makes `#[derive(Csv)]` mean something, and none of the per-target behaviour below
             * applies to it -- no imports it did not ask for, no struct-or-enum rule of the
             * compiler's, just its text spliced where the author wrote the attribute. */
            int is_user = !is_json && !is_row && !is_props && !is_ord && !is_entity && !is_validate
                          && ziman_has_ziman_generator(module, sd->derives[d]);
            if (is_user) {
                parse_and_splice(module,
                                 generated_in_ziman(module, sd->derives[d], sd, NULL,
                                                    derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col,
                                 fmt_str("derive(%s) for '%s'", sd->derives[d], sd->name));
                ziman_recovery_pop();
                continue;
            }
            if (!is_json && !is_row && !is_props && !is_ord && !is_entity && !is_validate) {
                ziman_error("syntax", sd->line, sd->col,
                            "unknown derive target '%s' on struct '%s' -- the compiler knows Json, "
                            "Row, Properties, Ord, Entity, Reflect and Validate, and a target of "
                            "your own is a module with '#![generator(%s)]' in this file's use list",
                            sd->derives[d], sd->name, sd->derives[d]);
            }
            if (sd->type_params.count > 0 && (is_row || is_props || is_entity || is_validate)) {
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
                /* **By name, not by derive** (2026-10-29). This used to require `derive(Row)` in
                 * the list, which refused a struct whose `<snake>_from_row` was written by hand and
                 * present -- and `derive(Entity)`'s own output never calls it: the requirement is
                 * on whoever *consumes* the entity. `#![derive(Repository(..))]` checks the same way (see
                 * `check_repository_names` in loader.c), so the two now agree: anything that puts
                 * the name in scope will do.
                 *
                 * The derive list is still consulted, because `#[derive(Entity, Row)]` with
                 * `Entity` first reaches here before `Row` has spliced anything. */
                char *from_row = fmt_str("%s_from_row", pascal_to_snake(sd->name));
                int has_row = module_binds_name(module, from_row);
                for (int other = 0; other < sd->derive_count; other++) {
                    if (strcmp(sd->derives[other], "Row") == 0) has_row = 1;
                }
                if (!has_row) {
                    ziman_error("syntax", sd->line, sd->col,
                                "derive(Entity) on '%s' needs `%s` -- an entity is read from a row, and that "
                                "is what reads it: add derive(Row), or write `%s` yourself",
                                sd->name, from_row, from_row);
                }
                /* The generated `eql.entity(..)` call needs the module, and a file that wrote
                 * only the attribute has no reason to have imported it. `ensure_module_import` is
                 * what the standard-type conversions already do for `time` and `uuid`. */
                if (derive_arg_count(sd, d) > 0) {
                    ensure_module_import(module, "eql", sd->line, sd->col);
                }
                parse_and_splice(module,
                                 generated_in_ziman(module, "Entity", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_validate) {
                /* **No per-attribute check here, deliberately.** This read `#[valid]` to require
                 * the nested type's `<snake>_violations` in scope, which bought a better message
                 * ("add derive(Validate) to 'Plain'") at the price of the compiler knowing what a
                 * field attribute means -- the fourth such name, and the one with the least excuse.
                 * What is left is the generic report: the generated call does not resolve, and it
                 * is raised at this declaration naming the generator and the missing function. The
                 * good message comes back for free when a generator can declare the names its
                 * output needs (docs/design/user-generators.md §5b). */
                parse_and_splice(module, generated_in_ziman(module, "Validate", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_ord) {
                if (sd->fields.count == 0) {
                    ziman_error("syntax", sd->line, sd->col, "derive(Ord) needs at least one field to compare ('%s' has none)", sd->name);
                }
                /* **The first derive answered by a generator written in Ziman** (F225), and
                 * still held to the bytes the C emitted by `tests/lang/derive_ord_in_ziman`. */
                parse_and_splice(module, generated_in_ziman(module, "Ord", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_props) {
                parse_and_splice(module, generated_in_ziman(module, "Properties", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else if (is_json) {
                parse_and_splice(module, generated_in_ziman(module, "Json", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
                                 sd->line, sd->col, label);
            } else {
                /* One call for both functions, and `tests/lang/derive_row_in_ziman` still holds
                 * them to the bytes the C pair emitted. */
                parse_and_splice(module, generated_in_ziman(module, "Row", sd, NULL, derive_attr(sd, d), sd->line, sd->col),
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
                parse_and_splice(module, generated_in_ziman(module, "Reflect", NULL, ed, NULL, ed->line, ed->col),
                                 ed->line, ed->col, fmt_str("derive(Reflect) for '%s'", ed->name));
                ziman_recovery_pop();
                continue;
            }
            if (strcmp(ed->derives[d], "Properties") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Properties) does not support an enum ('%s') -- configuration binds to a struct", ed->name);
            }
            if (strcmp(ed->derives[d], "Validate") == 0) {
                ziman_error("syntax", ed->line, ed->col, "derive(Validate) does not support an enum ('%s') -- a constraint is written on a struct field, and an enum has none", ed->name);
            }
            if (strcmp(ed->derives[d], "Ord") == 0) {
                const char *ord_label = fmt_str("derive(Ord) for '%s'", ed->name);
                parse_and_splice(module, generated_in_ziman(module, "Ord", NULL, ed, NULL, ed->line, ed->col),
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
            parse_and_splice(module, generated_in_ziman(module, "Json", NULL, ed, NULL, ed->line, ed->col),
                             ed->line, ed->col, label);
                ziman_recovery_pop();
}
    }
}
