/* Real notification/publisher, with firmware, translation and cache mocks */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "core/cache.h"
#include "core/thunk.h"
#include "core/translate.h"
#include "payload/early.h"
#include "payload/payload.h"
#include "payload/vamap.h"

#define US_CHECK_NAME "test_early"
#include "check.h"

#define PAYLOAD_PA UINT64_C(0x12345000)
#define PAYLOAD_VA UINT64_C(0xFFFFF80030000000)
#define POOL_VA UINT64_C(0xFFFFF80040000000)
/* One vector table per fixture stub, big enough for the EL0 slot: the landing
 * is worked out by following the branch the slot holds, so the fixture has to
 * hold one */
#define TABLE_WORDS 0x180U

static UsPayloadConfig cfg;
static _Alignas(US_PAGE_SIZE) unsigned char poolStorage[US_POOL_BYTES];
static uint32_t stubs[4][US_SLOT_RUNTIME_WORDS];
static uint32_t original[4][US_SLOT_RUNTIME_WORDS];
static uint32_t tables[4][TABLE_WORDS];
/* Which slot each fixture stub is for, and so which slot of the table above
 * reaches it. The first three are the ones the landing cache is exercised
 * with, in the order its SPSRs name them */
static const UsStubSlot kinds[4] = {
    UsStubSlotEL1t,
    UsStubSlotEL1h,
    UsStubSlotEL0,
    UsStubSlotEL1h,
};
static unsigned flushes;
static unsigned converts;
static unsigned writeChecks[4];
static bool failPool;
static bool failPayload;
static int denyWrite;
static bool wrongPhysical;
static bool denyLast;

UsPayloadConfig *usPayloadConfig(void) {
    return &cfg;
}

static uint64_t tableVA(unsigned index) {
    return (uintptr_t)tables[index];
}

bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa) {
    if (!write) {
        for (unsigned i = 0; i < 4; i++) {
            if (va == tableVA(i)) {
                *pa = cfg.stubs[i].tablePA;
                return true;
            }
            /* The stub itself: where the chain ends, and how the landing is
             * told which stub it reached */
            if (va == (uintptr_t)stubs[i]) {
                *pa = cfg.stubs[i].addressPA;
                return true;
            }
        }
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        uint64_t start = (uintptr_t)stubs[i];
        if (va == start || va == start + US_SLOT_RUNTIME_BYTES - 1) {
            writeChecks[i]++;
            if ((int)i == denyWrite || (denyLast && va != start)) {
                return false;
            }
            *pa = cfg.stubs[i].addressPA + va - start + (wrongPhysical ? 0x1000 : 0);
            return true;
        }
    }
    /* Boot addresses are intentionally not accessible */
    assert(!"unexpected write probe");
    return false;
}

static uint64_t convertMock(uint64_t flags, void **address) {
    assert(flags == 0);
    converts++;
    if ((uintptr_t)*address == PAYLOAD_PA) {
        if (failPayload) {
            return UINT64_C(0x800000000000000E);
        }
        *address = (void *)(uintptr_t)PAYLOAD_VA;
    } else {
        assert((uintptr_t)*address == (uintptr_t)poolStorage);
        if (failPool) {
            return UINT64_C(0x800000000000000E);
        }
        *address = (void *)(uintptr_t)POOL_VA;
    }
    return 0;
}

void usCacheFlushRange(const void *address, size_t bytes) {
    unsigned index = 4;
    for (unsigned i = 0; i < 4; i++) {
        if (address == stubs[i] + US_SLOT_STUB_WORDS
            || address == stubs[i] + cfg.stubs[i].targetIndex) {
            index = i;
            break;
        }
    }
    assert(index < 4 && cfg.stubs[index].published == 2);
    unsigned targetIndex = cfg.stubs[index].targetIndex;
    assert(cfg.selfVA == PAYLOAD_VA && cfg.poolBase == POOL_VA);
    assert(cfg.stackTop[0] == POOL_VA + US_POOL_STACK_OFFSET(0) + US_STACK_SIZE);
    if (bytes == US_SLOT_TARGET_BYTES) {
        uint32_t expected[US_SLOT_TARGET_WORDS];
        assert(address == stubs[index] + US_SLOT_STUB_WORDS);
        assert(stubs[index][targetIndex] == original[index][targetIndex]);
        usEncodeSlotTarget(expected, PAYLOAD_VA + cfg.entryOffset);
        assert(memcmp(expected, stubs[index] + US_SLOT_STUB_WORDS,
                      sizeof(expected)) == 0);
    } else {
        uint32_t branch;
        assert(address == stubs[index] + targetIndex && bytes == 4);
        assert(usEncodeBranch(targetIndex * 4U, US_SLOT_STUB_WORDS * 4U, &branch));
        assert(stubs[index][targetIndex] == branch);
    }
    flushes++;
}

