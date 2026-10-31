#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "derive.h"
#include "gc_boehm_roots.h"
#include "stack.h"
#include "format.h"
#include "lsp.h"
#include "native_compress.h"
#include "native_csv.h"
#include "native_json.h"
#include "native_http.h"
#include "native_bits.h"
#include "native_metrics.h"
#include "native_process.h"
#include "native_math.h"
#include "native_number.h"
#include "native_os.h"
#include "native_thread.h"
#include "coverage.h"
#include "locals.h"
#include "profile.h"
#include "error.h"
#include "interpreter.h"
#include "corelib.h"
#include "parser.h"
#include "lexer.h"
#include "loader.h"
#include "native_clock.h"
#include "native_crypto.h"
#include "native_sqlite.h"
#include "iter.h"
#include "native_fs.h"
#include "native_locale.h"
#include "native_regex.h"
#include "native_net.h"
#include "native_os.h"
#include "native_random.h"
#include "prelude.h"
#include "typecheck.h"
#include "xalloc.h"

#ifndef ZIMAN_VERSION
#define ZIMAN_VERSION "dev"
#endif

static void usage(FILE *out) {
    fputs("usage: ziman [options] <file.zm>\n"
          "       ziman test [options] <file.zm> [file2.zm ...]\n"
          "\n"
          "  test             run every #[test] function in the given files (see\n"
          "                   docs/syntax/ \"Testing\"); PASS/FAIL per test, then a\n"
          "                   summary; exit 1 if any failed. Takes --coverage,\n"
          "                   --coverage-lcov=F and --gc-stress, on either side of `test`\n"
          "  --api [module]   what a module offers: its `pub` types and function signatures, in\n"
        "                   the language's own notation. With no module, lists the bundled\n"
        "                   ones. Takes a bundled name (`--api fs`) or a path to a .zm file,\n"
        "                   so it works on your own modules too\n"
        "  --check          load and type-check only; do not run the program\n"
          "  --json           report errors as one JSON object per error on stderr\n"
          "                   (with --check, print {\"ok\":true} on success)\n"
          "  --lint           report style findings (a `var` never reassigned) in your own\n"
          "                   code instead of running it; exit 1 if there are any\n"
          "  --lint-bundled   as --lint, and lint the bundled library modules too. Point it at\n"
          "                   a program that imports them: pointing --lint at lib/x.zm loads\n"
          "                   that module as an entry, where its names go unqualified\n"
          "  --coverage       run, then report which lines of your own code did not\n"
          "                   execute; exit 1 if any did not\n"
          "  --coverage-lcov=F  also write the coverage to F in lcov's .info format (for\n"
          "                   genhtml, Codecov and the rest); implies --coverage. An existing\n"
          "                   F is merged into, so several runs into one path accumulate --\n"
          "                   delete it to start fresh\n"
          "  --profile        run, then report which functions the time went to, hottest\n"
          "                   first (CPU-time sampling at 1kHz, self time)\n"
          "  --locals         on a runtime error, show each stack frame's variables and\n"
          "                   their values under it\n"
          "  --lsp            run as a Language Server on stdin/stdout (diagnostics, formatting,\n"
          "                   symbols, go to definition, hover, completion)\n"
          "  -- args...       everything after `--` is the program's own arguments (os.args())\n"
          "  --fmt            print the source formatted (several files allowed, `-` is\n"
          "                   stdin); with --write rewrite the files in place, with\n"
          "                   --check list the files that would change and exit 1;\n"
          "                   a file it refuses (nesting past 200 brackets) is printed back\n"
          "                   unchanged, with the reason on stderr and exit 2, so filtering a\n"
          "                   buffer through `--fmt -` never empties it;\n"
          "                   --width=N wraps longer lines (default 120, 0 = never);\n"
          "                   --canonical also moves an opening brace onto the line of what it\n"
          "                   opens, and `else` onto the line of the brace that closed its `if`\n"
          "  --emit-derived   print the code each #[derive] generates, then continue\n"
          "  --gc-stress      collect at every safe point instead of on a growth threshold:\n"
          "                   turns a value the collector cannot see into an immediate failure\n"
          "                   rather than one that needs a large program to show up\n"
          "  --stack-mb=N     the main thread's stack in MB (1-768; default: computed from the memory available)\n"
          "  --thread-stack-mb=N  the stack of each `spawn`ed thread in MB (default: computed)\n"
          "  --threads=N      how many threads to plan stacks for (default 64)\n"
          "                   (the environment variables ZIMAN_STACK_MB, ZIMAN_THREAD_STACK_MB and\n"
          "                   ZIMAN_THREADS do the same; a flag wins)\n"
          "  --version        print the version and exit\n"
          "  --help           print this help and exit\n", out);
}

