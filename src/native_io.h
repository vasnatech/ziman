#ifndef ZIMAN_NATIVE_IO_H
#define ZIMAN_NATIVE_IO_H

/* Registers the `io` core module's natives (stdin_read). Called once from
 * main() before interpret() runs. See lib/core/io.zm. */
void native_io_register(void);

#endif
