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

/*
 * Where the kernel keeps the descriptor that translates a virtual address.
 *
 * Windows maps the translation tables into the address space and lays them out
 * in virtual address order, so the descriptor for an address is a computation
 * rather than a search. The base was read out of the image: every site shaped
 * like `lsr #12; and #0xfffffffff; lsl #3; add` takes its base from a literal
 * in .text, and three of those literals hold this value - the same one the x64
 * kernel uses. The end of the range is the other literal seen beside them, and
 * the check that uses these pins both down.
 *
 * The mask is the part that is easy to get wrong: shifting the page number of a
 * kernel half address without it overflows 64 bits. Keeping the address bits
 * above the page is what the kernel's own code does, and 4K granule with a
 * 48-bit address space is what this one is.
 */
#define US_PTE_SELFMAP_BASE UINT64_C(0xFFFFF68000000000)
#define US_PTE_SELFMAP_END  UINT64_C(0xFFFFF6FFFFFFFFFF)
#define US_PTE_VA_MASK      UINT64_C(0x0000FFFFFFFFF000)

/*
 * The descriptor of the page an address is on, given the page level's base.
 *
 * The same computation the kernel applies to an address, applied to a base as
 * well: the page holding a table's descriptors is described by an entry in the
 * table below it, so this is also how one level's base gives the next one up.
 */
static inline uint64_t usPteSlotFor(uint64_t selfMapBase, uint64_t va) {
    return selfMapBase + ((va & US_PTE_VA_MASK) >> 9);
}

/* What MiGetPteAddress computes, with the literal it uses as the base. It is
 * kept for the checks that pin that literal down; the payload does not use it,
 * because on this kernel the literal names an address that does not translate
 * and the live base is MmPteBase. */
static inline uint64_t usPteForAddress(uint64_t va) {
    return usPteSlotFor(US_PTE_SELFMAP_BASE, va);
}

/*
 * The same arrangement, one base per level, and the reason the base is an
 * argument rather than a constant.
 *
 * The page that holds a table's descriptors is itself mapped, and by the same
 * rule, which makes the levels a recursion: the base of the next level up is
 * the page descriptor of this level's base, computed by the same formula. The
 * top of the recursion is the base itself, and everything below it follows.
 *
 * That base is not the constant the image carries. MiGetPteAddress computes
 * descriptor addresses from a literal in .text -- 0xfffff68000000000, the
 * value the x64 kernel uses -- but this kernel runs a 47-bit address space
 * (TCR_EL1.T1SZ is 17), in which that address is not canonical and translates
 * to nothing at all. What its code actually dereferences is the variable
 * MmPteBase, which the boot sets; MiGetPteAddress's literal is only right on a
 * 48-bit configuration. So the base is read at runtime and passed in here.
 *
 * Given a base, the descriptor at level L that translates va is at
 *
 *     bases[L] + ((va >> shift(L)) & ((1 << bits(L)) - 1)) * 8
 *
 * in the order the table below lists: the top level first.
 */
#define US_PDE_SELFMAP_BASE UINT64_C(0xFFFFF6FB40000000)
#define US_PPE_SELFMAP_BASE UINT64_C(0xFFFFF6FB7DA00000)
#define US_PXE_SELFMAP_BASE UINT64_C(0xFFFFF6FB7DBED000)

/* AP[2] in a leaf descriptor: set means the page is read-only at EL1. */
#define US_PTE_AP2 (UINT64_C(1) << 7)

static inline uint64_t usPteWithWrite(uint64_t descriptor, bool writable) {
    return writable ? (descriptor & ~US_PTE_AP2) : (descriptor | US_PTE_AP2);
}

/*
 * Reading a descriptor, in the address space that is in force.
 *
 * The same shape as UsPhysRead above, and deliberately not the same thing: the
 * boot walk follows table entries to physical addresses because no mapping of
 * them exists yet, while at runtime the tables are mapped and are read through
 * their own addresses.
 */
typedef uint64_t (*UsWordRead)(void *ctx, uint64_t va);

/*
 * The shape of a level, apart from where it is.
 *
 * The index fields are nine bits each and start at bit 39 at the top, so both
 * the shift and the width follow from the level's number rather than being
 * written down: four levels of nine bits is the whole of a 47-bit address
 * space, which is what this kernel uses. What does not follow is what a leaf
 * descriptor at a level can be, which is the table below.
 */
#define US_LEVEL_SHIFT(index) (39U - 9U * (index))
#define US_LEVEL_BITS(index) (9U * ((index) + 1U))

typedef struct UsSelfMapLevel_t {
    /*
     * The physical address field of a leaf descriptor here, or zero where a
     * block is not a leaf at all.
     *
     * A block maps the address itself at the middle two levels; at the top it
     * would be a 512GB block, which is not a mapping this handles, and at the
     * bottom it is reserved. Which of those a level is shows in the width of
     * its address field, so the same number answers both questions.
     */
    uint64_t paField;
} UsSelfMapLevel;

/* Top down, the same order and numbering as the walk above: L0 to L3. */
#define US_SELF_MAP_LEVELS 4
extern const UsSelfMapLevel usSelfMapLevels[US_SELF_MAP_LEVELS];

typedef struct UsLeaf_t {
    bool     found;
    unsigned level;         /* which of the four maps the address */
    uint64_t descriptorVa;  /* where that descriptor is, writable in place */
    uint64_t descriptor;    /* what it holds now */
    uint64_t size;          /* how much it covers: 4K, 2M or 1G */
    uint64_t pa;            /* where the address translates to, offset included */
} UsLeaf;

/*
 * Finds the descriptor that translates an address which is already mapped.
 *
 * selfMapBase is the value of the kernel's MmPteBase. Every address this is
 * used on has just been executed or read, so the walk cannot run off the end
 * of a table: the chain that translates it exists. Each level is read only
 * after the level above it has been found to point at a table, which is what
 * keeps a read from following a descriptor that is not there. Reading a level
 * that a block ended would not fault, and that is the reason for the order: it
 * would return memory the block maps, and eight bytes of that can look exactly
 * like a descriptor.
 *
 * found is false when no level maps the address. Nothing is written.
 */
UsLeaf usLeafFind(uint64_t selfMapBase, UsWordRead read, void *ctx, uint64_t va);

#endif
