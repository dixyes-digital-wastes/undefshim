/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Drawing the exception path into the payload, see arm.h
 *
 * Both tables are taken over, and both of their synchronous slots:
 *
 *   - the loader installs a table before the kernel runs, and that is the one
 *     in force when the kernel takes its first undefined instruction
 *   - the kernel installs its own table once it is running, and images it
 *     loads later -- ci.dll among them -- fault into that one
 *
 * And both slots per table, because Windows switches between SP_EL0 and
 * SP_EL1: an exception taken with SP_EL0 selected arrives at offset 0 of the
 * table and one taken with SP_EL1 at offset 0x200. Only the second of those
 * was being written, so the exceptions this project exists for went past the
 * stub and into the kernel's own handler -- which is a fault loop, since that
 * handler has no idea what an RCpc load is
 *
 * A slot is replaced by a branch to a stub, whatever it held. The stub reads
 * the exception class and either leaves for the payload or runs the slot's
 * own two-instruction tail, which for the board's table means a branch to
 * where it branched and for the kernel's SP_EL0 slot means the instruction
 * that was written there before continuing past it
 *
 * The stubs live in a run of zero words inside the kernel image. They have to
 * be in the same image as the slots they are reached from, because the branch
 * that reaches them is relative and the image moves when the address space is
 * rebuilt
 *
 * Six slots: the three synchronous entries, and the three asynchronous ones
 * of the same group. The asynchronous three are taken over as well. They are
 * not the slots an RCpc load arrives at, but they are the three that reach the
 * kernel's own "an exception on the exception stack" handler, which is a
 * bugcheck whose cause nothing else records; diverting them is what makes that
 * answerable, and nothing else uses them
 */

#include <uefi.h>

#include "core/cache.h"
#include "core/scan.h"
#include "core/thunk.h"
#include "core/translate.h"
#include "payload/payload.h"
#include "payload_blob.h"
#include "uefi/arm_payloadcfg.h"
#include "uefi/arm_vectors.h"
#include "uefi/console.h"
#include "uefi/registry.h"

#define US_STUB_SLOTS 6U

typedef struct UsArmTarget_t {
    UsImage   *image;
    uint32_t   tableRVA;
    uint32_t   stubRVA;
    UsVectorSlot slot;
    /* RVA of the image's descriptor base, or zero when it has none */
    uint32_t   descriptorBaseRVA;
} UsArmTarget;

/*
 * Which stub shape a vector slot needs. The three synchronous entries differ in
 * what they may use to keep x18 and in whether a data abort has to reach the
 * payload; see core/thunk.h
 *
 * The asynchronous three have the exception stack in use, so they take the
 * EL1h shape: x18 is rebuilt from TPIDR_EL1 rather than saved, which is what
 * makes an entry with a stale SP_EL1 harmless
 */
static UsStubSlot stubSlotOf(UsVectorSlot slot) {
    switch (slot) {
    case UsVectorSlotEL1hSync:
    case UsVectorSlotEL1hIrq:
    case UsVectorSlotEL1hFiq:
    case UsVectorSlotEL1hSError:
        return UsStubSlotEL1h;
    case UsVectorSlotEL0Sync32:
        return UsStubSlotEL0;
    default:
        return UsStubSlotEL1t;
    }
}

