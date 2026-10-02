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
 * The loader installs a vector table before the kernel runs, and it is that
 * table which is in force when the kernel first executes an instruction this
 * hardware does not have. So taking over the table's synchronous slot is what
 * makes the shim reachable at all; the kernel's own table is a later problem,
 * and one that cannot happen before this one works.
 *
 * The table is found by following the register each write of VBAR_EL1 loads,
 * not by looking for a table-shaped thing. An image carries more than one,
 * and the one that looks most like a table is not the one in use.
 *
 * The slot is written only if it is still a branch to itself. That is a free
 * slot; anything else is a handler that works, and replacing it would break a
 * boot that was otherwise fine. Refusing costs a shim, which is the cheaper
 * of the two failures.
 */
typedef struct UsArmTarget_t {
    UsImage   *image;
    uint32_t   tableRva;
    UsVectorSlot slot;
} UsArmTarget;

static bool armSlot(UsSession *s, const UsArmTarget *target) {
    uint32_t thunk[US_THUNK_WORDS];
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

    usEncodeThunk(thunk, payloadEntry);
    memcpy(at, thunk, US_THUNK_BYTES);
    usCacheFlushRange(at, US_THUNK_BYTES);

    usConsolePuts("arm: vbar +");
    usConsolePutHex(target->tableRva);
    usConsolePuts(" slot ");
    usConsolePutDec((uint64_t)target->slot);
    usConsolePuts(" -> ");
    usConsolePutHex(payloadEntry);
    usConsolePuts("\n");
    return true;
}

bool usArmVectorTable(UsSession *s) {
    UsImage *loader = usRegistryGet(&s->registry, UsImageWinload);
    UsVbarTables tables;
    size_t armed = 0;

    if (loader == NULL) {
        usConsolePuts("arm: no loader to draw the exception path through\n");
        return false;
    }
    if (!s->payloadPlaced) {
        usConsolePuts("arm: no payload to enter\n");
        return false;
    }

    tables = usFindVbarTables(loader);
    usConsolePuts("arm: vbar sites=");
    usConsolePutDec(tables.sites);
    usConsolePuts(" unresolved=");
    usConsolePutDec(tables.unresolved);
    usConsolePuts(" tables=");
    usConsolePutDec(tables.count);
    usConsolePuts("\n");

    for (size_t i = 0; i < tables.count; i++) {
        UsArmTarget target = {
            .image = loader,
            .tableRva = tables.rvas[i],
            .slot = UsVectorSlotEl1hSync,
        };

        if (!tables.syncFree[i]) {
            usConsolePuts("arm: +");
            usConsolePutHex(tables.rvas[i]);
            usConsolePuts(" has a synchronous handler already, left alone\n");
            continue;
        }
        if (armSlot(s, &target)) {
            armed++;
        }
    }

    if (armed == 0) {
        usConsolePuts("arm: nothing taken over\n");
        return false;
    }
    return true;
}
