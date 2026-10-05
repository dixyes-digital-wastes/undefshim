/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The record the address change notification fills in, see vamap.h
 *
 * It lives here, in the payload, rather than in the driver, because of where
 * the two end up: the driver's memory is boot services memory and is released
 * once the firmware is done with it, while this blob is in a class the OS
 * keeps. A record written by the driver and read afterwards has to be in the
 * one that survives
 *
 * The driver reaches it through this blob's address, which it knows
 */

#include "payload/vamap.h"
#include "payload/early.h"

UsVAMapRecord usVAMapRecord;

typedef uint64_t (*UsSetVirtualAddressMapFn)(uint64_t, uint64_t, uint32_t, void *);

/*
 * Deployed over the firmware's SetVirtualAddressMap. Its job is to be seen:
 * whatever it does happens after the boot services are gone, so nothing can
 * be printed and nothing outside this blob may be touched. The answer goes
 * into the record, which is this blob's memory and outlives everything else
 *
 * Forwarded, because forwarding costs one call and not forwarding would leave
 * the machine to finish booting with the change half applied
 */
uint64_t usVAMapHook(uint64_t mapSize, uint64_t descSize, uint32_t descVersion,
                     void *descs) {
    usVAMapRecord.magic = US_VAMAP_MAGIC;
    usVAMapRecord.hookFired = 1;
    usVAMapRecord.hookMapSize = mapSize;
    usVAMapRecord.hookDescs = (uint64_t)(uintptr_t)descs;

    if (usVAMapRecord.svmOriginal != 0) {
        UsSetVirtualAddressMapFn original =
            (UsSetVirtualAddressMapFn)(uintptr_t)usVAMapRecord.svmOriginal;

        usVAMapRecord.hookStatus = original(mapSize, descSize, descVersion, descs);
    }
    return usVAMapRecord.hookStatus;
}

typedef uint64_t (*UsConvertPointerFn)(uint64_t, void **);

typedef struct UsVAMapTarget_t {
    uint64_t *before;
    uint64_t *after;
    uint64_t *status;
} UsVAMapTarget;

/*
 * Called by the firmware from inside the switch, so the firmware is still
 * answering questions and the boot services are not coming back
 *
 * The two regions are asked about separately, and each answer is stored
 * before the next question: a firmware that has no entry for one of them
 * says so, and that must not cost the other its answer
 */
void usVAMapNotify(void *event, void *context) {
    UsConvertPointerFn convert = (UsConvertPointerFn)(uintptr_t)usVAMapRecord.convertPointer;
    UsVAMapTarget targets[] = {
        { .before = &usVAMapRecord.poolBefore,
          .after = &usVAMapRecord.poolAfter,
          .status = &usVAMapRecord.poolStatus },
        { .before = &usVAMapRecord.payloadBefore,
          .after = &usVAMapRecord.payloadAfter,
          .status = &usVAMapRecord.payloadStatus },
    };

    (void)event;
    (void)context;

    usVAMapRecord.magic = US_VAMAP_MAGIC;
    usVAMapRecord.fired = 1;

    if (convert == 0) {
        return;
    }

    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        void *address = (void *)(uintptr_t)*targets[i].before;

        *targets[i].after = *targets[i].before;
        if (address != 0) {
            *targets[i].status = convert(0, &address);
            *targets[i].after = (uint64_t)(uintptr_t)address;
        }
    }
    usPayloadEarly();
}
