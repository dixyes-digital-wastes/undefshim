/* Consume UEFI aliases separately from publishing targets in kernel images */

#include "core/cache.h"
#include "core/thunk.h"
#include "core/translate.h"
#include "payload/early.h"
#include "payload/payload.h"
#include "payload/vamap.h"

bool usPayloadEarly(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t payloadVa = usVaMapRecord.payloadAfter;
    uint64_t poolVa = usVaMapRecord.poolAfter;

    if (usVaMapRecord.fired == 0 || usVaMapRecord.payloadStatus != 0
        || usVaMapRecord.poolStatus != 0 || payloadVa >> 48 != 0xFFFFU
        || poolVa >> 48 != 0xFFFFU || cfg->entryOffset >= cfg->selfBytes
        || payloadVa > UINT64_MAX - cfg->selfBytes
        || poolVa > UINT64_MAX - US_POOL_BYTES) {
        return false;
    }
    if (cfg->highVa != 0) {
        return cfg->highVa == payloadVa && cfg->highPoolVa == poolVa;
    }
    for (uint64_t i = 0; i < US_MAX_CPUS; i++) {
        if (cfg->stackTop[i] < cfg->poolPa
            || cfg->stackTop[i] - cfg->poolPa > US_POOL_BYTES) {
            return false;
        }
    }

    UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
    for (uint64_t i = 0; i < US_MAX_CPUS; i++) {
        cfg->stackTop[i] = poolVa + cfg->stackTop[i] - cfg->poolPa;
        pool->stackTop[i] = cfg->stackTop[i];
    }
    pool->selfVa = poolVa;
    cfg->poolBase = poolVa;
    cfg->selfVa = payloadVa;
    cfg->highPoolVa = poolVa;
    __atomic_store_n(&cfg->highVa, payloadVa, __ATOMIC_RELEASE);
    return true;
}

/* What the last successful lookup produced, per slot */
static uint64_t gLandingVbar[US_STUB_SLOT_COUNT];
static uint64_t gLanding[US_STUB_SLOT_COUNT];

bool usPayloadSlotTail(uint64_t vbar, uint64_t spsr, uint64_t *tail) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t tablePa;
    uint64_t imageVa;
    uint64_t stubVa;
    const UsPayloadStub *slot = NULL;
    UsStubSlot which = usSlotOfSpsr(spsr);

    /*
     * Which table VBAR names, asked of the translation - and if that cannot
     * answer, the answer it gave last time. The vector table does not move,
     * and refusing here stops a processor on the payload's own halt, which
     * the kernel sees as a CPU that stopped answering, so a stale-but-known
     * answer is the better one
     */
    static uint64_t lastTablePa;

    if (tail == NULL || cfg->stubCount == 0 || cfg->stubCount > US_PAYLOAD_MAX_STUBS) {
        return false;
    }
    if (usTranslateAddress(vbar, false, &tablePa)) {
        lastTablePa = tablePa;
    } else if (lastTablePa != 0) {
        tablePa = lastTablePa;
    } else {
        return false;
    }
    /*
     * The table is the one VBAR names, told apart by physical address rather
     * than by an assumed delta, and the slot is the one the interrupted SPSR
     * selects. Both tables carry both slots, so the pair is what identifies a
     * stub
     */
    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        const UsPayloadStub *stub = &cfg->stubs[i];

        if (stub->tablePa != tablePa || stub->targetIndex != usSlotStubTargetIndex(which)
            || stub->imageAddress == 0 || stub->tableAddress < stub->imageAddress
            || stub->address < stub->imageAddress) {
            continue;
        }
        if (slot != NULL && (slot->imageAddress != stub->imageAddress
                             || slot->tableAddress != stub->tableAddress)) {
            return false;
        }
        slot = stub;
    }
    if (slot == NULL || vbar < slot->tableAddress - slot->imageAddress) {
        return false;
    }
    imageVa = vbar - (slot->tableAddress - slot->imageAddress);
    stubVa = imageVa + (slot->address - slot->imageAddress);
    /*
     * Remembered per slot, keyed by the vector table it was worked out for.
     * The landing is a place in the image, and the image does not move: what
     * can fail is working out which table VBAR names, and failing there stops
     * the processor - which the kernel sees as a processor that has stopped
     * answering, and then waits for it forever. The same VBAR gives the same
     * answer, so an answer already worked out for it is as good as the one
     * this call could not produce
     */
    gLandingVbar[which] = vbar;
    gLanding[which] = stubVa + (uint64_t)usSlotStubTailIndex(which) * 4U;
    *tail = gLanding[which];
    return true;
}

