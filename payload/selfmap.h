/*
 * Finding our own mapping.
 *
 * The payload knows the physical address of the pool it lives in, and has to
 * find the virtual address that reaches it. Nothing about that address can be
 * worked out: the kernel places its regions independently, and the offset
 * between physical memory and the space it appears in is not constant even
 * within one boot.
 *
 * So the tables are read. That is only possible where physical memory is
 * still reachable at its own address, which is why this runs at the handover
 * and not later: after the switch, a table entry naming a physical address
 * could not be followed any more.
 */

#ifndef US_SELFMAP_H
#define US_SELFMAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct UsSelfMap_t {
    bool     found;
    uint64_t va;             /* where the pool can be reached from here on */
    uint64_t pa;             /* what was looked for */
    uint64_t size;           /* the mapping that covers it: 4K, 2M or 1G */
    uint64_t entriesRead;
    bool     exhausted;      /* the walk gave up before answering */
} UsSelfMap;

/*
 * Reads the kernel's translation tables and reports where the pool is in
 * them. Returns a result with found = false when there is no such mapping,
 * which is a real possibility and not a failure of the search.
 */
UsSelfMap usSelfMapFind(uint64_t targetPa, uint64_t targetBytes);

#endif
