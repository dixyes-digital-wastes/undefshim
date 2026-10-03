/*
 * Checks for the translation table walk.
 *
 * What is being checked is that a walk over a table finds the mapping that is
 * actually there, and reports the right advice about the ones that are not.
 * The tables are built here in memory, so the test says exactly which shape
 * of table produces which answer, and no kernel is involved.
 *
 * The cases that matter are the ones a real table has: a mapping split across
 * levels, a large block standing in for many pages, an entry that is present
 * but not the target, and a table that points somewhere unrelated.
 */

#include <stdio.h>
#include <string.h>

#include "core/pgtable.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-42s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

/* --- a table, laid out in plain memory ---------------------------------- */

/*
 * The walk reads physical addresses, so a table is an array indexed by
 * address. The base is chosen page aligned and the arrays are placed at
 * offsets that look like real allocations, which is what makes an address
 * arithmetic mistake show up here rather than on the target.
 */
#define TABLE_POOL_BYTES (16 * 4096)
#define TABLE_BASE 0x10000000ULL

static uint8_t gTables[TABLE_POOL_BYTES];

/* The kernel's virtual half, at the layout the walk assumes. */
#define KVA_BASE 0xFFFF000000000000ULL

static uint64_t physRead(void *ctx, uint64_t address) {
    uint64_t value;

    if (address < TABLE_BASE || address > TABLE_BASE + TABLE_POOL_BYTES - 8) {
        if (ctx != NULL) {
            *(bool *)ctx = true;
        }
        return 0;
    }
    memcpy(&value, gTables + (size_t)(address - TABLE_BASE), sizeof(value));
    return value;
}

static void writeEntry(uint64_t address, uint64_t value) {
    memcpy(gTables + (size_t)(address - TABLE_BASE), &value, sizeof(value));
}

/*
 * Builds a four level table that maps pages of a physical range at a virtual
 * address, one page per entry.
 */
#define L0_AT 0x10000000ULL
#define L1_AT 0x10001000ULL
#define L2_AT 0x10002000ULL
#define L3_AT 0x10003000ULL

static void buildTable(uint64_t va, uint64_t pa, size_t pages) {
    uint64_t i0 = (va >> 39) & 0x1FF;
    uint64_t i1 = (va >> 30) & 0x1FF;
    uint64_t i2 = (va >> 21) & 0x1FF;
    uint64_t i3 = (va >> 12) & 0x1FF;

    memset(gTables, 0, sizeof(gTables));

    /* The table descriptors, each pointing at the next level down. */
    writeEntry(L0_AT + i0 * 8, L1_AT | 3);
    writeEntry(L1_AT + i1 * 8, L2_AT | 3);
    writeEntry(L2_AT + i2 * 8, L3_AT | 3);

    for (size_t p = 0; p < pages; p++) {
        writeEntry(L3_AT + (i3 + p) * 8, (pa + p * US_GRANULE_4K) | 3);
    }
}

static UsPageWalk findIn(uint64_t va, uint64_t pa, size_t pages, uint64_t target,
                         uint64_t bytes) {
    buildTable(va, pa, pages);
    return usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + (1ULL << 40),
                          target, bytes);
}

/* --- the checks --------------------------------------------------------- */

static void testFindsAPage(void) {
    const uint64_t va = KVA_BASE + 0x12345000ULL;
    const uint64_t pa = 0x13bc00000ULL;
    UsPageWalk w;

    w = findIn(va, pa, 4, pa + 2 * US_GRANULE_4K, US_GRANULE_4K);
    ok("a mapped page is found", w.found);
    eqU64("and the address is the one it was mapped at",
          w.va, va + 2 * US_GRANULE_4K);
    eqU64("with the physical address it maps", w.mappedPa, pa + 2 * US_GRANULE_4K);
    eqU64("and a page is what covers it", w.size, US_GRANULE_4K);
    ok("without exhausting the budget", !w.budgetExhausted);

    /* An address that is not mapped anywhere is not found, and that is a
     * definite answer rather than a truncated one. */
    w = findIn(va, pa, 4, 0xDEADB0000ULL, US_GRANULE_4K);
    ok("an unmapped address is not found", !w.found);
    ok("and the walk ran to completion", !w.budgetExhausted);
    ok("having read some entries", w.entriesRead > 0);
}

/*
 * A region mapped as one large block has to be found too: the pool could be
 * covered by a 2MB block, and a walk that only understood page descriptors
 * would report nothing while the mapping sat right there.
 */
