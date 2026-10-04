/*
 * The work order
 *
 * Everything the boot intends to do is collected into one object before any
 * of it is done. The point is not tidiness: an edit made while it is being
 * decided is an edit nobody can look at, and the failure then appears as a
 * fault somewhere unrelated with nothing to compare against. A plan can be
 * printed, compared between the host and the target, and refused before
 * anything has been written
 *
 * Building a plan never writes to an image. That is what makes it usable on
 * both sides: the same code runs over a file on the host and over the loaded
 * image in firmware, and the two dumps are expected to agree. The images
 * differ in every way except their RVAs, which is exactly why RVAs are what
 * the plan is expressed in
 */

#ifndef US_PLAN_H
#define US_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/layout.h"
#include "core/pe.h"
#include "core/scan.h"
#include "core/thunk.h"

/*
 * Where a plan's text goes
 *
 * Output goes through this rather than through printf because the target side
 * has no formatter and must not acquire one: the driver is entered on stacks
 * it does not own, and the formatted output path wants tens of kilobytes of
 * them. Three primitives are all the plan needs, and both sides can supply
 * them cheaply
 */
typedef struct UsSink_t {
    void *ctx;
    void (*puts)(void *ctx, const char *s);
    void (*hex)(void *ctx, uint64_t value);
    void (*dec)(void *ctx, uint64_t value);
} UsSink;

typedef struct UsPlan_t {
    UsSiteList    sites;
    UsLDAPRCounts ldapr;

    /* Images the plan covers, for the dump to say what it was built from */
    size_t   imageCount;
    uint32_t ntoskrnlSizeOfImage;
    uint32_t winloadSizeOfImage;

    /*
     * Where the kernel image will carry the thunk, which is the one place a
     * vector table slot can reach: the slot's branch cannot leave its own
     * image, and the payload is not in it. See core/thunk.h
     */
    UsSpareSlot thunk;

    /*
     * The vector tables the loader installs, and whether the synchronous slot
     * of each is still free. The loader runs before the kernel does, and it is
     * the loader's table that is in force when the kernel first executes an
     * instruction this hardware does not have -- which is before the kernel
     * has installed a table of its own
     */
    UsVBARTables vbar;

    /* What the payload will need. Constant today, but it is a requirement of
     * the plan rather than a fact about the allocator, so it is stated here */
    uint64_t poolBytes;
    uint32_t poolPages;

    /*
     * True when every site the boot cannot do without was found exactly once.
     * A plan that is not complete is still worth printing, and is not worth
     * acting on
     */
    bool complete;
} UsPlan;

void usPlanInit(UsPlan *plan);

/*
 * Builds the plan from the images that are in memory. Either may be NULL, in
 * which case the plan says so and is not complete
 *
 * Returns plan->complete
 */
bool usPlanBuild(UsPlan *plan, UsImage *winload, UsImage *ntoskrnl);

/*
 * Writes the plan as text. The format is fixed and the order is sorted, so
 * that two plans over the same image compare equal whatever order the images
 * were examined in
 */
void usPlanEmit(const UsPlan *plan, const UsSink *sink);

#endif
