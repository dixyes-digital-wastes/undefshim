#ifndef US_TRANSLATE_H
#define US_TRANSLATE_H

#include <stdint.h>

/* Query EL1 access without dereferencing the VA, preserving PAR_EL1 */
bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa);

#endif
