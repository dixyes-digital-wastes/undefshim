/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The handover stub: the branch the loader enters the kernel with, and the
 * bytes that receive it
 *
 * The stub goes into a run of zeroes inside the loader's own image, because
 * the branch that reaches it is relative and the image moves when the address
 * space is rebuilt
 */

#include <uefi.h>

#include "core/cache.h"
#include "core/scan.h"
#include "core/thunk.h"
#include "payload_blob.h"
#include "transfer_blob.h"
#include "uefi/arm_transfer.h"
#include "uefi/console.h"
#include "uefi/registry.h"

/* The leaf the loader branches to the kernel from */
static bool transferLeaf(UsImage *loader, UsLeafSite *out) {
    *out = usLocateTransferLeaf(loader);
    if (!out->found) {
        usLogE("arm", "no handover to take over\n");
        return false;
    }
    return true;
}

/*
 * The room for the stub: the end of the run of zeroes, not its start
 *
 * The vector table stubs are placed at the start of the same run -- it is the
 * longest hole in the image, and both locators take the longest -- so a stub
 * at the start would be overwritten by them and the handover would branch into
 * a vector stub instead
 */
static bool transferSlot(UsImage *loader, UsSpareSlot *out) {
    *out = usLocateSpareSlot(loader, US_TRANSFER_BYTES);
    if (!out->found) {
        usLogE("arm", "no slot for the handover stub\n");
        return false;
    }
    out->rva += out->bytes - US_TRANSFER_BYTES;
    return true;
}

static bool transferBranch(const UsLeafSite *leaf, const UsSpareSlot *slot,
                           uint32_t *out) {
    if (!usEncodeBranch(leaf->patchRVA, slot->rva, out)) {
        usLogE("arm", "the stub slot is out of branch range\n");
        return false;
    }
    return true;
}

/*
 * The stub, then the address it jumps to, then the branch that reaches it:
 * until the stub is in place, a branch into the hole is a branch into zeroes
 */
static bool writeTransferStub(UsSession *s, UsImage *loader, const UsLeafSite *leaf,
                              const UsSpareSlot *slot, uint32_t branch) {
    uint8_t *branchAt = (uint8_t *)(uintptr_t)usImageRVAToPtr(loader, leaf->patchRVA);
    uint8_t *slotAt = (uint8_t *)(uintptr_t)usImageRVAToPtr(loader, slot->rva);
    uint64_t payloadEntry;

    if (branchAt == NULL || slotAt == NULL) {
        usLogE("arm", "the loader's code is not reachable\n");
        return false;
    }
    memcpy(slotAt, kTransferStub, US_TRANSFER_BYTES);
    payloadEntry = s->payloadPlace.baseVA + US_PAYLOAD_TRANSFER_OFFSET;
    memcpy(slotAt + US_TRANSFER_TARGET_OFFSET, &payloadEntry, sizeof(payloadEntry));
    memcpy(branchAt, &branch, sizeof(branch));
    usCacheFlushRange(slotAt, US_TRANSFER_BYTES);
    usCacheFlushRange(branchAt, sizeof(branch));
    usLogV("arm", "handover +" US_VALUE("%#x") " -> slot +" US_VALUE("%#x")
           " -> payload " US_VALUE("%#llx") "\n",
           leaf->patchRVA, slot->rva, (unsigned long long)payloadEntry);
    return true;
}

bool usArmStubHandover(UsSession *s) {
    UsImage *loader = usRegistryGet(&s->registry, UsImageWinload);
    UsLeafSite leaf;
    UsSpareSlot slot;
    uint32_t branch;

    if (loader == NULL) {
        return false;
    }
    if (!transferLeaf(loader, &leaf) || !transferSlot(loader, &slot)
        || !transferBranch(&leaf, &slot, &branch)) {
        return false;
    }
    return writeTransferStub(s, loader, &leaf, &slot, branch);
}
