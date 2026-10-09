/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the descriptors of an address, read through the self map
 *
 * What is being checked is that the arithmetic that locates a descriptor
 * names the address the kernel's own literals name, and that a descriptor
 * that is not a mapping stops the search rather than being read on. The
 * answers are planted, so the test says exactly which shape of table produces
 * which answer, and no kernel is involved
 */

#include <stdio.h>

#include "common/layout.h"
#include "core/mair.h"
#include "core/pgtable.h"

#define US_CHECK_NAME "test_pgtable"
#include "check.h"

/* The kernel's virtual half, at the layout the walk assumes */
#define KVA_BASE 0xFFFF000000000000ULL

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-42s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

/* --- a table, planted one answer per address ---------------------------- */

/*
 * The self map is addressed the same way at every level, so a table is a
 * planted answer per address and the reader records where it was asked. That
 * makes the arithmetic the thing under test: if the implementation computed a
 * different address, the answer it gets back is nothing
 */
#define FAKE_SLOTS 8
static struct {
    uint64_t at;
    uint64_t value;
} gPlanted[FAKE_SLOTS];
static unsigned gPlantedCount;
static uint64_t gAsked[FAKE_SLOTS];
static unsigned gAskedCount;

static void plant(uint64_t at, uint64_t value) {
    if (gPlantedCount < FAKE_SLOTS) {
        gPlanted[gPlantedCount].at = at;
        gPlanted[gPlantedCount].value = value;
        gPlantedCount++;
    }
}

static uint64_t leafRead(void *ctx, uint64_t at) {
    (void)ctx;
    if (gAskedCount < FAKE_SLOTS) {
        gAsked[gAskedCount++] = at;
    }
    for (unsigned i = 0; i < gPlantedCount; i++) {
        if (gPlanted[i].at == at) {
            return gPlanted[i].value;
        }
    }
    return 0;
}

/* Where the four descriptors are, written the long way: this is what the
 * implementation has to compute, so writing it out again is the check
 *
 * Every bit of the page number from this level's shift upwards, which is what
 * the implementation uses and is not nine: the self map is flat, so a level's
 * index carries the bits the levels above it have already accounted for as
 * well. Written as 48 - shift rather than as nine so that it is the flat rule
 * being restated and not a second guess at the width -- this helper is meant
 * to be an independent statement of the layout, and a copy of the
 * implementation's own expression would agree with it whatever it said */
static uint64_t slot(uint64_t base, uint64_t va, unsigned shift) {
    return base + ((va >> shift) & ((1ULL << (48 - shift)) - 1)) * 8;
}

/* The level bases as the payload will see them: the kernel's own value, which
 * the image's literal is not. Called before any planting, so the test does not
 * depend on the implementation's internals */
static void levelBases(uint64_t base, uint64_t out[4]) {
    out[3] = base;
    for (int i = 3; i > 0; i--) {
        out[i - 1] = base + ((out[i] & US_PTE_VA_MASK) >> 9);
    }
}

static void resetPlant(void) {
    gPlantedCount = 0;
    gAskedCount = 0;
}

/* The base the image's literal names, which everything below can be checked
 * against because the image also carries the descriptors that come out of it.
 * The kernel this runs under uses a different one, passed in at runtime */
#define IMAGE_BASE US_PTE_SELFMAP_BASE
/* A runtime value of MmPteBase: a 47-bit
 * kernel half address, which is why the image's literal cannot be used */
#define KERNEL_BASE 0xFFFFAE0000000000ULL

