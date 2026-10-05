/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The per boot state, see session.h
 */

#include "core/pool.h"
#include "uefi/acpi.h"
#include "uefi/session.h"

bool usSessionInit(UsSession *s) {
    usRegistryInit(&s->registry);
    s->config = NULL;
    s->patchApplied = 0;
    s->payloadPlaced = false;
    /*
     * What a document that says nothing gets: the whole mechanism. The driver
     * sets every one of these from the configuration file afterwards, and the
     * [debug] switches are what take a piece of it out
     */
    s->stubHandover = true;
    s->stubVectors = true;
    s->stubVectorsEl1t = true;
    s->vamap = true;
    s->spxStack = false;
    s->imageInplaceRewrite = true;

    /* Until the console is opened there is nowhere to say anything, which is
     * also what a machine that never finds a port stays at */
    s->uartOpen = false;
    s->uartKind = UsUARTOff;
    s->uartBase = 0;
    s->uartWidth = 32;

    /* Read once: the tables are the boot's and are gone with it */
    s->cpus = usACPIProbeCPUs();
    if (s->cpus.count == 0) {
        uint64_t mpidr;

        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        s->cpus.mpidr[0] = mpidr & US_MPIDR_AFFINITY_MASK;
        s->cpus.count = 1;
    }

    if (!usPoolAllocate(&s->poolAlloc)) {
        return false;
    }
    s->pool = s->poolAlloc.pool;

    /*
     * Slot zero is the boot CPU. The pool has a stack per possible CPU and the
     * payload maps them by MPIDR when it takes over; at this point only one
     * CPU is running, and it is the one whose stack this is
     */
    s->bootStackTop = usPoolStackTop(s->pool, 0);
    return s->bootStackTop != 0;
}
