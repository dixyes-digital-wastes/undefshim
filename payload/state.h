/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What the exception path keeps while it runs, and how the rest of it reaches
 * that state
 *
 * This is not the external contract, which is payload.h: these are the pieces
 * the handler is split across, and each of them either belongs to one
 * processor alone or is written once for the payload as a whole
 */

#ifndef US_PAYLOAD_STATE_H
#define US_PAYLOAD_STATE_H

#include <stdint.h>

#include "common/layout.h"
#include "payload/payload.h"

/*
 * What each processor is in the middle of
 *
 * A fault taken inside an emulated access is delivered back here by the entry
 * before anything else can run on that processor, and the question it has to
 * answer is which exception it belongs to: the one being handled, standing in
 * for an instruction of the interrupted code. Neither fact can be recovered
 * afterwards - the nested frame is the handler's own, not the interrupted
 * one - so both are kept per processor, indexed the same way the stacks are
 */
typedef struct UsActive_t {
    UsFrame *frame;    /* the exception the handler was given */
    uint64_t address;  /* what the emulated access is about to read */
    uint64_t loadBytes;/* how wide that access is, so a fault can be placed */
    bool     loading;  /* whether one is in flight at all */

    /*
     * An access the handler is making that it expects may be refused
     *
     * The rewrite has to try a store that the mapping might not allow, and
     * learning that from a fault is only useful if the fault comes back here
     * instead of being reported as a fault in a handler. While this is set,
     * a fault on exactly this address is the answer "no", and the handler
     * carries on at the instruction after the one that faulted
     *
     * These are volatile because the handler writes them at a moment the
     * compiler cannot see - between the two halves of the call that armed
     * them, with an exception taken in between. Left plain, the value the
     * arming wrote is forwarded to the reading half, and a store the mapping
     * refused comes back as one that happened
     */
    volatile uint64_t probeAt;
    volatile uint64_t probeBytes;
    volatile bool     probing;
    volatile bool     probeRefused;
} UsActive;

/* One per processor the payload serves, indexed as the stacks are */
extern UsActive gActive[US_MAX_CPUS];

/*
 * This processor's affinity, which is the key the configuration's CPU list is
 * searched by
 *
 * Here rather than in one user because three of them need it: the index
 * itself, and the two records that keep an affinity beside a frame. It is one
 * register read, which is cheaper inlined than behind a call on the path an
 * exception takes
 */
static inline uint64_t currentMPIDR(void) {
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & US_MPIDR_AFFINITY_MASK;
}

/*
 * Which processor this is, as an index
 *
 * Looked up rather than taken out of the register's low byte. That byte is
 * Aff0, the core within a cluster, so the first core of every cluster has the
 * same one: two processors would share a landing pad, a stack and a frame,
 * and the damage would appear wherever the other one happened to be running
 *
 * The list comes from the firmware, through the boot. A processor that is not
 * in it has no index, and the caller has to refuse rather than pick one
 */
int currentCPU(void);

/* The generic counter, read as a clock for this processor alone. Two readings
 * bound one entry, and the difference is what that entry cost */
uint64_t usCycleCount(void);

/* One processor's trap record, or NULL when there is no pool to keep it in */
UsPoolTrapCPU *trapAccount(int cpu);

/* Says that the frame in flight is finished with, so a later fault is not
 * blamed on it. Every return from the handler goes through here */
void usPayloadLeave(int cpu);

/* The exception the handler is working on, for a report that is written while
 * it is in flight. Not used for anything else, and not the ABI: the ABI is
 * the pointer passed in */
UsFrame *currentFrame(void);
void usPayloadSetFrame(UsFrame *frame);

#endif