static char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = ziman_xmalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static char *read_stdin(size_t *len) {
    size_t cap = 4096, n = 0;
    char *buf = ziman_xmalloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, stdin)) > 0) {
        n += got;
        if (n + 1 >= cap) { cap *= 2; buf = ziman_xrealloc(buf, cap); }
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* `ziman --fmt [--width=N] [--canonical] [--write | --check] files...`; a file named `-` is stdin
 * (printed to stdout). A file the formatter refuses is printed back unchanged with the reason on
 * stderr and exit 2, so a buffer filtered through `--fmt -` survives a refusal even if the caller
 * ignores the status (see run_format, and docs/reviews/tasks/ F17). */
static int run_format(char **files, int n_files, int write, int check, int width, int canonical) {
    int changed = 0, failed = 0;
    for (int i = 0; i < n_files; i++) {
        size_t len;
        int from_stdin = strcmp(files[i], "-") == 0;
        char *src = from_stdin ? read_stdin(&len) : read_whole(files[i], &len);
        if (src == NULL) { fprintf(stderr, "ziman: cannot open '%s'\n", files[i]); failed = 1; continue; }
        char err[256];
        char *out = format_source(src, len, width, canonical, err, sizeof err);
        if (out == NULL) {
            fprintf(stderr, "%s: %s\n", files[i], err);
            failed = 1;
            /* The input, unchanged, on the path that would otherwise print the formatted source.
             * `--fmt -` is a documented stdin filter, and an editor that pipes a buffer through it
             * and replaces the buffer with stdout -- without checking the exit status, which is the
             * common shape -- would otherwise blank the file (docs/reviews/tasks/ F17). A careful
             * caller still sees exit 2 and the message on stderr; a careless one gets its text
             * back. Nothing is printed under --check, whose output is a list of filenames, and
             * --write already leaves the file alone. */
            if (!check && !write) fwrite(src, 1, len, stdout);
            free(src);
            continue;
        }
        int differs = strlen(out) != len || memcmp(out, src, len) != 0;
        if (differs) changed++;
        if (check) {
            if (differs) printf("%s\n", files[i]);
        } else if (write && !from_stdin) {
            if (differs) {
                FILE *f = fopen(files[i], "wb");
                if (f == NULL) { fprintf(stderr, "ziman: cannot write '%s'\n", files[i]); failed = 1; }
                else { fwrite(out, 1, strlen(out), f); fclose(f); }
            }
        } else {
            fputs(out, stdout);
        }
        free(out);
        free(src);
    }
    if (failed) return 2;
    return check && changed > 0 ? 1 : 0;
}

/* Every native module a `.zm` program can `use`, registered once before
 * loading/running anything -- shared by ordinary execution, `--lsp` and
 * `ziman test`, so the three can never drift out of sync with each other. */
static void register_all_natives(void) {
    native_fs_register();
    native_iter_register();
    native_locale_register();
    native_regex_register();
    native_random_register();
    native_clock_register();
    native_sqlite_register();
    native_net_register();
    native_os_register();
    native_crypto_register();
    native_number_register();
    native_math_register();
    native_bits_register();
    native_metrics_register();
    native_process_register();
    native_http_register();
    native_thread_register();
    native_csv_register();
    native_json_register();
    native_compress_register();
    prelude_register();
}

/* Everything `ziman file.zm` does after the options: load, check, and run. It runs on a thread with a stack sized for deep recursion (see stack.h). */
typedef struct {
    const char *file;
    int json, lint, check_only, coverage, profile, locals;
    const char *coverage_lcov; /* NULL unless --coverage-lcov=PATH was given */
} RunOptions;

/* `--lsp` on a thread of its own, for ziman_run_on_big_stack -- see the call site. */
static void *run_lsp(void *unused) {
    (void)unused;
    ziman_stack_mark_main();
    return (void *)(intptr_t)lsp_main();
}

static void *run_program(void *raw) {
    RunOptions *o = raw;
    const char *file = o->file;
    int json = o->json, lint = o->lint, check_only = o->check_only, coverage = o->coverage;
    int profile = o->profile, locals = o->locals;
    const char *coverage_lcov = o->coverage_lcov;
    ziman_stack_mark_main();
    ziman_gc_register_thread_roots();

    /* Line-buffered stdout even when redirected to a file or a pipe (C's
     * default there is fully buffered). A long-running program -- a
     * server logging each request -- needs its output to appear as it
     * happens, not when 4 KiB have accumulated or the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    register_all_natives();

    /* Enabled before the load, not after: one lint rule lives in the loader (a redundant `as` alias),
     * because a module's `use` list belongs to the loader's per-module AST and is gone by the time the
     * merged Module reaches the type checker. */
    if (lint) {
        typecheck_enable_lint();
        /* A library module has no `main`, and refusing to lint one meant `--lint` could only ever
         * be pointed at an entry point -- so no rule in it had ever run over `lib/`. That is not a
         * small gap: the `spawn` rule's own message is "whoever is waiting on that channel waits
         * for ever", which is A47 word for word, and it could not see the two `spawn serve` calls
         * that had it (F133). `--lsp` and `ziman test` already skip the same check. */
        typecheck_skip_main_check(1);
    }
    Module *module = load_module_graph(file, NULL);
    typecheck(module);
    if (lint) {
        int n = typecheck_lint_count();
        if (json && n == 0) printf("{\"ok\":true}\n");
        exit(n == 0 ? 0 : 1);
    }
    if (check_only) {
        if (json) printf("{\"ok\":true}\n");
        ziman_gc_unregister_thread_roots();
        return NULL;
    }
    /* Enabled before the program runs and reported after it, so the report covers the whole run
     * including anything a `defer` did on the way out. Reported even when the program fails: what
     * ran before it failed is exactly what you want to see then. */
    if (coverage) ziman_coverage_enable();
    /* Started here rather than in main, so loading and type-checking are not sampled: the report is
     * about the program's own run. The samples taken before the first call still land in the "not in
     * Ziman code" row, which is the honest place for them. */
    /* Installed before the program runs, since the trace it affects is built at the moment of
     * failure and there is no later point to arrange it. */
    if (locals) ziman_locals_enable();
    if (profile) ziman_profile_start();
    interpret(module);
    if (profile) ziman_profile_report();
    if (coverage) {
        /* Exit 1 when something did not run, the same convention --lint uses for a finding: it
         * makes `ziman --coverage` usable as a gate without parsing its output. */
        ziman_coverage_accumulate(module);
        if (coverage_lcov != NULL && ziman_coverage_write_lcov(coverage_lcov) != 0) {
            fprintf(stderr, "ziman: cannot write coverage to %s\n", coverage_lcov);
            exit(1);
        }
        int uncovered = ziman_coverage_report();
        exit(uncovered == 0 ? 0 : 1);
    }

    /* Both exits unregister, so `g_roots` does not keep a node pointing at this thread's TLS after
     * it is joined -- the pairing `interpreter.c` already has for a spawned thread, and that
     * `run_program` was missing (F129).
     *
     * The practical reach is small today and worth saying so rather than overstating it: the only
     * caller is `ziman_run_on_big_stack` from `main`, which returns 0 immediately afterwards, so
     * nothing allocates or collects in the window. `run_tests_program` cannot leave one at all,
     * because every path out of it is an `exit`. What this closes is the trap for the next caller
     * that does work after the join. */
    ziman_gc_unregister_thread_roots();
    return NULL;
}

/* The last path segment of `path`, for a #[test] label ("dir/foo_test.zm" -> "foo_test.zm"). */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* `ziman test file.zm [file2.zm ...]`: loads and type-checks each file with
 * `main` optional (a test file need not have one -- the same relaxation
 * `--lsp` already uses, see typecheck_skip_main_check), runs every #[test]
 * function in it (interpreter.c's interpret_tests), and tallies one
 * PASS/FAIL report across every file. Runs on a thread with a stack sized
 * for deep recursion, same as run_program. */
typedef struct {
    char **files;
    int n_files;
    int coverage;
    const char *coverage_lcov; /* NULL unless --coverage-lcov=PATH was given */
} TestOptions;

static void *run_tests_program(void *raw) {
    TestOptions *o = raw;
    ziman_stack_mark_main();
    ziman_gc_register_thread_roots();
    setvbuf(stdout, NULL, _IOLBF, 0);
    register_all_natives();
    typecheck_skip_main_check(1);

    int total_run = 0, total_failed = 0;
    for (int i = 0; i < o->n_files; i++) {
        Module *module = load_module_graph(o->files[i], NULL);
        typecheck(module);
        interpret_tests(module, base_name(o->files[i]), &total_run, &total_failed);
        /* Per file, because each file is its own Module -- ziman_coverage_accumulate merges them by
         * source file name, so a helper two test files share is reported once and counts as covered
         * if either file's tests reached it. */
        if (o->coverage) ziman_coverage_accumulate(module);
    }
    printf("%d passed, %d failed\n", total_run - total_failed, total_failed);
    if (o->coverage && o->coverage_lcov != NULL && ziman_coverage_write_lcov(o->coverage_lcov) != 0) {
        fprintf(stderr, "ziman: cannot write coverage to %s\n", o->coverage_lcov);
        exit(1);
    }
    int uncovered = o->coverage ? ziman_coverage_report() : 0;
    /* A failing test outranks incomplete coverage in the exit status: the first thing to fix is the
     * failure, and a caller that only wants the coverage gate can run a passing suite. */
    exit(total_failed != 0 ? 1 : (uncovered == 0 ? 0 : 1));
}

/* `ziman --api [module]`: what a module offers, in the language's own notation.
 *
 * This exists because of a measurement rather than a hunch. In the 2026-10-09 generation benchmark
 * (docs/reviews/llm-benchmark-2026-10-09.md) the single largest consumer of the fix budget was not
 * syntax but *name discovery*: with no documentation to hand, runs brute-forced eight candidates
 * before reaching `fs.write_text`, six before `map_get`, and thirteen to fifteen probe compiles
 * hunting `int` and `string`. One reached for `--lsp` completion precisely because no command-line
 * way to ask existed. Diagnostics can name a fix for a habit somebody anticipated; they cannot
 * enumerate a library.
 *
 * Printed from the parsed declarations, not by scanning text, so what it says is what the compiler
 * sees -- and `pub` only, since anything else is not callable from outside the module anyway. */
static void print_type(const char *prefix, Type t) {
    char *shown = type_name(t);
    printf("%s%s", prefix, shown);
    free(shown);
}

/* `<T: Ord, K>` -- the bounds matter as much as the names here: the benchmark's runs had to
 * discover by probing that `array_sort` needs its type argument written out, which a
 * signature without its type parameters cannot tell anyone. */
static void print_type_params(const TypeParamList *tp) {
    if (tp->count == 0) return;
    printf("<");
    for (int i = 0; i < tp->count; i++) {
        if (i > 0) printf(", ");
        printf("%s", tp->items[i].name);
        int bound = tp->items[i].bound, any = 0;
        if (bound & BOUND_EQ) { printf(": Eq"); any = 1; }
        if (bound & BOUND_HASH) { printf("%s", any ? " + Hash" : ": Hash"); any = 1; }
        if (bound & BOUND_ORD) { printf("%s", any ? " + Ord" : ": Ord"); }
    }
    printf(">");
}

/* A declaration's doc comment: the run of `//` lines immediately above it, with no blank line in
 * between. Printed because round 3 of the 2026-10-12 benchmark measured the cost of leaving it out
 * -- 46-fold-average answered wrongly because it chose `number.float_to_text` off its signature and
 * could not know that a whole-number float prints as `18`, while the function's own comment gives
 * `150` as an example; and 15-sha256-hex spent 14 extra compiles hand-rolling hex because it would
 * not trust `bytes_to_hex`'s output format, which that function's comment states exactly. A
 * signature is not a behaviour. The comments come from the lexer's own record (lex_comments), so
 * this is still what the compiler saw rather than a re-scan of the file; contiguity is what keeps a
 * `// ----- section` banner, which a blank line separates, out of the next function's docs. */
static void print_doc_comment(int decl_line) {
    int count = 0;
    const CommentLine *all = lex_comments(&count);
    int first = -1;
    for (int want = decl_line - 1; ; want--) {
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (all[i].line == want) { found = i; break; }
        }
        if (found < 0) break;
        first = found;
    }
    if (first < 0) return;
    for (int i = first; i < count && all[i].line < decl_line; i++) {
        printf("    // %.*s\n", all[i].length, all[i].text);
    }
}

static void print_api_module(const char *name, Module *m) {
    printf("module %s%s\n", name, m->experimental ? "  (experimental)" : "");
    /* **Named rather than listed**, because this path reads one file and the functions come from
     * another: `#![derive(Repository(X))]` generates from X's own fields, X is in the module this one
     * imports, and nothing here has loaded it. Saying so is the point -- a reader who sees only
     * the hand-written half and no mention of the rest would conclude the rest does not exist. */
    if (m->repository_entity != NULL && m->repository_no_crud) {
        /* `crud: false` generates none of the nine, so saying it does would be the exact error
         * this note exists to prevent, pointed the other way. The attribute still matters to a
         * reader: it is what lets the `#[derive(Repository)]` functions above have no body. */
        printf("\n  #![derive(Repository(%s, crud: false))] names this module's entity and generates none of\n"
               "  the keyed CRUD. Every function above is either written here or derived from its\n"
               "  own name.\n",
               m->repository_entity);
    } else if (m->repository_entity != NULL) {
        printf("\n  #![derive(Repository(%s))] also generates insert, update, find, find_or_error, exists,\n"
               "  delete, find_all, count_all and delete_all -- minus any this module writes itself,\n"
               "  which win. Their key parameters are %s's #[pk] fields, so the signatures are in\n"
               "  that module, not this one, and are not listed here.\n",
               m->repository_entity, m->repository_entity);
    }
    for (int i = 0; i < m->structs.count; i++) {
        StructDecl *s = m->structs.items[i];
        if (s->visibility != VIS_PUBLIC) continue;
        printf("\n");
        print_doc_comment(s->line);
        printf("struct %s", s->name);
        print_type_params(&s->type_params);
        printf(" {\n");
        for (int f = 0; f < s->fields.count; f++) {
            printf("    %s: ", s->fields.items[f].name);
            print_type("", s->fields.items[f].type);
            printf("\n");
        }
        printf("}\n");
    }
    for (int i = 0; i < m->enums.count; i++) {
        EnumDecl *e = m->enums.items[i];
        if (e->visibility != VIS_PUBLIC) continue;
        printf("\n");
        print_doc_comment(e->line);
        printf("enum %s", e->name);
        print_type_params(&e->type_params);
        printf(" {\n");
        for (int v = 0; v < e->variants.count; v++) {
            printf("    %s", e->variants.items[v].name);
            if (e->variants.items[v].has_payload) {
                print_type("(", e->variants.items[v].payload_type);
                printf(")");
            }
            printf("\n");
        }
        printf("}\n");
    }
    for (int i = 0; i < m->functions.count; i++) {
        FunctionDecl *f = m->functions.items[i];
        if (f->visibility != VIS_PUBLIC) continue;
        printf("\n");
        print_doc_comment(f->line);
        printf("fn %s", f->name);
        print_type_params(&f->type_params);
        printf("(");
        for (int p = 0; p < f->params.count; p++) {
            if (p > 0) printf(", ");
            printf("%s: ", f->params.items[p].name);
            /* The keyword, not just the element type. `--api` printed `pieces: string` for a
             * variadic `concat` and so said nothing about the one thing a caller cannot guess;
             * harmless while the marker was a `...` nobody would read off a signature, actively
             * misleading once it is a keyword that has to be written to declare one. */
            if (f->params.items[p].is_variadic) printf("variadic ");
            print_type("", f->params.items[p].type);
        }
        printf(")");
        if (f->return_type.kind != TY_VOID) print_type(" -> ", f->return_type);
        printf("\n");
    }
}

/* What this listing claims is the vocabulary, which is not the same as what is callable: the
 * prelude is merged into every program, so every declaration in it is callable whatever it says
 * here. `pub` and `internal` choose what gets advertised. */
static int api_advertises(FunctionDecl *f) {
    if (!fn_is_advertised(f)) return 0;
    return f->visibility == VIS_PUBLIC || fn_is_global_vocabulary(f);
}

static void print_builtin_entry(FunctionDecl *f) {
    /* Printed because it is part of the contract the signature states: without it the list would
     * advertise `add(s, items: variadic T)` and say nothing about `add(s)` being refused. */
    if (f->variadic_at_least_one) printf("\n#[at_least_one]");
    /* Printed for the same reason: the signature alone says `remove(b, i) -> T`, and a reader
     * comparing that with `distinct(b) -> [T]` has no way to tell that one of the two writes its
     * argument and so may be called for the write alone. */
    if (f->is_effectful) printf("\n#[effectful]");
    /* Printed because it changes what the signatures mean: `concat`'s four cannot be told apart by
     * their arguments, so a reader comparing them needs to know the compiler chooses. */
    if (f->compiler_resolved) printf("\n#[compiler_resolved]");
    /* `builtin fn` keeps its keyword here rather than being flattened to `fn`: which of the two
     * owns the type rule is a real difference and the one docs/design/journal/'s "`native` and builtin
     * are two different mechanisms" entry is about. */
    printf(f->is_builtin ? "\nbuiltin fn %s" : "\nfn %s", f->name);
    print_type_params(&f->type_params);
    printf("(");
    for (int pi = 0; pi < f->params.count; pi++) {
        if (pi > 0) printf(", ");
        printf("%s: ", f->params.items[pi].name);
        if (f->params.items[pi].is_variadic) printf("variadic ");
        print_type("", f->params.items[pi].type);
    }
    printf(")");
    if (f->return_type.kind != TY_VOID) print_type(" -> ", f->return_type);
}

static int run_api(const char *module_name) {
    if (module_name == NULL) {
        printf("bundled modules (ziman --api <name> for one of them):\n");
        for (int i = 0; i < corelib_module_count(); i++) printf("  %s\n", corelib_module_name(i));
        printf("\n  builtins   the functions that need no 'use' at all -- ziman --api builtins\n");
        return 0;
    }
    /* Not a module, but the answer to the same question -- and the one the 2026-10-10 benchmark wave
     * showed was missing: a run that wanted a string's characters never found `chars`, because it is
     * a builtin and `--api` listed only modules, so it used `split(word, "")` instead, which
     * type-checks and then fails at runtime.
     *
     * Printed from `lib/prelude/<part>.zm` themselves rather than from a list kept here. A first version did
     * keep one, reusing typecheck.c's `BUILTINS_NO_NAMED_ARGS` -- and it did not contain `chars`,
     * because that table is the subset of builtins taking no named arguments, not the builtins. An
     * incomplete answer to "what exists" is worse than none, since it reads as "`chars` does not
     * exist". The prelude declares all of them with real signatures, so it is the honest source. */
    if (strcmp(module_name, "builtins") == 0) {
        /* Every part merged into one list, so the printing below -- and the "which builtins have
         * no declaration" scan after it -- sees the whole vocabulary rather than one file's share.
         * Only `functions` is needed here; the types come from the fixed list at the end. */
        Module *prelude = ziman_xcalloc(1, sizeof(Module));
        for (int p = 0; p < prelude_part_count(); p++) {
            Module *part = parse(lex(prelude_part_source(p)));
            for (int i = 0; i < part->functions.count; i++) {
                function_list_push(&prelude->functions, part->functions.items[i]);
            }
        }
        printf("builtins -- callable with no 'use', from any program\n");
        /* **An overload set prints as one block.** The parts are merged in file order above, and
         * printing them straight through scattered every set whose members live in different
         * files -- which, since the filing rule put a declaration in the file for its first
         * argument's type (2026-10-21), is most of them: measured before this change, `filter`
         * came out in two blocks 56 lines apart, `concat` in two, `get_or` in two, and `snapshot`
         * and `clear` in **four** each.
         *
         * That mattered more than a tidiness complaint, because the prelude headers had all ten
         * been citing this listing as the thing that made splitting a set across files safe --
         * "`--api builtins` still prints a set together, so the listing answers 'what does this
         * name accept' in one place even when the sources do not". It did not. It does now, and
         * the sentence is true for the first time.
         *
         * A set appears where its **first** advertised member does, rather than the whole list
         * being sorted by name: the file order is topical -- a string's functions together, an
         * array's together -- and sorting would scatter that to fix the smaller problem. */
        for (int i = 0; i < prelude->functions.count; i++) {
            FunctionDecl *f = prelude->functions.items[i];
            if (!api_advertises(f)) continue;
            int already = 0;
            for (int j = 0; j < i && !already; j++) {
                FunctionDecl *g = prelude->functions.items[j];
                if (api_advertises(g) && strcmp(g->name, f->name) == 0) already = 1;
            }
            if (already) continue;  /* printed with the group at its first member */
            for (int k = i; k < prelude->functions.count; k++) {
                FunctionDecl *g = prelude->functions.items[k];
                if (api_advertises(g) && strcmp(g->name, f->name) == 0) print_builtin_entry(g);
            }
        }
        /* And the ones the *interpreter* dispatches by name that still have no declaration to
         * print a signature from. Leaving them out entirely was the second half of the same
         * mistake: a run concluded "there is no `keys` function" when `map_keys` is one of these.
         * A name listed here is one of the two kinds F148 says cannot be declared -- a rule that
         * reads more than types (`format` validates its template; `print`/`str` take anything) or
         * a parser desugaring target (`$map_lit`) -- or one not yet declared. */
        int undeclared = 0;
        for (int i = 0; i < ziman_builtin_count(); i++) {
            const char *bname = ziman_builtin_name(i);
            int declared = 0;
            for (int fi = 0; fi < prelude->functions.count; fi++) {
                FunctionDecl *f = prelude->functions.items[fi];
                if (f->is_builtin && strcmp(f->name, bname) == 0) { declared = 1; break; }
            }
            if (!declared) undeclared++;
        }
        if (undeclared > 0) {
            /* Each with the reason it has none, from typecheck.c's `BUILTIN_NO_SIGNATURE` -- a
             * list under one covering sentence made the reader work out which sentence applied to
             * which name, and `$map_lit` sitting unexplained beside `format` reads like an
             * omission rather than a parser internal. */
            printf("\n\nalso builtin, dispatched by the interpreter -- these carry no signature, and why:\n");
            for (int i = 0; i < ziman_builtin_count(); i++) {
                const char *bname = ziman_builtin_name(i);
                int declared = 0;
                for (int fi = 0; fi < prelude->functions.count; fi++) {
                    FunctionDecl *f = prelude->functions.items[fi];
                    if (f->is_builtin && strcmp(f->name, bname) == 0) { declared = 1; break; }
                }
                if (declared) continue;
                const char *why = ziman_builtin_no_signature_reason(bname);
                /* No reason recorded means it simply has not been declared yet, which
                 * tools/check_builtins.py fails on -- said here too rather than printed as a bare
                 * name a reader would take for a deliberate exception. */
                printf("  %-16s %s\n", bname, why != NULL ? why : "not yet declared in lib/prelude/");
            }
            printf("\ndocs/syntax/ \"Builtins\" has what each one takes.\n");
        }
        printf("\nbuilt-in types, which are lowercase:\n  int  float  bool  string  byte  bigint  decimal\n"
               "  [T] (an array)  Map<K, V>  Set<T>  Option<T>  Result<T, E>  Channel<T>\n");
        return 0;
    }
    const char *source = corelib_source_for(module_name);
    char *owned = NULL;
    if (source == NULL) {
        /* Not bundled: try it as a file, so this works on a project's own modules too. */
        size_t len = strlen(module_name) + 4;
        char *path = ziman_xmalloc(len);
        snprintf(path, len, "%s.zm", module_name);
        FILE *f = fopen(strstr(module_name, ".zm") != NULL ? module_name : path, "rb");
        free(path);
        if (f == NULL) {
            fprintf(stderr, "ziman: no bundled module or file named '%s' -- 'ziman --api' lists the bundled ones\n",
                    module_name);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        owned = ziman_xmalloc((size_t)size + 1);
        size_t got = fread(owned, 1, (size_t)size, f);
        owned[got] = '\0';
        fclose(f);
        source = owned;
    }
    TokenList tokens = lex(source);
    Module *m = parse(tokens);
    /* `--api` lists what a module offers, and a derived function is part of that -- `--api` on an
     * entity prints its `<name>_from_row`. Asked for explicitly since 2026-10-26, when generation
     * moved out of `parse` into the loader: this path has no loader and no imports, so a generator
     * that needs an imported type can do nothing here, which is a limit of reading one file in
     * isolation rather than of the generator. */
    generate_derived_functions(m);
    print_api_module(module_name, m);
    free(owned);
    return 0;
}

static const char *const TEST_USAGE =
    "usage: ziman test [--coverage] [--coverage-lcov=F] [--gc-stress] <file.zm> [file2.zm ...]\n";

static int run_tests_main(int argc, char **argv) {
    /* Accept the options this mode takes anywhere among the file names and strip them, so
     * `ziman test --coverage a.zm` and `ziman test a.zm --coverage` both work and neither leaves
     * the flag to be opened as a source file. */
    int coverage = 0, n = 0;
    const char *coverage_lcov = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--coverage") == 0) coverage = 1;
        else if (strncmp(a, "--coverage-lcov=", 16) == 0) {
            coverage_lcov = a + 16;
            coverage = 1;
        }
        /* `--gc-stress` is most valuable here and reached everywhere but here: this is where a
         * user's own `#[test]` functions run. */
        else if (strcmp(a, "--gc-stress") == 0) ziman_set_gc_stress(1);
        /* An option this mode does not take was a file name, so `ziman test --lint f.zm` reported
         * that it could not open '--lint' -- a path error for what is a usage error. */
        else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "ziman: `test` does not take '%s'\n", a);
            fputs(TEST_USAGE, stderr);
            return 2;
        }
        else argv[n++] = argv[i];
    }
    argc = n;
    if (argc == 0) {
        fputs(TEST_USAGE, stderr);
        return 2;
    }
    if (coverage) ziman_coverage_enable();
    ziman_stack_configure(0, 0, 0);
    TestOptions options = {argv, argc, coverage, coverage_lcov};
    ziman_run_on_big_stack(run_tests_program, &options);
    return 0;
}

