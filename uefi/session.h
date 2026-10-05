/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The state the driver accumulates during boot
 *
 * One object rather than a set of file static variables, because the hooks
 * need to reach the same pool and the same registry, and because the payload
 * has to be handed the same thing later. It is the object the design document
 * calls a session: everything that is true of this boot
 */

#ifndef US_SESSION_H
#define US_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "core/acpi.h"
#include "core/cfg.h"
#include "uefi/payload_place.h"
#include "uefi/pool.h"
#include "uefi/registry.h"

typedef struct UsSession_t {
    UsRegistry   registry;
    UsPool      *pool;
    UsPoolAlloc  poolAlloc;

    /*
     * The machine's processors, read once at boot. Everything kept per CPU is
     * indexed by a position in this list, because the obvious index -- the low
     * byte of MPIDR_EL1 -- is the same for the first core of every cluster
     */
    UsACPICPUs   cpus;

    /* Where the payload was put, once it has been. Placed once and kept: the
     * addresses in it may already have been handed out */
    UsPayloadPlace payloadPlace;
    bool           payloadPlaced;

    /*
     * Whether to take over the branch winload jumps into the kernel with.
     * This is what tells every other stub where the payload ended up, so it
     * is the one piece that has to happen first
     */
    bool           stubHandover;

    /*
     * Whether to take the synchronous slots of the two vector tables. The
     * loader's is in force until the kernel installs its own, and the
     * kernel's after that; a load the rewrite did not reach arrives here
     */
    bool           stubVectors;

    /* Whether to catch the address change notification */
    bool           vamap;

    /*
     * Whether to take over the EL1t synchronous slot as well as the EL1h one
     *
     * The two are not interchangeable and are not equally safe to write. The
     * EL1h slot holds a branch, so its own behaviour is two instructions that
     * the stub can replay. The EL1t slot holds a handler written out in place,
     * and that handler's first instructions clobber registers the stub also
     * needs -- so taking it over is a different proposition and is kept
     * separate rather than assumed
     */
    bool           stubVectorsEl1t;

    /*
     * Whether to treat the SPx vector's stack as one the stub may push on
     *
     * It is a question, not a preference, and it is kept switchable because
     * two recorded observations disagree: a probe logged that pushing there
     * faults until the machine resets, and a frame read later shows SP holding
     * an ordinary kernel stack. One run settles it
     */
    bool           spxStack;

    /*
     * Whether to replace the RCpc loads in the images rather than only
     * handling the exceptions they cause. On by default: it is the mechanism
     * that covers the kernel, and the exception path covers what it cannot
     */
    bool           imageInplaceRewrite;

    /*
     * The parsed configuration, owned here. Its patch table names stages that
     * are not loaded yet when the driver starts, so it has to outlive the
     * function that read it
     */
    UsConfig    *config;
    /* One bit per applied patch, see patch.h */
    uint32_t     patchApplied;

    /*
     * The stack our own work runs on when we have been entered on someone
     * else's. Hooks are called on the caller's stack, which we can make no
     * assumptions about, so any work of consequence is run on this one
     * instead. See stack.h
     */
    uint64_t bootStackTop;
} UsSession;

/*
 * Brings up everything the boot needs: the pool, the stacks, and the registry.
 * Returns false when the firmware refuses the pool, which is fatal: without it
 * there is no stack and no handler
 */
bool usSessionInit(UsSession *s);

#endif
