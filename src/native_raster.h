#ifndef ZIMAN_NATIVE_RASTER_H
#define ZIMAN_NATIVE_RASTER_H

/* Registers the two per-pixel natives behind lib/graphics/raster.zm (clear_premul,
 * to_bytes_premul). Called once from main() before interpret() runs.
 *
 * These are an optimisation of code that still exists in Ziman, not a new capability: see
 * src/native_raster.c for why these two loops and not the drawing, and
 * tests/stdlib/raster_native_test.zm for the oracle that holds them to the Ziman arithmetic. */
void native_raster_register(void);

#endif
