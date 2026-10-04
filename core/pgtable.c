/*
 * Walking a translation table, see pgtable.h
 *
 * Four levels, 4KB granule. Each level's entries cover a fixed span of virtual
 * address, and a descriptor either continues to the next level or ends the
 * walk by mapping a block itself. Both endings are handled here, because a
 * runtime services region can perfectly well be mapped as one large block and
 * finding nothing in that case would be a silent wrong answer
 */

#include "core/pgtable.h"

/* Descriptor bits that are the same at every level */
#define US_DESC_VALID 1ULL
/* bits [1:0] == 0b11 is a table at L0-L2 and a page at L3 */
#define US_DESC_TABLE_OR_PAGE 3ULL
/* bits [1:0] == 0b01 is a block, which only L1 and L2 may use */
#define US_DESC_BLOCK 1ULL

/* Virtual bits each level's index is taken from, and the span one entry
 * covers at that level. The same numbers the mapped levels are described
 * with, from the same rule */
#define US_L0_SHIFT US_LEVEL_SHIFT(0)
#define US_L1_SHIFT US_LEVEL_SHIFT(1)
#define US_L2_SHIFT US_LEVEL_SHIFT(2)
#define US_L3_SHIFT US_LEVEL_SHIFT(3)

#define US_INDEX_MASK 0x1FFULL

/* A block descriptor at L1 covers 1GB, at L2 2MB, in 4KB granule */
#define US_L1_BLOCK_MASK 0x0000FFFFC0000000ULL
#define US_L2_BLOCK_MASK 0x0000FFFFFFE00000ULL

#define US_DESC_PXN (1ULL << 53)
#define US_DESC_PXN_TABLE (1ULL << 59)

typedef struct Walk_t {
    UsPhysRead read;
    void      *ctx;
    uint64_t   targetPa;
    uint64_t   targetEnd;
    uint64_t   vaFirst;
    uint64_t   vaLast;
    uint64_t   budget;
    UsPageWalk result;
} Walk;

/* Records a mapping if it covers any of the target, and says whether the walk
 * should stop. A block is reported at its own base, with the offset into it
 * left to the caller: the payload only needs the block's address to reach it,
 * since the pool is contiguous inside whatever covers it */
static bool consider(Walk *w, uint64_t va, uint64_t pa, uint64_t size,
                     uint64_t descriptor, bool tablePxn) {
    uint64_t end = pa + size;

    if (end <= w->targetPa || pa >= w->targetEnd) {
        return false;
    }
    w->result.found = true;
    w->result.va = va;
    w->result.mappedPa = pa;
    w->result.size = size;
    w->result.descriptor = descriptor;
    w->result.pxn = tablePxn || (descriptor & US_DESC_PXN) != 0;
    return true;
}

static bool readEntry(Walk *w, uint64_t address, uint64_t *out) {
    if (w->budget == 0) {
        w->result.budgetExhausted = true;
        return false;
    }
    w->budget--;
    w->result.entriesRead++;
    *out = w->read(w->ctx, address);
    return true;
}

/*
 * Whether an entry at this level is worth descending into at all. A level 0
 * entry must be a table, since a block is not allowed there
 */
static bool descendsToTable(uint64_t entry) {
    return (entry & US_DESC_VALID) != 0
           && ((entry & US_DESC_TABLE_OR_PAGE) == US_DESC_TABLE_OR_PAGE);
}

/* The two levels at which a block may end the walk */
static bool isBlock(uint64_t entry) {
    return (entry & US_DESC_TABLE_OR_PAGE) == US_DESC_BLOCK;
}

/* The TTBR half supplies the all-zero/all-one prefix above vaBits. The
 * remaining bits index a shortened L0 root followed by three full tables */
