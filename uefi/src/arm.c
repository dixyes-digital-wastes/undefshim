/*
 * Taking over the handover, see arm.h.
 */

#include <uefi.h>

#include "common/layout.h"
#include "core/scan.h"
#include "core/thunk.h"
#include "payload_blob.h"
#include "transfer_blob.h"
#include "uefi/src/arm.h"
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
 * Both tables are taken over, and both are needed:
 *
 *   - the loader installs a table before the kernel runs, and that is the one
 *     in force when the kernel takes its first undefined instruction
 *   - the kernel installs its own table once it is running, and images it
 *     loads later -- ci.dll among them -- fault into that one
 *
 * The second is the one that matters for everything the replacement pass
 * cannot reach. An image that is read off disk after the boot has never been
 * scanned, so its instructions are still the ones this hardware does not have.
 *
 * Each table is found by following the register the writes of VBAR_EL1 load,
 * not by looking for a table-shaped thing. An image carries more than one, and
 * the one that looks most like a table is not the one in use.
 *
 * A slot holding a branch is written; one holding anything else is left alone,
 * because those bytes are a handler and the stub has nothing to keep.
 */
typedef struct UsArmTarget_t {
    UsImage   *image;
    uint32_t   tableRva;
    uint32_t   syncWord;
    UsVectorSlot slot;
} UsArmTarget;

static bool armSlot(UsSession *s, const UsArmTarget *target) {
    uint32_t stub[US_STUB_WORDS];
    uint8_t *at;
    uint64_t payloadEntry;

    at = (uint8_t *)(uintptr_t)usImageRvaToPtr(target->image,
                                               target->tableRva
                                                   + (uint32_t)target->slot * 0x80U);
    if (at == NULL) {
        return false;
    }

    /*
     * The pool's address as it is now. Physical memory stays reachable at its
     * own address from the kernel's side, which was measured rather than
     * assumed: the kernel's page tables are not the only ones in force, and
     * the identity mapping built at boot survives.
     */
    payloadEntry = s->payloadPlace.baseVa + US_PAYLOAD_ENTRY_OFFSET;

    usEncodeVectorStub(stub, payloadEntry, target->syncWord);
    memcpy(at, stub, US_STUB_BYTES);
    usCacheFlushRange(at, US_STUB_BYTES);

    usConsolePuts("arm: vbar +");
    usConsolePutHex(target->tableRva);
    usConsolePuts(" slot ");
    usConsolePutDec((uint64_t)target->slot);
    usConsolePuts(" -> ");
    usConsolePutHex(payloadEntry);
    usConsolePuts("\n");
    return true;
}

static bool armImage(UsSession *s, UsImageKind kind, size_t *armed) {
    UsImage *img = usRegistryGet(&s->registry, kind);
    UsVbarTables tables;

    if (img == NULL) {
        return false;
    }
    tables = usFindVbarTables(img);

    usConsolePuts("arm: ");
    usConsolePuts(usImageKindName(kind));
    usConsolePuts(" vbar sites=");
    usConsolePutDec(tables.sites);
    usConsolePuts(" unresolved=");
    usConsolePutDec(tables.unresolved);
    usConsolePuts(" tables=");
    usConsolePutDec(tables.count);
    usConsolePuts("\n");

    for (size_t i = 0; i < tables.count; i++) {
        UsArmTarget target = {
            .image = img,
            .tableRva = tables.rvas[i],
            .syncWord = tables.syncWord[i],
            .slot = UsVectorSlotEl1hSync,
        };

        if (!tables.syncUsable[i]) {
            usConsolePuts("arm: +");
            usConsolePutHex(tables.rvas[i]);
            usConsolePuts(" has a handler written out in full, left alone\n");
            continue;
        }
        if (armSlot(s, &target)) {
            (*armed)++;
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
    armImage(s, UsImageNtoskrnl, &armed);

    if (armed == 0) {
        usConsolePuts("arm: nothing taken over\n");
        return false;
    }
    return true;
}
