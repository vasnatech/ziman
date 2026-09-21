#include "native_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

static Value native_io_read_file(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *path = args[0].as.s;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ziman_error("runtime", line, col, "io.read_file: cannot open '%s'", path);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);

    return value_string(buf);
}

static Value native_io_write_file(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *path = args[0].as.s;
    const char *contents = args[1].as.s;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ziman_error("runtime", line, col, "io.write_file: cannot open '%s' for writing", path);
    }
    fwrite(contents, 1, strlen(contents), f);
    fclose(f);

    return value_int(0); /* void return -- the caller never uses this */
}

void native_io_register(void) {
    native_register("io::read_file", native_io_read_file);
    native_register("io::write_file", native_io_write_file);
}
