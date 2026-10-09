/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Reading a translation table to find where something is mapped
 *
 * The payload has to answer a question about itself that nothing else can
 * answer for it: it knows the physical address of the pool it lives in, and
 * needs to know which virtual address reaches that pool once the kernel's
 * address space is in force. Nothing about that address can be computed: the
 * kernel places things independently of each other, so the mapping has to be
 * found by reading the tables that describe it
 */

#ifndef US_PGTABLE_H
#define US_PGTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Granule sizes a descriptor can cover */
#define US_GRANULE_4K (1ULL << 12)
#define US_GRANULE_2M (1ULL << 21)
#define US_GRANULE_1G (1ULL << 30)

/* Address mask for child table/page descriptors, not shortened TTBR roots */
#define US_PAGE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/*
 * Where the kernel keeps the descriptor that translates a virtual address
 *
 * Windows maps the translation tables into the address space and lays them out
 * in virtual address order, so the descriptor for an address is a computation
 * rather than a search. The kernel keeps that base in a variable and reads it
 * through a four-instruction body whose literal is the base, so the locator
 * finds the body by shape and takes the literal from it. The value below is
 * the one the image carries; the live one is the kernel's variable, and
 * the check that uses these pins both down
 *
 * The mask is the part that is easy to get wrong: shifting the page number of a
 * kernel half address without it overflows 64 bits. Keeping the address bits
 * above the page is what the kernel's own code does, and 4K granule with a
 * 48-bit address space is what this one is
 */
#define US_PTE_SELFMAP_BASE UINT64_C(0xFFFFF68000000000)
#define US_PTE_SELFMAP_END  UINT64_C(0xFFFFF6FFFFFFFFFF)
#define US_PTE_VA_MASK      UINT64_C(0x0000FFFFFFFFF000)

/*
 * The descriptor of the page an address is on, given the page level's base
 *
 * The same computation the kernel applies to an address, applied to a base as
 * well: the page holding a table's descriptors is described by an entry in the
 * table below it, so this is also how one level's base gives the next one up
 */
static inline uint64_t usPTESlotFor(uint64_t selfMapBase, uint64_t va) {
    return selfMapBase + ((va & US_PTE_VA_MASK) >> 9);
}

/* What MiGetPteAddress computes, with the literal it uses as the base. It is
 * kept for the checks that pin that literal down; the payload does not use it,
 * because on this kernel the literal names an address that does not translate
 * and the live base is MmPteBase */
static inline uint64_t usPTEForAddress(uint64_t va) {
    return usPTESlotFor(US_PTE_SELFMAP_BASE, va);
}

/*
 * The same arrangement, one base per level, and the reason the base is an
 * argument rather than a constant
 *
 * The page that holds a table's descriptors is itself mapped, and by the same
 * rule, which makes the levels a recursion: the base of the next level up is
 * the page descriptor of this level's base, computed by the same formula. The
 * top of the recursion is the base itself, and everything below it follows
 *
 * That base is not the constant the image carries. MiGetPteAddress computes
 * descriptor addresses from a literal in .text -- 0xfffff68000000000, the
 * value the x64 kernel uses -- but this kernel runs a 47-bit address space
 * (TCR_EL1.T1SZ is 17), in which that address is not canonical and translates
 * to nothing at all. What its code actually dereferences is the variable
 * MmPteBase, which the boot sets; MiGetPteAddress's literal is only right on a
 * 48-bit configuration. So the base is read at runtime and passed in here
 *
 * Given a base, the descriptor at level L that translates va is at
 *
 *     bases[L] + ((va >> shift(L)) & ((1 << bits(L)) - 1)) * 8
 *
 * in the order the table below lists: the top level first
 */
#define US_PDE_SELFMAP_BASE UINT64_C(0xFFFFF6FB40000000)
#define US_PPE_SELFMAP_BASE UINT64_C(0xFFFFF6FB7DA00000)
#define US_PXE_SELFMAP_BASE UINT64_C(0xFFFFF6FB7DBED000)

