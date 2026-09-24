#include <stdio.h>
#include <string.h>

#include "derive.h"
#include "error.h"
#include "interpreter.h"
#include "loader.h"
#include "native_clock.h"
#include "native_crypto.h"
#include "native_sqlite.h"
#include "native_io.h"
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
          "  --emit-derived   print the code each #[derive] generates, then continue\n"
          "  --version        print the version and exit\n"
          "  --help           print this help and exit\n", out);
}

int main(int argc, char **argv) {
    int check_only = 0, json = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--check") == 0) check_only = 1;
        else if (strcmp(a, "--json") == 0) json = 1;
        else if (strcmp(a, "--emit-derived") == 0) g_emit_derived = 1;
        else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) { printf("ziman %s\n", ZIMAN_VERSION); return 0; }
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "ziman: unknown option '%s'\n", a); usage(stderr); return 2; }
        else if (file == NULL) file = a;
        else { fprintf(stderr, "ziman: more than one file given ('%s' and '%s')\n", file, a); usage(stderr); return 2; }
    }
    if (file == NULL) {
        usage(stderr);
        return 1;
    }
    ziman_set_json_errors(json);

    /* Line-buffered stdout even when redirected to a file or a pipe (C's
     * default there is fully buffered). A long-running program -- a
     * server logging each request -- needs its output to appear as it
     * happens, not when 4 KiB have accumulated or the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    native_io_register();
    native_random_register();
    native_clock_register();
    native_sqlite_register();
    native_net_register();
    native_os_register();
    native_crypto_register();
    prelude_register();

    Module *module = load_module_graph(file);
    typecheck(module);
    if (check_only) {
        if (json) printf("{\"ok\":true}\n");
        return 0;
    }
    interpret(module);

    return 0;
}
