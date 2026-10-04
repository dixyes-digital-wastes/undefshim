/*
 * Taking over the handover, see arm.h.
 */

#include <uefi.h>

#include "common/layout.h"
#include "core/scan.h"
#include "core/thunk.h"
#include "core/translate.h"
#include "payload_blob.h"
#include "payload/payload.h"
#include "transfer_blob.h"
#include "uefi/src/arm.h"
#include "uefi/src/patch_apply.h"
#include "uefi/src/cache.h"
#include "uefi/src/console.h"
#include "uefi/src/payload_place.h"
#include "uefi/src/registry.h"

bool usArmTransfer(UsSession *s) {
    UsImage *loader = usRegistryGet(&s->registry, UsImageWinload);
    UsLeafSite leaf;
    UsSpareSlot slot;
    uint32_t branch;
    uint8_t *slotVa;
    uint8_t *branchVa;
    uint64_t payloadEntry;

    if (!s->payloadPlaced) {
        usConsolePuts("arm: no payload placed\n");
        return false;
    }
    if (loader == NULL) {
        usConsolePuts("arm: no loader\n");
        return false;
    }

    leaf = usLocateTransferLeaf(loader);
    if (!leaf.found) {
        usConsolePuts("arm: no handover to take over\n");
        return false;
    }

    slot = usLocateSpareSlot(loader, US_TRANSFER_BYTES);
    if (!slot.found) {
        usConsolePuts("arm: no slot large enough for ");
        usConsolePutDec(US_TRANSFER_BYTES);
        usConsolePuts(" bytes\n");
        return false;
    }

    if (!usEncodeBranch(leaf.patchRva, slot.rva, &branch)) {
        usConsolePuts("arm: the slot is out of branch range\n");
        return false;
    }
    branchVa = (uint8_t *)(uintptr_t)usImageRvaToPtr(loader, leaf.patchRva);
    slotVa = (uint8_t *)(uintptr_t)usImageRvaToPtr(loader, slot.rva);
    if (branchVa == NULL || slotVa == NULL) {
        usConsolePuts("arm: the loader's code is not reachable\n");
        return false;
    }

    /*
     * The stub goes in first, with the address it is to call already in it.
     * Writing the branch before the stub exists would leave a window, however
     * short, in which the loader would branch into whatever was there.
     */
    memcpy(slotVa, kTransferStub, US_TRANSFER_BYTES);

    payloadEntry = s->payloadPlace.baseVa + US_PAYLOAD_TRANSFER_OFFSET;
    memcpy(slotVa + US_TRANSFER_TARGET_OFFSET, &payloadEntry, sizeof(payloadEntry));

    memcpy(branchVa, &branch, sizeof(branch));

    usCacheFlushRange(slotVa, US_TRANSFER_BYTES);
    usCacheFlushRange(branchVa, sizeof(branch));

    usConsolePuts("arm: handover +");
    usConsolePutHex(leaf.patchRva);
    usConsolePuts(" -> slot +");
    usConsolePutHex(slot.rva);
    usConsolePuts(" (");
    usConsolePutDec(slot.bytes);
    usConsolePuts(" bytes) calls ");
    usConsolePutHex(payloadEntry);
    usConsolePuts("\n");
    return true;
}

/*
 * Drawing the exception path into the payload, see arm.h.
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
 * handler has no idea what an RCpc load is.
 *
 * A slot is replaced by a branch to a stub, whatever it held. The stub reads
 * the exception class and either leaves for the payload or runs the slot's
 * own two-instruction tail, which for the board's table means a branch to
 * where it branched and for the kernel's SP_EL0 slot means the instruction
 * that was written there before continuing past it.
 *
 * The stubs live in a run of zero words inside the kernel image. They have to
 * be in the same image as the slots they are reached from, because the branch
 * that reaches them is relative and the image moves when the address space is
 * rebuilt.
 */
#define US_STUB_SLOTS 3U

/*
 * Where the kernel keeps the base of its descriptor mapping, from the 26100PE
 * symbols: MmPteBase, in the ALMOSTRO section.
 *
 * The payload needs this to find the descriptor that translates a page it
 * wants to rewrite, and it cannot be a constant in the payload: the value is
 * the kernel's own, chosen at boot, and the literal the kernel's own
 * MiGetPteAddress uses for it is the x64 one, which on a 47-bit address space
 * names an address that does not translate at all.
 */
#define US_NTOSKRNL_DESCRIPTOR_BASE_RVA 0xE06368U

typedef struct UsArmTarget_t {
    UsImage   *image;
    uint32_t   tableRva;
    uint32_t   stubRva;
    UsVectorSlot slot;
    /* RVA of the image's descriptor base, or zero when it has none. */
    uint32_t   descriptorBaseRva;
} UsArmTarget;

/*
 * Which stub shape a vector slot needs. The three synchronous entries differ in
 * what they may use to keep x18 and in whether a data abort has to reach the
 * payload; see core/thunk.h.
 */
static UsStubSlot stubSlotOf(UsVectorSlot slot) {
    switch (slot) {
    case UsVectorSlotEl1hSync:
        return UsStubSlotEl1h;
    case UsVectorSlotEl0Sync32:
        return UsStubSlotEl0;
    default:
        return UsStubSlotEl1t;
    }
}

