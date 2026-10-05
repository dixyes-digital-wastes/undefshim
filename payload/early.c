/* Consume UEFI aliases separately from publishing targets in kernel images */

#include "core/cache.h"
#include "core/thunk.h"
#include "core/translate.h"
#include "payload/early.h"
#include "payload/payload.h"
#include "payload/vamap.h"

bool usPayloadEarly(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t payloadVA = usVAMapRecord.payloadAfter;
    uint64_t poolVA = usVAMapRecord.poolAfter;

    if (usVAMapRecord.fired == 0 || usVAMapRecord.payloadStatus != 0
        || usVAMapRecord.poolStatus != 0 || payloadVA >> 48 != 0xFFFFU
        || poolVA >> 48 != 0xFFFFU || cfg->entryOffset >= cfg->selfBytes
        || payloadVA > UINT64_MAX - cfg->selfBytes
        || poolVA > UINT64_MAX - US_POOL_BYTES) {
        return false;
    }
    if (cfg->highVA != 0) {
        return cfg->highVA == payloadVA && cfg->highPoolVA == poolVA;
    }
    for (uint64_t i = 0; i < US_MAX_CPUS; i++) {
        if (cfg->stackTop[i] < cfg->poolPA
            || cfg->stackTop[i] - cfg->poolPA > US_POOL_BYTES) {
            return false;
        }
    }

    UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
    for (uint64_t i = 0; i < US_MAX_CPUS; i++) {
        cfg->stackTop[i] = poolVA + cfg->stackTop[i] - cfg->poolPA;
        pool->stackTop[i] = cfg->stackTop[i];
    }
    pool->selfVA = poolVA;
    cfg->poolBase = poolVA;
    cfg->selfVA = payloadVA;
    cfg->highPoolVA = poolVA;
    __atomic_store_n(&cfg->highVA, payloadVA, __ATOMIC_RELEASE);
    return true;
}

/*
 * Whether stubs can be written at this address right now
 *
 * Asked of the translation rather than assumed: which addresses reach an image
 * is what changes with the switch, and a store aimed at the wrong one is a
 * fault inside the payload at the moment it has least to recover with. Both
 * ends of the stub are asked about, since the allocation is smaller than a page
 * and a page is mapped whole
 */
static bool mapsStub(const UsPayloadStub *stub, uint64_t address) {
    uint64_t pa;
    uint64_t last;

    return usTranslateAddress(address, true, &pa) && pa == stub->addressPA
           && usTranslateAddress(address + US_SLOT_RUNTIME_BYTES - 1U, true, &last)
           && last == stub->addressPA + US_SLOT_RUNTIME_BYTES - 1U;
}

/*
 * The published form of one stub, written where it is reachable: the
 * destination beside the stub, then the stub's own word turned into a branch
 * to it
 *
 * The branch goes last and it is one word, so a processor reading that word
 * finds the trap or the branch and nothing in between, and what the branch
 * reaches is in place before it is. False means another processor has the
 * stub in hand, or has already finished it
 */