/*
 * The landing worked out for this table and slot before, if there is one. Used
 * when the identity of the table cannot be worked out now: the exception still
 * has to go somewhere, and the place it went last time is that place
 */
bool usPayloadSlotTailCached(uint64_t vbar, uint64_t spsr, uint64_t *tail) {
    UsStubSlot which = usSlotOfSpsr(spsr);

    if (tail == NULL || which > UsStubSlotEl0 || gLandingVbar[which] != vbar
        || gLanding[which] == 0) {
        return false;
    }
    *tail = gLanding[which];
    return true;
}

/*
 * The last landing worked out for this slot, whatever table it was for
 *
 * This is the answer of last resort, and it is worse than the one above: the
 * handler it names may belong to another image's table. It is still better
 * than the alternative, which is a processor that stops and is waited for
 * forever - the kernel has no way to know an exception was never delivered,
 * and a machine that keeps running with the wrong handler fails where it can
 * be seen
 */
bool usPayloadSlotTailLast(uint64_t spsr, uint64_t *tail) {
    UsStubSlot which = usSlotOfSpsr(spsr);

    if (tail == NULL || which > UsStubSlotEl0 || gLanding[which] == 0) {
        return false;
    }
    *tail = gLanding[which];
    return true;
}

bool usPayloadPublish(uint64_t vbar) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t highVa = __atomic_load_n(&cfg->highVa, __ATOMIC_ACQUIRE);
    uint64_t tablePa;
    const UsPayloadStub *active = NULL;

    if (highVa == 0 || cfg->stubCount == 0
        || cfg->stubCount > US_PAYLOAD_MAX_STUBS
        || !usTranslateAddress(vbar, false, &tablePa)) {
        return false;
    }
    /* Physical identity selects the table, not an assumed EFI or ASLR delta */
    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        const UsPayloadStub *stub = &cfg->stubs[i];
        if (stub->imageAddress == 0 || stub->tableAddress < stub->imageAddress
            || stub->address < stub->imageAddress || (stub->address & 3U) != 0
            || (stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEl1t)
                && stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEl1h)
                && stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEl0))) {
            return false;
        }
        if (stub->tablePa == tablePa) {
            if (active != NULL && (active->imageAddress != stub->imageAddress
                || active->tableAddress != stub->tableAddress)) {
                return false;
            }
            active = stub;
        }
    }
    if (active == NULL || vbar < active->tableAddress - active->imageAddress) {
        return false;
    }
    uint64_t imageVa = vbar - (active->tableAddress - active->imageAddress);
    bool complete = true;

    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        UsPayloadStub *stub = &cfg->stubs[i];
        uint32_t expected = 0;
        uint32_t branch;
        uint64_t offset = stub->address - stub->imageAddress;
        uint64_t pa;
        uint64_t lastPa;

        if (stub->imageAddress != active->imageAddress) {
            continue;
        }
        if (__atomic_load_n(&stub->published, __ATOMIC_ACQUIRE) == 1) {
            continue;
        }
        if (offset > UINT64_MAX - US_SLOT_RUNTIME_BYTES
            || imageVa > UINT64_MAX - offset - US_SLOT_RUNTIME_BYTES) {
            complete = false;
            continue;
        }
        uint64_t address = imageVa + offset;
        /* This allocation is smaller than a page; endpoints cover all pages */
        if (!usTranslateAddress(address, true, &pa) || pa != stub->addressPa
            || !usTranslateAddress(address + US_SLOT_RUNTIME_BYTES - 1, true, &lastPa)
            || lastPa != pa + US_SLOT_RUNTIME_BYTES - 1) {
            complete = false;
            continue;
        }
        if (!__atomic_compare_exchange_n(&stub->published, &expected, 2, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            complete = false;
            continue;
        }
        usEncodeBranch(stub->targetIndex * 4U, US_SLOT_STUB_WORDS * 4U, &branch);
        uint32_t *words = (uint32_t *)(uintptr_t)address;
        usEncodeSlotTarget(words + US_SLOT_STUB_WORDS, highVa + cfg->entryOffset);
        usCacheFlushRange(words + US_SLOT_STUB_WORDS, US_SLOT_TARGET_BYTES);
        __atomic_store_n(words + stub->targetIndex, branch, __ATOMIC_RELEASE);
        usCacheFlushRange(words + stub->targetIndex, sizeof(branch));
        __atomic_store_n(&stub->published, 1, __ATOMIC_RELEASE);
    }
    return complete;
}
