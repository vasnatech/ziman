#define _POSIX_C_SOURCE 200809L
#include "native_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "iter.h"
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

/* Reads the whole file. A NUL byte can't live in a string, so a file that
 * has one is an error rather than a silently truncated string. */
static Value native_fs_read_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    Type str = string_type();
    FILE *f = fopen(path, "rb");
    if (f == NULL) return fs_err(str, "cannot read", path);
    size_t cap = 4096, n = 0;
    char *buf = zalloc_atomic(cap + 1);
    for (;;) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        cap *= 2;
        char *bigger = zalloc_atomic(cap + 1);
        memcpy(bigger, buf, n);
        buf = bigger;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) return fs_err(str, "cannot read", path);
    buf[n] = '\0';
    if (strlen(buf) != n) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "cannot read '%s': file contains a NUL byte", path);
        return value_result(type_result(str, str), result_obj_new(0, value_string(strdup(msg))));
    }
    return value_result(type_result(str, str), result_obj_new(1, value_string(buf)));
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

/* ---- bytes, directories, copying, atomic writes -------------------------------------- */
static Value fs_ok_bytes(uint8_t *buf, size_t n) {
    Type bt = type_array(type_scalar(TY_BYTE));
    return value_result(type_result(bt, string_type()), result_obj_new(1, value_byte_array(buf, n)));
}

static Value native_fs_read_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    Type bt = type_array(type_scalar(TY_BYTE));
    FILE *f = fopen(path, "rb");
    if (f == NULL) return fs_err(bt, "cannot read", path);
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        cap *= 2;
        buf = realloc(buf, cap);
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) { free(buf); return fs_err(bt, "cannot read", path); }
    Value v = fs_ok_bytes(buf, n);
    free(buf);
    return v;
}

static Value fs_write_bytes(Value *args, const char *mode, const char *what) {
    const char *path = value_cstr(args[0]);
    int n = 0;
    uint8_t *data = array_copy_bytes(args[1].as.arr, &n);
    FILE *f = fopen(path, mode);
    if (f == NULL) { free(data); return fs_err(type_scalar(TY_INT), what, path); }
    size_t wrote = n > 0 ? fwrite(data, 1, (size_t)n, f) : 0;
    int failed = wrote != (size_t)n;
    if (fclose(f) != 0) failed = 1;
    free(data);
    if (failed) return fs_err(type_scalar(TY_INT), what, path);
    return fs_ok_int((int64_t)n);
}

static Value native_fs_write_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return fs_write_bytes(args, "wb", "cannot write");
}

static Value native_fs_append_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return fs_write_bytes(args, "ab", "cannot append to");
}

/* Writes to a temporary file in the same directory, then renames it over `path`, so a reader
 * (or a crash) sees the old contents or the new, never half of the new. */
static Value native_fs_write_text_atomic(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return fs_err(type_scalar(TY_INT), "cannot write", path);
    size_t n = fwrite(args[1].as.str.ptr, 1, args[1].as.str.len, f);
    int failed = n != args[1].as.str.len;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed || rename(tmp, path) != 0) {
        int saved = errno;
        remove(tmp);
        errno = saved;
        return fs_err(type_scalar(TY_INT), "cannot write", path);
    }
    return fs_ok_int((int64_t)n);
}

static Value native_fs_is_file(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    struct stat st;
    return value_bool(stat(value_cstr(args[0]), &st) == 0 && S_ISREG(st.st_mode));
}

/* mkdir -p: creates every missing directory on the path; Ok(0) if it already exists as a directory. */
static Value native_fs_make_dirs(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    char buf[4096];
    if (strlen(path) >= sizeof buf) { errno = ENAMETOOLONG; return fs_err(type_scalar(TY_INT), "cannot create directory", path); }
    strcpy(buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return fs_err(type_scalar(TY_INT), "cannot create directory", path);
        *p = '/';
    }
    if (mkdir(buf, 0777) != 0) {
        struct stat st;
        if (errno == EEXIST && stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) return fs_ok_int(0);
        return fs_err(type_scalar(TY_INT), "cannot create directory", path);
    }
    return fs_ok_int(0);
}

