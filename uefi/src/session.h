/*
 * The state the driver accumulates during boot.
 *
 * One object rather than a set of file static variables, because the hooks
 * need to reach the same pool and the same registry, and because the payload
 * has to be handed the same thing later. It is the object the design document
 * calls a session: everything that is true of this boot.
 */

#ifndef US_SESSION_H
#define US_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cfg.h"
#include "uefi/src/payload_place.h"
#include "uefi/src/pool.h"
#include "uefi/src/registry.h"

typedef struct UsSession_t {
    UsRegistry   registry;
    UsPool      *pool;
    UsPoolAlloc  poolAlloc;

    /* Where the payload was put, once it has been. Placed once and kept: the
     * addresses in it may already have been handed out. */
    UsPayloadPlace payloadPlace;
    bool           payloadPlaced;

    /* Whether to take over the loader's handover to the kernel. Off by
     * default: see work.c for what still has to be true for it to work. */
    bool           armEnabled;

    /* Whether to catch the address change notification. Off by default. */
    bool           vamapEnabled;

    /*
     * The parsed configuration, owned here. Its patch table names stages that
     * are not loaded yet when the driver starts, so it has to outlive the
     * function that read it.
     */
    UsConfig    *config;
    /* One bit per applied patch, see patch.h. */
    uint32_t     patchApplied;

    /*
     * The stack our own work runs on when we have been entered on someone
     * else's. Hooks are called on the caller's stack, which we can make no
     * assumptions about, so any work of consequence is run on this one
     * instead. See stack.h.
     */
    uint64_t bootStackTop;
} UsSession;

/*
 * Brings up everything the boot needs: the pool, the stacks, and the registry.
 * Returns false when the firmware refuses the pool, which is fatal: without it
 * there is no stack and no handler.
 */
bool usSessionInit(UsSession *s);

#endif