static void testSelfMapBases(void) {
    eqU64("the image's literal is the x64 base",
          US_PTE_SELFMAP_BASE, 0xFFFFF68000000000ULL);
    eqU64("the next level is that base's own page descriptor",
          US_PDE_SELFMAP_BASE, usPTESlotFor(IMAGE_BASE, IMAGE_BASE));
    {
        uint64_t literal[4];
        uint64_t runtime[4];

        levelBases(IMAGE_BASE, literal);
        eqU64("the image's own literal for the page directory level",
              literal[2], US_PDE_SELFMAP_BASE);
        eqU64("and for the level above it", literal[1], US_PPE_SELFMAP_BASE);
        eqU64("and for the top", literal[0], US_PXE_SELFMAP_BASE);
        /* The same arithmetic on the kernel's base has to give a different
         * answer, or the base would not matter and this would all be moot */
        levelBases(KERNEL_BASE, runtime);
        ok("the kernel's base gives different levels", runtime[0] != literal[0]);
        /* Both are canonical in a 47-bit kernel half; what tells them apart is
         * which of the top level's regions each one names, and only the
         * kernel's own variable names a region this kernel maps */
        ok("both bases are canonical in the kernel half",
           (IMAGE_BASE >> 47) == 0x1FFFF && (KERNEL_BASE >> 47) == 0x1FFFF);
        ok("and they name different top level entries",
           ((IMAGE_BASE >> 39) & 0x1FF) != ((KERNEL_BASE >> 39) & 0x1FF));
    }
    eqU64("and it is the value the x64 kernel uses for the same level",
          US_PDE_SELFMAP_BASE, 0xFFFFF6FB40000000ULL);
    eqU64("the level above follows from it",
          US_PPE_SELFMAP_BASE, usPTESlotFor(IMAGE_BASE, US_PDE_SELFMAP_BASE));
    eqU64("and is the x64 value too",
          US_PPE_SELFMAP_BASE, 0xFFFFF6FB7DA00000ULL);
    eqU64("as does the top one",
          US_PXE_SELFMAP_BASE, usPTESlotFor(IMAGE_BASE, US_PPE_SELFMAP_BASE));
    eqU64("which is the address the image carries a descriptor for",
          usPTEForAddress(US_PXE_SELFMAP_BASE), 0xFFFFF6FB7DBEDF68ULL);
    ok("the level bases are distinct", US_PDE_SELFMAP_BASE != US_PPE_SELFMAP_BASE);
}

/* The three plants a page needs, from a base the test computes the levels
 * for. Both the image's literal and the kernel's own base are used, because
 * the arithmetic has to hold for whichever one the machine is running with */
static void plantLevels(uint64_t base, uint64_t va, uint64_t leafValue,
                        unsigned leafLevel) {
    const unsigned shifts[4] = { 39, 30, 21, 12 };
    uint64_t bases[4];

    levelBases(base, bases);
    /* Every level above the leaf points at the next table down; the leaf
     * level holds what the test wants the walk to stop on. Nothing below the
     * leaf is planted, because the walk must not read it */
    for (unsigned i = 0; i <= leafLevel && i < 4; i++) {
        uint64_t value = (i == leafLevel) ? leafValue
                                          : ((0x10002000ULL + i * 0x1000) | 3);

        plant(slot(bases[i], va, shifts[i]), value);
    }
}

static void testLeafFindsAPage(void) {
    const uint64_t va = KVA_BASE + 0x12345678ULL;
    const uint64_t pa = 0x13BC00000ULL;
    const uint64_t bases[2] = { IMAGE_BASE, KERNEL_BASE };
    UsLeaf leaf;

    for (int k = 0; k < 2; k++) {
        uint64_t levels[4];

        resetPlant();
        plantLevels(bases[k], va, (pa & US_PAGE_ADDR_MASK) | 3, 3);
        levelBases(bases[k], levels);
        leaf = usLeafFind(bases[k], leafRead, NULL, va);
        ok("a page is found", leaf.found);
        ok("at the bottom level", leaf.level == 3);
        eqU64("and it is the page descriptor that maps it", leaf.descriptor,
              (pa & US_PAGE_ADDR_MASK) | 3);
        eqU64("at the address the arithmetic names", leaf.descriptorVA,
              slot(levels[3], va, 12));
        eqU64("covering a page", leaf.size, US_GRANULE_4K);
        eqU64("and translating the whole address", leaf.pa, pa + (va & 0xFFF));
        eqU64("with four levels read", gAskedCount, 4);
    }
}

/*
 * The case the image is in: a 2MB block, so the page level holds nothing.
 * What makes this worth its own check is that reading one level too far would
 * not fault and would not come back empty either: it would read the block's
 * own memory, and eight bytes of that can look like a descriptor
 */
