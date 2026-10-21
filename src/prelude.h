#ifndef ZIMAN_PRELUDE_H
#define ZIMAN_PRELUDE_H

#include <string.h>

#include "value.h"

/* Ziman's prelude: everything declared in lib/prelude/<part>.zm -- native
 * functions, but also ordinary Ziman types/functions/vals -- is always
 * available, bare and unqualified, in every program, no `use` needed,
 * unlike every other module (`fs`, which needs `use fs` and
 * `fs.read_text(...)`; even a bundled core module like `collections`
 * still needs `use collections`). This is a deliberate, narrow exception
 * to "every cross-module reference is qualified": the native functions
 * here were already unqualified core builtins before they moved into
 * this file (parse_int, floor/ceil/round, concat, substring, split,
 * trim, to_upper, to_lower, plus a few added alongside bigint/decimal
 * that never had a hardcoded home to begin with) -- moving them here,
 * instead of leaving them hardcoded in typecheck.c/interpreter.c
 * forever, is what actually lets that dispatch machinery stop growing
 * every time a new scalar type shows up. `Option<T>` lives here for the
 * same "always needed, never worth a `use`" reason, despite being
 * ordinary Ziman source (an enum + one helper function) rather than
 * native code -- see lib/prelude/prelude.zm's own header comment for
 * why these are the files in corelib that are never `use`-able, and
 * docs/DESIGN.md's "Prelude" entry for the fuller reasoning.
 *
 * Mechanically, this is not a new loading concept -- each part's
 * source is parsed exactly like any other module and the declarations
 * (functions, structs, enums, global vals) of all of them are merged
 * directly into every program's flat Module, the same way the entry
 * module's own declarations are (see loader.c's load_module_graph) --
 * just always, and never self-qualified (so `parse_int`, not
 * `prelude::parse_int`), since nothing ever addresses them through a
 * qualifier. Everything downstream (type-checking a call, calling a
 * native function at runtime, resolving a bare enum reference) already
 * works generically regardless of which module a declaration came from
 * -- see native.h -- so no new machinery was needed there either. */

/* The prelude is several files, not one: lib/prelude/<part>.zm, each embedded
 * by the Makefile's prelude rule and listed in prelude.c's PARTS table.
 * They are split by the type their names are about, with one exception --
 * a name whose declarations would otherwise be spread over two or more
 * parts lives whole in collections.zm, so that an overload set is never
 * split across files and `--api builtins` can be read a part at a time.
 *
 * Each part is lexed separately, so a diagnostic inside the prelude gets
 * that part's own line numbers; prelude_part_name() is what it is
 * registered under for the source excerpt, and is a real repo path. */
int prelude_part_count(void);
const char *prelude_part_name(int i);
const char *prelude_part_source(int i);

/* Registers every prelude function's C implementation under its bare
 * name (native_register, from native.h) -- call once at startup, before
 * any Ziman program runs, same as native_io_register(). */
void prelude_register(void);

/* A MutableArray's length, and one of its elements -- what `for x in builder` needs from the interpreter
 * (see exec_stmt's STMT_FOR). Both take the builder's own thread-ownership check, as every other call on one
 * does, so a builder read from the wrong thread fails there rather than here. */
int ziman_array_builder_length(Value ab);
Value ziman_array_builder_at(Value ab, int i);
/* `b[i]` and `b[i] = value`, bounds-checked. Called from interpreter.c's EXPR_INDEX and
 * STMT_INDEX_ASSIGN, not registered as natives: a MutableArray's element access is an index
 * expression, not a function with a name (F209). */
Value ziman_mutable_array_index_get(Value ab, int64_t i, int line, int col);
void ziman_mutable_array_index_set(Value ab, int64_t i, Value value, int line, int col);

/* The length of any of the four builders, for the bare `len` builtin: a MutableArray's or
 * MutableString's element/byte count, a MutableMap's entries or a MutableSet's elements. Only
 * called once the typechecker has established the argument is one of them. */
/* Which of the five mutable types a value is, in one cheap test instead of a run of `strcmp`s.
 *
 * They are all named `Mutable`+suffix (`Mutable`, `MutableString`, `MutableArray`, `MutableMap`,
 * `MutableSet`), which is exactly why the obvious dispatch is slow: every one of those names
 * shares a seven-character prefix, so a `strcmp` against any of them compares eight bytes before
 * it can differ, and F140's trick of guarding on the first character is useless here because they
 * all start with `M`. `contains` on a `MutableMap` was paying three such comparisons -- one in
 * `call_contains` to rule out the `Mutable` cell, then two more inside `ziman_builder_contains` to
 * rule out `MutableArray` and `MutableString` -- which measured ~25ns against the equivalent
 * `mutable_map_get_or`, a native that does no type dispatch at all.
 *
 * The suffixes diverge at index 7, so one switch there picks a single candidate, and a `strcmp` of
 * the short suffix confirms it. Confirming matters: it keeps this exact, so a user struct named
 * `MutableMapOfThings` answers NONE rather than being taken for a `MutableMap`, which a bare
 * `n[7] == 'M'` test would get wrong. `strncmp` rather than `memcmp` for the prefix, since a
 * struct named `Row` is only three bytes long and `memcmp` would read past it. */
