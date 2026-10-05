/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Intercepting the firmware's image loader
 *
 * bootmgfw is the only stage this driver gets told about: the firmware calls
 * LoadImage to read it off disk, and everything after that is loaded by
 * bootmgfw itself, out of sight. So this is where the driver sees its first
 * image, and where it has to be before anything else can happen
 *
 * The hook replaces one entry in the boot services table. That table is
 * shared with the firmware, so the original is kept and called through, and
 * the entry is put back before returning to the caller
 */

#ifndef US_LOADIMAGE_HOOK_H
#define US_LOADIMAGE_HOOK_H

#include <stdbool.h>

#include "uefi/session.h"

/*
 * Installs the hook. Returns false when the table entry could not be written,
 * which is a reason to stop: without it the driver never learns about
 * bootmgfw
 */
bool usLoadImageHookInstall(UsSession *session);

#endif