static void testLeafStopsAtABlock(void) {
    const uint64_t va = KVA_BASE + 0x40012345ULL;
    const uint64_t pa = 0x138000000ULL;
    UsLeaf leaf;

    resetPlant();
    plantLevels(KERNEL_BASE, va, (pa & 0x0000FFFFFFE00000ULL) | 1, 2);
    /* Something that would pass for a page descriptor, if it were read */
    {
        uint64_t levels[4];

        levelBases(KERNEL_BASE, levels);
        plant(slot(levels[3], va, 12), 0xDEADB000ULL | 3);
    }
    leaf = usLeafFind(KERNEL_BASE, leafRead, NULL, va);
    ok("a block is found", leaf.found);
    ok("at the level above the page", leaf.level == 2);
    eqU64("covering 2MB", leaf.size, US_GRANULE_2M);
    eqU64("with the address inside the block", leaf.pa,
          (pa & 0x0000FFFFFFE00000ULL) + (va & (US_GRANULE_2M - 1)));
    eqU64("and nothing was read below it", gAskedCount, 3);

    /* A 1GB block, with plausible entries underneath it as well */
    resetPlant();
    plantLevels(KERNEL_BASE, va, (pa & 0x0000FFFFC0000000ULL) | 1, 1);
    {
        uint64_t levels[4];

        levelBases(KERNEL_BASE, levels);
        plant(slot(levels[2], va, 21), 0x10004000ULL | 3);
        plant(slot(levels[3], va, 12), 0xDEADB000ULL | 3);
    }
    leaf = usLeafFind(KERNEL_BASE, leafRead, NULL, va);
    ok("a 1GB block is found", leaf.found && leaf.level == 1);
    eqU64("covering 1GB", leaf.size, US_GRANULE_1G);
    eqU64("with the address inside it", leaf.pa,
          (pa & 0x0000FFFFC0000000ULL) + (va & (US_GRANULE_1G - 1)));
    eqU64("and only two levels were read", gAskedCount, 2);
}

static void testLeafUnmappedAndReserved(void) {
    const uint64_t va = KVA_BASE + 0x2000;
    UsLeaf leaf;

    /* Nothing at all: the walk stops at the first level, without reading on */
    resetPlant();
    leaf = usLeafFind(KERNEL_BASE, leafRead, NULL, va);
    ok("an unmapped address has no leaf", !leaf.found);
    eqU64("and only the top level was read", gAskedCount, 1);

    /* A table that leads to an empty page level: the address is not mapped
     * even though three levels of it are */
    resetPlant();
    plantLevels(KERNEL_BASE, va, 0, 3);
    leaf = usLeafFind(KERNEL_BASE, leafRead, NULL, va);
    ok("an empty page table maps nothing", !leaf.found);
    eqU64("with all four levels read", gAskedCount, 4);

    /* A block where a block is not allowed: at the page level it is reserved,
     * and treating it as a mapping would be reading a descriptor that is not
     * one */
    resetPlant();
    plantLevels(KERNEL_BASE, va, 0x10000000ULL | 1, 3);
    leaf = usLeafFind(KERNEL_BASE, leafRead, NULL, va);
    ok("a reserved descriptor is not a mapping", !leaf.found);

    ok("no base means no leaf", !usLeafFind(0, leafRead, NULL, va).found);
    ok("no reader means no leaf", !usLeafFind(KERNEL_BASE, NULL, NULL, va).found);
}

/* --- the shape of an entry, and where a mapping may go ------------------ */

/*
 * The same bases the walk computes, for a test that needs one of them without
 * having the array the implementation fills in
 */
static const uint64_t *levelBasesOf(uint64_t base) {
    static uint64_t out[4];

    out[3] = base;
    for (int i = 3; i > 0; i--) {
        out[i - 1] = base + ((out[i] & US_PTE_VA_MASK) >> 9);
    }
    return out;
}

/*
 * A reader that was refused, which is what a probe reports when the mapping
 * will not answer: the reserved shape, so that a walk treats the entry as one
 * it must not touch rather than as one that is free
 */
static uint64_t unreadableRead(void *ctx, uint64_t at) {
    (void)ctx;
    (void)at;
    return 2U;
}

/*
 * The tables above a level, with that level left empty
 *
 * This is the shape an address the kernel has not mapped for has: the levels
 * that lead to it are the ones its own mappings put there, and what is missing
 * is the entry that would name this page. Everything below the empty one is
 * left out, because the walk must not read it
 */
