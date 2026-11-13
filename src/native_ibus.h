#ifndef ZIMAN_NATIVE_IBUS_H
#define ZIMAN_NATIVE_IBUS_H

/* Driving an ibus input context, so a composition typed in Japanese, Chinese or Korean reaches
 * `lib/ui/edit.zm`'s pre-edit model. See native_ibus.c for why this speaks D-Bus directly where
 * the accessibility binding went through ATK, and why it is in the `make gui` build only. */
void native_ibus_register(void);

#endif
