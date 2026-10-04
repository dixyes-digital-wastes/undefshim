/*
 * Finding our own mapping, by asking the hardware
 *
 * The payload knows the physical address of the pool it lives in and has to
 * find the virtual address that reaches it once the kernel's tables are in
 * force. Reading those tables is not possible from here: they are not
 * reachable at their own physical address, which was tried and faults
 *
 * So the hardware is asked to translate instead. A translation attempt
 * answers in a register and does not require the tables to be readable, which
 * makes it the only instrument available at the moment this runs
 *
 * The search is bounded by where the answer can be. The pool has to appear
 * somewhere near the kernel, because both are mapped by the same tables, so
 * the search starts from the kernel's own entry point and looks outwards. It
 * is a coarse sweep first, since a region this size is covered by one block
 * entry, and a fine one only over the block that matched
 */

#ifndef US_SELFMAP_H
#define US_SELFMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"

typedef struct UsSelfMap_t {
    bool     found;
    uint64_t va;             /* where the pool can be reached from here on */
    uint64_t pa;             /* what was looked for */
    uint64_t size;           /* the mapping that covers it */
    uint64_t probes;         /* translation attempts spent */
    bool     faulted;        /* an attempt reported a failure */
    bool     exhausted;      /* the search gave up before answering */
} UsSelfMap;

/*
 * Looks for an address that translates to targetPA, searching near nearVA
 *
 * nearVA is expected to be the kernel's own entry point, which is known at
 * the handover because it is the register the loader is about to branch to.
 * The search covers a window either side of it, which is where a runtime
 * region has been measured to land
 */
UsSelfMap usSelfMapFind(uint64_t targetPA, uint64_t targetBytes, uint64_t nearVA);

/*
 * The window searched, either side of nearVA
 *
 * Measured across boots, the pool's address sits between 81 and 141 MB from
 * the kernel's, the variation coming from how the two regions are placed
 * independently. 256 MB leaves room for that without turning the search into
 * a sweep of the address space
 */
#define US_SELFMAP_WINDOW (256ULL << 20)

/*
 * Attempts allowed. The sweep is at page granularity over twice the window,
 * which is 131073, and that is the whole of the budget: a search that stopped
 * early would be one whose negative answer means nothing
 */
#define US_SELFMAP_MAX_PROBES (((2ULL * US_SELFMAP_WINDOW) / US_PAGE_SIZE) + 1)


#endif
