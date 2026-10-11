/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Carrying out the RCpc loads by replacing them, see rewrite.h
 */

#include <uefi.h>

#include "core/ldapr.h"
#include "core/pe.h"
#include "core/scan.h"
#include "core/cache.h"
#include "core/rcpc_scan.h"
#include "uefi/console.h"
#include "uefi/registry.h"
#include "uefi/rewrite.h"
#include "uefi/session.h"

/*
 * The sections of a placed image, one at a time
 *
 * The walk does not read a section table, because where an RVA points is a
 * property of the view and this is the view that knows
 */
static bool sectionAt(void *context, uint16_t index, UsRcpcSection *out) {
    const UsImage *img = context;
    const UsPESection *s = &img->sections[index];

    out->base = usImageRVAToPtr(img, s->virtualAddress);
    out->virtualAddress = s->virtualAddress;
    out->virtualSize = s->virtualSize;
    out->rawSize = s->rawSize;
    out->characteristics = s->characteristics;
    return true;
}

/* An image that has not started is writable where it lies, so a site is four
 * bytes written over four bytes: the same length, the same registers, and an
 * ordering that cannot be weaker than the load it replaces */
static bool replaceSite(const UsRcpcSite *site, void *context) {
    uint8_t *at = (uint8_t *)(uintptr_t)site->at;
    uint32_t replacement = 0;

    if (!usLDAPRToLDAR(site->insn, &replacement)) {
        return false;
    }
    at[0] = (uint8_t)replacement;
    at[1] = (uint8_t)(replacement >> 8);
    at[2] = (uint8_t)(replacement >> 16);
    at[3] = (uint8_t)(replacement >> 24);
    (*(size_t *)context)++;
    return true;
}

/*
 * The whole section is flushed once rather than each instruction: this is a
 * boot time pass over a large image, and per-instruction flushes would spend
 * all of their time on barrier overhead
 */
static void publishSection(const UsRcpcSection *section, uint32_t span, void *context) {
    (void)context;
    usCacheFlushRange(section->base, span);
}

/*
 * Walks the executable sections of one image and replaces each RCpc load
 *
 * The scan is over sections that carry data, for the same reason the other
 * locators use that rule: a section that is only an entry in the section table
 * is not mapped, and reading it produces zeros rather than code
 */
static size_t rewriteImage(UsImage *img) {
    size_t changed = 0;
    UsRcpcScan scan = { 0 };

    if (img == NULL || !img->valid) {
        return 0;
    }
    /* One field at a time, never as one aggregate: see UsRcpcScan */
    scan.visit = replaceSite;
    scan.visitContext = &changed;
    scan.spanDone = publishSection;
    usRcpcScanImage(img->sectionCount, sectionAt, img, &scan);
    return changed;
}

size_t usRewriteOne(UsImage *img) {
    size_t changed = rewriteImage(img);

    if (changed != 0) {
        usLogI("rewrite", US_VALUE("%u") " in an image at " US_VALUE("%#llx") "\n",
               (unsigned)changed,
               (unsigned long long)(uintptr_t)img->base);
    }
    return changed;
}

size_t usRewriteLDAPR(UsSession *session) {
    static const UsImageKind kinds[] = {
        UsImageNtoskrnl,
        UsImageWinload,
    };
    size_t total = 0;

    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        UsImage *img = usRegistryGet(&session->registry, kinds[i]);
        size_t changed;

        if (img == NULL) {
            continue;
        }
        /* How many instructions the image carried, whether or not any were
         * replaced: a scan that found nothing and a scan that did not run
         * look the same otherwise, and they mean opposite things */
        usLogI("rewrite", "%s holds " US_VALUE("%u") "\n", usImageKindName(kinds[i]),
               (unsigned)usCountLDAPR(img).total);
        changed = rewriteImage(img);
        if (changed != 0) {
            usLogI("rewrite", US_VALUE("%u") " in %s\n", (unsigned)changed,
                   usImageKindName(kinds[i]));
        }
        total += changed;
    }

    return total;
}
