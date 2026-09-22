#ifndef ZIMAN_NATIVE_IO_H
#define ZIMAN_NATIVE_IO_H

/* Registers the `io` native module's functions (read_file, write_file)
 * with native.c's table. Called once from main() before interpret() runs.
 * See docs/SYNTAX.md's "Modules" section for io.zm, the Ziman-side
 * signature file -- bundled as a core module (corelib/io.zm, see
 * corelib.h), so a project needs no local copy of it to `use io`. */
void native_io_register(void);

#endif