typedef enum {
    MUTABLE_NONE = 0, /* not one of the five */
    MUTABLE_CELL,     /* Mutable<T> */
    MUTABLE_STRING,
    MUTABLE_ARRAY,
    MUTABLE_MAP,
    MUTABLE_SET
} MutableKind;

static inline MutableKind ziman_mutable_kind(const Type *t) {
    if (t->kind != TY_STRUCT || t->struct_name == NULL) return MUTABLE_NONE;
    const char *n = t->struct_name;
    if (strncmp(n, "Mutable", 7) != 0) return MUTABLE_NONE;
    switch (n[7]) {
        case '\0': return MUTABLE_CELL;
        case 'A': return strcmp(n + 7, "Array") == 0 ? MUTABLE_ARRAY : MUTABLE_NONE;
        case 'M': return strcmp(n + 7, "Map") == 0 ? MUTABLE_MAP : MUTABLE_NONE;
        case 'S':
            if (strcmp(n + 7, "String") == 0) return MUTABLE_STRING;
            return strcmp(n + 7, "Set") == 0 ? MUTABLE_SET : MUTABLE_NONE;
        default: return MUTABLE_NONE;
    }
}

int ziman_builder_length(Value v, MutableKind kind);

/* Whether a builder holds `needle`: a substring of a MutableString, a key of a MutableMap, an
 * element of a MutableSet, or -- the one O(n) case -- an element of a MutableArray. Only called
 * once the typechecker has established the argument types. */
int ziman_builder_contains(Value v, Value needle, MutableKind kind);

/* A builder's contents copied out as the immutable value it corresponds to: an `[T]`, a
 * `Map<K, V>`, a `Set<T>` or a `string`. The caller sets the result's type from the call's
 * resolved type, as the per-type natives behind it already require. */
Value ziman_builder_snapshot(Value v);

/* Empties a builder in place, leaving it usable: the buffer kinds reset their length, the two trie
 * kinds take a fresh empty trie so that a snapshot taken earlier is undisturbed. */
void ziman_builder_clear(Value v);

/* Whether a `Mutable<T>` cell holds `needle`, for the bare `contains`. A cell's `len` is 1 without
 * asking, so there is no accessor for it. */
int ziman_cell_contains(Value c, Value needle);

/* A MutableMap's or MutableSet's entries in insertion order, and how many: what `for k, v in m`
 * walks. `vals` may be NULL for a set. */
int ziman_mutable_map_entries(Value m, Value **keys, Value **vals);

/* The `from` constructors, which take either world: a handle holding what the argument holds. The
 * typechecker has established which kind `source` is, so `source_is_handle` says so rather than
 * these guessing from the type again. */
Value ziman_mutable_array_from(Value source, int source_is_handle);
Value ziman_mutable_array_lit(const Value *items, int n);
Value ziman_slice_array(Value a, int64_t start, int64_t end, int line, int col);
Value ziman_slice_mutable_array(Value b, int64_t start, int64_t end, int line, int col);
Value ziman_narrow_mutable_array(Value b, int64_t start, int64_t end, int line, int col);
Value ziman_swap_mutable_array(Value b, int64_t i, int64_t j, int line, int col);
Value ziman_concat_strings(const ArrayObj *pieces);
Value ziman_concat_arrays(const ArrayObj *parts);
Value ziman_mutable_map_from(Value source, int is_set);
Value ziman_mutable_set_from_array(Value source, int source_is_handle);

/* The operations that take a second collection from either world. `op` for combine is 0 union,
 * 1 intersect, 2 difference. Each handles self-aliasing, and the three answers differ: appending a
 * handle to itself doubles it, setting a map from itself is a no-op, and differencing a set with
 * itself empties it. */
void ziman_mutable_array_push_all(Value xs, Value source, int source_is_handle);
/* `push`'s MutableArray implementation and its one-element fast path -- the symbol keeps the
 * world prefix the declaration gave up. See the comment on the latter in
 * prelude.c. Both are reached by pointer identity from interpreter.c, not by name. */
Value ziman_mutable_array_push(Value *args, int argc, int line, int col);
void ziman_mutable_array_push_one(Value ab, Value element);
/* `push`'s array half and its one-value shortcut, the twins of the two above. Both are reached by
 * pointer identity from interpreter.c, not by name. */
Value ziman_array_push_values(Value *args, int argc, int line, int col);
Value ziman_array_push_one(Value arr, Value x);
void ziman_mutable_map_set_all(Value m, Value source, int source_is_handle);
void ziman_mutable_set_combine(Value s, Value source, int source_is_handle, int op);
int ziman_mutable_set_is_subset(Value s, Value source, int source_is_handle);

#endif
