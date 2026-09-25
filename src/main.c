#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "derive.h"
#include "format.h"
#include "lsp.h"
#include "native_compress.h"
#include "native_csv.h"
#include "native_json.h"
#include "native_number.h"
#include "native_os.h"
#include "error.h"
#include "interpreter.h"
#include "loader.h"
#include "native_clock.h"
#include "native_crypto.h"
#include "native_sqlite.h"
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
          "\n"
          "  --check          load and type-check only; do not run the program\n"
          "  --json           report errors as one JSON object per error on stderr\n"
          "                   (with --check, print {\"ok\":true} on success)\n"
          "  --lint           report style findings (a `var` never reassigned) in your own\n"
          "                   code instead of running it; exit 1 if there are any\n"
          "  --lsp            run as a Language Server on stdin/stdout (diagnostics, formatting,\n"
          "                   symbols, go to definition, hover, completion)\n"
          "  -- args...       everything after `--` is the program's own arguments (os.args())\n"
          "  --fmt            print the source formatted (several files allowed, `-` is\n"
          "                   stdin); with --write rewrite the files in place, with\n"
          "                   --check list the files that would change and exit 1;\n"
          "                   --width=N wraps longer lines (default 120, 0 = never)\n"
          "  --emit-derived   print the code each #[derive] generates, then continue\n"
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
    int check_only = 0, json = 0, lint = 0, fmt = 0, fmt_write = 0, fmt_width = 120, lsp = 0;
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
        else if (strcmp(a, "--fmt") == 0) fmt = 1;
        else if (strcmp(a, "--lsp") == 0) lsp = 1;
        else if (strcmp(a, "--write") == 0) fmt_write = 1;
        else if (strncmp(a, "--width=", 8) == 0) fmt_width = atoi(a + 8);
        else if (strcmp(a, "--emit-derived") == 0) g_emit_derived = 1;
        else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) { printf("ziman %s\n", ZIMAN_VERSION); return 0; }
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "ziman: unknown option '%s'\n", a); usage(stderr); return 2; }
        else { files[n_files++] = (char *)a; if (file == NULL) file = a; }
    }
    if (lsp) {
        native_fs_register();
        native_regex_register();
        native_random_register();
        native_clock_register();
        native_sqlite_register();
        native_net_register();
        native_os_register();
        native_crypto_register();
        native_number_register();
        native_csv_register();
        native_json_register();
        native_compress_register();
        prelude_register();
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
    ziman_set_entry_file(file);

    /* Line-buffered stdout even when redirected to a file or a pipe (C's
     * default there is fully buffered). A long-running program -- a
     * server logging each request -- needs its output to appear as it
     * happens, not when 4 KiB have accumulated or the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    native_fs_register();
    native_regex_register();
    native_random_register();
    native_clock_register();
    native_sqlite_register();
    native_net_register();
    native_os_register();
    native_crypto_register();
    native_number_register();
    native_csv_register();
    native_json_register();
    native_compress_register();
    prelude_register();

    Module *module = load_module_graph(file);
    if (lint) typecheck_enable_lint();
    typecheck(module);
    if (lint) {
        int n = typecheck_lint_count();
        if (json && n == 0) printf("{\"ok\":true}\n");
        return n == 0 ? 0 : 1;
    }
    if (check_only) {
        if (json) printf("{\"ok\":true}\n");
        return 0;
    }
    interpret(module);

    return 0;
}
