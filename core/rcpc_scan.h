/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding the RCpc loads in an image, see rcpc_scan.c
 *
 * Two moments in this project want the same walk: an image that is being
 * placed and has not run, and an image the kernel has mapped and is about to
 * start. The two do not agree on how much of a section is really there, on
 * whether a page can be read at all, or on what should happen to a site, so
 * the walk settles none of those. It finds sites and shows each one to the
 * caller, and the caller answers for it
 *
 * Nothing here calls into the firmware, allocates, or keeps state, so the same
 * file is built into the driver and into the payload blob
 */

#ifndef US_CORE_RCPC_SCAN_H
#define US_CORE_RCPC_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "core/ldapr.h"

/*
 * One section of an image, as the caller can describe it
 *
 * The walk does not read the section table itself: where an RVA points is a
 * property of the image's view, and only the caller holds that. Asking for one
 * section at a time rather than taking an array is also what keeps the walk
 * out of a stack budget neither caller can spend
 */
typedef struct UsRcpcSection_t {
    const uint8_t *base;   /* where the contents are, NULL when they are not mapped */
    uint32_t       virtualAddress;
    uint32_t       virtualSize;
    uint32_t       rawSize;   /* bytes the image carries; the rest is zero fill */
    uint32_t       characteristics;
} UsRcpcSection;

/* Fills one section, indexed the way the image orders them. False ends the
 * walk: a caller that cannot describe a section cannot be asked for the one
 * after it either */
typedef bool (*UsRcpcSectionAt)(void *context, uint16_t index, UsRcpcSection *out);

/* One RCpc load, as the walk found it. The encoding is carried rather than
 * left in memory: a caller that writes the substitute already has what it
 * needs, and a caller that only counts paid nothing for it */
typedef struct UsRcpcSite_t {
    uint64_t    at;
    uint32_t    insn;
    UsLDAPRKind kind;
} UsRcpcSite;

/* What the caller does with a site. True means it was replaced: those are
 * counted, and a section that had one is published */
typedef bool (*UsRcpcVisit)(const UsRcpcSite *site, void *context);

/*
 * Whether a page holds anything that can be read
 *
 * This is not the same question as whether the page is mapped. An image that
 * has been mapped and not run has pages that are brought in as they are
 * touched, and reading one of the others is a fault; the walk answers a
 * refusal by leaving the whole page alone, which is slower and correct
 */
typedef bool (*UsRcpcPageReadable)(const uint8_t *page, void *context);

/*
 * Called once for a section the visitor wrote into, with the bytes the walk
 * covered
 *
 * A section is published whole rather than per instruction: the boot's case is
 * a large image, and a barrier and an invalidate per instruction would spend
 * all of its time on those
 */
typedef void (*UsRcpcSpanDone)(const UsRcpcSection *section, uint32_t span, void *context);

typedef enum UsRcpcMode_e {
    UsRcpcRewrite = 0,   /* show every site found to the visitor */
    UsRcpcReport,        /* count the sites and show none of them */
} UsRcpcMode;

/*
 * What one walk does, and what it does with what it finds
 *
 * A caller fills this in on its own stack, one field at a time, and never as
 * one aggregate: a field that names a function holds an address, and an
 * aggregate initializer that names one is emitted as a constant copy in the
 * image - which in the blob carries a link time address, the one thing a blob
 * copied to an address the linker never knew cannot have (see
 * tests/tools/blobcheck.c)
 */
typedef struct UsRcpcScan_t {
    UsRcpcMode mode;
    /*
     * Only this address, or zero for every site. The handler that trapped on
     * one load knows its address and wants no other; a load hook has an image
     * and no addresses yet
     */
    uint64_t target;
    UsRcpcVisit    visit;
    void          *visitContext;
    UsRcpcSpanDone spanDone;
    void          *spanDoneContext;
    /* Optional here and required by the runtime layer: a caller that reads
     * pages it cannot ask about takes the faults the probe exists to avoid */
    UsRcpcPageReadable readable;
    void              *readableContext;
} UsRcpcScan;

typedef struct UsRcpcScanResult_t {
    uint64_t sites;     /* RCpc loads found, whether or not they were shown */
    uint64_t offered;   /* sites the visitor was shown */
    uint64_t written;   /* sites the visitor said it replaced */
    uint64_t skipped;   /* pages the reader refused */
} UsRcpcScanResult;

/*
 * What an image says it is, read the way a loader reads it
 *
 * Wanted by the load hook for its record as much as by the walk: an address
 * that names a header with a size in the megabytes is an image being loaded,
 * and an address that does not is a call that was diverted by mistake
 */
typedef struct UsRcpcImage_t {
    const uint8_t *base;
    uint32_t       sizeOfImage;
    uint32_t       entryRVA;
    uint32_t       sectionTableOffset;   /* into the image, not an RVA */
    uint16_t       sectionCount;
} UsRcpcImage;

/* Reads the header of an image the kernel has already mapped. False when what
 * is there is not a 64 bit PE image, which leaves nothing to walk */
bool usRcpcDescribeImage(const void *base, UsRcpcImage *out);

/*
 * The walk itself: every executable section the caller describes, one word at
 * a time from the start of the section
 *
 * imageLimit keeps a site inside the image's own size, which is what an image
 * the kernel has mapped has to be read within; zero leaves each section to its
 * own extent. rawBound stops a section where the bytes the image carries end,
 * which is what an image being placed has to be read within
 */
UsRcpcScanResult usRcpcScanSections(uint16_t sectionCount, UsRcpcSectionAt at, void *context,
                                    uint32_t imageLimit, bool rawBound, const UsRcpcScan *scan);

/*
 * An image that is being placed: the sections are the caller's, each one is
 * read as far as the bytes it carries reach, and there is nothing to ask about
 * a page because all of it is there
 */
UsRcpcScanResult usRcpcScanImage(uint16_t sectionCount, UsRcpcSectionAt at, void *context,
                                 const UsRcpcScan *scan);

/*
 * An image the kernel has mapped: the section table is read out of the image
 * itself, no site is taken from past the image's own size, and no page is read
 * that the reader has not answered for
 *
 * A caller with no reader gets nothing. At this point an unread page is a
 * fault, and there is no answer for one from in here
 */
UsRcpcScanResult usRcpcScanMappedImage(const UsRcpcImage *image, const UsRcpcScan *scan);

#endif
