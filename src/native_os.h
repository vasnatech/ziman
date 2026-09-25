#ifndef ZIMAN_NATIVE_OS_H
#define ZIMAN_NATIVE_OS_H

/* Registers the `os` core module's natives (getenv_raw). Called once from
 * main() before interpret() runs. See lib/core/os.zm. */
void native_os_register(void);

/* Non-zero once SIGINT or SIGTERM has arrived after os.install_shutdown_handler().
 * net.accept polls it, so a server's accept loop wakes up and can drain. */
int ziman_shutdown_requested(void);

/* The arguments after `--` on the command line (`ziman prog.zm -- a b`), for os.args(). */
void ziman_set_program_args(int count, char **args);

#endif
