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
 * The caller must establish that the address is identity mapped and readable
 * before dereferencing it (for example, using AT at EL1). On failure it may
 * return zero and record the failure in ctx; the caller must check that flag
 * before trusting the result. The walker itself never dereferences an address.
 * On a host the callback can instead read a table represented by an array.
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

    /* Raw leaf descriptor and effective privileged execute-never: leaf PXN
     * (bit 53) or PXNTable (bit 59) on any ancestor table. */
    uint64_t descriptor;
    bool     pxn;
} UsPageWalk;

/*
 * Finds the first mapping, at or above vaFirst and at or below vaLast, whose
 * physical range overlaps [targetPa, targetPa + targetBytes).
 *
 * tableBase is the translation table base as it appears in a TTBR. ASID and
 * bits below the root's alignment are masked off. The root has 2^(vaBits-39)
 * entries and is aligned to its size in bytes (47 bits: 2KB, 48 bits: 4KB).
 * Child tables are still 4KB aligned. vaBits must be in [40, 48], with an
 * untagged, canonical VA range wholly in one TTBR half. Invalid arguments,
 * including overflow of targetPa + targetBytes, return an empty result.
 *
 * A match means overlap, not validation of the entire target range. The caller
 * must search/check every page when validating a full range, and must also
 * check any read-failure flag maintained in ctx.
 *
 * Terminates early on a match. Returns found = false when there is no such
 * mapping, or when the budget ran out first, which the budgetExhausted flag
 * distinguishes: an exhausted budget means the answer is unknown rather than
 * negative, and a caller that acts on it would be guessing.
 */
UsPageWalk usPageWalkFindBits(UsPhysRead read, void *ctx, uint64_t tableBase,
                              uint64_t vaFirst, uint64_t vaLast,
                              uint64_t targetPa, uint64_t targetBytes,
                              unsigned vaBits);

/* Compatibility entry point: a 48-bit VA regime. */
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

/* Address mask for child table/page descriptors, not shortened TTBR roots. */
#define US_PAGE_ADDR_MASK 0x0000FFFFFFFFF000ULL

#endif
