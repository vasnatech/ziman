#ifndef ZIMAN_NATIVE_TERMINAL_H
#define ZIMAN_NATIVE_TERMINAL_H

/* Registers the `terminal` core module's natives (is_tty, size_raw, set_raw,
 * read_raw). Called once from main() before interpret() runs. See
 * lib/core/terminal.zm and docs/design/terminal.md. */
void native_terminal_register(void);

#endif