static bool publishStub(UsPayloadStub *stub, uint64_t address, uint64_t target) {
    uint32_t expected = 0;
    uint32_t branch;
    uint32_t *words;

    if (!__atomic_compare_exchange_n(&stub->published, &expected, 2, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    usEncodeBranch(stub->targetIndex * 4U, US_SLOT_STUB_WORDS * 4U, &branch);
    words = (uint32_t *)(uintptr_t)address;
    usEncodeSlotTarget(words + US_SLOT_STUB_WORDS, target);
    usCacheFlushRange(words + US_SLOT_STUB_WORDS, US_SLOT_TARGET_BYTES);
    __atomic_store_n(words + stub->targetIndex, branch, __ATOMIC_RELEASE);
    usCacheFlushRange(words + stub->targetIndex, sizeof(branch));
    __atomic_store_n(&stub->published, 1, __ATOMIC_RELEASE);
    return true;
}

/*
 * Points every stub at the payload's runtime address, at the handover
 *
 * The runtime address is known here: the address change ran before the
 * handover and the kernel is about to start, so publishing now means no stub
 * ever holds an address the kernel will not have mapped, and the kernel's code
 * is never written while it runs
 *
 * Where a stub is reachable from is not one question for every image on this
 * side of the switch. The tables in force are the kernel's and they hold none
 * of the boot's images where the boot put them: the loader's own is still
 * where it was, the kernel's is where its image names it, which the loader
 * block says and the caller passes in. A stub that no candidate reaches is
 * left alone rather than guessed at - the publish that runs on an exception
 * names the address from the table it came through, and it comes back to that
 * stub
 */
void usPayloadPublishAll(uint64_t kernelBase) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t highVA = __atomic_load_n(&cfg->highVA, __ATOMIC_ACQUIRE);
    uint64_t target;

    if (highVA == 0) {
        return;
    }
    target = highVA + cfg->entryOffset;

    for (uint64_t i = 0; i < cfg->stubCount && i < US_PAYLOAD_MAX_STUBS; i++) {
        UsPayloadStub *stub = &cfg->stubs[i];
        uint64_t address;

        if (stub->addressPA == 0 || stub->address < stub->imageAddress
            || stub->targetIndex >= US_SLOT_STUB_WORDS
            || __atomic_load_n(&stub->published, __ATOMIC_ACQUIRE) == 1) {
            continue;
        }
        address = stub->address;
        if (!mapsStub(stub, address)) {
            if (kernelBase == 0) {
                continue;
            }
            address = kernelBase + (stub->address - stub->imageAddress);
            if (!mapsStub(stub, address)) {
                continue;
            }
        }
        /* Best effort: a stub this could not do is one the publish that runs
         * on an exception comes back to */
        (void)publishStub(stub, address, target);
    }
}

/* What the last successful lookup produced, per slot */
static uint64_t gLandingVBAR[US_STUB_SLOT_COUNT];
static uint64_t gLanding[US_STUB_SLOT_COUNT];

/*
 * How the stub an exception came through is found: the slot its SPSR names,
 * then the branches from there
 *
 * Branches rather than the table's address, because the table in force is not
 * always one the boot armed: the kernel installs a table per processor whose
 * synchronous slots branch into the armed one, so the chain has two steps and
 * the stub is at the end of it. Both are branches that are there to be read,
 * which is what makes the walk exact and what lets it survive the kernel
 * rebuilding the address space - neither step needs an address, and no delta
 * between the two is assumed
 *
 * The bound below is what keeps a table that leads somewhere else, or nowhere,
 * from being followed: past a couple of steps the answer is that this
 * exception has no stub of ours
 */
#define US_SLOT_HOP_LIMIT 4U

/*
 * Whether the payload's own stub of this kind stands at this address: the
 * stub's physical address is what the boot recorded, and it is the one thing
 * about the stub that does not move
 */
static bool isStubAt(const UsPayloadConfig *cfg, uint64_t at, UsStubSlot which) {
    uint64_t pa;

    if (!usTranslateAddress(at, false, &pa)) {
        return false;
    }
    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        if (cfg->stubs[i].addressPA == pa
            && cfg->stubs[i].targetIndex == usSlotStubTargetIndex(which)) {
            return true;
        }
    }
    return false;
}