static bool armSlot(UsSession *s, const UsArmTarget *target) {
    uint32_t stub[US_SLOT_RUNTIME_WORDS];
    UsPayloadConfig *cfg = (UsPayloadConfig *)(uintptr_t)s->payloadPlace.configVA;
    uint8_t *slotAt;
    uint8_t *stubAt;
    uint32_t original;
    uint32_t enter;
    uint32_t tail0;
    uint32_t tail1;
    /*
     * What the slot's stub has to do about x18 and about faults of our own
     *
     * The EL1t and EL1h vectors are kernel mode, where x18 is the per-CPU block
     * and is rebuilt from TPIDR_EL1 rather than saved - and there is no stack
     * word to be had on the EL1t vector, whose SP the interrupted code was not
     * using. EL0 is the one vector whose x18 is user state and therefore has to
     * be pushed somewhere; the kernel's own entry there uses SP_EL1 as the
     * thread's kernel stack, so the red zone below it is that somewhere
     */
    UsStubSlot stubSlot = stubSlotOf(target->slot);
    uint32_t entryOffset = US_PAYLOAD_ENTRY_OFFSET;

    slotAt = (uint8_t *)(uintptr_t)usImageRVAToPtr(target->image,
                                                   target->tableRVA
                                                       + (uint32_t)target->slot * 0x80U);
    stubAt = (uint8_t *)(uintptr_t)usImageRVAToPtr(target->image, target->stubRVA);
    if (slotAt == NULL || stubAt == NULL || cfg->stubCount >= US_PAYLOAD_MAX_STUBS) {
        return false;
    }
    uint64_t tableAddress = (uintptr_t)usImageRVAToPtr(target->image, target->tableRVA);
    uint64_t tablePA;
    uint64_t stubPA;
    uint64_t lastPA;
    /*
     * The physical addresses are recorded for the publication that runs
     * later, and the question asked here is the one the boot's own stores
     * answer: where does *this* exception level's regime put the address. The
     * EL1&0 regime has already been rebuilt for the kernel by the time this
     * runs and no longer holds the images where the loader put them, so asking
     * it here refuses a write that would in fact succeed
     */
    if (!usTranslateOwnAddress(tableAddress, false, &tablePA)
        || !usTranslateOwnAddress((uintptr_t)stubAt, true, &stubPA)
        || !usTranslateOwnAddress((uintptr_t)stubAt + sizeof(stub) - 1, true, &lastPA)
        || lastPA != stubPA + sizeof(stub) - 1) {
        usLogE("arm", "the stub's own address cannot be translated\n");
        return false;
    }
    original = (uint32_t)slotAt[0] | ((uint32_t)slotAt[1] << 8)
               | ((uint32_t)slotAt[2] << 16) | ((uint32_t)slotAt[3] << 24);


    /*
     * The tail: what happens when the exception is not one of ours -- and the
     * test for that is the instruction, not the class, so the branch slots are
     * not the only ones this is safe for
     *
     * A slot holding a branch keeps its meaning, adjusted for the distance the
     * stub is away from it. A slot holding a handler written out in place runs
     * its first instruction and continues past it. The stub now touches only
     * x18, and gives that back before the tail runs, so the tail is reached
     * with every register but NZCV flags as the exception left them
     */
    {
        uint32_t tailIndex = usSlotStubTailIndex(stubSlot);
        /* The branch the tail ends with is the last of its words, and the
         * encoder decides how many that is per slot */
        uint32_t from = target->stubRVA
                        + (tailIndex + usSlotStubTailWords(stubSlot) - 1U) * 4U;
        uint32_t slotRVA = target->tableRVA + (uint32_t)target->slot * 0x80U;

        if ((original & 0xFC000000U) == 0x14000000U) {
            int32_t displacement = (int32_t)(original << 6) >> 6;

            tail0 = US_NOP;
            if (displacement == 0) {
                /* A branch to itself is what an unused slot holds. Keeping
                 * that meaning rather than adjusting it: the place it would
                 * point at is this stub */
                tail1 = 0x14000000U;
            } else if (!usEncodeBranch(from, slotRVA + (uint32_t)(displacement * 4),
                                       &tail1)) {
                return false;
            }
        } else {
            tail0 = original;
            if (!usEncodeBranch(from, slotRVA + 4U, &tail1)) {
                return false;
            }
        }
    }

    if (!usEncodeBranch(target->tableRVA + (uint32_t)target->slot * 0x80U,
                        target->stubRVA, &enter)) {
        return false;
    }

    usEncodeSlotStub(stub, s->payloadPlace.baseVA + entryOffset, tail0, tail1,
                     stubSlot);
    usEncodeSlotTarget(stub + US_SLOT_STUB_WORDS,
                       s->payloadPlace.baseVA + entryOffset);
    memcpy(stubAt, stub, sizeof(stub));
    usCacheFlushRange(stubAt, sizeof(stub));
    cfg->stubs[cfg->stubCount++] = (UsPayloadStub){
        .address = (uint64_t)(uintptr_t)stubAt,
        .tableAddress = tableAddress,
        .imageAddress = (uintptr_t)target->image->base,
        .tablePA = tablePA,
        .addressPA = stubPA,
        .targetIndex = usSlotStubTargetIndex(stubSlot),
        .descriptorBaseRVA = target->descriptorBaseRVA,
    };

    /* The slot last: until the stub is there, a branch into it would be a
     * branch into whatever the hole held, which is zeroes */
    memcpy(slotAt, &enter, sizeof(enter));
    usCacheFlushRange(slotAt, sizeof(enter));

    usLogV("arm", "vbar +" US_VALUE("%#x") " slot " US_VALUE("%u") " -> stub +"
           US_VALUE("%#x") " -> payload " US_VALUE("%#llx") "\n",
           target->tableRVA, (unsigned)target->slot, target->stubRVA,
           (unsigned long long)(s->payloadPlace.baseVA + entryOffset));
    return true;
}

