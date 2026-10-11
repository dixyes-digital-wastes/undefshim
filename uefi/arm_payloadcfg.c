/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The payload's configuration block, written item by item as each becomes
 * known
 */

#include <stdint.h>

#include "payload/payload.h"
#include "payload_blob.h"
#include "uefi/arm_payloadcfg.h"
#include "uefi/console.h"

static UsPayloadConfig *payloadConfig(const UsSession *s) {
    return (UsPayloadConfig *)(uintptr_t)s->payloadPlace.configVA;
}

uint32_t usArmConfigDescriptorBase(UsSession *s, const UsPteBaseSite *pte) {
    if (pte->found) {
        /*
         * The payload needs the same base, at the handover, to build a mapping
         * of its own. What it gets is the RVA: the variable's address moves with
         * the image and the RVA does not, and the payload is the side that
         * knows where the image ended up
         */
        payloadConfig(s)->descriptorBaseRVA = pte->rva;
        usLogV("arm", "descriptor base at +" US_VALUE("%#x")
               " (body +" US_VALUE("%#x") ", literal " US_VALUE("%#llx")
               ", " US_VALUE("%zu") " load(s))\n",
               (unsigned)pte->rva, (unsigned)pte->helperRVA,
               (unsigned long long)pte->base, pte->candidates);
        return pte->rva;
    }
    if (s->config != NULL && s->config->hasDescriptorBase) {
        /*
         * A build whose shape is not recognised. The configured value is still
         * a candidate, and the handler checks it against the hardware before
         * trusting it, so a stale one costs a rewrite rather than a wrong store
         */
        usLogW("arm", "descriptor base not located (" US_VALUE("%zu")
               " body(s), " US_VALUE("%zu") " load(s)); using the"
               " configured +" US_VALUE("%#x") "\n",
               pte->helpers, pte->candidates,
               (unsigned)s->config->descriptorBaseRVA);
        return s->config->descriptorBaseRVA;
    }
    usLogW("arm", "descriptor base not located (" US_VALUE("%zu")
           " body(s), " US_VALUE("%zu") " load(s)) and none is"
           " configured; no site can be rewritten in place\n",
           pte->helpers, pte->candidates);
    return 0U;
}

void usArmConfigEl0InPlace(UsSession *s, uint32_t descriptorBaseRVA) {
    UsPayloadConfig *cfg = payloadConfig(s);
    /*
     * Whether the user-mode replacement can happen, which is first known here
     *
     * With no base every attempt is refused, so a payload told to try anyway
     * would enter the probe bank - a stack the kernel owns - on every RCpc load
     * in user code to reach an answer it could have been given here. The
     * exception path still stands: an RCpc load that is not replaced is carried
     * out by the handler, which is correct and slower, and that is exactly what
     * a machine with no base is
     */
    cfg->el0InPlace = descriptorBaseRVA != 0 && s->config != NULL
                          && s->config->el0InplaceRewrite
                      ? 1U
                      : 0U;
    /*
     * Said out loud, because the two ways it can be off look the same from
     * everywhere else: the configuration did not ask for it, or the base was
     * not found. A machine whose user code takes the exception path for every
     * RCpc load has this line to say which
     */
    usLogV("arm", "user-mode replacement " US_VALUE("%s") " (asked "
           US_VALUE("%s") ", base " US_VALUE("%s") ")\n",
           cfg->el0InPlace != 0 ? "on" : "off",
           s->config != NULL && s->config->el0InplaceRewrite ? "yes" : "no",
           descriptorBaseRVA != 0 ? "found" : "none");
}

void usArmConfigDriverHook(UsSession *s, const UsDriverLoadHook *hook) {
    UsPayloadConfig *cfg = payloadConfig(s);

    cfg->driverLoadThunkRVA = s->kernelSpareRVA;
    cfg->driverLoadCallRVA = hook->callRVA;
    cfg->driverLoadTargetRVA = hook->witnessRVA;
    cfg->driverLoadEntryOffset = US_PAYLOAD_DRIVERHOOK_OFFSET;
    /*
     * Printed at the boot, because it is the last moment anything can be said:
     * by the time the hook is written the console's page is not in the kernel's
     * tables and the report has to go to a pool instead. It names both halves,
     * so a call that was recorded and a call that was not can be told apart
     */
    usLogI("arm", "driver load +" US_VALUE("%#x") " -> thunk +" US_VALUE("%#x")
           " -> payload +" US_VALUE("%#x") ", call's target +" US_VALUE("%#x") "\n",
           hook->callRVA, s->kernelSpareRVA, US_PAYLOAD_DRIVERHOOK_OFFSET,
           hook->witnessRVA);
}