#ifdef ZIMAN_GC
/* Boehm's own warnings went straight to stderr unfiltered, because nothing ever called
 * GC_set_warn_proc. That is normally invisible and occasionally catastrophic: a runaway recursion
 * used to make the trace builder allocate hundreds of thousands of ever-larger buffers, and Boehm
 * announced each one, producing 23,170 of the 198,578 stderr lines a single stack-depth error wrote.
 * The allocation behaviour is fixed (src/error.c's build_trace), but a warning channel with no cap is
 * still a channel an input can flood, so it is capped here as well.
 *
 * The first few are printed, because they can be the only clue to a real problem, and the rest are
 * counted away behind one line that says how to see them. ZIMAN_GC_WARN=1 prints all of them. */
#define ZIMAN_GC_WARN_SHOWN 3
static int g_gc_warn_seen = 0;

static void GC_CALLBACK ziman_gc_warn(const char *msg, GC_uintptr_t arg) {
    int n = __atomic_fetch_add(&g_gc_warn_seen, 1, __ATOMIC_RELAXED);
    static int show_all = -1;
    if (show_all < 0) show_all = getenv("ZIMAN_GC_WARN") != NULL;
    if (show_all || n < ZIMAN_GC_WARN_SHOWN) {
        fprintf(stderr, msg, arg);
    } else if (n == ZIMAN_GC_WARN_SHOWN) {
        fprintf(stderr, "ziman: further garbage-collector warnings suppressed; set ZIMAN_GC_WARN=1 to see them all\n");
    }
}
#endif

