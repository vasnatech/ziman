#ifndef ZIMAN_NATIVE_BITS_H
#define ZIMAN_NATIVE_BITS_H

/* Registers the `bits` native module's functions -- bitwise and/or/xor/not/shift on `byte`,
 * `int` and `bigint`, plus the `BitSet`/`BigBitSet` scalar storage they back -- with native.c's
 * table. Called once from main() before interpret() runs. See lib/core/bits.zm and
 * docs/design/journal/'s "A bits module" entry. */
void native_bits_register(void);

#endif