/* AP[2] in a leaf descriptor: set means the page is read-only at EL1 */
#define US_PTE_AP2 (UINT64_C(1) << 7)

/*
 * The rest of what a leaf descriptor has to say besides its address
 *
 * The access flag is not optional here and its absence is not a lazy fault
 * that gets fixed up: on the path that uses these, a fault is the end of
 * everything, so the first access has to be allowed rather than arranged to
 * be allowed later
 *
 * Both execute-never bits are set because a frame buffer is data. Leaving
 * them clear would make the memory executable at EL1 and EL0 for no reason,
 * on an address the kernel never chose
 */
#define US_PTE_AF  (UINT64_C(1) << 10)
#define US_PTE_PXN (UINT64_C(1) << 53)
#define US_PTE_UXN (UINT64_C(1) << 54)

/* Where a descriptor's attribute index sits */
#define US_ATTR_INDEX_SHIFT 2U

/* Bits [1:0] of a descriptor, which say which of the three things it is */
#define US_DESC_BLOCK 1ULL
#define US_DESC_TABLE_OR_PAGE 3ULL

/*
 * A block descriptor covering the block pa falls in, with an attribute index
 *
 * Only the middle levels may hold a block, and how much it covers is what
 * says which of them this is: 1GB at the first, 2MB at the second, and 4KB is
 * a page rather than a block. The address is rounded down rather than checked,
 * because a block maps its whole region whatever the address given means
 */
static inline uint64_t usBlockDescriptor(uint64_t pa, uint64_t blockSize,
                                         uint64_t attrIndex) {
    return (pa & ~(blockSize - 1)) | (attrIndex << US_ATTR_INDEX_SHIFT)
           | US_PTE_AF | US_DESC_BLOCK | US_PTE_PXN | US_PTE_UXN;
}

/*
 * Reading a descriptor, in the address space that is in force
 *
 * The same shape as UsPhysRead above, and deliberately not the same thing: the
 * boot walk follows table entries to physical addresses because no mapping of
 * them exists yet, while at runtime the tables are mapped and are read through
 * their own addresses
 */
typedef uint64_t (*UsWordRead)(void *ctx, uint64_t va);

/*
 * The shape of a level, apart from where it is
 *
 * The index fields are nine bits each and start at bit 39 at the top, so both
 * the shift and the width follow from the level's number rather than being
 * written down: four levels of nine bits is the whole of a 47-bit address
 * space, which is what this kernel uses
 */
#define US_LEVEL_SHIFT(index) (39U - 9U * (index))
/*
 * How many bits of the page number a level's index carries
 *
 * Not nine, and that is the thing to be careful about. The self map is flat:
 * the entry for an address is at MmPteBase plus the whole of its page number,
 * and a level's base is that same array moved so that the level's entries
 * start there. So a level's index is every bit of the page number from its
 * shift upwards -- nine at the top and nine more at each level down -- rather
 * than the nine bits an entry has within its own table
 *
 * Written this way it reads like a width, and reading it as one is what makes
 * it look wrong: nine bits is what a table has, so the mask "should" be nine.
 * It is not. With nine bits the walk addresses the entry for a low address
 * instead of the one it wants, and on a machine whose tables are full of page
 * mappings the wrong entry is usually a table pointer as well -- so the walk
 * keeps descending, through tables that have nothing to do with the address,
 * and comes back with a descriptor that looks entirely plausible. The first
 * and the last level are the ones where the two readings agree, which is why
 * the mistake survives a test that only walks those
 */
#define US_LEVEL_BITS(index) (9U * ((index) + 1U))

/*
 * Where the entry that translates an address sits at one level
 *
 * The walk's own arithmetic, written once so that a caller wanting a single
 * entry does not restate it -- and restating it is exactly how it gets written
 * with nine bits
 */
