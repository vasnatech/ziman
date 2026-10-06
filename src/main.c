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
#include "native_math.h"
#include "native_number.h"
#include "native_os.h"
#include "native_thread.h"
#include "coverage.h"
#include "locals.h"
#include "profile.h"
#include "error.h"
#include "interpreter.h"
#include "loader.h"
#include "native_clock.h"
#include "native_crypto.h"
#include "native_sqlite.h"
#include "iter.h"
#include "native_fs.h"
#include "native_regex.h"
#include "native_net.h"
#include "native_os.h"
#include "native_random.h"
#include "prelude.h"
#include "typecheck.h"

#ifndef ZIMAN_VERSION
#define ZIMAN_VERSION "dev"
#endif

static void usage(FILE *out) {
    fputs("usage: ziman [options] <file.zm>\n"
          "       ziman test <file.zm> [file2.zm ...]\n"
          "\n"
          "  test             run every #[test] function in the given files (see\n"
          "                   docs/SYNTAX.md \"Testing\"); PASS/FAIL per test, then a\n"
          "                   summary; exit 1 if any failed\n"
          "  --check          load and type-check only; do not run the program\n"
          "  --json           report errors as one JSON object per error on stderr\n"
          "                   (with --check, print {\"ok\":true} on success)\n"
          "  --lint           report style findings (a `var` never reassigned) in your own\n"
          "                   code instead of running it; exit 1 if there are any\n"
          "  --coverage       run, then report which lines of your own code did not\n"
          "                   execute; exit 1 if any did not\n"
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
          "                   --width=N wraps longer lines (default 120, 0 = never)\n"
          "  --emit-derived   print the code each #[derive] generates, then continue\n"
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
    char *buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static char *read_stdin(size_t *len) {
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, stdin)) > 0) {
        n += got;
        if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* `ziman --fmt [--width=N] [--write | --check] files...`; a file named `-` is stdin (printed to stdout). */
static int run_format(char **files, int n_files, int write, int check, int width) {
    int changed = 0, failed = 0;
    for (int i = 0; i < n_files; i++) {
        size_t len;
        int from_stdin = strcmp(files[i], "-") == 0;
        char *src = from_stdin ? read_stdin(&len) : read_whole(files[i], &len);
        if (src == NULL) { fprintf(stderr, "ziman: cannot open '%s'\n", files[i]); failed = 1; continue; }
        char err[256];
        char *out = format_source(src, len, width, err, sizeof err);
        if (out == NULL) { fprintf(stderr, "%s: %s\n", files[i], err); failed = 1; free(src); continue; }
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
} RunOptions;

static void *run_program(void *raw) {
    RunOptions *o = raw;
    const char *file = o->file;
    int json = o->json, lint = o->lint, check_only = o->check_only, coverage = o->coverage;
    int profile = o->profile, locals = o->locals;
    ziman_stack_mark_main();
    ziman_gc_register_thread_roots();

    /* Line-buffered stdout even when redirected to a file or a pipe (C's
     * default there is fully buffered). A long-running program -- a
     * server logging each request -- needs its output to appear as it
     * happens, not when 4 KiB have accumulated or the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    register_all_natives();

    Module *module = load_module_graph(file, NULL);
    if (lint) typecheck_enable_lint();
    typecheck(module);
    if (lint) {
        int n = typecheck_lint_count();
        if (json && n == 0) printf("{\"ok\":true}\n");
        exit(n == 0 ? 0 : 1);
    }
    if (check_only) {
        if (json) printf("{\"ok\":true}\n");
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
        int uncovered = ziman_coverage_report();
        exit(uncovered == 0 ? 0 : 1);
    }

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
    int uncovered = o->coverage ? ziman_coverage_report() : 0;
    /* A failing test outranks incomplete coverage in the exit status: the first thing to fix is the
     * failure, and a caller that only wants the coverage gate can run a passing suite. */
    exit(total_failed != 0 ? 1 : (uncovered == 0 ? 0 : 1));
}

static int run_tests_main(int argc, char **argv) {
    /* Accept --coverage anywhere among the file names and strip it, so `ziman test --coverage a.zm`
     * and `ziman test a.zm --coverage` both work and neither leaves the flag to be opened as a
     * source file. */
    int coverage = 0, n = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--coverage") == 0) coverage = 1;
        else argv[n++] = argv[i];
    }
    argc = n;
    if (argc == 0) {
        fprintf(stderr, "usage: ziman test [--coverage] <file.zm> [file2.zm ...]\n");
        return 2;
    }
    if (coverage) ziman_coverage_enable();
    ziman_stack_configure(0, 0, 0);
    TestOptions options = {argv, argc, coverage};
    ziman_run_on_big_stack(run_tests_program, &options);
    return 0;
}

int main(int argc, char **argv) {
#ifdef ZIMAN_GC
    /* Before the first allocation: the first one initialises the collector with defaults. */
    /* Two settings measured on the sample backend (tests/bench/server_load.sh):
     * a 32 MB initial heap avoids the early rounds of collections while the heap
     * grows, and two marker threads keep marking off most of a many-core machine.
     * GC_MARKERS (and the collector's other environment variables) still win. */
    if (getenv("GC_MARKERS") == NULL) GC_set_markers_count(2);
    GC_INIT();
    (void)GC_expand_hp(32u * 1024 * 1024);
#endif
    ziman_gc_install_boehm_root_hook();
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        return run_tests_main(argc - 2, argv + 2);
    }
    int check_only = 0, json = 0, lint = 0, fmt = 0, fmt_write = 0, fmt_width = 120, lsp = 0, coverage = 0, profile = 0, locals = 0;
    long stack_main_mb = 0, stack_thread_mb = 0, stack_threads = 0;
    const char *file = NULL;
    char **files = malloc((size_t)argc * sizeof(char *));
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
        else if (strcmp(a, "--coverage") == 0) coverage = 1;
        else if (strcmp(a, "--profile") == 0) profile = 1;
        else if (strcmp(a, "--locals") == 0) locals = 1;
        else if (strcmp(a, "--fmt") == 0) fmt = 1;
        else if (strcmp(a, "--lsp") == 0) lsp = 1;
        else if (strcmp(a, "--write") == 0) fmt_write = 1;
        else if (strncmp(a, "--width=", 8) == 0) fmt_width = atoi(a + 8);
        else if (strcmp(a, "--emit-derived") == 0) g_emit_derived = 1;
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
        return lsp_main();
    }
    if (fmt) {
        if (n_files == 0) { usage(stderr); return 2; }
        return run_format(files, n_files, fmt_write, check_only, fmt_width);
    }
    if (n_files > 1) { fprintf(stderr, "ziman: more than one file given ('%s' and '%s')\n", files[0], files[1]); usage(stderr); return 2; }
    if (file == NULL) {
        usage(stderr);
        return 1;
    }
    ziman_set_json_errors(json);
    ziman_stack_configure(stack_main_mb, stack_thread_mb, stack_threads);
    RunOptions options = {file, json, lint, check_only, coverage, profile, locals};
    ziman_run_on_big_stack(run_program, &options);
    return 0;
}