static void init(void) {
    memset(&cfg, 0, sizeof(cfg));
    memset(poolStorage, 0, sizeof(poolStorage));
    memset(writeChecks, 0, sizeof(writeChecks));
    cfg.selfPA = cfg.selfVA = PAYLOAD_PA;
    cfg.selfBytes = 0x4000;
    cfg.entryOffset = 0x1240;
    cfg.poolPA = cfg.poolBase = (uintptr_t)poolStorage;
    cfg.stubCount = 4;
    UsPool *pool = (UsPool *)(void *)poolStorage;
    pool->selfPA = pool->selfVA = cfg.poolPA;
    for (unsigned i = 0; i < US_MAX_CPUS; i++) {
        cfg.stackTop[i] = cfg.poolPA + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE;
        pool->stackTop[i] = cfg.stackTop[i];
    }
    for (unsigned i = 0; i < 4; i++) {
        usEncodeSlotStub(stubs[i], PAYLOAD_PA + cfg.entryOffset, US_NOP,
                         US_BRANCH_OPCODE, kinds[i] != UsStubSlotEL1t);
        usEncodeSlotTarget(stubs[i] + US_SLOT_STUB_WORDS, PAYLOAD_PA + cfg.entryOffset);
        cfg.stubs[i] = (UsPayloadStub){
            /*
             * The publisher works a stub's address out from its image's, so
             * the image's base is the fixture's own and the two addresses are
             * the host's: that is what makes the store the mock accepts the
             * store this test is about. Two images, so that a publish for one
             * of them leaves the other
             */
            .address = (uintptr_t)stubs[i],
            .imageAddress = i < 2 ? UINT64_C(0x1000) : UINT64_C(0x2000),
            .tableAddress = (uintptr_t)tables[i],
            .tablePA = 0x50000800 + i * 0x800,
            .addressPA = 0x60001000 + i * US_SLOT_RUNTIME_BYTES,
            .targetIndex = usSlotStubTargetIndex(kinds[i]),
        };
    }
    memcpy(original, stubs, sizeof(original));
    /*
     * The chain the landing is worked out through. The table the exception is
     * taken through is not the one the boot armed, in general: the kernel
     * installs a table per processor whose synchronous slots branch into the
     * armed one, so the fixture gets both steps - a slot branch to the armed
     * table, and that table's slot branch to the stub
     */
    memset(tables, 0, sizeof(tables));
    for (unsigned i = 0; i < 4; i++) {
        uint32_t branch;
        uint32_t offset = (uint32_t)usStubSlotOffset(kinds[i]);

        assert(usEncodeBranch((uint32_t)(tableVA(i) + offset),
                              (uint32_t)(uintptr_t)stubs[i], &branch));
        tables[i][offset / 4U] = branch;
    }
    usVAMapRecord = (UsVAMapRecord){
        .poolBefore = cfg.poolPA,
        .payloadBefore = PAYLOAD_PA,
        .convertPointer = (uintptr_t)convertMock,
    };
    converts = flushes = 0;
    failPool = failPayload = wrongPhysical = denyLast = false;
    denyWrite = -1;
}

static void unchanged(const UsPayloadConfig *before) {
    assert(memcmp(&cfg, before, sizeof(cfg)) == 0);
    assert(memcmp(stubs, original, sizeof(stubs)) == 0);
    assert(flushes == 0);
}

/*
 * The landing for a vector table and slot, worked out once, has to survive the
 * call that could not work it out again: a processor that stops because it
 * cannot name a destination is one the kernel waits for forever. The cache is
 * keyed by the table, so a different table is not served by another one's
 * answer
 */
static void testLandingCache(void) {
    uint64_t tail = 0;

    /* Nothing is known before anything has been worked out */
    assert(!usPayloadSlotTailCached(tableVA(0), 4U, &tail));
    assert(!usPayloadSlotTailCached(tableVA(0), 5U, &tail));
    assert(!usPayloadSlotTailCached(tableVA(0), 0U, &tail));

    denyWrite = -1;                    /* let the lookups succeed */
    const uint64_t spsr[3] = { 4U, 5U, 0U };
    bool known[3] = { false, false, false };
    uint64_t worked[3] = { 0, 0, 0 };

    /* Whatever this configuration can work out, it has to remember */
    for (unsigned i = 0; i < 3; i++) {
        if (usPayloadSlotTail(tableVA(i), spsr[i], &worked[i])) {
            known[i] = true;
            uint64_t cached = 0;

            assert(usPayloadSlotTailCached(tableVA(i), spsr[i], &cached));
            assert(cached == worked[i]);
            /* Another slot's answer is not this slot's */
            assert(!usPayloadSlotTailCached(tableVA(i), spsr[(i + 1) % 3], &cached));
        }
    }
    /* Every one of them, since each has a stub at the end of its chain: a
     * landing that only works for one of the three slots is a landing the
     * other two exceptions never reach */
    assert(known[0] && known[1] && known[2]);
    /* Nor is another table's */
    uint64_t cached = 0;
    assert(!usPayloadSlotTailCached(0xdead000, spsr[0], &cached));
    usCheckNote("landing cache: works out once, remembers per table and slot\n");
}