static void testFindsABlock(void) {
    const uint64_t va = KVA_BASE + 0x40000000ULL;
    const uint64_t pa = 0x138000000ULL;
    const uint64_t i0 = (va >> 39) & 0x1FF;
    const uint64_t i1 = (va >> 30) & 0x1FF;
    const uint64_t i2 = (va >> 21) & 0x1FF;
    UsPageWalk w;

    memset(gTables, 0, sizeof(gTables));
    writeEntry(L0_AT + i0 * 8, L1_AT | 3);
    writeEntry(L1_AT + i1 * 8, L2_AT | 3);
    /* A block descriptor: bits [1:0] are 0b01, and the address lives higher
     * up than it does in a page descriptor. */
    writeEntry(L2_AT + i2 * 8, (pa & 0x0000FFFFFFE00000ULL) | 1);

    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + (1ULL << 40),
                       pa + 0x1000, 33 * US_GRANULE_4K);
    ok("a block mapping is found", w.found);
    eqU64("the block's own address is reported", w.va,
          (va & ~(US_GRANULE_2M - 1)));
    eqU64("with the physical address it maps", w.mappedPa,
          pa & 0x0000FFFFFFE00000ULL);
    eqU64("and its size is the block's", w.size, US_GRANULE_2M);
}

/*
 * The range being searched has to be respected. A caller looking only at the
 * kernel's half must not be handed a mapping from the other one.
 */
static void testRespectsTheRange(void) {
    const uint64_t va = KVA_BASE + 0x8000000ULL;
    const uint64_t pa = 0x13bc00000ULL;
    UsPageWalk w;

    buildTable(va, pa, 4);

    /* Searching a range that does not contain the mapping finds nothing. */
    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE + (1ULL << 39),
                       KVA_BASE + (1ULL << 40), pa, US_GRANULE_4K);
    ok("a mapping outside the range is not found", !w.found);
    ok("but the walk completed", !w.budgetExhausted);

    /* Searching the range that does contain it finds it. */
    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + (1ULL << 39),
                       pa, US_GRANULE_4K);
    ok("a mapping inside the range is found", w.found);
    eqU64("at the address it was mapped at", w.va, va);
}

/*
 * A table that points at unmapped memory must not be followed forever. The
 * budget is what stops it, and a truncated answer has to say so rather than
 * look like "there is nothing there".
 */
static void testBudgetStopsARunaway(void) {
    /* Every level 3 entry points at another table, so the walk never reaches
     * a leaf and keeps going until the budget runs out. */
    memset(gTables, 0, sizeof(gTables));
    for (uint64_t i = 0; i <= 0x1FF; i++) {
        writeEntry(L0_AT + i * 8, L1_AT | 3);
        writeEntry(L3_AT + i * 8, L2_AT | 3);
    }
    for (uint64_t i = 0; i <= 0x1FF; i++) {
        writeEntry(L1_AT + i * 8, L2_AT | 3);
        writeEntry(L2_AT + i * 8, L3_AT | 3);
    }

    UsPageWalk w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE,
                                  KVA_BASE + (1ULL << 39), 0x13bc00000ULL,
                                  US_GRANULE_4K);
    ok("a table that never ends is cut off", w.budgetExhausted);
    ok("and the answer is not a negative one", !w.found);
    ok("with a bounded amount of reading", w.entriesRead <= US_PAGE_WALK_BUDGET);
}

static void testBadArguments(void) {
    UsPageWalk w;

    w = usPageWalkFind(NULL, NULL, L0_AT, KVA_BASE, KVA_BASE + 0x1000,
                       0x1000, 0x1000);
    ok("no reader means no walk", !w.found);

    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + 0x1000,
                       0x1000, 0);
    ok("a zero-sized target is nothing to find", !w.found);

    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE + 0x2000, KVA_BASE,
                       0x1000, 0x1000);
    ok("an inverted range is nothing to find", !w.found);
}

/*
 * The base arrives straight from a translation base register, so it has flags
 * in the low bits, and possibly an address space identifier above. Neither
 * may end up in the address that gets read.
 */