bool usArmVectorTables(UsSession *s, UsImageKind kind, size_t *armed) {
    /*
     * The SPx slot always, the others when asked for. All six are entries the
     * kernel's own code can reach: the synchronous three carry the RCpc loads,
     * and the asynchronous three are the ones that would otherwise reach the
     * kernel's own stack exception handler
     *
     * The order matters: the lower EL entry is last, so a table that gets
     * every slot but that one - which is what the loader's table wants, since
     * nothing runs at EL0 while the loader has it - is the first five
     */
    const UsVectorSlot slots[US_STUB_SLOTS] = {
        UsVectorSlotEL1tSync,
        UsVectorSlotEL1hSync,
        UsVectorSlotEL1hIrq,
        UsVectorSlotEL1hFiq,
        UsVectorSlotEL1hSError,
        UsVectorSlotEL0Sync32,
    };
    /*
     * The EL0 entry is taken over in the kernel's own table only. The loader's
     * tables are the ones in force while the loader runs, and nothing runs at
     * EL0 then: the exception level is not carried down to a user mode until
     * the kernel has installed this table and started a process. Taking that
     * slot in the loader buys nothing and costs a stub there
     */
    const size_t slotCount = s->stubVectorsEl1t
                                 ? (kind == UsImageNtoskrnl ? US_STUB_SLOTS
                                                            : US_STUB_SLOTS - 1U)
                                 : 1U;
    UsImage *img = usRegistryGet(&s->registry, kind);
    UsVBARTables tables;
    UsSpareSlot hole;
    uint32_t stubs;
    /* Where the image's descriptor base is, or zero when it has none */
    uint32_t descriptorBaseRVA = 0U;

    if (img == NULL) {
        return false;
    }
    tables = usFindVBARTables(img);
    if (tables.count == 0) {
        return false;
    }
    stubs = US_SLOT_RUNTIME_BYTES * US_STUB_SLOTS * (uint32_t)tables.count;
    /*
     * Where the stubs go. The hole has to be in the same image as the slots,
     * for the branch that reaches it
     *
     * The driver hook's thunk goes in the same run, so it is asked for in the
     * same request: the locator answers with the longest run wherever it is
     * asked from, and a second trip to it for the thunk would be answered with
     * the same bytes the stubs are in
     */
    hole = usLocateSpareSlot(img, stubs
                                 + (kind == UsImageNtoskrnl
                                            && s->driverInplaceRewrite
                                        ? US_SLOT_TARGET_BYTES
                                        : 0U));
    if (!hole.found) {
        usLogE("arm", "no room in %s for a stub\n", usImageKindName(kind));
        return false;
    }
    if (kind == UsImageNtoskrnl) {
        UsPteBaseSite pte;

        s->kernelSpareRVA = hole.rva + stubs;
        s->kernelSpareBytes = hole.bytes > stubs ? hole.bytes - stubs : 0U;

        /*
         * Where this image keeps the base of its own descriptor mapping. The
         * kernel chooses the value and it moves between builds, so it is read
         * out of the image rather than configured
         */
        pte = usLocatePteBase(img);
        descriptorBaseRVA = usArmConfigDescriptorBase(s, &pte);
        usArmConfigEl0InPlace(s, descriptorBaseRVA);
    }

    usLogV("arm", "%s tables=" US_VALUE("%u") " stubs at +" US_VALUE("%#x")
           " room=" US_VALUE("%u") "\n",
           usImageKindName(kind), (unsigned)tables.count, hole.rva, hole.bytes);

    for (size_t i = 0; i < tables.count; i++) {
        for (size_t k = 0; k < slotCount; k++) {
            /* Each table gets its own pair. Sharing one pair between two
             * tables means the second write lands on the first table's stub,
             * and that table's slots then branch into code whose tail belongs
             * to the other one */
            UsArmTarget target = {
                .image = img,
                .tableRVA = tables.rvas[i],
                .stubRVA = hole.rva
                           + (uint32_t)(i * US_STUB_SLOTS + k) * US_SLOT_RUNTIME_BYTES,
                .slot = s->stubVectorsEl1t ? slots[k] : UsVectorSlotEL1hSync,
                /*
                 * Emptied for every image that is not the kernel, and for the
                 * kernel it is whatever the locator answered or nothing
                 */
                .descriptorBaseRVA = descriptorBaseRVA,
            };

            if (armSlot(s, &target)) {
                (*armed)++;
            }
        }
    }

    return true;
}

/*
 * Recording the kernel's driver load call, and where its thunk will go
 *
 * Nothing is written here. That is the design and not an omission: at the boot
 * the kernel has no address and the payload has none that will survive, so
 * there is nothing either of the two things being joined could be written with.
 * Both exist at once at the handover - the same moment every vector stub is
 * pointed at the payload - and that is where this is written, by the payload
 * itself (see payload/early.c)
 *
 * What the boot does know is where each of them will be, in the one form that
 * survives the switch: an RVA into the kernel image. So what it leaves behind is
 * three of those and the entry's offset into the blob
 *
 * A refusal here is a machine that still works. The kernel loads its drivers
 * through its own call, and the only thing lost is the speed of replacing their
 * loads rather than carrying each one out as it traps
 */
bool usArmDriverHookRecord(UsSession *s) {
    UsDriverLoadHook hook;

    if (!s->payloadPlaced) {
        usLogE("arm", "no payload to divert a driver load into\n");
        return false;
    }
    /*
     * The thunk goes in the run the vector stubs are in, and that run is only
     * recorded by arming them. So this depends on the vectors having been taken
     * over - which is honest, because it also depends on them at run time: the
     * replacement this entry will do faults on purpose, and the stub is what
     * answers a fault of ours rather than the kernel's
     */
    if (s->kernelSpareBytes < US_SLOT_TARGET_BYTES) {
        usLogE("arm", "no room beside the vector stubs for a thunk\n");
        return false;
    }
    hook = usLocateDriverLoadHook(usRegistryGet(&s->registry, UsImageNtoskrnl));
    if (!hook.found) {
        usLogE("arm", "no driver load call to take over\n");
        return false;
    }
    usArmConfigDriverHook(s, &hook);
    return true;
}
