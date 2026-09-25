#define _POSIX_C_SOURCE 200809L
#include "native_compress.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "error.h"
#include "native.h"

/* The `compress` core module's natives, on zlib: compress_raw(data, format) and
 * decompress_raw(data, max_size, format). format: 0 gzip, 1 zlib, 2 raw deflate, 3 (decompress
 * only) auto-detect gzip or zlib. See lib/core/compress.zm. */

static Value err_of(Type ok_type, const char *message) {
    return value_result(type_result(ok_type, type_scalar(TY_STRING)), result_obj_new(0, value_string(strdup(message))));
}

static Value native_compress_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    int n = 0;
    uint8_t *in = array_copy_bytes(args[0].as.arr, &n);
    int format = (int)args[1].as.i;
    int window = format == 0 ? 15 + 16 : (format == 2 ? -15 : 15);
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(in);
        ziman_error("runtime", line, col, "compress: cannot start compression");
    }
    uLong bound = deflateBound(&zs, (uLong)n) + 32;
    unsigned char *out = malloc(bound);
    zs.next_in = in;
    zs.avail_in = (uInt)n;
    zs.next_out = out;
    zs.avail_out = (uInt)bound;
    int rc = deflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    deflateEnd(&zs);
    free(in);
    if (rc != Z_STREAM_END) {
        free(out);
        ziman_error("runtime", line, col, "compress: compression failed");
    }
    Value v = value_byte_array(out, produced);
    free(out);
    return v;
}

static Value native_decompress_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type bytes_type = type_array(type_scalar(TY_BYTE));
    int n = 0;
    uint8_t *in = array_copy_bytes(args[0].as.arr, &n);
    int64_t max = args[1].as.i;
    int format = (int)args[2].as.i;
    int window = format == 0 ? 15 + 16 : (format == 2 ? -15 : (format == 1 ? 15 : 15 + 32));
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, window) != Z_OK) { free(in); return err_of(bytes_type, "cannot start decompression"); }
    size_t cap = (size_t)n * 4 + 256, len = 0;
    unsigned char *out = malloc(cap);
    zs.next_in = in;
    zs.avail_in = (uInt)n;
    int rc;
    do {
        if (len == cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        zs.next_out = out + len;
        zs.avail_out = (uInt)(cap - len);
        rc = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (max >= 0 && (int64_t)len > max) {
            inflateEnd(&zs);
            free(in);
            free(out);
            return err_of(bytes_type, "decompressed data is too large");
        }
    } while (rc == Z_OK);
    inflateEnd(&zs);
    free(in);
    if (rc != Z_STREAM_END) {
        free(out);
        return err_of(bytes_type, rc == Z_BUF_ERROR ? "compressed data is truncated" : "compressed data is corrupt");
    }
    Value bytes = value_byte_array(out, len);
    free(out);
    return value_result(type_result(bytes_type, type_scalar(TY_STRING)), result_obj_new(1, bytes));
}

void native_compress_register(void) {
    native_register("compress::compress_raw", native_compress_raw);
    native_register("compress::decompress_raw", native_decompress_raw);
}
