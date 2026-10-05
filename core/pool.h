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
 * Fills in a pool header for memory at baseVA / basePA
 *
 * The two are passed separately because they stop being equal once the
 * address space is rebuilt, and the payload has to know both: one to reach the
 * pool with, the other to ask the injector to map
 *
 * Returns false if either address is not page aligned, which would mean the
 * stacks are misaligned too
 */
bool usPoolInitLayout(UsPool *pool, uint64_t baseVA, uint64_t basePA);

/*
 * Top of a CPU's stack, or 0 when the slot does not exist. The caller checks
 * for 0 rather than getting a wild pointer: a CPU without a stack is a
 * configuration the payload has to refuse, not something to run with
 */
uint64_t usPoolStackTop(const UsPool *pool, uint32_t slot);

/* Checks a pool header for internal consistency. Used on both sides of the
 * handover, and by the tests */
bool usPoolIsValid(const UsPool *pool);

#endif
