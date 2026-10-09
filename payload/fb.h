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
 * Whether the mapping is there and still points at the frame buffer
 *
 * The mapping is made on the first question and asked about on every one after
 * it, of the tables rather than of a flag remembered from before: the kernel
 * is free to take the address back, and a remembered "yes" would make the
 * first draw a fault on a path that has nothing left to recover with. A
 * mapping that is gone is forgotten rather than defended
 *
 * The question is whether the address translates to the frame buffer's own
 * physical address at the granularity it was mapped with
 */
bool usFBAvailable(void);

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
