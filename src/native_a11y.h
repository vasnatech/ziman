#ifndef ZIMAN_NATIVE_A11Y_H
#define ZIMAN_NATIVE_A11Y_H

/* Publishing `lib/ui/a11y.zm`'s tree over AT-SPI. See native_a11y.c for why ATK rather than D-Bus
 * by hand, and why this is in the `make gui` build only. */
void native_a11y_register(void);

#endif