int main(int argc, char **argv) {
#ifdef ZIMAN_GC
    /* Before the first allocation: the first one initialises the collector with defaults. */
    /* Two settings measured on the sample backend (tests/bench/server_load.sh):
     * a 32 MB initial heap avoids the early rounds of collections while the heap
     * grows, and two marker threads keep marking off most of a many-core machine.
     * GC_MARKERS (and the collector's other environment variables) still win. */
    if (getenv("GC_MARKERS") == NULL) GC_set_markers_count(2);
    GC_set_warn_proc(ziman_gc_warn);
    GC_INIT();
    (void)GC_expand_hp(32u * 1024 * 1024);
#endif
    ziman_gc_install_boehm_root_hook();
    /* `test` is a subcommand rather than a flag (docs/design/journal/, "#[test], ziman test, and the
     * test module"), and it was recognised at argv[1] only -- so an option written before it,
     * `ziman --gc-stress test f.zm`, read `test` as a second source file and died with "more than
     * one file given". That position is the one that matters for a whole-suite run: prepending a
     * flag to every fixture's command line is how the suite is run under one, and the 8 fixtures
     * whose `.args` say `test` are the ones such a run could not reach.
     *
     * So `test` is now the subcommand wherever it is the first argument that is not an option, and
     * the options before it are handed to the mode with the file names -- `run_tests_main` already
     * accepts its own in any position. A file named exactly `test` is shadowed by this, as it
     * already was at argv[1]. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        /* `--api` takes the next argument and `--` hands the rest to the program, so neither can
         * be followed by a subcommand: `ziman --api test` asks the bundled `test` module for its
         * API and stays that. */
        if (strcmp(a, "--api") == 0 || strcmp(a, "--") == 0) break;
        if (a[0] == '-') continue;
        if (strcmp(a, "test") == 0) {
            /* Close the gap `test` itself leaves, so the mode gets argv[1..i-1] then argv[i+1..]. */
            for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1];
            return run_tests_main(argc - 2, argv + 1);
        }
        break;
    }
    int check_only = 0, json = 0, lint = 0, lint_bundled = 0, fmt = 0, fmt_write = 0, fmt_width = 120, fmt_canonical = 0, lsp = 0, coverage = 0, profile = 0, locals = 0;
    const char *coverage_lcov = NULL;
    long stack_main_mb = 0, stack_thread_mb = 0, stack_threads = 0;
    const char *file = NULL;
    char **files = ziman_xmalloc((size_t)argc * sizeof(char *));
    int n_files = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            ziman_set_program_args(argc - i - 1, argv + i + 1);
            break;
        }
        if (strcmp(a, "--check") == 0) check_only = 1;
        else if (strcmp(a, "--json") == 0) json = 1;
        else if (strcmp(a, "--lint") == 0) lint = 1;
        else if (strcmp(a, "--lint-bundled") == 0) { lint = 1; lint_bundled = 1; }
        else if (strcmp(a, "--coverage") == 0) coverage = 1;
        /* Implies --coverage: asking for the file is asking for the measurement. */
        else if (strncmp(a, "--coverage-lcov=", 16) == 0) {
            coverage_lcov = a + 16;
            coverage = 1;
        }
        else if (strcmp(a, "--profile") == 0) profile = 1;
        else if (strcmp(a, "--locals") == 0) locals = 1;
        else if (strcmp(a, "--fmt") == 0) fmt = 1;
        else if (strcmp(a, "--api") == 0) {
            /* The module name is optional: `--api` alone lists what exists, which is the first
             * question a reader without documentation has. */
            return run_api(i + 1 < argc ? argv[i + 1] : NULL);
        }
        else if (strcmp(a, "--lsp") == 0) lsp = 1;
        else if (strcmp(a, "--write") == 0) fmt_write = 1;
        else if (strncmp(a, "--width=", 8) == 0) fmt_width = atoi(a + 8);
        else if (strcmp(a, "--canonical") == 0) fmt_canonical = 1;
        else if (strcmp(a, "--emit-derived") == 0) g_emit_derived = 1;
        else if (strcmp(a, "--gc-stress") == 0) ziman_set_gc_stress(1);
        else if (strncmp(a, "--stack-mb=", 11) == 0) stack_main_mb = atol(a + 11);
        else if (strncmp(a, "--thread-stack-mb=", 18) == 0) stack_thread_mb = atol(a + 18);
        else if (strncmp(a, "--threads=", 10) == 0) stack_threads = atol(a + 10);
        else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) { printf("ziman %s\n", ZIMAN_VERSION); return 0; }
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "ziman: unknown option '%s'\n", a); usage(stderr); return 2; }
        else { files[n_files++] = (char *)a; if (file == NULL) file = a; }
    }
    if (lsp) {
        register_all_natives();
        /* On the same big stack as a program run, and honouring the same options. The server
         * recurses over whatever a user's editor has open, exactly as `--check` does, and used to
         * run on the process stack instead: `ulimit -s` (8 MB typically), with --stack-mb accepted
         * and then ignored -- so a deep type took the server down, and the "raise it with
         * --stack-mb=N" the error suggests could not be acted on. */
        ziman_stack_configure(stack_main_mb, stack_thread_mb, stack_threads);
        return (int)(intptr_t)ziman_run_on_big_stack(run_lsp, NULL);
    }
    if (fmt) {
        if (n_files == 0) { usage(stderr); return 2; }
        return run_format(files, n_files, fmt_write, check_only, fmt_width, fmt_canonical);
    }
    if (n_files > 1) { fprintf(stderr, "ziman: more than one file given ('%s' and '%s')\n", files[0], files[1]); usage(stderr); return 2; }
    if (file == NULL) {
        usage(stderr);
        return 1;
    }
    ziman_set_json_errors(json);
    ziman_stack_configure(stack_main_mb, stack_thread_mb, stack_threads);
    if (lint_bundled) typecheck_enable_lint_bundled();
    RunOptions options = {file, json, lint, check_only, coverage, profile, locals, coverage_lcov};
    ziman_run_on_big_stack(run_program, &options);
    return 0;
}
