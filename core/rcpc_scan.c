/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding the RCpc loads in an image, see rcpc_scan.h
 *
 * The walk goes one word at a time from the start of each section and never
 * tries to find instruction boundaries. It does not have to: an RCpc load is a
 * whole word with fixed bits, so a word that matches is one wherever it sits,
 * and a word that does not is passed over
 */

// us-build: both

#include "core/rcpc_scan.h"

#include "common/layout.h"
#include "core/pe.h"

#define US_SECTION_HEADER_BYTES 40U
#define US_PE_OPTIONAL_MAGIC_64 0x20BU

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

bool usRcpcDescribeImage(const void *base, UsRcpcImage *out) {
    const uint8_t *p = base;
    const uint8_t *coff;
    uint32_t lfanew;

    if (rd16(p) != US_PE_DOS_MAGIC) {
        return false;
    }
    lfanew = rd32(p + 0x3CU);
    coff = p + lfanew;
    if (rd32(coff) != US_PE_MAGIC) {
        return false;
    }
    /*
     * The optional header's magic says which form this is, and the fields
     * wanted sit at different offsets in the two. This is a report and not a
     * loader: an image of a shape this does not know is said to be unreadable
     * rather than guessed at
     */
    if (rd16(coff + 24) != US_PE_OPTIONAL_MAGIC_64) {
        return false;
    }
    out->base = p;
    out->entryRVA = rd32(coff + 24 + 16);
    out->sizeOfImage = rd32(coff + 24 + 56);
    out->sectionCount = rd16(coff + 6);
    out->sectionTableOffset = lfanew + 24 + rd16(coff + 20);
    return out->sizeOfImage != 0;
}

UsRcpcScanResult usRcpcScanSections(uint16_t sectionCount, UsRcpcSectionAt at, void *context,
                                    uint32_t imageLimit, bool rawBound, const UsRcpcScan *scan) {
    UsRcpcScanResult result = { 0 };

    for (uint16_t i = 0; i < sectionCount; i++) {
        UsRcpcSection section = { 0 };
        uint32_t span;
        bool written = false;

        if (!at(context, i, &section)) {
            break;
        }
        /*
         * A section that is only an entry in the table is not mapped, so there
         * are no bytes to read, and a section the caller could not place has
         * none either
         */
        if ((section.characteristics & US_PE_SECTION_EXECUTABLE) == 0
            || section.virtualSize == 0 || section.rawSize == 0
            || section.base == NULL) {
            continue;
        }
        /* An entry that claims more than the image holds is not one to read
         * past, and one that cannot hold a whole instruction holds no site */
        if (imageLimit != 0 && section.virtualAddress + 4U > imageLimit) {
            continue;
        }
        span = section.virtualSize;
        if (rawBound && section.rawSize < span) {
            span = section.rawSize;
        }
        for (uint32_t page = 0; page < span; page += US_PAGE_SIZE) {
            uint32_t end = span - page;

            if (end > US_PAGE_SIZE) {
                end = US_PAGE_SIZE;
            }
            /*
             * One word per page decides the whole page. The image has been
             * mapped and not run, so its pages arrive as they are touched, and
             * reading one that has not is a fault the walk would take once per
             * word and never finish. A page left alone keeps trapping on the
             * loads in it, which is slower and correct
             */
            if (scan->readable != NULL
                && !scan->readable(section.base + page, scan->readableContext)) {
                result.skipped++;
                continue;
            }
            for (uint32_t off = 0; off + 4U <= end; off += 4U) {
                const uint8_t *p = section.base + page + off;
                UsRcpcSite site;
                uint32_t insn;

                if (imageLimit != 0
                    && section.virtualAddress + page + off + 4U > imageLimit) {
                    break;
                }
                insn = rd32(p);
                site.kind = usLDAPRDecode(insn).kind;
                if (site.kind == UsLDAPRNone) {
                    continue;
                }
                site.at = (uint64_t)(uintptr_t)p;
                site.insn = insn;
                result.sites++;
                if (scan->target != 0 && site.at != scan->target) {
                    continue;
                }
                if (scan->mode == UsRcpcReport) {
                    continue;
                }
                result.offered++;
                if (scan->visit != NULL && scan->visit(&site, scan->visitContext)) {
                    result.written++;
                    written = true;
                }
            }
        }
        if (written && scan->spanDone != NULL) {
            scan->spanDone(&section, span, scan->spanDoneContext);
        }
    }
    return result;
}

UsRcpcScanResult usRcpcScanImage(uint16_t sectionCount, UsRcpcSectionAt at, void *context,
                                 const UsRcpcScan *scan) {
    return usRcpcScanSections(sectionCount, at, context, 0, true, scan);
}

/* The section table of a mapped image, one entry at a time. The header has
 * already been read by the caller, so the entries are where it said they are */
static bool mappedSectionAt(void *context, uint16_t index, UsRcpcSection *out) {
    const UsRcpcImage *image = context;
    const uint8_t *sh = image->base + image->sectionTableOffset
                        + (size_t)index * US_SECTION_HEADER_BYTES;

    out->virtualAddress = rd32(sh + 12);
    out->base = image->base + out->virtualAddress;
    out->virtualSize = rd32(sh + 8);
    out->rawSize = rd32(sh + 16);
    out->characteristics = rd32(sh + 36);
    return true;
}

UsRcpcScanResult usRcpcScanMappedImage(const UsRcpcImage *image, const UsRcpcScan *scan) {
    /*
     * No reader is no answer, and an answer is what a read here needs: a page
     * of an image the kernel has mapped may not be there yet, and a fault
     * taken inside this walk has nothing in here to answer it
     */
    if (image == NULL || scan == NULL || scan->readable == NULL) {
        UsRcpcScanResult nothing = { 0 };

        return nothing;
    }
    return usRcpcScanSections(image->sectionCount, mappedSectionAt, (void *)image,
                              image->sizeOfImage, false, scan);
}
