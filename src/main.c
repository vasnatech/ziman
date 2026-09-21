#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "ziman: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ziman <file.zm>\n");
        return 1;
    }

    char *source = read_file(argv[1]);
    TokenList tokens = lex(source);
    Module *module = parse(tokens);
    if (module->uses.count > 0) {
        /* Parseable already, but nothing resolves a `use` yet -- that's the
         * loader, still ahead (see docs/ROADMAP.md). Fail clearly instead
         * of silently ignoring the import. */
        UseDecl *u = &module->uses.items[0];
        ziman_error("unsupported", u->line, u->col, "'use' is not implemented yet (module loading isn't wired up)");
    }
    typecheck(module);
    interpret(module);

    token_list_free(&tokens);
    free(source);
    return 0;
}
