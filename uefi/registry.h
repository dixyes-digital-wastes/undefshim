/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The images the driver has found, indexed by role
 *
 * Only bootmgfw arrives with a protocol attached to it; winload and the
 * kernel are read off disk by the stage before them, so they have to be found
 * by looking in memory. Either way, everything downstream asks this registry
 * for "the kernel" rather than carrying a base address around, which is what
 * keeps the patch table addressable by name
 */

#ifndef US_REGISTRY_H
#define US_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "core/pe.h"

typedef struct UsRegistry_t {
    /* Views are kept here, not pointed to, because a loaded image does not
     * move once it is registered */
    UsImage bootmgfw;
    UsImage winload;
    UsImage ntoskrnl;

    bool bootmgfwPresent;
    bool winloadPresent;
    bool ntoskrnlPresent;
} UsRegistry;

void usRegistryInit(UsRegistry *reg);

/*
 * Records bootmgfw, which the firmware loaded. The view is taken over as is;
 * the caller must not modify the image it covers afterwards
 *
 * This is called from the LoadImage hook, which runs on the caller's stack and
 * therefore has none of its own to spend. The work here is bounded and shallow
 * on purpose: it takes the image header apart in place and writes into static
 * storage, so no large frame is involved. Anything that grows this function
 * has to keep that property
 */
bool usRegistryAddBootmgfw(UsRegistry *reg, const void *base, size_t size);

/*
 * Searches a memory region for images that are not announced by a protocol,
 * and records any it recognises
 *
 * Scans for PE headers rather than for a pattern: the loaders write images
 * into memory as whole images, so their headers are present and are the one
 * thing that is guaranteed to be where it says it is
 *
 * Returns the number of images added
 */
int usRegistryScanRegion(UsRegistry *reg, const void *base, size_t size);

/* The image for a role, or NULL */
UsImage *usRegistryGet(UsRegistry *reg, UsImageKind kind);

/* Looks an image up by the name used in the patch table, or NULL */
UsImage *usRegistryByName(UsRegistry *reg, const char *name);

/* Everything found so far, for logging */
int usRegistryCount(const UsRegistry *reg);

#endif
