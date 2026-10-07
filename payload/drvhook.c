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

/* The header of an image the kernel has already mapped, read the way a loader
 * reads it. Everything past the DOS stub is checked against the image's own
 * header, so a pointer that is not one fails rather than running off */
#define US_PE_DOS_MAGIC 0x5A4DU   /* "MZ" */
#define US_PE_MAGIC 0x00004550U   /* "PE\0\0" */
#define US_PE_OPTIONAL_MAGIC_64 0x20BU

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

/*
 * What the image says it is: how big it is and where it starts executing
 *
 * This is the report, and it is worth its few loads. An address that names a PE
 * header with a size in the megabytes is an image being loaded; anything else
 * would mean the call that was diverted is not the one that was meant, and the
 * difference between those two is what nothing else here can see
 */
typedef struct DriverImage_t {
    uint32_t sizeOfImage;
    uint32_t entryRVA;
} DriverImage;

static bool describeImage(const void *base, DriverImage *out) {
    const uint8_t *p = base;
    uint32_t lfanew;
    const uint8_t *coff;

    if (rd16(p) != US_PE_DOS_MAGIC) {
        return false;
    }
    lfanew = rd32(p + 0x3CU);
    coff = p + lfanew;
    if (rd32(coff) != US_PE_MAGIC) {
        return false;
    }
    /*
     * The optional header's magic says which form this is, and the two fields
     * wanted sit at different offsets in the two. This is a report and not a
     * loader: an image of a shape this does not know is said to be unreadable
     * rather than guessed at
     */
    if (rd16(coff + 24) != US_PE_OPTIONAL_MAGIC_64) {
        return false;
    }
    out->entryRVA = rd32(coff + 24 + 16);
    out->sizeOfImage = rd32(coff + 24 + 56);
    return out->sizeOfImage != 0;
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
 * What it does today is record what arrived and call through. That is the first
 * step rather than a placeholder for nothing: it puts the mechanism in place -
 * a thunk in the kernel's text, the kernel's own call diverted to it, an entry
 * reached and returned from - with the machine's behaviour unchanged, so the
 * part that changes behaviour can be added to something already known to be
 * sound
 *
 * The record is what says the mechanism was reached at all. Whether the hook is
 * written and whether anything comes through it are different questions, and a
 * thunk nothing reaches looks exactly like one that was never written from
 * every other angle
 */
void *usDriverLoadEntry(void *imageBase) {
    UsPayloadConfig *cfg = usPayloadConfig();
    void *(*target)(void *) = (void *(*)(void *))(uintptr_t)cfg->driverLoadTarget;

    if (cfg->poolBase != 0) {
        UsPoolEntry *entry = &((UsPool *)(uintptr_t)cfg->poolBase)->entry;
        DriverImage image = { 0 };

        /*
         * Counted without an atomic. Two processors can be loading a driver at
         * the same time, so the count can be a little short, and what it is for
         * is "did anything arrive at all" rather than an exact tally
         */
        entry->driverLoads++;
        entry->driverImage = (uint64_t)(uintptr_t)imageBase;
        if (describeImage(imageBase, &image)) {
            entry->driverSize = image.sizeOfImage;
            entry->driverEntry = image.entryRVA;
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
