/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The descriptors of an address, see pgtable.h
 *
 * Four levels, 4KB granule. The page that holds a table's descriptors is
 * itself mapped, by the same rule that maps any other address, which is what
 * makes finding a descriptor a computation rather than a search
 */

#include "core/pgtable.h"

/* bits [1:0] == 0b11 is a table at L0-L2 and a page at L3 */
#define US_DESC_TABLE_OR_PAGE 3ULL
/* bits [1:0] == 0b01 is a block, which only L1 and L2 may use */
#define US_DESC_BLOCK 1ULL

/* A block descriptor at L1 covers 1GB, at L2 2MB, in 4KB granule */
#define US_L1_BLOCK_MASK 0x0000FFFFC0000000ULL
#define US_L2_BLOCK_MASK 0x0000FFFFFFE00000ULL

/*
 * The mapped descriptors of an address, see pgtable.h
 *
 * The index fields are the ones the walk above uses, and their widths are what
 * makes the levels different sizes: nine bits at the top, nine more each time
 * down. The bottom one is where the 36-bit mask belongs
 */
const UsSelfMapLevel usSelfMapLevels[US_SELF_MAP_LEVELS] = {
    { 0 },
    { US_L1_BLOCK_MASK },
    { US_L2_BLOCK_MASK },
    { US_PAGE_ADDR_MASK },
};

UsLeaf usLeafFind(uint64_t selfMapBase, UsWordRead read, void *ctx, uint64_t va) {
    UsLeaf out = { 0 };
    uint64_t bases[US_SELF_MAP_LEVELS];

    if (read == NULL || selfMapBase == 0) {
        return out;
    }

    /* The bases, from the page level up. Each level's descriptors are mapped
     * by the level below, so the address of a level's descriptors is that
     * level's own page descriptor: the same computation as for an address,
     * with that level's base in place of the address, and the page level's
     * base as the base it is computed from. The table below lists the levels
     * the other way round, top first */
    bases[US_SELF_MAP_LEVELS - 1] = selfMapBase;
    for (unsigned i = US_SELF_MAP_LEVELS - 1; i > 0; i--) {
        bases[i - 1] = usPTESlotFor(selfMapBase, bases[i]);
    }

    for (unsigned i = 0; i < US_SELF_MAP_LEVELS; i++) {
        const UsSelfMapLevel *level = &usSelfMapLevels[i];
        unsigned shift = US_LEVEL_SHIFT(i);
        uint64_t size = 1ULL << shift;
        uint64_t index = (va >> shift) & ((1ULL << US_LEVEL_BITS(i)) - 1);
        uint64_t at = bases[i] + index * 8;
        uint64_t descriptor = read(ctx, at);
        bool last = (i + 1 == US_SELF_MAP_LEVELS);

        if ((descriptor & US_DESC_TABLE_OR_PAGE) == US_DESC_TABLE_OR_PAGE && !last) {
            continue;                       /* a table: the answer is below it */
        }
        if ((descriptor & US_DESC_TABLE_OR_PAGE) == US_DESC_TABLE_OR_PAGE) {
            /* The page itself */
        } else if (!last && level->paField != 0
                   && (descriptor & US_DESC_TABLE_OR_PAGE) == US_DESC_BLOCK) {
            /* A block covering the address, which ends the walk here */
        } else {
            return out;
        }

        out.found = true;
        out.level = i;
        out.descriptorVA = at;
        out.descriptor = descriptor;
        out.size = size;
        out.pa = (descriptor & level->paField) | (va & (size - 1));
        return out;
    }

    return out;
}
