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

#define PAYLOAD_PA UINT64_C(0x12345000)
#define PAYLOAD_VA UINT64_C(0xFFFFF80030000000)
#define POOL_VA UINT64_C(0xFFFFF80040000000)

static UsPayloadConfig cfg;
static _Alignas(US_PAGE_SIZE) unsigned char poolStorage[US_POOL_BYTES];
static uint32_t stubs[4][US_SLOT_RUNTIME_WORDS];
static uint32_t original[4][US_SLOT_RUNTIME_WORDS];
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

static uint64_t tableVa(unsigned index) {
    unsigned first = index < 2 ? 0 : 2;
    return (uintptr_t)stubs[first] - 0x1000 + 0x800 + (index % 2) * 0x800;
}

bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa) {
    if (!write) {
        for (unsigned i = 0; i < 4; i++) {
            if (va == tableVa(i)) {
                *pa = cfg.stubs[i].tablePa;
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
            *pa = cfg.stubs[i].addressPa + va - start + (wrongPhysical ? 0x1000 : 0);
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
    assert(cfg.selfVa == PAYLOAD_VA && cfg.poolBase == POOL_VA);
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
    cfg.selfPa = cfg.selfVa = PAYLOAD_PA;
    cfg.selfBytes = 0x4000;
    cfg.entryOffset = 0x1240;
    cfg.poolPa = cfg.poolBase = (uintptr_t)poolStorage;
    cfg.stubCount = 4;
    UsPool *pool = (UsPool *)(void *)poolStorage;
    pool->selfPa = pool->selfVa = cfg.poolPa;
    for (unsigned i = 0; i < US_MAX_CPUS; i++) {
        cfg.stackTop[i] = cfg.poolPa + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE;
        pool->stackTop[i] = cfg.stackTop[i];
    }
    for (unsigned i = 0; i < 4; i++) {
        uint64_t image = i < 2 ? UINT64_C(0x100000000) : UINT64_C(0x40000000);
        usEncodeSlotStub(stubs[i], PAYLOAD_PA + cfg.entryOffset, US_NOP,
                         US_BRANCH_OPCODE, (i & 1) != 0);
        usEncodeSlotTarget(stubs[i] + US_SLOT_STUB_WORDS, PAYLOAD_PA + cfg.entryOffset);
        cfg.stubs[i] = (UsPayloadStub){
            .address = image + 0x1000 + (i % 2) * US_SLOT_RUNTIME_BYTES,
            .imageAddress = image,
            .tableAddress = image + 0x800 + (i % 2) * 0x800,
            .tablePa = 0x50000800 + i * 0x800,
            .addressPa = 0x60001000 + i * US_SLOT_RUNTIME_BYTES,
            .targetIndex = usSlotStubTargetIndex(i == 0 ? UsStubSlotEl1t
                                                       : UsStubSlotEl1h),
        };
    }
    memcpy(original, stubs, sizeof(original));
    usVaMapRecord = (UsVaMapRecord){
        .poolBefore = cfg.poolPa,
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

int main(void) {
    init();
    assert(!usPayloadEarly());
    assert(!usPayloadPublish(tableVa(0)));
    usVaMapNotify(NULL, NULL);
    assert(converts == 2 && flushes == 0);
    assert(memcmp(stubs, original, sizeof(stubs)) == 0);
    assert(cfg.highVa == PAYLOAD_VA && cfg.highPoolVa == POOL_VA);
    assert(cfg.selfPa == PAYLOAD_PA && cfg.poolPa == (uintptr_t)poolStorage);
    UsPool *pool = (UsPool *)(void *)poolStorage;
    assert(pool->selfVa == POOL_VA && pool->selfPa == cfg.poolPa);
    for (unsigned i = 0; i < US_MAX_CPUS; i++) {
        assert(cfg.stackTop[i] == POOL_VA + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE);
        assert(pool->stackTop[i] == cfg.stackTop[i]);
    }
    assert(!usPayloadPublish(0xdead000));
    assert(flushes == 0);
    assert(usPayloadPublish(tableVa(1)));
    assert(flushes == 4 && cfg.stubs[0].published == 1 && cfg.stubs[1].published == 1);
    assert(cfg.stubs[2].published == 0 && cfg.stubs[3].published == 0);
    assert(usPayloadPublish(tableVa(2)));
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
    assert(usPayloadPublish(tableVa(0)));
    usVaMapNotify(NULL, NULL);
    assert(converts == 4 && flushes == 8);
    assert(memcmp(&cfg, &done, sizeof(cfg)) == 0);

    for (unsigned failure = 0; failure < 2; failure++) {
        init();
        failPool = failure == 0;
        failPayload = failure != 0;
        UsPayloadConfig before = cfg;
        usVaMapNotify(NULL, NULL);
        assert(converts == 2);
        unchanged(&before);
    }
    init();
    usVaMapRecord.convertPointer = 0;
    UsPayloadConfig before = cfg;
    usVaMapNotify(NULL, NULL);
    assert(converts == 0);
    unchanged(&before);

    init();
    cfg.stackTop[0] = cfg.poolPa - 8;
    before = cfg;
    usVaMapNotify(NULL, NULL);
    unchanged(&before);

    for (unsigned invalid = 0; invalid < 4; invalid++) {
        init();
        usVaMapNotify(NULL, NULL);
        if (invalid == 0) {
            cfg.stubs[1].targetIndex = 99;
        } else if (invalid == 1) {
            cfg.stubs[1].imageAddress = 0;
        } else if (invalid == 2) {
            cfg.stubs[1].tablePa = cfg.stubs[0].tablePa;
        } else {
            cfg.stubCount = US_PAYLOAD_MAX_STUBS + 1;
        }
        before = cfg;
        assert(!usPayloadPublish(tableVa(0)));
        unchanged(&before);
    }
    for (unsigned failure = 0; failure < 3; failure++) {
        init();
        usVaMapNotify(NULL, NULL);
        denyWrite = failure == 0 ? 0 : -1;
        wrongPhysical = failure == 1;
        denyLast = failure == 2;
        assert(!usPayloadPublish(tableVa(0)));
        assert(cfg.stubs[0].published == 0);
        assert(memcmp(stubs[0], original[0], sizeof(stubs[0])) == 0);
        denyWrite = -1;
        wrongPhysical = denyLast = false;
        assert(usPayloadPublish(tableVa(0)));
        assert(cfg.stubs[0].published == 1 && cfg.stubs[1].published == 1);
        assert(flushes == 4);
    }
    init();
    usVaMapNotify(NULL, NULL);
    cfg.stubs[0].published = 2;
    assert(!usPayloadPublish(tableVa(0)));
    assert(memcmp(stubs[0], original[0], sizeof(stubs[0])) == 0);
    cfg.stubs[0].published = 0;
    assert(usPayloadPublish(tableVa(0)) && flushes == 4);
    puts("early: conversion, deferred ASLR publication, table/image identity, write checks and retry passed");
    return 0;
}
