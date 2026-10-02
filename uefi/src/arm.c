/*
 * Taking over the handover, see arm.h.
 */

#include <uefi.h>

#include "core/scan.h"
#include "payload_blob.h"
#include "transfer_blob.h"
#include "uefi/src/arm.h"
#include "uefi/src/cache.h"
#include "uefi/src/console.h"

/* arm64's unconditional branch: a 26 bit word offset, in instructions. */
#define US_BRANCH_OPCODE 0x14000000U
#define US_BRANCH_RANGE (1U << 27)

static bool encodeBranch(uint32_t fromRva, uint32_t toRva, uint32_t *out) {
    int64_t delta = (int64_t)(int32_t)toRva - (int64_t)(int32_t)fromRva;

    if (delta < -(int64_t)US_BRANCH_RANGE || delta > (int64_t)(US_BRANCH_RANGE - 4)) {
        return false;
    }
    if ((delta & 3) != 0) {
        return false;
    }
    *out = US_BRANCH_OPCODE | (((uint32_t)(delta >> 2)) & 0x03FFFFFFU);
    return true;
}

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

    if (!encodeBranch(leaf.patchRva, slot.rva, &branch)) {
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
