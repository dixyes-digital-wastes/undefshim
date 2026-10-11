/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Vector arming: the slots of the tables an image installs, and the stubs that
 * take them over
 */

#ifndef US_UEFI_ARM_VECTORS_H
#define US_UEFI_ARM_VECTORS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/pe.h"
#include "uefi/session.h"

/*
 * Takes over the slots of every vector table the image installs, and adds them
 * to *armed
 *
 * Which slots are taken depends on the session: the asynchronous three and the
 * lower EL entry only when they were asked for, and always the synchronous one
 * the kernel's own exceptions arrive at
 *
 * Returns false when the image has no table, which is a refusal rather than an
 * empty arming
 */
bool usArmVectorTables(UsSession *session, UsImageKind kind, size_t *armed);

/*
 * Records the kernel's driver load call and where its thunk will go, for the
 * payload to act on at the handover
 *
 * Returns false when there is no call to take over or nowhere to put the thunk,
 * which costs the speed of covering drivers and not the boot
 */
bool usArmDriverHookRecord(UsSession *session);

#endif
