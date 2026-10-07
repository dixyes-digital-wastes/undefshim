/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The entry a diverted kernel call arrives at, see drvhook.c

 */

#ifndef US_DRVHOOK_H
#define US_DRVHOOK_H

/*
 * The signature is the one of the function the kernel meant to call: the
 * address of an image in, a pointer to its headers back. Nothing passes
 * through it that the kernel can see - whatever the hook does happens before
 * the call, and the answer it returns is the answer the call returned
 */
void *usDriverLoadEntry(void *imageBase);

#endif
