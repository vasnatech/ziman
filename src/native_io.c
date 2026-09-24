#define _POSIX_C_SOURCE 200809L
#include "native_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* A missing/unreadable file is an expected, recoverable condition (unlike
 * a bug such as an out-of-bounds index), so it's a Result, not a fatal
 * ziman_error -- see docs/DESIGN.md's Result<T, E> entry on that line. */
static Value native_io_read_file(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *path = value_cstr(args[0]);
    Type result_type = type_result(type_scalar(TY_STRING), type_scalar(TY_STRING));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot open '%s'", path);
        return value_result(result_type, result_obj_new(0, value_string(strdup(msg))));
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = zalloc_atomic((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);

    return value_result(result_type, result_obj_new(1, value_string(buf)));
}

static Value native_io_write_file(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *path = value_cstr(args[0]);
    const char *contents = value_cstr(args[1]);

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