static void testBaseFlagsAreMasked(void) {
    const uint64_t va = KVA_BASE + 0x1000ULL;
    const uint64_t pa = 0x13bc00000ULL;
    UsPageWalk w;

    buildTable(va, pa, 1);

    /* The same base, with the low bits set the way a register would have
     * them, and an identifier in the top half. */
    w = usPageWalkFind(physRead, NULL, L0_AT | 0xABCD000000000FFFULL, KVA_BASE,
                       KVA_BASE + (1ULL << 39), pa, US_GRANULE_4K);
    ok("flags in the base are ignored", w.found);
    eqU64("and the mapping is still found", w.va, va);
}

/* A 47-bit high VA uses only eight L0 index bits. The root is the second
 * 2KB allocation in the same 4KB page as the low-half root. */
static void test47BitRoot(void) {
    const uint64_t prefix = 0xFFFF800000000000ULL;
    const uint64_t root = L0_AT + 0x800;
    const uint64_t pa = 0x13BC00000ULL;
    const uint64_t offsets[] = { 0x12345000ULL, (255ULL << 39) | 0x12345000ULL };

    for (size_t n = 0; n < sizeof(offsets) / sizeof(offsets[0]); n++) {
        uint64_t va = prefix | offsets[n];
        uint64_t i0 = (va >> 39) & 0xFF;
        uint64_t i1 = (va >> 30) & 0x1FF;
        uint64_t i2 = (va >> 21) & 0x1FF;
        uint64_t i3 = (va >> 12) & 0x1FF;
        bool readFailed = false;
        UsPageWalk w;

        memset(gTables, 0, sizeof(gTables));
        writeEntry(root + i0 * 8, L1_AT | 3);
        writeEntry(L1_AT + i1 * 8, L2_AT | 3);
        writeEntry(L2_AT + i2 * 8, L3_AT | 3);
        writeEntry(L3_AT + i3 * 8, pa | 3);
        w = usPageWalkFindBits(physRead, &readFailed,
                               root | 0xABCD000000000001ULL,
                               va, va + 0xFFF, pa, US_GRANULE_4K, 47);
        ok("47-bit high-half root at +0x800 is found", w.found);
        eqU64("47-bit VA has the correct high prefix", w.va, va);
        eqU64("47-bit root reports the physical page", w.mappedPa, pa);
        eqU64("47-bit root reports the raw leaf", w.descriptor, pa | 3);
        eqU64("47-bit root follows exactly four entries", w.entriesRead, 4);
        ok("47-bit root callback read succeeds", !readFailed);
        ok("47-bit executable leaf is not PXN", !w.pxn);

        /* With this particular shared-page layout the 48-bit high-half
         * index happens to add back the root's lost 0x800. The compatibility
         * entry point must still behave exactly like an explicit 48-bit walk. */
        w = usPageWalkFind(physRead, NULL, root, va, va + 0xFFF,
                           pa, US_GRANULE_4K);
        UsPageWalk explicit48 = usPageWalkFindBits(physRead, NULL, root,
                                                  va, va + 0xFFF,
                                                  pa, US_GRANULE_4K, 48);
        ok("compatibility API behaves as a 48-bit walk", w.found == explicit48.found);
        eqU64("compatibility API reports the same VA", w.va, explicit48.va);
    }
}

static void testAllVaWidths(void) {
    const uint64_t pa = 0x13BC00000ULL;

    for (unsigned bits = 40; bits <= 48; bits++) {
        uint64_t lowMask = (1ULL << bits) - 1;
        uint64_t rootBytes = 1ULL << (3 + bits - 39);
        uint64_t root = L0_AT + 4096 - rootBytes;
        uint64_t offset = lowMask & ~0xFFFULL;

        for (unsigned half = 0; half < 2; half++) {
            uint64_t va = (half ? ~lowMask : 0) | offset;
            uint64_t i0 = offset >> 39;
            UsPageWalk w;

            memset(gTables, 0, sizeof(gTables));
            writeEntry(root + i0 * 8, L1_AT | 3);
            writeEntry(L1_AT + 511 * 8, L2_AT | 3);
            writeEntry(L2_AT + 511 * 8, L3_AT | 3);
            writeEntry(L3_AT + 511 * 8, pa | 3);
            w = usPageWalkFindBits(physRead, NULL,
                                   root | 0x1234000000000000ULL | (rootBytes - 1),
                                   va, va + 0xFFF, pa, US_GRANULE_4K, bits);
            ok("40..48-bit root alignment and flags work", w.found);
            eqU64("40..48-bit last root index restores VA", w.va, va);
        }
    }
}

