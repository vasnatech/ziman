#include <stdio.h>

#include "interpreter.h"
#include "loader.h"
#include "native_clock.h"
#include "native_crypto.h"
#include "native_db.h"
#include "native_io.h"
#include "native_net.h"
#include "native_random.h"
#include "prelude.h"
#include "typecheck.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ziman <file.zm>\n");
        return 1;
    }

    native_io_register();
    native_random_register();
    native_clock_register();
    native_db_register();
    native_net_register();
    native_crypto_register();
    prelude_register();

    Module *module = load_module_graph(argv[1]);
    typecheck(module);
    interpret(module);

    return 0;
}
