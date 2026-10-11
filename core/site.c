/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The sites a boot has to act on, see site.h for the contract
 */

// us-build: driver

#include "core/site.h"

void usSiteListInit(UsSiteList *list) {
    list->count = 0;
    list->total = 0;
}

bool usSiteAdd(UsSiteList *list, UsSiteKind kind, UsImageKind image, uint32_t rva,
               uint32_t auxiliary) {
    list->total++;
    if (list->count >= US_SITE_MAX) {
        return false;
    }
    list->sites[list->count].kind = kind;
    list->sites[list->count].image = image;
    list->sites[list->count].rva = rva;
    list->sites[list->count].auxiliary = auxiliary;
    list->count++;
    return true;
}

const char *usSiteKindName(UsSiteKind kind) {
    switch (kind) {
    case UsSiteVBARWrite:
        return "vbar-write";
    case UsSiteTransferLeaf:
        return "transfer-leaf";
    case UsSiteTTBRHandoff:
        return "ttbr-handoff";
    }
    return "unknown";
}

/*
 * The vector table writes, found by walking the executable sections directly
 * rather than through usScanImage. That helper keeps at most sixty four
 * matches, which is a sensible bound for a locator that has to be unique but
 * the wrong one here: these are collected precisely because there are many of
 * them, and a list that stopped at the first sixty four would be a list that
 * silently missed some
 */
static size_t collectVBARWrites(UsSiteList *list, UsImage *img, UsImageKind kind) {
    size_t before = list->total;

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;
        size_t offset = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0) {
            continue;
        }
        p = usImageRVASpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }

        while (offset + 4 <= avail) {
            if (patMatchAt(p + offset, &usPatMsrVBAREL1)) {
                uint32_t word = (uint32_t)p[offset] | ((uint32_t)p[offset + 1] << 8)
                                | ((uint32_t)p[offset + 2] << 16)
                                | ((uint32_t)p[offset + 3] << 24);
                uint32_t rva = s->virtualAddress + (uint32_t)offset;

                /* The low five bits are the register holding the address */
                usSiteAdd(list, UsSiteVBARWrite, kind, rva, word & 0x1FU);
            }
            offset += 4;
        }
    }

    return list->total - before;
}

size_t usCollectSites(UsSiteList *list, UsImage *img, UsImageKind kind) {
    size_t before = list->total;

    if (img == NULL || !img->valid) {
        return 0;
    }

    collectVBARWrites(list, img, kind);

    /*
     * The other two only exist in the loader: the leaf is what hands control
     * to the kernel, and the handoff is what rebuilds the address space for
     * it. Neither appears in the kernel itself
     */
    if (kind == UsImageWinload) {
        UsLeafSite leaf = usLocateTransferLeaf(img);
        UsHandoffSite handoff = usLocateTTBRHandoff(img);

        if (leaf.found) {
            usSiteAdd(list, UsSiteTransferLeaf, kind, leaf.patchRVA, 0);
        }
        if (handoff.found) {
            usSiteAdd(list, UsSiteTTBRHandoff, kind, handoff.rva, (uint32_t)handoff.form);
        }
    }

    return list->total - before;
}

static int compareSites(const void *a, const void *b) {
    const UsSite *x = a;
    const UsSite *y = b;

    if (x->image != y->image) {
        return (int)x->image - (int)y->image;
    }
    if (x->rva != y->rva) {
        return x->rva < y->rva ? -1 : (x->rva > y->rva ? 1 : 0);
    }
    return (int)x->kind - (int)y->kind;
}

/*
 * Insertion sort, because the list is short and this avoids pulling in qsort,
 * which is a libcall the payload cannot make. The driver could, but the same
 * code is meant to be usable from either side of the handoff
 */
void usSiteListSort(UsSiteList *list) {
    for (size_t i = 1; i < list->count; i++) {
        UsSite key = list->sites[i];
        size_t j = i;

        while (j > 0 && compareSites(&list->sites[j - 1], &key) > 0) {
            list->sites[j] = list->sites[j - 1];
            j--;
        }
        list->sites[j] = key;
    }
}