static Value native_fs_copy_file(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *from = value_cstr(args[0]);
    const char *to = value_cstr(args[1]);
    FILE *in = fopen(from, "rb");
    if (in == NULL) return fs_err(type_scalar(TY_INT), "cannot read", from);
    FILE *out = fopen(to, "wb");
    if (out == NULL) { int saved = errno; fclose(in); errno = saved; return fs_err(type_scalar(TY_INT), "cannot write", to); }
    char chunk[65536];
    size_t total = 0, got;
    int failed = 0;
    while ((got = fread(chunk, 1, sizeof chunk, in)) > 0) {
        if (fwrite(chunk, 1, got, out) != got) { failed = 1; break; }
        total += got;
    }
    if (ferror(in)) failed = 1;
    fclose(in);
    if (fclose(out) != 0) failed = 1;
    if (failed) return fs_err(type_scalar(TY_INT), "cannot copy to", to);
    return fs_ok_int((int64_t)total);
}

/* Last modification time, in seconds since the Unix epoch. */
static Value native_fs_modified_seconds(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *path = value_cstr(args[0]);
    struct stat st;
    if (stat(path, &st) != 0) return fs_err(type_scalar(TY_INT), "cannot stat", path);
    return fs_ok_int((int64_t)st.st_mtime);
}


/* fs.lines(path): a lazy iterator over the lines of a file. Each item is a Result<string, string>:
 * the line without its "\n" or "\r\n", or an Err for a line that cannot be a string (a NUL byte) or a
 * read error, after which the iteration ends. The file is read a buffer at a time, so a file of any
 * size uses memory for one line. */
static Value line_item(int ok, Value v) {
    Type str = string_type();
    return value_result(type_result(str, str), result_obj_new(ok, v));
}

static int lines_next(IterObj *it, Value *out) {
    LineSource *ls = it->state;
    const char *line;
    size_t len;
    int64_t number;
    char err[256];
    int r = line_source_next(ls, &line, &len, &number, err, sizeof err);
    if (r == 0) return 0;
    if (r < 0) {
        *out = line_item(0, value_string(strdup(err)));
        return 1;
    }
    if (memchr(line, '\0', len) != NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "line %lld contains a NUL byte", (long long)number);
        *out = line_item(0, value_string(strdup(msg)));
        return 1;
    }
    *out = line_item(1, value_string_copy(line, len));
    return 1;
}

static void lines_release(IterObj *it) { line_source_close(it->state); }

static Value native_fs_lines(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    const char *path = value_cstr(args[0]);
    Type iter_type = type_struct("Iter");
    LineSource *ls = line_source_open(path);
    if (ls == NULL) return fs_err(iter_type, "cannot read", path);
    IterObj *it = calloc(1, sizeof(IterObj));
    it->next = lines_next;
    it->release = lines_release;
    it->state = ls;
    return value_result(type_result(iter_type, string_type()), result_obj_new(1, iter_make(it)));
}

void native_fs_register(void) {
    native_register("fs::read_text", native_fs_read_text);
    native_register("fs::lines", native_fs_lines);
    native_register("fs::write_text", native_fs_write_text);
    native_register("fs::append_text", native_fs_append_text);
    native_register("fs::exists", native_fs_exists);
    native_register("fs::is_dir", native_fs_is_dir);
    native_register("fs::size", native_fs_size);
    native_register("fs::remove", native_fs_remove);
    native_register("fs::make_dir", native_fs_make_dir);
    native_register("fs::rename", native_fs_rename);
    native_register("fs::list_dir", native_fs_list_dir);
    native_register("fs::read_bytes", native_fs_read_bytes);
    native_register("fs::write_bytes", native_fs_write_bytes);
    native_register("fs::append_bytes", native_fs_append_bytes);
    native_register("fs::write_text_atomic", native_fs_write_text_atomic);
    native_register("fs::is_file", native_fs_is_file);
    native_register("fs::make_dirs", native_fs_make_dirs);
    native_register("fs::copy_file", native_fs_copy_file);
    native_register("fs::modified_seconds", native_fs_modified_seconds);
}
