/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Laying out the runtime pool
 *
 * Kept apart from getting the memory so that the arithmetic can be checked
 * without a firmware: the interesting part is that every stack top is aligned
 * and that the region is a whole number of pages, and both are easy to get
 * subtly wrong and hard to notice
 */

#ifndef US_POOL_H
#define US_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/layout.h"

/*
 * Fills in a pool header for memory at baseVA / basePA, with one stack per
 * `stackSlots` processor
 *
 * The two addresses are passed separately because they stop being equal once
 * the address space is rebuilt, and the payload has to know both: one to reach
 * the pool with, the other to ask the injector to map
 *
 * Returns false if either address is not page aligned, which would mean the
 * stacks are misaligned too, or if the slot count is not one the pool can
 * describe
 */
bool usPoolInitLayout(UsPool *pool, uint64_t baseVA, uint64_t basePA,
                      uint32_t stackSlots);

/*
 * Top of a CPU's stack, or 0 when the slot does not exist. The caller checks
 * for 0 rather than getting a wild pointer: a CPU without a stack is a
 * configuration the payload has to refuse, not something to run with
 */
uint64_t usPoolStackTop(const UsPool *pool, uint32_t slot);

/*
 * Records the firmware's frame buffer in the pool, and where the payload
 * mapped it
 *
 * Written by the boot, which is the side that can ask the firmware, so that
 * the geometry is in the pool whether or not anything is ever drawn on it. A
 * pa of zero is a machine with no usable frame buffer, and the record is
 * left unmarked rather than written with zeroes in it, so that "the boot
 * looked and there was nothing" and "the boot never looked" stay different
 * answers
 *
 * The payload fills in the alias afterwards, from usPoolFBSet, and it is the
 * alias being non-zero that says a mapping was made rather than asked for
 */
bool usPoolRecordFB(UsPool *pool, uint64_t pa, uint64_t bytes, uint32_t width,
                    uint32_t height, uint32_t stride, bool blueFirst);

/*
 * Where the payload mapped the frame buffer, once it has
 *
 * Zero means it did not, and that is not an error: the kernel may have taken
 * the address, the attribute index may not have been there, or the machine may
 * have no frame buffer at all. Nothing is drawn in any of those cases, and
 * nothing is reported either, since the only thing that would report it cannot
 */
void usPoolFBSet(UsPool *pool, uint64_t aliasVA, uint64_t blocks,
                 uint64_t attrIndex);

/*
 * Records why no mapping was made
 *
 * A refusal here is silent by nature -- nothing can be said at the time,
 * because the thing being built is the only channel left -- so the reason goes
 * into the record for whoever reads the pool afterwards. The reasons are the
 * US_FB_* constants in common/layout.h
 */
void usPoolFBRefuse(UsPool *pool, uint64_t reason, uint64_t detail);

/* Checks a pool header for internal consistency. Used on both sides of the
 * handover, and by the tests */
bool usPoolIsValid(const UsPool *pool);

#endif