static void plantTablesAbove(uint64_t base, uint64_t va, unsigned leafLevel) {
    const unsigned shifts[4] = { 39, 30, 21, 12 };

    for (unsigned i = 0; i < leafLevel && i < 4; i++) {
        plant(slot(levelBasesOf(base)[i], va, shifts[i]), (0x10002000ULL) | 3);
    }
}

/*
 * The two bits that say what a descriptor is do not say the same thing at
 * every level, and that is the whole of what this is here to pin down: a
 * table above the last level is a place to keep walking, the same bits at the
 * last level are the mapping itself, and a block is only a shape the middle
 * two may hold
 */
static void testKindOfDescriptor(void) {
    /* Nothing there at all */
    for (unsigned level = 0; level < 4; level++) {
        ok("an empty entry is empty at every level",
           usDescriptorKind(0, level) == UsDescriptorEmpty);
    }

    /* Bits [1:0] == 11: a table down to the last, the page at the last */
    for (unsigned level = 0; level < 3; level++) {
        ok("11 above the last level is a table",
           usDescriptorKind((0x1000ULL << 4) | 3, level) == UsDescriptorTable);
    }
    ok("11 at the last level is the page",
       usDescriptorKind((0x1000ULL << 4) | 3, 3) == UsDescriptorLeaf);

    /* Bits [1:0] == 01: a block, which the middle two hold */
    ok("a block at the first level is reserved",
       usDescriptorKind(0x1000ULL | 1, 0) == UsDescriptorOther);
    ok("a block at the second level is a leaf",
       usDescriptorKind(0x200000ULL | 1, 1) == UsDescriptorLeaf);
    ok("a block at the third level is a leaf",
       usDescriptorKind(0x200000ULL | 1, 2) == UsDescriptorLeaf);
    ok("a block at the last level is reserved",
       usDescriptorKind(0x1000ULL | 1, 3) == UsDescriptorOther);

    /* Bits [1:0] == 10 is a shape that does not exist at any level */
    ok("an invalid descriptor is reserved",
       usDescriptorKind(0x1000ULL | 2, 3) == UsDescriptorOther);
}

/*
 * Where a mapping may be put, which is the question the frame buffer's alias
 * is built on: the kernel may not have the address, and everything above it
 * has to be a table already, since making one is not something this can do
 */
static void testFreeSlot(void) {
    const uint64_t va = KVA_BASE + 0x40000000ULL;
    UsFreeSlot free1;

    /*
     * Nothing under the top level: there is no table below it to write into,
     * and making one is not something this does. Refusing is the answer, not
     * a mapping at a level that does not exist
     */
    resetPlant();
    free1 = usFreeSlotFind(KERNEL_BASE, leafRead, NULL, va);
    ok("an address with no tables above it has nowhere to go", !free1.found);

    /*
     * The realistic case, and the one the frame buffer's alias is in: the
     * tables above are the kernel's own linear mapping and exist, and what is
     * empty is the entry that would name this page
     */
    resetPlant();
    plantTablesAbove(KERNEL_BASE, va, 3);
    free1 = usFreeSlotFind(KERNEL_BASE, leafRead, NULL, va);
    ok("an unmapped address under a table has a free entry", free1.found);
    eqU64("and it is at the page level", free1.level, 3);

    /* Every level present down to a page: nothing is free and nothing may be
     * written over */
    resetPlant();
    plantLevels(KERNEL_BASE, va, (0x13BC00000ULL & US_PAGE_ADDR_MASK) | 3, 3);
    ok("a mapped address has no free entry",
       !usFreeSlotFind(KERNEL_BASE, leafRead, NULL, va).found);

    /* The top level points at a table, and that table has an entry for the
     * second level, but nothing fills in the first: the walk stops at the
     * first empty entry it meets, which is the one it would write */
    resetPlant();
    plant(slot(levelBasesOf(KERNEL_BASE)[0], va, 39), (0x10002000ULL) | 3);
    plant(slot(levelBasesOf(KERNEL_BASE)[2], va, 21), (0x10002000ULL) | 3);
    free1 = usFreeSlotFind(KERNEL_BASE, leafRead, NULL, va);
    ok("an entry below a missing table is free", free1.found);
    eqU64("at the first level with nothing in it", free1.level, 1);

    /* A block covers the address, so there is nothing to fill in and nothing
     * to descend through */
    resetPlant();
    plant(slot(levelBasesOf(KERNEL_BASE)[1], va, 30), 0x40000000ULL | 1);
    ok("a block leaves nothing free",
       !usFreeSlotFind(KERNEL_BASE, leafRead, NULL, va).found);

    /* A read that was refused is not an empty entry: an entry that could not
     * be read is one to leave alone, and writing over it would be writing
     * over whatever is there */
    resetPlant();
    {
        UsFreeSlot refused;
        /* The reader answers the reserved shape for everything, which is what
         * a probe that was refused reports */
        refused = usFreeSlotFind(KERNEL_BASE, unreadableRead, NULL, va);
        ok("an unreadable entry is not free", !refused.found);
    }

    ok("no base means no free entry",
       !usFreeSlotFind(0, leafRead, NULL, va).found);
    ok("no reader means no free entry",
       !usFreeSlotFind(KERNEL_BASE, NULL, NULL, va).found);
}

