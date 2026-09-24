#define _POSIX_C_SOURCE 200809L
#include "native_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native.h"
#include "value.h"

/* Every failing filesystem call is an expected condition (missing file,
 * permissions, full disk), so each one returns a Result with the OS's own
 * message -- never a fatal ziman_error. */
static Type string_type(void) { return type_scalar(TY_STRING); }

static Value fs_err(Type ok_type, const char *what, const char *path) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "%s '%s': %s", what, path, strerror(errno));
    return value_result(type_result(ok_type, string_type()), result_obj_new(0, value_string(strdup(msg))));
}

static Value fs_ok_int(int64_t n) {
    return value_result(type_result(type_scalar(TY_INT), string_type()), result_obj_new(1, value_int(n)));
}

static Value fs_write(Value *args, const char *mode, const char *what) {
    const char *path = value_cstr(args[0]);
    FILE *f = fopen(path, mode);
    if (f == NULL) return fs_err(type_scalar(TY_INT), what, path);
    size_t n = fwrite(args[1].as.str.ptr, 1, args[1].as.str.len, f);
    int failed = n != args[1].as.str.len;
    if (fclose(f) != 0) failed = 1;
    if (failed) return fs_err(type_scalar(TY_INT), what, path);
    return fs_ok_int((int64_t)n);
}

static Value native_fs_write_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return fs_write(args, "wb", "cannot write");
}

static Value native_fs_append_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return fs_write(args, "ab", "cannot append to");
}

static Value native_fs_exists(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    struct stat st;
    return value_bool(stat(value_cstr(args[0]), &st) == 0);
}

static Value native_fs_is_dir(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    struct stat st;
    return value_bool(stat(value_cstr(args[0]), &st) == 0 && S_ISDIR(st.st_mode));
}

static Value native_fs_size(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    struct stat st;
    if (stat(path, &st) != 0) return fs_err(type_scalar(TY_INT), "cannot stat", path);
    return fs_ok_int((int64_t)st.st_size);
}

static Value native_fs_remove(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    if (remove(path) != 0) return fs_err(type_scalar(TY_INT), "cannot remove", path);
    return fs_ok_int(0);
}

static Value native_fs_make_dir(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    if (mkdir(path, 0777) != 0) return fs_err(type_scalar(TY_INT), "cannot create directory", path);
    return fs_ok_int(0);
}

static Value native_fs_rename(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *from = value_cstr(args[0]);
    if (rename(from, value_cstr(args[1])) != 0) return fs_err(type_scalar(TY_INT), "cannot rename", from);
    return fs_ok_int(0);
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Names only (no "." / ".."), sorted so the result doesn't depend on the
 * directory's on-disk order. */
static Value native_fs_list_dir(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    Type list_type = type_array(string_type());
    DIR *d = opendir(path);
    if (d == NULL) return fs_err(list_type, "cannot list", path);
    int cap = 16, n = 0;
    char **names = malloc((size_t)cap * sizeof(char *));
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n == cap) {
            cap *= 2;
            names = realloc(names, (size_t)cap * sizeof(char *));
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), compare_names);
    Value *items = malloc((size_t)(n > 0 ? n : 1) * sizeof(Value));
    for (int i = 0; i < n; i++) items[i] = value_string(names[i]);
    free(names);
    Value arr = value_array(list_type, array_obj_new(items, n));
    return value_result(type_result(list_type, string_type()), result_obj_new(1, arr));
}

void native_fs_register(void) {
    native_register("fs::write_text", native_fs_write_text);
    native_register("fs::append_text", native_fs_append_text);
    native_register("fs::exists", native_fs_exists);
    native_register("fs::is_dir", native_fs_is_dir);
    native_register("fs::size", native_fs_size);
    native_register("fs::remove", native_fs_remove);
    native_register("fs::make_dir", native_fs_make_dir);
    native_register("fs::rename", native_fs_rename);
    native_register("fs::list_dir", native_fs_list_dir);
}
