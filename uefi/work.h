/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The point at which the boot has everything it needs
 *
 * The images all arrive at different times: the firmware loads the first, and
 * each stage reads the next one off disk. By the time the kernel is in memory
 * everything the plan is built from exists, and this is where that is noticed
 *
 * Work that will write to memory goes here when it is written. For now this
 * collects the plan and prints it, which is deliberate: the plan is worth
 * being able to read before anything acts on it, and the driver's dump is
 * compared against the one the host tool produces from the same images
 */

#ifndef US_WORK_H
#define US_WORK_H

#include "uefi/session.h"

/*
 * Builds the plan from the registry and prints it. Safe to call more than
 * once; later calls recompute rather than accumulate
 */
void usWorkCollect(UsSession *session);

#endif
