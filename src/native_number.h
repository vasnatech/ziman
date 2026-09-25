#ifndef ZIMAN_NATIVE_NUMBER_H
#define ZIMAN_NATIVE_NUMBER_H

/* Registers the `number` core module's natives (float bytes, hex text of byte arrays).
 * Called once from main() before interpret() runs. See lib/core/number.zm. */
void native_number_register(void);

#endif
