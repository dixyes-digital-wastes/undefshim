/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding the configuration file
 *
 * The document lives on whichever volume has it, so every simple file system
 * is searched for by name on every volume, and us.toml is the name. The volume
 * the driver was loaded from is tried first, which keeps the common case (the
 * file sits next to the driver) deterministic even when other media also
 * carry a copy
 *
 * Nothing here is general purpose, so it stays out of core: reading files is
 * a bootPhase concern, parsing is not
 */

#ifndef US_CONFIG_H
#define US_CONFIG_H

#include <uefi.h>

#include "core/cfg.h"

typedef enum UsConfigLoad_e {
    /* No file anywhere: the driver runs on its built in defaults */
    UsConfigAbsent,
    UsConfigLoaded,
    /* A file was found but could not be used. The caller must not arm */
    UsConfigBroken,
} UsConfigLoad;

/*
 * Locates, reads and parses the configuration. Never returns NULL config for
 * UsConfigLoaded or UsConfigAbsent; for UsConfigBroken out stays NULL and msg
 * holds the reason
 */
UsConfigLoad usConfigLoad(UsConfig **out, char *msg, size_t msgLen);

/* The volume the configuration came from, or NULL when there was none. A path
 * written in the configuration is relative to the volume it was read from */
efi_handle_t usConfigVolume(void);

#endif
