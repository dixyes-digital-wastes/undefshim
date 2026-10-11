/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The entry a diverted kernel call arrives at
 *
 * This is not an exception, and everything about the moment is a call's: the
 * arguments are in x0 and the registers after it, the return address is in x30,
 * the stack is the caller's, and x16 and x17 are free because the calling
 * convention says a call may clobber them. What that buys is the whole of the
 * difference from the exception path - there is nothing to save, nothing to
 * reconstruct, and no frame to hand back. The work is done, the function the
 * kernel meant to call is called, and its answer is returned as though nothing
 * had happened in between
 *
 * x18 is the one thing that would have needed care, and the care is in what is
 * not done here: the thunk that reaches this carries the address in x17 rather
 * than x18, so the per-processor block the kernel keeps in x18 is untouched and
 * the code being returned into finds it where it left it
 *
 * None of this is reachable until a driver is loaded, which is long after the
 * handover, and the hook is only in place at all when the handover managed to
 * point the kernel's own call at it. A machine where that did not happen loads
 * its drivers exactly as it did before
 */

#include "payload/drvhook.h"
#include "payload/payload.h"
#include "core/rcpc_scan.h"
#include "payload/rewrite.h"

/*
 * Whether a page of the image can be read at all, which is not the same
 * question as whether it is mapped
 *
 * The image has been mapped and not yet run, and its pages are brought in as
 * they are touched - so some of what the walk reaches is not there yet, and
 * the kernel is the only thing that can bring it in. The walk asks this about
 * one word of each page and leaves the whole page alone when the answer is no,
 * because a fault taken here would be taken once per word
 */
static bool pageReadable(const uint8_t *page, void *context) {
    uint32_t word = 0;

    (void)context;
    return usPayloadProbeReadWord((uint64_t)(uintptr_t)page, &word);
}

/*
 * What becomes of a site the walk found
 *
 * The image is the kernel's text by the time this runs, so a site cannot be
 * written where it lies: the write goes through the descriptor it is under,
 * and whether that happened is an answer rather than a fault - see
 * usRewriteSiteAtLoad
 *
 * Alignment is the one thing this cannot know. The acquire load that stands in
 * for an RCpc load has to be naturally aligned and the instruction it replaces
 * does not, and the address a load will read is only known when it runs. So a
 * site is taken on unless the rewrite itself refuses it, and a site that is
 * refused keeps trapping
 */
static bool rewriteSite(const UsRcpcSite *site, void *context) {
    (void)context;
    return usRewriteSiteAtLoad(site->at, site->kind) == UsRewriteWritten;
}

/*
 * Called with the address of the image being loaded, and answering with the
 * address of its headers
 *
 * The kernel's call was diverted so that this could happen first, and what
 * happens here is the reason the whole mechanism exists: an image that has been
 * mapped and not yet started can have its RCpc loads replaced in place, which
 * is cheaper by orders of magnitude than carrying out each one as it traps
 *
 * What it does with the image is replace its RCpc loads, so that the machine
 * never has to trap on one, and count what it found and what it wrote. The
 * count is the only evidence there is that it did anything: the machine cannot
 * print once the kernel is running, so it goes in the pool and the host reads
 * it there
 */
void *usDriverLoadEntry(void *imageBase) {
    UsPayloadConfig *cfg = usPayloadConfig();
    void *(*target)(void *) = (void *(*)(void *))(uintptr_t)cfg->driverLoadTarget;

    if (cfg->poolBase != 0) {
        UsPoolEntry *entry = &((UsPool *)(uintptr_t)cfg->poolBase)->entry;
        UsRcpcImage image = { 0 };

        UsPoolDriver *at;

        /*
         * Counted without an atomic. Two processors can be loading a driver at
         * the same time, so the count can be a little short, and what it is for
         * is "did anything arrive at all" rather than an exact tally
         */
        at = &entry->loadRing[entry->driverLoads % US_POOL_LOAD_SLOTS];
        entry->driverLoads++;
        entry->driverImage = (uint64_t)(uintptr_t)imageBase;
        /*
         * One record per load, so a machine that stops later can name the image
         * a faulting address was in. The scan below fills in what it did to
         * this image; a load that is never scanned keeps the zeroes
         */
        at->image = (uint64_t)(uintptr_t)imageBase;
        at->sizeOfImage = 0;
        at->sites = 0;
        at->written = 0;
        if (usRcpcDescribeImage(imageBase, &image)) {
            entry->driverSize = image.sizeOfImage;
            at->sizeOfImage = image.sizeOfImage;
            entry->driverEntry = image.entryRVA;
            if (cfg->driverRewrite != 0) {
                /* Every site in the image and not one of them: none of it has
                 * run yet, so there is no address to aim the walk at */
                UsRcpcScan scan = { 0 };
                UsRcpcScanResult found;

                /* One field at a time, never as one aggregate: see UsRcpcScan */
                scan.readable = pageReadable;
                scan.visit = rewriteSite;
                found = usRcpcScanMappedImage(&image, &scan);

                entry->driverSites += found.sites;
                entry->driverWritten += found.written;
                entry->driverSkipped += found.skipped;
                at->sites = found.sites;
                at->written = found.written;
            }
        }
    }

    if (target == NULL) {
        /*
         * Unreachable: the call is only pointed here once this has been filled
         * in, and the thunk is written before the call is diverted. Answering
         * with nothing is the one failure that is visible to the caller instead
         * of one that stops the machine where nothing can say why
         */
        return NULL;
    }
    return target(imageBase);
}
