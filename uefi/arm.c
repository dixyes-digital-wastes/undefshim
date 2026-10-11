/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Taking over the handover, see arm.h
 *
 * The three entries are the session level decisions: whether there is a payload
 * to enter and an image to take over, which images are armed, and whether the
 * driver hook was asked for. What each of them writes lives in arm_transfer.c,
 * arm_vectors.c and arm_payloadcfg.c
 */

#include <stdbool.h>
#include <stddef.h>

#include "core/pe.h"
#include "uefi/arm.h"
#include "uefi/arm_transfer.h"
#include "uefi/arm_vectors.h"
#include "uefi/console.h"
#include "uefi/patch_apply.h"
#include "uefi/registry.h"

bool usArmTransfer(UsSession *s) {
    if (!s->payloadPlaced) {
        return false;
    }
    return usArmStubHandover(s);
}

bool usArmVectorTable(UsSession *s) {
    size_t armed = 0;

    if (!s->payloadPlaced) {
        usLogE("arm", "no payload to enter\n");
        return false;
    }

    usArmVectorTables(s, UsImageWinload, &armed);
    /*
     * Before the kernel's own slots are written and long before it runs: this
     * is the moment its image is loaded and still writable, and the lists are
     * meant to be part of what it starts with rather than a change to it
     */
    {
        UsImage *kernel = usRegistryGet(&s->registry, UsImageNtoskrnl);

        if (kernel != NULL) {
            usPatchApplyLists(s, kernel);
        }
    }
    usArmVectorTables(s, UsImageNtoskrnl, &armed);

    if (armed == 0) {
        usLogE("arm", "nothing taken over\n");
        return false;
    }
    return true;
}

bool usArmDriverLoadHook(UsSession *s) {
    if (!s->driverInplaceRewrite) {
        return true;
    }
    return usArmDriverHookRecord(s);
}
