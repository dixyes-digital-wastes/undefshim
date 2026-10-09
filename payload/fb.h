/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Drawing on the firmware's frame buffer, from the payload, with no firmware
 */

#ifndef US_FB_H
#define US_FB_H

#include <stdbool.h>
#include <stdint.h>

#include "core/pgtable.h"
#include "core/pixel.h"

/*
 * How many two megabyte entries a frame buffer takes
 *
 * This is the one number the mapping must not work out for itself, because
 * getting it wrong is silent in exactly the way a screen is: the frame buffer
 * is not aligned to the start of a block, so what it needs is the rest of the
 * block it starts in and then as many whole ones as its length takes. A screen
 * of 800 by 600 is 1,875 kilobytes sitting 192 kilobytes into its block, which
 * takes two -- and one is a screen whose last six rows are not mapped at all
 */
static inline uint64_t usFBBlocksFor(uint64_t fbPA, uint64_t fbBytes) {
    const uint64_t offset = fbPA & (US_GRANULE_2M - 1U);

    /* A mapping of nothing is nothing, and the caller refuses it rather than
     * taking an entry for it */
    if (fbBytes == 0) {
        return 0;
    }
    return (offset + fbBytes + US_GRANULE_2M - 1U) / US_GRANULE_2M;
}

/*
 * Records where the frame buffer is and where the kernel's descriptor mapping
 * will be, and nothing else
 *
 * Called once, at the handover, because that is when the kernel image is known
 * and no other processor is running. The mapping itself is not made here: the
 * kernel sets the base of its own descriptor mapping after this, so an address
 * computed from it now would name a table nothing reads. It is read at the
 * moment it is wanted instead, and this only keeps the two facts it is read
 * from -- the address of the variable, as an RVA into the image, and the image
 *
 * False means there is nothing to map through, which is a machine reported
 * about some other way rather than one that is broken
 */
bool usFBBuild(uint64_t kernelBase);

/*
 * The frame buffer as somewhere to draw, or false when there is nowhere
 *
 * The mapping is made on the first question and asked about on every one after
 * it, of the tables rather than of a flag remembered from before: the kernel
 * is free to take the address back, and a remembered "yes" would make the
 * first draw a fault on a path that has nothing left to recover with. A
 * mapping that is gone is forgotten rather than defended
 *
 * The question is whether the address translates to the frame buffer's own
 * physical address at the granularity it was mapped with, and the surface
 * handed back is bounded by what the firmware said the picture is. A report is
 * read off a photograph, so what it needs is to land on the screen; a caller
 * that has a surface in hand draws into it directly, and the calls below ask
 * the same question again for the caller that does not, because the pen they
 * carry is not part of it. What such a surface is drawn at is the scale set
 * above
 */
bool usFBSurface(UsSurface *out);

/*
 * Where the next character goes, counted in characters and rows rather than in
 * pixels
 *
 * The pen is state and stays between calls, because a report is written a
 * fragment at a time -- a label, then a number, then the next label -- and
 * each call carries on where the one before it stopped. This is what puts it
 * somewhere else to begin with: the face of the report is not written from the
 * top left, since the picture above it comes first
 */
void usFBPenSet(uint32_t column, uint32_t row);

/*
 * The colour the text is drawn in, as 0xRRGGBB
 *
 * The report has a heading and a value in it that a reader is looking for,
 * and the console the driver writes to already shows those two in red and in
 * magenta -- US_RED and US_VALUE in uefi/console.h. The screen is the other
 * channel of the same report, so it is read the same way: the same two facts
 * are found by the same two colours
 *
 * This is the ink and not the paper: what is drawn behind the text is white
 * and stays white, because a photograph is exposed for the whole screen
 */
void usFBInkSet(uint32_t rgb);

/*
 * How large a pixel of the drawing is on the screen
 *
 * The face decides this from the screen and says so here, because the text is
 * drawn by a call that has no face to ask: every call below reaches the frame
 * buffer through the same question, and a scale kept on one of those answers
 * rather than on all of them would be a report drawn in two sizes
 */
void usFBScaleSet(uint32_t scale);

/*
 * Drawing, which writes and nothing else: no scrolling, no cursor, no
 * attribute, and no line that depends on a previous one. The report this
 * exists for is written once and read from a photograph, so what it needs is
 * to land on the screen and not to be a console
 *
 * Every one of these is a no-op when the mapping is not available, and none of
 * them can fault: a row that would run off the buffer is not drawn
 */
void usFBText(const char *text);
void usFBHex(uint64_t value);
void usFBDec(uint64_t value);

#endif
