#include <stdio.h>

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ziman <file.zm>\n");
        return 1;
    }

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

    Module *module = load_module_graph(argv[1]);
    typecheck(module);
    interpret(module);

    return 0;
}