/* A block built with an attribute index, checked against what it is made of */
static void testBlockDescriptor(void) {
    /* The address field of a 2MB block at the second level, written the long
     * way: this is what the constructor has to clear */
    const uint64_t block2M = 0x0000FFFFFFE00000ULL;
    const uint64_t pa = 0x138400000ULL;
    uint64_t descriptor = usBlockDescriptor(pa, US_GRANULE_2M, 3);

    eqU64("the address is rounded down to the block",
          descriptor & block2M, pa & block2M);
    eqU64("the attribute index is where the architecture puts it",
          (descriptor >> US_ATTR_INDEX_SHIFT) & 7U, 3);
    ok("it is a block", (descriptor & 3U) == US_DESC_BLOCK);
    ok("the access flag is set, since a fault here is the end of everything",
       (descriptor & US_PTE_AF) != 0);
    ok("it is not executable at EL1", (descriptor & US_PTE_PXN) != 0);
    ok("it is not executable at EL0", (descriptor & US_PTE_UXN) != 0);

    /* A 4KB block is the page level's shape: the same bits, a different size,
     * and the rounding is what tells them apart */
    descriptor = usBlockDescriptor(pa + 0x123, US_PAGE_SIZE, 0);
    eqU64("a page-sized descriptor is rounded to its own size",
          descriptor & US_PAGE_ADDR_MASK, pa & US_PAGE_ADDR_MASK);
}

/* --- the attribute a mapping is made with ------------------------------- */

static void testMAIRIndex(void) {
    /*
     * The assignment a running machine uses, written out as the
     * bytes it is made of: attribute 3 is the non-cacheable one, and the two
     * below it are the ordinary memory ones. What matters is that the answer
     * comes from where the byte is rather than from a constant
     */
    const uint64_t mair = 0x00000000440004FFULL;

    eqU64("the non-cacheable attribute is found where it is",
          (uint64_t)usMAIRNormalNonCacheableIndex(mair), 3);
    eqU64("an attribute at the bottom is found too",
          (uint64_t)usMAIRIndex(mair, 0xFF), 0);

    /* Zero is a legitimate index, so "not there" has to be negative rather
     * than zero: a caller that tested for zero would take the low byte */
    eqU64("an attribute that is not there is negative",
          (uint64_t)usMAIRNormalNonCacheableIndex(0x00000000000004FFULL) >> 63, 1);
    eqU64("and so is a byte that appears nowhere",
          (uint64_t)usMAIRIndex(mair, 0xAB) >> 63, 1);

    /* All eight entries are reachable */
    eqU64("the last entry is reachable",
          (uint64_t)usMAIRIndex(0x4400000000000000ULL, 0x44), 7);

    /* A machine whose low byte is the non-cacheable one is answered with that
     * index rather than the usual one: this is the whole point of looking it
     * up instead of writing three */
    eqU64("the index follows the machine, not a convention",
          (uint64_t)usMAIRNormalNonCacheableIndex(0x0000000000000044ULL), 0);
}

int main(void) {
    testSelfMapBases();
    testLeafFindsAPage();
    testLeafStopsAtABlock();
    testLeafUnmappedAndReserved();
    testKindOfDescriptor();
    testFreeSlot();
    testBlockDescriptor();
    testMAIRIndex();

    return usCheckSummary(checks, failures);
}
