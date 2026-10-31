#ifndef ZIMAN_GENERATORS_H
#define ZIMAN_GENERATORS_H

#include "ast.h"

/* The bridge from `#[derive(X)]` to a generator written in Ziman.
 *
 * `lib/core/derive_*.zm` and `lib/db/derive_*.zm` are the generators -- the three that write
 * SQL live beside `eql`; each declares `#![generator(X)]` and exports
 * `generate(d: meta.Decl) -> Result<string, string>`. This loads them as a program of their own,
 * initialises their globals, reifies a declaration as a `meta.Decl` and calls one -- answering
 * the source text to splice, exactly as a C generator's `cb.data` is.
 *
 * **The generator program is loaded, checked and initialised before the user program's load
 * begins**, which is what makes one `g_global_env` enough: this is sequential, not re-entrant.
 * See ziman_init_module_globals (interpreter.h) and docs/design/user-generators.md §6.
 *
 * Answers NULL when no Ziman generator claims `target`, which is how the four C generators keep
 * running while the port finishes. A generator that *refuses* or *fails* raises a `ziman_error`
 * at `line`/`col` -- the declaration carrying the attribute -- rather than answering NULL. */
char *ziman_generate_in_ziman(Module *module, const char *target, StructDecl *sd, EnumDecl *ed, const FieldAttr *attr, int line, int col);

/* Whether any Ziman generator claims `target`, without running it: `derive.c` asks before
 * deciding which path a derive takes. */
int ziman_has_ziman_generator(Module *module, const char *target);

/* ---- the second generator kind: a module generator ----------------------------------------
 *
 * `#![repository(Entity)]` is not a derive (docs/design/meta.md §8). It is a module attribute
 * naming a struct declared in *another file*, and what it generates depends on what the current
 * module already defines -- none of which is a function of one declaration, the only shape
 * `generate(d: Decl)` has. So it has its own input type and its own entry points:
 *
 *     pub fn generate_module(t: meta.Target) -> Result<string, string>
 *     pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>
 *
 *     pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>
 *
 * The caller assembles a `ZimanTarget` from the module and the entity it named; this reifies it
 * as a `meta.Target` and calls one. **The per-function loop stays in the caller**, so that a
 * refused derived query is reported at its own line and three of them are three errors. */
typedef struct {
    /* The entity the attribute named, found wherever it is declared. */
    StructDecl *entity;
    /* `#![repository(E, label: "..")]`'s label, or NULL for an attribute that carried none. */
    const char *label;
    /* 0 for `crud: false`: name the entity and generate none of the nine. */
    int crud;
    /* The functions the module already defines. A hand-written one wins. */
    const char **defined;
    int defined_count;
} ZimanTarget;

/* The declarations the attribute itself generates (the keyed CRUD), or NULL when the generator
 * module did not load. A refusal raises at `line`/`col` -- the module attribute. */
char *ziman_generate_module_in_ziman(const ZimanTarget *t, int line, int col);

/* A module generator of a project's own: `meta.ModuleTarget` reified -- the attribute's arguments,
 * the module's functions (each carrying its own attributes), and the names it already defines.
 * Unlike `ZimanTarget` this is not entity-shaped, which is the whole point (user-generators.md
 * §5c): a generator over routes, or a client over an interface, has no entity. */
typedef struct {
    const char *name;             /* the module's own name, no folder and no `.zm` */
    const FieldAttrArg *args;     /* `#![derive(X(a: 1))]`'s arguments; NULL when written bare */
    int arg_count;
    FunctionDecl **functions;
    int function_count;
    const char **defined;
    int defined_count;
} ZimanModuleTarget;

/* Runs `<generator>::generate_module` for a project's own module target, where `generator` is the
 * module the target name resolved to through the importing module's `use` list -- the same
 * resolution a declaration target goes through (F254). NULL when no generator claims the name. */
char *ziman_generate_module_for(Module *module, const char *target, const ZimanModuleTarget *mt,
                                int line, int col);

/* One bodyless `pub fn`'s body, from its name read against the entity. **Called once per such
 * function, and that is the point**: a refusal raises at `fn`'s own position, so an unreadable
 * name is reported where it is written and three of them in one file are three errors. */
char *ziman_generate_query_in_ziman(const ZimanTarget *t, FunctionDecl *fn, int line, int col);

#endif

/* One field attribute a generator declares: `meta.AttrSpec` reified. `args` is the spec grammar
 * `name:kind,other:kind?`; see lib/core/meta.zm's `AttrSpec`. */
typedef struct {
    const char *name;
    const char *args;
    /* Which declaration may carry it: "field" or "function". NULL when the generator's `attributes()`
     * did not say, which means a field -- the only position there was before 2026-10-31. */
    const char *on;
} ZimanAttrSpec;

/* Every field attribute the loaded generators declare, collected once. Loads the generators, so
 * a caller that has no field attribute to check should not call it. */
const ZimanAttrSpec *ziman_generator_attr_specs(int *count);

/* The field attributes `module` may carry: the bundled generators' union plus the ones declared by
 * every generator in its own `use` list. Refuses a name two imported generators both claim. See
 * the implementation's comment and docs/design/user-generators.md §5a. */
const ZimanAttrSpec *ziman_module_attr_specs(Module *module, int *count);
