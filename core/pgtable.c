/*
 * Walking a translation table, see pgtable.h.
 *
 * Four levels, 4KB granule. Each level's entries cover a fixed span of virtual
 * address, and a descriptor either continues to the next level or ends the
 * walk by mapping a block itself. Both endings are handled here, because a
 * runtime services region can perfectly well be mapped as one large block and
 * finding nothing in that case would be a silent wrong answer.
 */

#include "core/pgtable.h"

/* Descriptor bits that are the same at every level. */
#define US_DESC_VALID 1ULL
/* bits [1:0] == 0b11 is a table at L0-L2 and a page at L3. */
#define US_DESC_TABLE_OR_PAGE 3ULL
/* bits [1:0] == 0b01 is a block, which only L1 and L2 may use. */
#define US_DESC_BLOCK 1ULL

/* Virtual bits each level's index is taken from, and the span one entry
 * covers at that level. */
#define US_L0_SHIFT 39
#define US_L1_SHIFT 30
#define US_L2_SHIFT 21
#define US_L3_SHIFT 12

#define US_INDEX_MASK 0x1FFULL

/* A block descriptor at L1 covers 1GB, at L2 2MB, in 4KB granule. */
#define US_L1_BLOCK_MASK 0x0000FFFFFFC00000ULL
#define US_L2_BLOCK_MASK 0x0000FFFFFFE00000ULL

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
 * since the pool is contiguous inside whatever covers it. */
static bool consider(Walk *w, uint64_t va, uint64_t pa, uint64_t size) {
    uint64_t end = pa + size;

    if (end <= w->targetPa || pa >= w->targetEnd) {
        return false;
    }
    w->result.found = true;
    w->result.va = va;
    w->result.mappedPa = pa;
    w->result.size = size;
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
 * entry must be a table, since a block is not allowed there.
 */
static bool descendsToTable(uint64_t entry) {
    return (entry & US_DESC_VALID) != 0
           && ((entry & US_DESC_TABLE_OR_PAGE) == US_DESC_TABLE_OR_PAGE);
}

/* The two levels at which a block may end the walk. */
static bool isBlock(uint64_t entry) {
    return (entry & US_DESC_TABLE_OR_PAGE) == US_DESC_BLOCK;
}

/*
 * The address bits a four level walk actually indexes, and the prefix above
 * them.
 *
 * A translation base register describes a half of the address space, and the
 * bits above bit 47 are not part of any index: they are either all zero or
 * all one, and which it is decides which register the walk belongs to. So the
 * indices are taken from the low 48 bits, and the prefix is put back when a
 * virtual address is reported.
 */
#define US_VA_LOW_BITS 0x0000FFFFFFFFFFFFULL
#define US_VA_PREFIX_MASK (~US_VA_LOW_BITS)

UsPageWalk usPageWalkFind(UsPhysRead read, void *ctx, uint64_t tableBase,
                          uint64_t vaFirst, uint64_t vaLast,
                          uint64_t targetPa, uint64_t targetBytes) {
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

    if (read == NULL || targetBytes == 0 || vaLast < vaFirst) {
        return w.result;
    }
    /*
     * The range has to sit in one half of the address space, because a walk
     * follows one base register and cannot cross between them.
     */
    if ((vaFirst & US_VA_PREFIX_MASK) != (vaLast & US_VA_PREFIX_MASK)) {
        return w.result;
    }

    prefix = vaFirst & US_VA_PREFIX_MASK;
    lo = vaFirst & US_VA_LOW_BITS;
    hi = vaLast & US_VA_LOW_BITS;

    l0 = tableBase & US_PAGE_ADDR_MASK;
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
                if (consider(&w, va1, e1 & US_L1_BLOCK_MASK, US_GRANULE_1G)) {
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
                    if (consider(&w, va2, e2 & US_L2_BLOCK_MASK, US_GRANULE_2M)) {
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
                    if (consider(&w, va3, e3 & US_PAGE_ADDR_MASK, US_GRANULE_4K)) {
                        return w.result;
                    }
                }
            }
        }
    }

    return w.result;
}
