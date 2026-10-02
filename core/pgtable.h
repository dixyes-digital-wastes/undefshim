/*
 * Reading a translation table to find where something is mapped.
 *
 * The payload has to answer a question about itself that nothing else can
 * answer for it: it knows the physical address of the pool it lives in, and
 * needs to know which virtual address reaches that pool once the kernel's
 * address space is in force. Nothing about that address can be computed: the
 * kernel places things independently of each other, so the mapping has to be
 * found by reading the tables that describe it.
 *
 * This is done at the handover, where physical memory is still reachable at
 * its own address, which is what makes reading a table at all possible: a
 * table entry names a physical address, and the only way to follow it is to
 * be able to read that address directly.
 *
 * The walk is bounded by a virtual range rather than run over the whole
 * address space. The caller knows roughly where the mapping lives, and
 * scanning 256 TB of address space to find 33 pages would be absurd.
 */

#ifndef US_PGTABLE_H
#define US_PGTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Reads a 64-bit word from a physical address.
 *
 * In the payload this is a plain dereference, because physical memory is
 * identity mapped at the time. It is a function here so that the walk can be
 * tested on a host, where a table is just an array.
 */
typedef uint64_t (*UsPhysRead)(void *ctx, uint64_t address);

/* Granule sizes a descriptor can cover. */
#define US_GRANULE_4K (1ULL << 12)
#define US_GRANULE_2M (1ULL << 21)
#define US_GRANULE_1G (1ULL << 30)

typedef struct UsPageWalk_t {
    bool     found;
    /* The start of the mapping that covers the target. */
    uint64_t va;
    uint64_t mappedPa;
    /* How much that mapping covers: 4K, 2M or 1G. */
    uint64_t size;

    /* What the walk cost, so a caller can tell a full scan from an early
     * exit, and a truncated one from either. */
    uint64_t tablesRead;
    uint64_t entriesRead;
    bool     budgetExhausted;
} UsPageWalk;

/*
 * Finds the first mapping, at or above vaFirst and at or below vaLast, whose
 * physical range overlaps [targetPa, targetPa + targetBytes).
 *
 * tableBase is the translation table base for the regime being searched, as
 * it appears in a TTBR: the low bits are flags and are masked off here, so a
 * value read straight from the register can be passed in.
 *
 * Terminates early on a match. Returns found = false when there is no such
 * mapping, or when the budget ran out first, which the budgetExhausted flag
 * distinguishes: an exhausted budget means the answer is unknown rather than
 * negative, and a caller that acts on it would be guessing.
 */
UsPageWalk usPageWalkFind(UsPhysRead read, void *ctx, uint64_t tableBase,
                          uint64_t vaFirst, uint64_t vaLast,
                          uint64_t targetPa, uint64_t targetBytes);

/*
 * How many entries a walk may examine before it gives up. A translation
 * regime that is intact needs far fewer; this is a guard against a
 * descriptor that points somewhere unexpected turning the walk into a very
 * long read of unrelated memory.
 */
#define US_PAGE_WALK_BUDGET 20000ULL

/* The mask applied to a table base or a page descriptor to get its address. */
#define US_PAGE_ADDR_MASK 0x0000FFFFFFFFF000ULL

#endif
