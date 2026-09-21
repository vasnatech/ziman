#include <stdio.h>

#include "interpreter.h"
#include "loader.h"
#include "typecheck.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ziman <file.zm>\n");
        return 1;
    }

    Module *module = load_module_graph(argv[1]);
    typecheck(module);
    interpret(module);

    return 0;
}
