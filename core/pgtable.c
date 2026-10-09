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

void usLevelBases(uint64_t selfMapBase, uint64_t bases[US_SELF_MAP_LEVELS]) {
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
}

UsDescriptorKind usDescriptorKind(uint64_t descriptor, unsigned level) {
    bool last = (level + 1U == US_SELF_MAP_LEVELS);

    if (descriptor == 0) {
        return UsDescriptorEmpty;
    }
    if ((descriptor & US_DESC_TABLE_OR_PAGE) == US_DESC_TABLE_OR_PAGE) {
        /* The same bits mean a table above the last level and the mapped page
         * at it, and which of the two is not a detail: only one of them can be
         * written into or walked past */
        return last ? UsDescriptorLeaf : UsDescriptorTable;
    }
    if ((descriptor & US_DESC_BLOCK) == US_DESC_BLOCK) {
        /* A block is a thing at the middle two levels: at the top it would
         * cover 512GB, which this does not handle, and at the page level it is
         * not a shape that exists */
        return (!last && level > 0) ? UsDescriptorLeaf : UsDescriptorOther;
    }
    return UsDescriptorOther;
}

UsFreeSlot usFreeSlotFind(uint64_t selfMapBase, UsWordRead read, void *ctx,
                          uint64_t va) {
    UsFreeSlot out = { 0 };
    uint64_t bases[US_SELF_MAP_LEVELS];

    if (read == NULL || selfMapBase == 0) {
        return out;
    }
    usLevelBases(selfMapBase, bases);

    for (unsigned i = 0; i < US_SELF_MAP_LEVELS; i++) {
        uint64_t at = usLevelEntry(bases[i], i, va);
        UsDescriptorKind kind = usDescriptorKind(read(ctx, at), i);

        if (kind == UsDescriptorEmpty) {
            /*
             * Free, and usable only where a leaf may go. An empty entry at the
             * top level is not one of those: filling it means either a 512GB
             * block, which is not a mapping this makes, or a table that does
             * not exist yet, which is not something this can create
             */
            if (i == 0) {
                return out;
            }
            out.found = true;
            out.level = i;
            out.descriptorVA = at;
            return out;
        }
        if (kind != UsDescriptorTable || i + 1U >= US_SELF_MAP_LEVELS) {
            /* Mapped, or a shape nothing may descend through */
            return out;
        }
    }
    return out;
}

UsLeaf usLeafFind(uint64_t selfMapBase, UsWordRead read, void *ctx, uint64_t va) {
    UsLeaf out = { 0 };
    uint64_t bases[US_SELF_MAP_LEVELS];

    if (read == NULL || selfMapBase == 0) {
        return out;
    }

    usLevelBases(selfMapBase, bases);

    for (unsigned i = 0; i < US_SELF_MAP_LEVELS; i++) {
        const UsSelfMapLevel *level = &usSelfMapLevels[i];
        unsigned shift = US_LEVEL_SHIFT(i);
        uint64_t size = 1ULL << shift;
        uint64_t at = usLevelEntry(bases[i], i, va);
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
