/*
 * Finding the configuration file.
 *
 * The document lives on whichever volume has it, so every simple file system
 * is searched in a fixed order and the first undefshim.toml wins. The volume
 * the driver was loaded from is tried first, which keeps the common case (the
 * file sits next to the driver) deterministic even when other media also
 * carry a copy.
 *
 * Nothing here is general purpose, so it stays out of core: reading files is
 * a bootPhase concern, parsing is not.
 */

#ifndef US_CONFIG_H
#define US_CONFIG_H

#include "core/cfg.h"

typedef enum UsConfigLoad_e {
    /* No file anywhere: the driver runs on its built in defaults. */
    UsConfigAbsent,
    UsConfigLoaded,
    /* A file was found but could not be used. The caller must not arm. */
    UsConfigBroken,
} UsConfigLoad;

/*
 * Locates, reads and parses the configuration. Never returns NULL config for
 * UsConfigLoaded or UsConfigAbsent; for UsConfigBroken out stays NULL and msg
 * holds the reason.
 */
UsConfigLoad usConfigLoad(UsConfig **out, char *msg, size_t msgLen);

#endif
