#include <stdio.h>

#include "interpreter.h"
#include "loader.h"
#include "native_io.h"
#include "prelude.h"
#include "typecheck.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ziman <file.zm>\n");
        return 1;
    }

    native_io_register();
    prelude_register();

    Module *module = load_module_graph(argv[1]);
    typecheck(module);
    interpret(module);

    return 0;
}
