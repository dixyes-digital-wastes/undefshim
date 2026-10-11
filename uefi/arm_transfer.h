/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The handover stub: the branch the loader enters the kernel with, and the
 * bytes that receive it
 */

#ifndef US_UEFI_ARM_TRANSFER_H
#define US_UEFI_ARM_TRANSFER_H

#include <stdbool.h>

#include "uefi/session.h"

/*
 * Redirects the loader's branch into the kernel to a stub that enters the
 * payload first
 *
 * Returns false when the loader has no such branch, or no room for the stub
 */
bool usArmStubHandover(UsSession *session);

#endif