bool usPayloadSlotTail(uint64_t vbar, uint64_t spsr, uint64_t *tail) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsStubSlot which = usSlotOfSPSR(spsr);
    uint64_t at;
    uint64_t stubVA = 0;

    if (tail == NULL || cfg->stubCount == 0 || cfg->stubCount > US_PAYLOAD_MAX_STUBS) {
        return false;
    }
    /*
     * The slot the interrupted SPSR selects, and then the branches in it. An
     * exception taken through a table the boot did not arm arrives at a stub
     * all the same, because that table's slot branches into the armed one's,
     * and the slot it is resumed at is the armed slot's - which is the same
     * handler the hardware would have run had nothing been armed
     */
    at = vbar + usStubSlotOffset(which);
    for (uint32_t hop = 0; hop < US_SLOT_HOP_LIMIT; hop++) {
        int64_t offset;

        if (!usDecodeBranch(*(const volatile uint32_t *)(uintptr_t)at, &offset)) {
            return false;
        }
        at += (uint64_t)offset;
        if (isStubAt(cfg, at, which)) {
            stubVA = at;
            break;
        }
    }
    if (stubVA == 0) {
        return false;
    }
    /*
     * Remembered per slot, keyed by the vector table it was worked out for.
     * The same VBAR gives the same answer, and refusing here stops the
     * processor on the payload's own halt, which the kernel sees as a
     * processor that stopped answering and waits for forever
     */
    gLandingVBAR[which] = vbar;
    gLanding[which] = stubVA + (uint64_t)usSlotStubTailIndex(which) * 4U;
    *tail = gLanding[which];
    return true;
}

/*
 * The landing worked out for this table and slot before, if there is one. Used
 * when the walk above cannot be made now: the exception still has to go
 * somewhere, and where it went for this VBAR last time is that place
 */
bool usPayloadSlotTailCached(uint64_t vbar, uint64_t spsr, uint64_t *tail) {
    UsStubSlot which = usSlotOfSPSR(spsr);

    if (tail == NULL || which > UsStubSlotEL0 || gLandingVBAR[which] != vbar
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
    UsStubSlot which = usSlotOfSPSR(spsr);

    if (tail == NULL || which > UsStubSlotEL0 || gLanding[which] == 0) {
        return false;
    }
    *tail = gLanding[which];
    return true;
}

bool usPayloadPublish(uint64_t vbar) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t highVA = __atomic_load_n(&cfg->highVA, __ATOMIC_ACQUIRE);
    uint64_t tablePA;
    const UsPayloadStub *active = NULL;

    if (highVA == 0 || cfg->stubCount == 0
        || cfg->stubCount > US_PAYLOAD_MAX_STUBS
        || !usTranslateAddress(vbar, false, &tablePA)) {
        return false;
    }
    /* Physical identity selects the table, not an assumed EFI or ASLR delta */
    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        const UsPayloadStub *stub = &cfg->stubs[i];
        if (stub->imageAddress == 0 || stub->tableAddress < stub->imageAddress
            || stub->address < stub->imageAddress || (stub->address & 3U) != 0
            || (stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEL1t)
                && stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEL1h)
                && stub->targetIndex != usSlotStubTargetIndex(UsStubSlotEL0))) {
            return false;
        }
        if (stub->tablePA == tablePA) {
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
    uint64_t imageVA = vbar - (active->tableAddress - active->imageAddress);
    bool complete = true;

    for (uint64_t i = 0; i < cfg->stubCount; i++) {
        UsPayloadStub *stub = &cfg->stubs[i];
        uint64_t offset = stub->address - stub->imageAddress;

        if (stub->imageAddress != active->imageAddress) {
            continue;
        }
        if (__atomic_load_n(&stub->published, __ATOMIC_ACQUIRE) == 1) {
            continue;
        }
        if (offset > UINT64_MAX - US_SLOT_RUNTIME_BYTES
            || imageVA > UINT64_MAX - offset - US_SLOT_RUNTIME_BYTES) {
            complete = false;
            continue;
        }
        uint64_t address = imageVA + offset;

        if (!mapsStub(stub, address)) {
            complete = false;
            continue;
        }
        if (!publishStub(stub, address, highVA + cfg->entryOffset)) {
            /* Another processor has it in hand or has finished it: either way
             * this call did not do it, and saying so is what leaves the next
             * one to try again */
            complete = false;
        }
    }
    return complete;
}