int main(void) {
    init();
    testLandingCache();
    assert(!usPayloadEarly());
    assert(!usPayloadPublish(tableVA(0)));
    usVAMapNotify(NULL, NULL);
    assert(converts == 2 && flushes == 0);
    assert(memcmp(stubs, original, sizeof(stubs)) == 0);
    assert(cfg.highVA == PAYLOAD_VA && cfg.highPoolVA == POOL_VA);
    assert(cfg.selfPA == PAYLOAD_PA && cfg.poolPA == (uintptr_t)poolStorage);
    UsPool *pool = (UsPool *)(void *)poolStorage;
    assert(pool->selfVA == POOL_VA && pool->selfPA == cfg.poolPA);
    for (unsigned i = 0; i < US_MAX_CPUS; i++) {
        assert(cfg.stackTop[i] == POOL_VA + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE);
        assert(pool->stackTop[i] == cfg.stackTop[i]);
    }
    assert(!usPayloadPublish(0xdead000));
    assert(flushes == 0);
    assert(usPayloadPublish(tableVA(1)));
    assert(flushes == 4 && cfg.stubs[0].published == 1 && cfg.stubs[1].published == 1);
    assert(cfg.stubs[2].published == 0 && cfg.stubs[3].published == 0);
    assert(usPayloadPublish(tableVA(2)));
    assert(flushes == 8);
    for (unsigned i = 0; i < 4; i++) {
        assert(cfg.stubs[i].published == 1 && writeChecks[i] == 2);
        for (unsigned word = 0; word < US_SLOT_STUB_WORDS; word++) {
            if (word != cfg.stubs[i].targetIndex) {
                assert(stubs[i][word] == original[i][word]);
            }
        }
    }
    UsPayloadConfig done = cfg;
    assert(usPayloadEarly());
    assert(usPayloadPublish(tableVA(0)));
    usVAMapNotify(NULL, NULL);
    assert(converts == 4 && flushes == 8);
    assert(memcmp(&cfg, &done, sizeof(cfg)) == 0);

    for (unsigned failure = 0; failure < 2; failure++) {
        init();
        failPool = failure == 0;
        failPayload = failure != 0;
        UsPayloadConfig before = cfg;
        usVAMapNotify(NULL, NULL);
        assert(converts == 2);
        unchanged(&before);
    }
    init();
    usVAMapRecord.convertPointer = 0;
    UsPayloadConfig before = cfg;
    usVAMapNotify(NULL, NULL);
    assert(converts == 0);
    unchanged(&before);

    init();
    cfg.stackTop[0] = cfg.poolPA - 8;
    before = cfg;
    usVAMapNotify(NULL, NULL);
    unchanged(&before);

    for (unsigned invalid = 0; invalid < 4; invalid++) {
        init();
        usVAMapNotify(NULL, NULL);
        if (invalid == 0) {
            cfg.stubs[1].targetIndex = 99;
        } else if (invalid == 1) {
            cfg.stubs[1].imageAddress = 0;
        } else if (invalid == 2) {
            cfg.stubs[1].tablePA = cfg.stubs[0].tablePA;
        } else {
            cfg.stubCount = US_PAYLOAD_MAX_STUBS + 1;
        }
        before = cfg;
        assert(!usPayloadPublish(tableVA(0)));
        unchanged(&before);
    }
    for (unsigned failure = 0; failure < 3; failure++) {
        init();
        usVAMapNotify(NULL, NULL);
        denyWrite = failure == 0 ? 0 : -1;
        wrongPhysical = failure == 1;
        denyLast = failure == 2;
        assert(!usPayloadPublish(tableVA(0)));
        assert(cfg.stubs[0].published == 0);
        assert(memcmp(stubs[0], original[0], sizeof(stubs[0])) == 0);
        denyWrite = -1;
        wrongPhysical = denyLast = false;
        assert(usPayloadPublish(tableVA(0)));
        assert(cfg.stubs[0].published == 1 && cfg.stubs[1].published == 1);
        assert(flushes == 4);
    }
    init();
    usVAMapNotify(NULL, NULL);
    cfg.stubs[0].published = 2;
    assert(!usPayloadPublish(tableVA(0)));
    assert(memcmp(stubs[0], original[0], sizeof(stubs[0])) == 0);
    cfg.stubs[0].published = 0;
    assert(usPayloadPublish(tableVA(0)) && flushes == 4);
    return usCheckPassed("conversion, deferred ASLR publication, table/image identity, write checks and retry passed");
}