UsPageWalk usPageWalkFindBits(UsPhysRead read, void *ctx, uint64_t tableBase,
                              uint64_t vaFirst, uint64_t vaLast,
                              uint64_t targetPa, uint64_t targetBytes,
                              unsigned vaBits) {
    Walk w = {
        .read = read,
        .ctx = ctx,
        .targetPa = targetPa,
        .targetEnd = targetPa + targetBytes,
        .vaFirst = vaFirst,
        .vaLast = vaLast,
        .budget = US_PAGE_WALK_BUDGET,
        .result = { 0 },
    };
    uint64_t l0;
    uint64_t l1 = 0;
    uint64_t l2 = 0;
    uint64_t prefix;
    uint64_t lo;
    uint64_t hi;
    uint64_t i0First;
    uint64_t i0Last;
    uint64_t lowMask;
    uint64_t prefixMask;
    uint64_t rootAlignment;

    if (read == NULL || targetBytes == 0 || vaLast < vaFirst
        || vaBits < 40 || vaBits > 48 || targetBytes > UINT64_MAX - targetPa) {
        return w.result;
    }
    lowMask = (1ULL << vaBits) - 1;
    prefixMask = ~lowMask;
    prefix = vaFirst & prefixMask;
    /* Reject noncanonical prefixes and ranges crossing TTBR halves. The
     * high half is not a signed extension of bit vaBits-1: that bit indexes
     * the root too, so only bits above it must all be one */
    if ((prefix != 0 && prefix != prefixMask)
        || (vaLast & prefixMask) != prefix) {
        return w.result;
    }

    lo = vaFirst & lowMask;
    hi = vaLast & lowMask;

    /* Strip ASID and low flags, but retain bit 11 for a 2KB (47-bit) root */
    rootAlignment = 1ULL << (3 + (vaBits - US_L0_SHIFT));
    l0 = tableBase & 0x0000FFFFFFFFFFFFULL & ~(rootAlignment - 1);
    w.result.tablesRead = 1;

    i0First = lo >> US_L0_SHIFT;
    i0Last = hi >> US_L0_SHIFT;

    for (uint64_t i0 = i0First; i0 <= i0Last; i0++) {
        uint64_t va0 = prefix | (i0 << US_L0_SHIFT);
        uint64_t hi0 = va0 | ((1ULL << US_L0_SHIFT) - 1);
        uint64_t e0;
        uint64_t l1First;
        uint64_t l1Last;

        if (va0 < vaFirst && hi0 < vaFirst) {
            continue;
        }
        if (va0 > vaLast) {
            break;
        }
        if (!readEntry(&w, l0 + i0 * 8, &e0)) {
            return w.result;
        }
        if (!descendsToTable(e0)) {
            continue;
        }
        l1 = e0 & US_PAGE_ADDR_MASK;
        w.result.tablesRead++;

        l1First = (va0 < vaFirst) ? ((vaFirst >> US_L1_SHIFT) & US_INDEX_MASK) : 0;
        l1Last = (hi0 > vaLast) ? ((vaLast >> US_L1_SHIFT) & US_INDEX_MASK) : US_INDEX_MASK;

        for (uint64_t i1 = l1First; i1 <= l1Last; i1++) {
            uint64_t va1 = va0 | (i1 << US_L1_SHIFT);
            uint64_t hi1 = va1 | ((1ULL << US_L1_SHIFT) - 1);
            uint64_t e1;
            uint64_t l2First;
            uint64_t l2Last;

            if (va1 < vaFirst && hi1 < vaFirst) {
                continue;
            }
            if (va1 > vaLast) {
                break;
            }
            if (!readEntry(&w, l1 + i1 * 8, &e1)) {
                return w.result;
            }
            if ((e1 & US_DESC_VALID) == 0) {
                continue;
            }
            if (isBlock(e1)) {
                if (consider(&w, va1, e1 & US_L1_BLOCK_MASK, US_GRANULE_1G,
                             e1, (e0 & US_DESC_PXN_TABLE) != 0)) {
                    return w.result;
                }
                continue;
            }
            if (!descendsToTable(e1)) {
                continue;
            }
            l2 = e1 & US_PAGE_ADDR_MASK;
            w.result.tablesRead++;

            l2First = (va1 < vaFirst) ? ((vaFirst >> US_L2_SHIFT) & US_INDEX_MASK) : 0;
            l2Last = (hi1 > vaLast) ? ((vaLast >> US_L2_SHIFT) & US_INDEX_MASK) : US_INDEX_MASK;

            for (uint64_t i2 = l2First; i2 <= l2Last; i2++) {
                uint64_t va2 = va1 | (i2 << US_L2_SHIFT);
                uint64_t hi2 = va2 | ((1ULL << US_L2_SHIFT) - 1);
                uint64_t e2;
                uint64_t l3First;
                uint64_t l3Last;
                uint64_t l3;

                if (va2 < vaFirst && hi2 < vaFirst) {
                    continue;
                }
                if (va2 > vaLast) {
                    break;
                }
                if (!readEntry(&w, l2 + i2 * 8, &e2)) {
                    return w.result;
                }
                if ((e2 & US_DESC_VALID) == 0) {
                    continue;
                }
                if (isBlock(e2)) {
                    if (consider(&w, va2, e2 & US_L2_BLOCK_MASK, US_GRANULE_2M,
                                 e2, ((e0 | e1) & US_DESC_PXN_TABLE) != 0)) {
                        return w.result;
                    }
                    continue;
                }
                if (!descendsToTable(e2)) {
                    continue;
                }
                l3 = e2 & US_PAGE_ADDR_MASK;
                w.result.tablesRead++;

                l3First = (va2 < vaFirst) ? ((vaFirst >> US_L3_SHIFT) & US_INDEX_MASK) : 0;
                l3Last = (hi2 > vaLast) ? ((vaLast >> US_L3_SHIFT) & US_INDEX_MASK) : US_INDEX_MASK;

                for (uint64_t i3 = l3First; i3 <= l3Last; i3++) {
                    uint64_t va3 = va2 | (i3 << US_L3_SHIFT);
                    uint64_t e3;

                    if (va3 < vaFirst) {
                        continue;
                    }
                    if (va3 > vaLast) {
                        break;
                    }
                    if (!readEntry(&w, l3 + i3 * 8, &e3)) {
                        return w.result;
                    }
                    if ((e3 & US_DESC_TABLE_OR_PAGE) != US_DESC_TABLE_OR_PAGE) {
                        continue;
                    }
                    if (consider(&w, va3, e3 & US_PAGE_ADDR_MASK, US_GRANULE_4K,
                                 e3, ((e0 | e1 | e2) & US_DESC_PXN_TABLE) != 0)) {
                        return w.result;
                    }
                }
            }
        }
    }

    return w.result;
}

UsPageWalk usPageWalkFind(UsPhysRead read, void *ctx, uint64_t tableBase,
                          uint64_t vaFirst, uint64_t vaLast,
                          uint64_t targetPa, uint64_t targetBytes) {
    return usPageWalkFindBits(read, ctx, tableBase, vaFirst, vaLast,
                             targetPa, targetBytes, 48);
}

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
        bases[i - 1] = usPteSlotFor(selfMapBase, bases[i]);
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
        out.descriptorVa = at;
        out.descriptor = descriptor;
        out.size = size;
        out.pa = (descriptor & level->paField) | (va & (size - 1));
        return out;
    }

    return out;
}