static bool armSlot(UsSession *s, const UsArmTarget *target, uint32_t next) {
    uint32_t stub[US_SLOT_RUNTIME_WORDS];
    UsPayloadConfig *cfg = (UsPayloadConfig *)(uintptr_t)s->payloadPlace.configVa;
    uint8_t *slotAt;
    uint8_t *stubAt;
    uint32_t original;
    uint32_t enter;
    uint32_t tail0;
    uint32_t tail1;
    /*
     * What the slot's stub has to do about x18 and about faults of our own.
     *
     * The EL1t and EL1h vectors are kernel mode, where x18 is the per-CPU block
     * and is rebuilt from TPIDR_EL1 rather than saved - and there is no stack
     * word to be had on the EL1t vector, whose SP the interrupted code was not
     * using. EL0 is the one vector whose x18 is user state and therefore has to
     * be pushed somewhere; the kernel's own entry there uses SP_EL1 as the
     * thread's kernel stack, so the red zone below it is that somewhere.
     */
    UsStubSlot stubSlot = stubSlotOf(target->slot);
    uint32_t entryOffset = US_PAYLOAD_ENTRY_OFFSET;

    slotAt = (uint8_t *)(uintptr_t)usImageRvaToPtr(target->image,
                                                   target->tableRva
                                                       + (uint32_t)target->slot * 0x80U);
    stubAt = (uint8_t *)(uintptr_t)usImageRvaToPtr(target->image, target->stubRva);
    if (slotAt == NULL || stubAt == NULL || cfg->stubCount >= US_PAYLOAD_MAX_STUBS) {
        return false;
    }
    uint64_t tableAddress = (uintptr_t)usImageRvaToPtr(target->image, target->tableRva);
    uint64_t tablePa;
    uint64_t stubPa;
    uint64_t lastPa;
    /*
     * The physical addresses are recorded for the publication that runs
     * later, and the question asked here is the one the boot's own stores
     * answer: where does *this* exception level's regime put the address. The
     * EL1&0 regime has already been rebuilt for the kernel by the time this
     * runs and no longer holds the images where the loader put them, so asking
     * it here refuses a write that would in fact succeed.
     */
    if (!usTranslateOwnAddress(tableAddress, false, &tablePa)
        || !usTranslateOwnAddress((uintptr_t)stubAt, true, &stubPa)
        || !usTranslateOwnAddress((uintptr_t)stubAt + sizeof(stub) - 1, true, &lastPa)
        || lastPa != stubPa + sizeof(stub) - 1) {
        usConsolePuts("arm: the stub's own address cannot be translated\n");
        return false;
    }
    original = (uint32_t)slotAt[0] | ((uint32_t)slotAt[1] << 8)
               | ((uint32_t)slotAt[2] << 16) | ((uint32_t)slotAt[3] << 24);


    /*
     * The tail: what happens when the exception is not one of ours -- and the
     * test for that is the instruction, not the class, so the branch slots are
     * not the only ones this is safe for.
     *
     * A slot holding a branch keeps its meaning, adjusted for the distance the
     * stub is away from it. A slot holding a handler written out in place runs
     * its first instruction and continues past it. The stub now touches only
     * x18, and gives that back before the tail runs, so the tail is reached
     * with every register but NZCV flags as the exception left them.
     */
    {
        uint32_t tailIndex = usSlotStubTailIndex(stubSlot);
        /* The branch the tail ends with is the last of its words, and the
         * encoder decides how many that is per slot. */
        uint32_t from = target->stubRva
                        + (tailIndex + usSlotStubTailWords(stubSlot) - 1U) * 4U;
        uint32_t slotRva = target->tableRva + (uint32_t)target->slot * 0x80U;

        if ((original & 0xFC000000U) == 0x14000000U) {
            int32_t displacement = (int32_t)(original << 6) >> 6;

            tail0 = US_NOP;
            if (displacement == 0) {
                /* A branch to itself is what an unused slot holds. Keeping
                 * that meaning rather than adjusting it: the place it would
                 * point at is this stub. */
                tail1 = 0x14000000U;
            } else if (!usEncodeBranch(from, slotRva + (uint32_t)(displacement * 4),
                                       &tail1)) {
                return false;
            }
        } else {
            tail0 = original;
            if (!usEncodeBranch(from, slotRva + 4U, &tail1)) {
                return false;
            }
        }
    }

    if (!usEncodeBranch(target->tableRva + (uint32_t)target->slot * 0x80U,
                        target->stubRva, &enter)) {
        return false;
    }

    usEncodeSlotStub(stub, s->payloadPlace.baseVa + entryOffset, tail0, tail1,
                     stubSlot);
    usEncodeSlotTarget(stub + US_SLOT_STUB_WORDS,
                       s->payloadPlace.baseVa + entryOffset);
    memcpy(stubAt, stub, sizeof(stub));
    usCacheFlushRange(stubAt, sizeof(stub));
    cfg->stubs[cfg->stubCount++] = (UsPayloadStub){
        .address = (uint64_t)(uintptr_t)stubAt,
        .tableAddress = tableAddress,
        .imageAddress = (uintptr_t)target->image->base,
        .tablePa = tablePa,
        .addressPa = stubPa,
        .targetIndex = usSlotStubTargetIndex(stubSlot),
        .descriptorBaseRva = target->descriptorBaseRva,
    };

    /* The slot last: until the stub is there, a branch into it would be a
     * branch into whatever the hole held, which is zeroes. */
    memcpy(slotAt, &enter, sizeof(enter));
    usCacheFlushRange(slotAt, sizeof(enter));

    usConsolePuts("arm: vbar +");
    usConsolePutHex(target->tableRva);
    usConsolePuts(" slot ");
    usConsolePutDec((uint64_t)target->slot);
    usConsolePuts(" -> stub +");
    usConsolePutHex(target->stubRva);
    usConsolePuts(" -> payload ");
    usConsolePutHex(s->payloadPlace.baseVa + entryOffset);
    usConsolePuts("\n");
    (void)next;
    return true;
}

