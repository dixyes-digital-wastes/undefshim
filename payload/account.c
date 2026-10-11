/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The pool records an exception leaves behind
 *
 * See account.h for the interface. There are three of them and they answer
 * three different questions: what each processor traps and what that cost,
 * which addresses are still trapping, and what the registers were at the
 * moment an exception was handed on
 */

#include "payload/account.h"

#include "common/layout.h"
#include "core/pan.h"
#include "payload/payload.h"
#include "payload/regs.h"
#include "payload/state.h"

void usTrapAccountEnter(UsPoolTrapCPU *acc, UsFrame *frame, uint64_t cycle0) {
    uint64_t at;

    if (acc == NULL || frame == NULL) {
        return;
    }
    if (acc->entries == 0) {
        uint64_t sctlr;

        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        acc->firstCycle = cycle0;
        acc->sctlrEl1 = sctlr;
    }
    acc->entries++;
    switch (usSlotOfSPSR(frame->spsr)) {
    case UsStubSlotEL0:
        acc->el0++;
        break;
    case UsStubSlotEL1h:
        acc->el1h++;
        break;
    default:
        acc->el1t++;
        break;
    }
    /* A kernel context with PAN set is a state nothing in the kernel asks
     * for, so the first one is worth keeping whole */
    if (usSlotOfSPSR(frame->spsr) != UsStubSlotEL0
        && (frame->spsr & US_SPSR_PAN) != 0) {
        if (acc->kernelPan == 0) {
            acc->kernelPanELR = frame->elr;
            acc->kernelPanSPSR = frame->spsr;
        }
        acc->kernelPan++;
    }
    /* The address decides the slot, so a loop that traps on the same few
     * instructions overwrites nothing else and its counts climb */
    at = (frame->elr >> 2) & (US_POOL_HOT_SLOTS - 1U);
    if (acc->hotAddress[at] == frame->elr) {
        acc->hotCount[at]++;
    } else {
        acc->hotAddress[at] = frame->elr;
        acc->hotCount[at] = 1;
    }
}

void usTrapAccountDone(UsPoolTrapCPU *acc, uint64_t cycle0) {
    uint64_t now;
    uint64_t cost;

    if (acc == NULL) {
        return;
    }
    now = usCycleCount();
    cost = now - cycle0;
    acc->busyCycles += cost;
    if (cost > acc->busyMax) {
        acc->busyMax = cost;
    }
    acc->lastCycle = now;
}

/*
 * The frame on the way out, at the same index the way in used
 *
 * Called from every path that has a frame, including the ones that claim
 * nothing, because those hand the registers back too -- to the kernel's own
 * handler rather than to the code that was interrupted, but from the same
 * copy of them
 */
void usRecordHandback(UsFrame *frame) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPoolEntry *e;
    UsPoolTrace *t;
    const uint64_t *src;
    uint64_t at;

    if (cfg->poolBase == 0 || frame == NULL) {
        return;
    }
    e = &((UsPool *)(uintptr_t)cfg->poolBase)->entry;
    at = e->entries - 1;
    t = &e->handback[at % US_POOL_TRACE_SLOTS];

    src = (const uint64_t *)(const void *)frame;
    t->cpu = (uint64_t)(int64_t)currentCPU();
    t->mpidr = currentMPIDR();
    for (uint32_t i = 0; i < US_POOL_TRACE_WORDS; i++) {
        t->words[i] = src[i];
    }
}

/*
 * One record per trapping address, so that which sites are still costing an
 * exception can be read out afterwards. It is deliberately dumb: look the
 * address up, count it, and take the first free slot when it is new. A probe
 * would be better, but the trap path is not the place for one
 */
void usStatsRecord(uint64_t va) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;

    if (cfg->poolBase == 0) {
        return;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    if (pool->stats.magic != US_POOL_STATS_MAGIC) {
        if (pool->stats.magic != 0) {
            return;
        }
        pool->stats.magic = US_POOL_STATS_MAGIC;
    }
    pool->stats.offered++;
    for (uint32_t i = 0; i < US_STATS_SLOTS; i++) {
        if (pool->stats.slots[i].va == va) {
            pool->stats.slots[i].count++;
            pool->stats.recorded++;
            return;
        }
        if (pool->stats.slots[i].va == 0) {
            pool->stats.slots[i].va = va;
            pool->stats.slots[i].count = 1;
            pool->stats.recorded++;
            return;
        }
    }
    pool->stats.overflows++;
}