static inline uint64_t usLevelEntry(uint64_t base, unsigned level, uint64_t va) {
    return base + ((va >> US_LEVEL_SHIFT(level))
                   & ((1ULL << US_LEVEL_BITS(level)) - 1ULL))
                      * 8U;
}

typedef struct UsSelfMapLevel_t {
    /*
     * The physical address field of a leaf descriptor here, or zero where a
     * block is not a leaf at all
     *
     * A block maps the address itself at the middle two levels; at the top it
     * would be a 512GB block, which is not a mapping this handles, and at the
     * bottom it is reserved. Which of those a level is shows in the width of
     * its address field, so the same number answers both questions
     */
    uint64_t paField;
} UsSelfMapLevel;

/* Top down, the same order and numbering as the walk above: L0 to L3 */
#define US_SELF_MAP_LEVELS 4
extern const UsSelfMapLevel usSelfMapLevels[US_SELF_MAP_LEVELS];

typedef struct UsLeaf_t {
    bool     found;
    unsigned level;         /* which of the four maps the address */
    uint64_t descriptorVA;  /* where that descriptor is, writable in place */
    uint64_t descriptor;    /* what it holds now */
    uint64_t size;          /* how much it covers: 4K, 2M or 1G */
    uint64_t pa;            /* where the address translates to, offset included */
} UsLeaf;

/*
 * Where each level's descriptors are, given the page level's base
 *
 * The recursion, written once so that everything that walks the tables agrees
 * about it: a level's descriptors are mapped by the level below, so the base
 * of the level above is that level's own page descriptor, computed by the same
 * formula. Filled top first, in the numbering the walk uses
 */
void usLevelBases(uint64_t selfMapBase, uint64_t bases[US_SELF_MAP_LEVELS]);

/*
 * What a descriptor at a level is
 *
 * The levels are not the same shape and this is where that shows: the same two
 * bits mean "a table" above the page level and "a page" at it, and a block is
 * only a thing at the middle two. A descriptor that is not one of these is
 * reserved, and is reported as something to leave alone rather than guessed at
 */
typedef enum UsDescriptorKind_e {
    /* Nothing is mapped through this entry, and it is all zeroes */
    UsDescriptorEmpty = 0,
    /* A pointer to the next table down */
    UsDescriptorTable,
    /* A page or a block, which ends the walk */
    UsDescriptorLeaf,
    /* Reserved, or a shape that cannot appear at this level */
    UsDescriptorOther,
} UsDescriptorKind;

UsDescriptorKind usDescriptorKind(uint64_t descriptor, unsigned level);

/*
 * The first level at which nothing is mapped for an address
 *
 * This is the question an address has to answer before anything may be written
 * over it: a mapping may only be put where the kernel has none, and the levels
 * above it must already be tables, since making one is not something this can
 * do. The top level is excluded for that reason, and its emptiness is reported
 * as nothing found
 *
 * found is false when the address is mapped, when a block covers it, or when
 * the walk cannot get past a level. Nothing is written either way
 */
typedef struct UsFreeSlot_t {
    bool     found;
    unsigned level;         /* which level's descriptor is free */
    uint64_t descriptorVA;  /* where that descriptor is, ready to be written */
} UsFreeSlot;

UsFreeSlot usFreeSlotFind(uint64_t selfMapBase, UsWordRead read, void *ctx,
                          uint64_t va);

/*
 * Finds the descriptor that translates an address which is already mapped
 *
 * selfMapBase is the value of the kernel's MmPteBase. Every address this is
 * used on has just been executed or read, so the walk cannot run off the end
 * of a table: the chain that translates it exists. Each level is read only
 * after the level above it has been found to point at a table, which is what
 * keeps a read from following a descriptor that is not there. Reading a level
 * that a block ended would not fault, and that is the reason for the order: it
 * would return memory the block maps, and eight bytes of that can look exactly
 * like a descriptor
 *
 * found is false when no level maps the address. Nothing is written
 */
UsLeaf usLeafFind(uint64_t selfMapBase, UsWordRead read, void *ctx, uint64_t va);

#endif
