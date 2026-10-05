/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What the address change notification found
 *
 * The notification runs while both address spaces are still meaningful, and
 * nothing about it can be printed: it fires during a firmware call whose
 * address space is about to be replaced, and whether the serial port is
 * reachable at that moment is not something to find out by writing to it
 *
 * So the result goes into memory that is guaranteed to outlive the change,
 * and is read afterwards by something that can report. That memory is this
 * blob: it is in a class the OS keeps, and its physical address is known, so
 * the record can be read from a dump even when no address in the machine
 * resolves any more
 *
 * The same reasoning applies to the functions below, and it is the reason
 * this file exists at all rather than being a header beside the driver's
 * event registration: the driver's own pages are boot services memory and are
 * released before the notification runs, so a notification handler living
 * there is never entered. It was tried, and it is why this was thought to be
 * a firmware problem for a while
 */

#ifndef US_VAMAP_H
#define US_VAMAP_H

#include <stdint.h>

typedef struct UsVAMapRecord_t {
    uint64_t magic;
    uint64_t fired;          /* the notification was called */
    uint64_t poolBefore;
    uint64_t poolAfter;
    uint64_t payloadBefore;
    uint64_t payloadAfter;
    uint64_t poolStatus;
    uint64_t payloadStatus;

    /*
     * The hook, deployed over the firmware's SetVirtualAddressMap. It is
     * there to answer whether the loader asks the firmware to move its
     * runtime memory at all: without that call there is no notification
     * either, and no way to be told the address afterwards
     */
    uint64_t hookFired;
    uint64_t hookMapSize;
    uint64_t hookDescs;
    uint64_t hookStatus;
    uint64_t svmOriginal;    /* the function the hook forwards to */
    uint64_t rt;             /* runtime services table, for the record */
    uint64_t convertPointer; /* the driver's handle on the translation */
} UsVAMapRecord;

/* "USVAMAP" - the address change record */
#define US_VAMAP_MAGIC 0x0050414D41565355ULL

extern UsVAMapRecord usVAMapRecord;

/*
 * Built for the firmware's own signature and called after the boot services
 * are gone, so it lives in this blob rather than in the driver
 */
uint64_t usVAMapHook(uint64_t mapSize, uint64_t descSize, uint32_t descVersion,
                     void *descs);

/*
 * The firmware calls this one in the middle of the switch, which is the only
 * moment at which both address spaces are still meaningful
 */
void usVAMapNotify(void *event, void *context);

#endif
