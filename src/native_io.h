#ifndef ZIMAN_NATIVE_IO_H
#define ZIMAN_NATIVE_IO_H

/* Registers the `io` native module's functions (read_file, write_file)
 * with native.c's table. Called once from main() before interpret() runs.
 * See docs/SYNTAX.md's "Modules" section for io.zm, the Ziman-side
 * signature file every project using this needs its own copy of for now
 * (no bundled-library search path exists yet). */
void native_io_register(void);

#endif
