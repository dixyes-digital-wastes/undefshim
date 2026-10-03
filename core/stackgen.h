#ifndef US_STACKGEN_H
#define US_STACKGEN_H

#define US_STACK_LOOKUP_WORDS 320

#ifndef __ASSEMBLER__
#include <stdint.h>

#include "common/layout.h"

/* RVAs share a payload base; stackTop entries already carry relocated VAs
 * Returns used words, or zero without changing out on invalid input
 * mpidr contains distinct normalized affinity IDs; zero is a valid ID
 */
uint32_t usGenerateStackLookup(const uint64_t *mpidr, uint32_t cpuCount,
                               uint32_t codeRva, uint32_t stackTopTableRva,
                               uint32_t readyRva, uint32_t haltRva,
                               uint32_t out[US_STACK_LOOKUP_WORDS]);
#endif

#endif
