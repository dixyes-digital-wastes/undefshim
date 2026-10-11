/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The state the exception path keeps, and the processor it is running on
 *
 * See state.h for the interface. The one thing here that is not per processor
 * is the frame in flight, which exists for the report a nested entry writes:
 * it is the only path that can run while the handler is on the stack, and it
 * arrives with a frame of its own
 */

#include "payload/state.h"

#include "common/layout.h"
#include "payload/payload.h"

UsActive gActive[US_MAX_CPUS];

/* Written by the handler, read by a report, and never the ABI */
static UsFrame *gCurrentFrame;

int currentCPU(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t mpidr = currentMPIDR();

    for (uint64_t i = 0; i < cfg->cpuCount && i < US_MAX_CPUS; i++) {
        if (cfg->cpus[i].mpidr == mpidr) {
            return (int)cfg->cpus[i].index;
        }
    }
    return -1;
}

uint64_t usCycleCount(void) {
    uint64_t count;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    return count;
}

UsPoolTrapCPU *trapAccount(int cpu) {
    UsPayloadConfig *cfg = usPayloadConfig();

    if (cfg->poolBase == 0 || cpu < 0 || cpu >= (int)US_MAX_CPUS) {
        return NULL;
    }
    return &((UsPool *)(uintptr_t)cfg->poolBase)->trap[cpu];
}

void usPayloadLeave(int cpu) {
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].frame = NULL;
        gActive[cpu].loading = false;
    }
}

UsFrame *currentFrame(void) {
    return gCurrentFrame;
}

void usPayloadSetFrame(UsFrame *frame) {
    gCurrentFrame = frame;
}
