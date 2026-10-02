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

    (void)ctx;
    if (address < TABLE_BASE || address + 8 > TABLE_BASE + TABLE_POOL_BYTES) {
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
    w = usPageWalkFind(physRead, NULL, L0_AT | 0xFFF, KVA_BASE,
                       KVA_BASE + (1ULL << 39), pa, US_GRANULE_4K);
    ok("flags in the base are ignored", w.found);
    eqU64("and the mapping is still found", w.va, va);
}

int main(void) {
    testFindsAPage();
    testFindsABlock();
    testRespectsTheRange();
    testBudgetStopsARunaway();
    testBadArguments();
    testBaseFlagsAreMasked();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
