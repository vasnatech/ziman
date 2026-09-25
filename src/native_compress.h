#ifndef ZIMAN_NATIVE_COMPRESS_H
#define ZIMAN_NATIVE_COMPRESS_H

/* Registers the `compress` core module's natives (zlib). Called once from main() before
 * interpret() runs. See lib/core/compress.zm. */
void native_compress_register(void);

#endif