static void testDescriptorAndPxn(void) {
    const uint64_t va = KVA_BASE + 0x12345000ULL;
    const uint64_t pa = 0x13BC00000ULL;
    const uint64_t tableAt[] = {
        L0_AT + ((va >> 39) & 0x1FF) * 8,
        L1_AT + ((va >> 30) & 0x1FF) * 8,
        L2_AT + ((va >> 21) & 0x1FF) * 8,
    };
    const uint64_t nextTable[] = { L1_AT, L2_AT, L3_AT };
    const uint64_t leafAt = L3_AT + ((va >> 12) & 0x1FF) * 8;
    const uint64_t pxn = 1ULL << 53;
    const uint64_t pxnTable = 1ULL << 59;
    UsPageWalk w;

    for (unsigned level = 0; level < 3; level++) {
        buildTable(va, pa, 1);
        writeEntry(tableAt[level], nextTable[level] | 3 | pxnTable);
        w = usPageWalkFind(physRead, NULL, L0_AT, va, va + 0xFFF,
                           pa, US_GRANULE_4K);
        ok("a page inherits PXNTable from each ancestor", w.found && w.pxn);
        eqU64("inherited PXN does not change raw leaf", w.descriptor, pa | 3);
    }

    buildTable(va, pa, 1);
    writeEntry(leafAt, pa | 3 | pxn);
    w = usPageWalkFind(physRead, NULL, L0_AT, va, va + 0xFFF,
                       pa, US_GRANULE_4K);
    ok("leaf PXN is reported", w.found && w.pxn);
    eqU64("raw PXN leaf is preserved", w.descriptor, pa | 3 | pxn);

    /* UXN alone is not privileged execute-never. */
    writeEntry(leafAt, pa | 3 | (1ULL << 54));
    w = usPageWalkFind(physRead, NULL, L0_AT, va, va + 0xFFF,
                       pa, US_GRANULE_4K);
    ok("leaf UXN alone is not PXN", w.found && !w.pxn);

    /* A rejected earlier subtree must not poison an executable sibling. */
    buildTable(va + US_GRANULE_1G, pa, 1);
    writeEntry(L1_AT, (L0_AT + 0x4000) | 3 | pxnTable);
    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE,
                       KVA_BASE + (1ULL << 31), pa, US_GRANULE_4K);
    ok("PXNTable does not leak from sibling", w.found && !w.pxn);
}

static void testBlockDescriptorAndPxn(void) {
    const uint64_t va = KVA_BASE + 0x40000000ULL;
    const uint64_t pxn = 1ULL << 53;
    const uint64_t pxnTable = 1ULL << 59;

    for (unsigned level = 1; level <= 2; level++) {
        uint64_t size = level == 1 ? US_GRANULE_1G : US_GRANULE_2M;
        uint64_t pa = level == 1 ? 0x140000000ULL : 0x138000000ULL;
        uint64_t leafAt = level == 1 ? L1_AT + ((va >> 30) & 0x1FF) * 8
                                     : L2_AT + ((va >> 21) & 0x1FF) * 8;

        /* First an executable block, then leaf PXN, then PXNTable at each
         * possible ancestor. Address bits below block size are legal RES0
         * in the ordinary descriptor, not evidence of a runtime bug. */
        for (unsigned mode = 0; mode <= level + 1; mode++) {
            uint64_t descriptor = pa | 1 | (1ULL << 10);
            UsPageWalk w;

            memset(gTables, 0, sizeof(gTables));
            writeEntry(L0_AT + ((va >> 39) & 0x1FF) * 8,
                       L1_AT | 3 | (mode == 2 ? pxnTable : 0));
            if (level == 2) {
                writeEntry(L1_AT + ((va >> 30) & 0x1FF) * 8,
                           L2_AT | 3 | (mode == 3 ? pxnTable : 0));
            }
            if (mode == 1) {
                descriptor |= pxn;
            }
            writeEntry(leafAt, descriptor);
            w = usPageWalkFind(physRead, NULL, L0_AT, va, va + size - 1,
                               pa + 0x123450, US_GRANULE_4K);
            ok("legal L1/L2 block descriptor is found", w.found);
            eqU64("block base uses the correct address mask", w.mappedPa, pa);
            eqU64("block mapping size is preserved", w.size, size);
            eqU64("raw block descriptor is preserved", w.descriptor, descriptor);
            ok("block inherits PXN only from its own ancestors", w.pxn == (mode != 0));
        }
    }

    /* Exercise bits [29:22] explicitly: an L1 address mask clears all thirty
     * low bits. Ignore them when extracting an address, without treating
     * ordinary (zero) RES0 bits as a reason to reject a valid descriptor. */
    memset(gTables, 0, sizeof(gTables));
    writeEntry(L0_AT, L1_AT | 3);
    writeEntry(L1_AT + 8, 0x140000001ULL | 0x3FC00000ULL);
    UsPageWalk w = usPageWalkFind(physRead, NULL, L0_AT, va, va + US_GRANULE_1G - 1,
                                  0x140001000ULL, US_GRANULE_4K);
    ok("L1 mask clears all low thirty address bits", w.found);
    eqU64("L1 mask does not retain bits 29:22", w.mappedPa, 0x140000000ULL);
}

