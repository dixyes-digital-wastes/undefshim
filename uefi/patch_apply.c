/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Applying the patch lists named by the configuration to the image they are
 * for, at the moment the image is loaded and before anything of it has run
 *
 * That moment is the point of the whole exercise: the list is in place when
 * the kernel takes its first instruction, so whatever integrity check it
 * makes later is made against what the list left behind, and there is no
 * change for it to notice
 */

#include <uefi.h>

#include "core/patchapply.h"
#include "core/patchlist.h"
#include "uefi/console.h"
#include "uefi/image_identity.h"
#include "uefi/patch_apply.h"
#include "uefi/patch_lists.h"
#include "uefi/patch_report.h"

static void applyOne(UsImage *image, const char *name, const char *text,
                     size_t length) {
    UsPatchSite *sites;
    UsPatchFile file;
    UsPatchStats stats;
    uint32_t textBytes = 0;
    uint8_t *code = usImageTextBytes(image, &textBytes);
    uint32_t capacity = (uint32_t)(length / 8U) + 1U;

    memset(&file, 0, sizeof(file));
    memset(&stats, 0, sizeof(stats));
    if (code == NULL) {
        usLogE("patch", "no text in %s\n", name);
        return;
    }
    sites = NULL;
    if (EFI_ERROR(BS->AllocatePool(LIP ? LIP->ImageDataType : EfiLoaderData,
                                   (uintn_t)capacity * sizeof(UsPatchSite),
                                   (void **)&sites))) {
        return;
    }
    UsPatchStatus status = usPatchParse(text, (uint32_t)length, sites, capacity, &file);

    if (status != UsPatchOk) {
        usLogE("patch", "%s not usable, status " US_VALUE("%u") "\n", name,
               (unsigned)status);
        BS->FreePool(sites);
        return;
    }
    uint8_t digest[32];
    UsPatchIdentity identity;
    UsPatchMatchers matchers;

    usImageTextDigest(image, code, textBytes, digest);
    matchers.imageName = "ntoskrnl";
    matchers.digest = digest;
    matchers.identity = usImageIdentity(image, &identity) ? &identity : NULL;
    stats.files = 1;
    /*
     * Sites are image RVAs and they are not all in .text: the kernel keeps
     * plenty of instructions in paged sections that are executed just the
     * same, and a list written from the file names those too. So the range a
     * site is checked against is the whole image, and what keeps a write from
     * landing somewhere it should not is the match against the bytes already
     * there, which every site carries
     */
    UsPatchApplyResult result = usPatchApplyMatched(&file, sites, &matchers, 0U,
                                                    (uint8_t *)(uintptr_t)image->base,
                                                    image->sizeOfImage, &stats);
    if (result != UsPatchApplied) {
        usPatchReportRefusal(name, &file, &matchers, &stats);
        BS->FreePool(sites);
        return;
    }
    usLogI("patch", "%s applied " US_VALUE("%u") " refused " US_VALUE("%u")
           " out of range " US_VALUE("%u") "\n",
           name, (unsigned)stats.applied, (unsigned)stats.refused,
           (unsigned)stats.outOfRange);
    usPatchReportSites(&stats);
    BS->FreePool(sites);
}

void usPatchApplyLists(UsSession *session, UsImage *image) {
    UsPatchListDir list;
    const char *path;
    uint32_t files = 0;

    if (session->config == NULL || image == NULL) {
        return;
    }
    path = session->config->patchDir;
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (!usPatchListDirOpen(&list, path)) {
        return;
    }

    char name[US_PATCH_LIST_NAME_BYTES];
    for (;;) {
        char *text = NULL;
        size_t length = 0;

        if (!usPatchListDirNext(&list, name, sizeof(name), &text, &length)) {
            break;
        }
        files++;
        applyOne(image, name, text, length);
        BS->FreePool(text);
    }
    usPatchListDirClose(&list);
    usLogV("patch", US_VALUE("%u") " list file(s) from %s\n", (unsigned)files, path);
}
