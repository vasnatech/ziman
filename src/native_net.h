#ifndef ZIMAN_NATIVE_NET_H
#define ZIMAN_NATIVE_NET_H

/* Registers the `net` core module's native functions (blocking TCP over
 * POSIX sockets) and the `bytes` module's two string/[byte] conversions
 * with native.c's table. Called once from main() before interpret()
 * runs. See corelib/net.zm and corelib/bytes.zm for the Ziman-side API. */
void native_net_register(void);

#endif
