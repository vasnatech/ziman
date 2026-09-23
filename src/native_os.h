#ifndef ZIMAN_NATIVE_OS_H
#define ZIMAN_NATIVE_OS_H

/* Registers the `os` core module's natives (getenv_raw). Called once from
 * main() before interpret() runs. See corelib/os.zm. */
void native_os_register(void);

#endif