static bool armImage(UsSession *s, UsImageKind kind, size_t *armed) {
    /*
     * The SPx slot always, the others when asked for. All three are synchronous
     * entries: EL1t and EL1h for the kernel's own code, EL0 for the user's, and
     * an LDAPR in any of them is an undefined instruction on this hardware.
     * Leaving one out means that one's loads are not carried out at all.
     */
    const UsVectorSlot slots[US_STUB_SLOTS] = {
        UsVectorSlotEl1tSync,
        UsVectorSlotEl1hSync,
        UsVectorSlotEl0Sync32,
    };
    /*
     * The EL0 entry is taken over in the kernel's own table only. The loader's
     * tables are the ones in force while the loader runs, and nothing runs at
     * EL0 then: the exception level is not carried down to a user mode until
     * the kernel has installed this table and started a process. Taking that
     * slot in the loader buys nothing and costs a stub - and, in the loader,
     * hole space that turned out to matter: the boot stopped in the loader
     * with every stub in place and nothing ever entering the payload.
     */
    const size_t slotCount = s->armSlot0
                                 ? (kind == UsImageNtoskrnl ? US_STUB_SLOTS
                                                            : US_STUB_SLOTS - 1U)
                                 : 1U;
    UsImage *img = usRegistryGet(&s->registry, kind);
    UsVbarTables tables;
    UsSpareSlot hole;

    if (img == NULL) {
        return false;
    }
    tables = usFindVbarTables(img);
    if (tables.count == 0) {
        return false;
    }
    /*
     * Where the stubs go. The hole has to be in the same image as the slots,
     * for the branch that reaches it.
     */
    hole = usLocateSpareSlot(img, US_SLOT_RUNTIME_BYTES * US_STUB_SLOTS
                                 * (uint32_t)tables.count);
    if (!hole.found) {
        usConsolePuts("arm: no room in ");
        usConsolePuts(usImageKindName(kind));
        usConsolePuts(" for a stub\n");
        return false;
    }

    usConsolePuts("arm: ");
    usConsolePuts(usImageKindName(kind));
    usConsolePuts(" tables=");
    usConsolePutDec(tables.count);
    usConsolePuts(" stubs at +");
    usConsolePutHex(hole.rva);
    usConsolePuts(" room=");
    usConsolePutDec(hole.bytes);
    usConsolePuts("\n");

    for (size_t i = 0; i < tables.count; i++) {
        for (size_t k = 0; k < slotCount; k++) {
            /* Each table gets its own pair. Sharing one pair between two
             * tables means the second write lands on the first table's stub,
             * and that table's slots then branch into code whose tail belongs
             * to the other one. */
            UsArmTarget target = {
                .image = img,
                .tableRva = tables.rvas[i],
                .stubRva = hole.rva
                           + (uint32_t)(i * US_STUB_SLOTS + k) * US_SLOT_RUNTIME_BYTES,
                .slot = s->armSlot0 ? slots[k] : UsVectorSlotEl1hSync,
                .descriptorBaseRva = kind == UsImageNtoskrnl
                                         ? US_NTOSKRNL_DESCRIPTOR_BASE_RVA
                                         : 0U,
            };

            if (armSlot(s, &target, 0)) {
                (*armed)++;
            }
        }
    }

    return true;
}

bool usArmVectorTable(UsSession *s) {
    size_t armed = 0;

    if (!s->payloadPlaced) {
        usConsolePuts("arm: no payload to enter\n");
        return false;
    }

    armImage(s, UsImageWinload, &armed);
    /*
     * Before the kernel's own slots are written and long before it runs: this
     * is the moment its image is loaded and still writable, and the lists are
     * meant to be part of what it starts with rather than a change to it.
     */
    {
        UsImage *kernel = usRegistryGet(&s->registry, UsImageNtoskrnl);

        if (kernel != NULL) {
            usPatchApplyLists(s, kernel);
        }
    }
    armImage(s, UsImageNtoskrnl, &armed);

    if (armed == 0) {
        usConsolePuts("arm: nothing taken over\n");
        return false;
    }
    return true;
}