static void testBitsBadArguments(void) {
    const uint64_t prefix = 0xFFFF800000000000ULL;
    const uint64_t pa = 0x13BC00000ULL;
    const unsigned badBits[] = { 0, 39, 49, 64, (unsigned)-1 };
    UsPageWalk w;

    for (size_t i = 0; i < sizeof(badBits) / sizeof(badBits[0]); i++) {
        w = usPageWalkFindBits(physRead, NULL, L0_AT, prefix, prefix + 0xFFF,
                               pa, US_GRANULE_4K, badBits[i]);
        ok("bad vaBits is rejected", !w.found);
        eqU64("bad vaBits performs no reads", w.entriesRead, 0);
        eqU64("bad vaBits opens no root", w.tablesRead, 0);
    }

    w = usPageWalkFindBits(physRead, NULL, L0_AT, prefix, prefix + 0xFFF,
                           UINT64_MAX - 0xFFF, US_GRANULE_4K, 47);
    ok("target end overflow is rejected", !w.found);
    eqU64("target overflow performs no reads", w.entriesRead, 0);
    w = usPageWalkFind(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + 0xFFF,
                       UINT64_MAX, 1);
    eqU64("48-bit wrapper rejects target overflow", w.entriesRead, 0);

    w = usPageWalkFindBits(physRead, NULL, L0_AT, 0x0000800000000000ULL,
                           0x0000800000000FFFULL, pa, US_GRANULE_4K, 47);
    eqU64("noncanonical low VA performs no reads", w.entriesRead, 0);
    w = usPageWalkFindBits(physRead, NULL, L0_AT, KVA_BASE, KVA_BASE + 0xFFF,
                           pa, US_GRANULE_4K, 47);
    eqU64("noncanonical high VA performs no reads", w.entriesRead, 0);
    w = usPageWalkFindBits(physRead, NULL, L0_AT, 0, prefix,
                           pa, US_GRANULE_4K, 47);
    eqU64("range crossing TTBR halves performs no reads", w.entriesRead, 0);
    w = usPageWalkFindBits(physRead, NULL, L0_AT, 0, 0x0000800000000000ULL,
                           pa, US_GRANULE_4K, 47);
    eqU64("noncanonical range endpoint performs no reads", w.entriesRead, 0);

    /* The largest nonoverflowing endpoint is allowed. */
    w = usPageWalkFindBits(physRead, NULL, L0_AT, prefix, prefix + 0xFFF,
                           UINT64_MAX - 0x1000, 0x1000, 47);
    ok("nonoverflowing target ending at UINT64_MAX walks", w.entriesRead > 0);
}

static void testReadFailureContext(void) {
    const uint64_t va = KVA_BASE + 0x1000;
    const uint64_t pa = 0x13BC00000ULL;
    bool readFailed = false;
    UsPageWalk w;

    buildTable(va, pa, 1);
    writeEntry(L0_AT, 0x40000000ULL | 3);
    w = usPageWalkFindBits(physRead, &readFailed, L0_AT, va, va + 0xFFF,
                           pa, US_GRANULE_4K, 48);
    ok("failed physical read is recorded by callback", readFailed);
    ok("failed physical read returns no mapping", !w.found);
    ok("read failure is separate from the budget", !w.budgetExhausted);
}

int main(void) {
    testFindsAPage();
    testFindsABlock();
    testRespectsTheRange();
    testBudgetStopsARunaway();
    testBadArguments();
    testBaseFlagsAreMasked();
    test47BitRoot();
    testAllVaWidths();
    testDescriptorAndPxn();
    testBlockDescriptorAndPxn();
    testBitsBadArguments();
    testReadFailureContext();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
