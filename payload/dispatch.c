/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What an exception is answered with, and what is left for the kernel
 *
 * See dispatch.h and payload.h for the interfaces. Nothing here decides
 * whether an exception is ours: that answer is emulate.c's, and a probe's
 * refusal is probe.c's. This file is what follows from either, and the one
 * thing it works out for itself is where the kernel's own handler was
 */

#include "payload/dispatch.h"

#include "common/layout.h"
#include "core/ldapr.h"
#include "core/pan.h"
#include "payload/account.h"
#include "payload/early.h"
#include "payload/emulate.h"
#include "payload/panic.h"
#include "payload/payload.h"
#include "payload/probe.h"
#include "payload/regs.h"
#include "payload/state.h"
#include "payload/uart.h"

/*
 * Where the kernel's own handler for this exception is, and whether there is
 * one
 *
 * The slot the exception came through decides, and the three lookups are the
 * stub the boot left at the end of that slot's chain, the answer a previous
 * exception cached, and the last one standing. Which of them answers is the
 * same question the entry's own landing is built from, so it is asked the
 * same way here
 */
static bool landingFor(UsFrame *frame, uint64_t vbar) {
    return usPayloadSlotTail(vbar, frame->spsr, &frame->landing)
        || usPayloadSlotTailCached(vbar, frame->spsr, &frame->landing)
        || usPayloadSlotTailLast(frame->spsr, &frame->landing);
}

int usPayloadHandle(UsFrame *frame) {
    uint64_t cycle0 = usCycleCount();
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPoolTrapCPU *acc;
    uint64_t vbar;
    uint32_t ec;
    int cpu;

    if (cfg->statsEnabled != 0 && frame != NULL) {
        usStatsRecord(frame->elr);
    }
    usPayloadSetFrame(frame);
    cpu = currentCPU();
    acc = trapAccount(cpu);
    usTrapAccountEnter(acc, frame, cycle0);

    /*
     * The exception in flight, recorded before anything that can fault: a
     * fault here is delivered to the entry, which has to tell it from the
     * handler's own and knows nothing about this frame otherwise
     */
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].frame = frame;
        gActive[cpu].loading = false;
    }

    /*
     * The trace goes down first, and before anything that can fault. It is
     * the only evidence this code ran: the console is unreachable from here,
     * and a fault in the payload would otherwise leave nothing behind but a
     * machine that stopped
     */
    if (cfg->poolBase != 0) {
        UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
        uint64_t at = pool->entry.entries;

        pool->entry.magic = US_POOL_ENTRY_MAGIC;
        pool->entry.entries++;
        pool->entry.lastCPU = (uint64_t)(int64_t)cpu;
        pool->entry.lastSP = frame != NULL ? frame->sp : 0;
        if (frame != NULL) {
            pool->entry.lastESR = frame->esr;
            pool->entry.lastELR = frame->elr;
            pool->entry.lastFAR = frame->far;
        }

        /* Kept apart from the summary so that two exceptions arriving at once
         * read as two events rather than as one that cannot have happened
         *
         * The slot is the entry count modulo the ring, so what is kept is the
         * most recent arrivals. Which slot holds the newest is worked out
         * from the count, so the order survives the wrap */
        if (frame != NULL) {
            UsPoolTrace *t = &pool->entry.trace[at % US_POOL_TRACE_SLOTS];
            const uint64_t *src = (const uint64_t *)(const void *)frame;

            t->cpu = (uint64_t)(int64_t)cpu;
            t->mpidr = currentMPIDR();
            for (uint32_t i = 0; i < US_POOL_TRACE_WORDS; i++) {
                t->words[i] = src[i];
            }
        }
    }

    /*
     * The console is only usable before the kernel has its own page tables.
     * Printing from here would fault on the write and the fault would not be
     * survivable, so the quiet flag is obeyed rather than discovered
     */
    if (cfg->quiet == 0) {
        usUARTInit(cfg->uartBase, (uint32_t)cfg->uartKind, (uint32_t)cfg->uartWidth);
    }

    /*
     * The injected stop, once the trace is down and the port is set up
     *
     * After the trace and not before: what it exists for is showing that a
     * report can be made from a machine in this state, and stopping before the
     * record of what arrived would leave the two indistinguishable. After the
     * port rather than before it, because the report goes to both channels,
     * and a stop taken earlier is one that only reaches the screen
     */
    usPanicCountTrap();

    if (frame == NULL) {
        if (cfg->quiet == 0) {
            usUARTPuts("payload: no-frame\n");
        }
        usTrapAccountDone(acc, cycle0);
        return 0;
    }

    vbar = currentVBAR();
    usPayloadPublish(vbar);
    if (cfg->poolBase != 0) {
        ((UsPool *)(uintptr_t)cfg->poolBase)->entry.lastInsn =
            *(const volatile uint32_t *)(uintptr_t)frame->elr;
    }

    ec = (uint32_t)US_ESR_EC(frame->esr);
    if (cfg->quiet == 0) {
        usUARTPuts("payload: ec=");
        usUARTPutHex(ec);
        usUARTPuts(" elr=");
        usUARTPutHex(frame->elr);
        usUARTPuts(" far=");
        usUARTPutHex(frame->far);
        usUARTPuts(" cpu=");
        usUARTPutDec((uint64_t)(int64_t)cpu);
        usUARTPuts("\n");
    }

    if (ec == US_EC_UNKNOWN && usPayloadEmulate(frame, cpu)) {
        if (cfg->poolBase != 0) {
            ((UsPool *)(uintptr_t)cfg->poolBase)->entry.handled++;
        }
        if (cfg->quiet == 0) {
            usUARTPuts("payload: emulated\n");
        }
        /* Claimed: the entry resumes at the instruction after this one */
        if (acc != NULL) {
            acc->claimed++;
        }
        usPayloadLeave(cpu);
        usRecordHandback(frame);
        usTrapAccountDone(acc, cycle0);
        return 1;
    }

    /*
     * Everything else is not ours to carry out, and the honest answer is the
     * one that leaves the kernel to deal with it exactly as it would have.
     * That is not a halt: the entry can hand the frame to the handler the
     * slot originally held, with the registers and the exception state as
     * they were, which is indistinguishable from the exception never having
     * been taken by us. The addresses are the only thing that has to be
     * worked out, and the SPSR says which slot it was
     */
    if (cfg->quiet == 0) {
        usUARTPuts("payload: not-ours\n");
    }
    if (landingFor(frame, vbar)) {
        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;

            pool->entry.handedBack++;
            pool->entry.handbackESR = frame->esr;
            pool->entry.handbackELR = frame->elr;
        }
        if (acc != NULL) {
            acc->handedBack++;
        }
        usPANEnterHandler(frame->spsr);
        usPayloadLeave(cpu);
        usRecordHandback(frame);
        usTrapAccountDone(acc, cycle0);
        return 2;
    }
    usPayloadLeave(cpu);
    usRecordHandback(frame);
    usTrapAccountDone(acc, cycle0);
    return 0;
}

/*
 * The entry on the CPU's own stack, entered when the payload itself faults
 *
 * The frame of the exception being handled is what the kernel has to be
 * given, because the emulated access stands in for one instruction of the
 * interrupted code: a fault on that access is that instruction's fault, and
 * the kernel's page fault handler is what resolves it and retries it
 *
 * What comes back here is the nested fault's own account, which is already
 * the right exception class and status - nothing has to be made up - and the
 * entry it came from, which says which slot's handler is to finish it
 *
 * The answer is the frame to restore; the entry branches to its landing.
 * NULL means there is nothing to restore and the entry stops
 */
UsFrame *usPayloadFault(void) {
    int cpu = currentCPU();
    UsFrame *outer;
    uint64_t vbar;
    uint64_t far;

    /*
     * PAN is not touched here, though a handler's own entry to EL1 leaves it
     * set. Two of the answers below return into the payload rather than to the
     * kernel - a probe's answer and a fault with no interrupted frame - and
     * what the payload is in the middle of is work that reads and writes
     * addresses whose mapping is not known, which it holds PAN clear for. The
     * hand-back is the one answer that needs it set, and by then it is not
     * this function's caller but the entry's landing that decides, so the
     * entry sets it there
     */
    if (cpu < 0 || cpu >= (int)US_MAX_CPUS) {
        return NULL;
    }
    far = currentFAR();

    /*
     * An access the handler asked to be told about. ELR still names the
     * instruction that faulted, so moving past it is the whole recovery: the
     * entry puts the handler's own registers back and returns to it. What was
     * refused is recorded, and the caller reads that rather than the fault
     *
     * Asked before the interrupted frame is looked for, because the answer is
     * the same on the path where there is no frame at all, and it is the one
     * answer that needs nothing else
     */
    if (usPayloadProbeAnswered(cpu, far)) {
        UsPayloadConfig *cfg = usPayloadConfig();

        if (cfg->poolBase != 0) {
            UsPoolEntry *entry = &((UsPool *)(uintptr_t)cfg->poolBase)->entry;
            uint64_t seen = usReadX18();
            uint64_t expected = currentTPIDR() & ~0xFFFULL;

            /* What the entry is about to give back, against the value the
             * kernel keeps there. The answer returns into the payload, whose
             * own code does not read that register, so a difference here stays
             * invisible until the payload calls back into the kernel */
            entry->probeAnswers++;
            if (seen != expected) {
                if (entry->probeX18Wrong == 0) {
                    entry->probeX18Seen = seen;
                    entry->probeX18Expected = expected;
                }
                entry->probeX18Wrong++;
            }
        }
        usSetELR(currentELR() + 4);
        return US_PAYLOAD_RESUME;
    }

    outer = gActive[cpu].frame;
    if (outer == NULL) {
        /*
         * No interrupted context to answer for. This fault was taken while the
         * payload was doing its own work, entered as a call rather than as an
         * exception - a driver's image being rewritten as the loader loads it.
         * There is no frame to hand to the kernel and no instruction of the
         * kernel's to blame, so the access is refused and the work goes on
         * without it. The accesses that can fault there are the translation
         * queries and the probes: `AT` leaves PAR_EL1 naming the failure, so
         * the query reads its own answer, and a probe reads its flag
         *
         * It is recorded all the same: a fault we took is worth knowing about
         * whether or not it is survivable
         */
        UsPayloadConfig *cfg = usPayloadConfig();
        UsPool *pool = cfg->poolBase != 0 ? (UsPool *)(uintptr_t)cfg->poolBase : NULL;

        if (pool != NULL) {
            pool->entry.nestedFaults++;
            pool->entry.nestedESR = currentESR();
            pool->entry.nestedFAR = far;
            pool->entry.nestedELR = currentELR();
            pool->entry.nestedSelfVA = cfg->selfVA;
            pool->entry.nestedPoolBase = cfg->poolBase;
            pool->entry.nestedStackTop = cfg->stackTop[cpu];
            pool->entry.nestedCPU = (uint64_t)(int64_t)cpu;
        }
        gActive[cpu].loading = false;
        usSetELR(currentELR() + 4);
        return US_PAYLOAD_RESUME;
    }
    vbar = currentVBAR();

    outer->esr = currentESR();
    outer->far = far;

    if (gActive[cpu].loading
        && usAccessCovers(outer->far, gActive[cpu].address, gActive[cpu].loadBytes)) {
        /* The emulated access: the kernel's handler for the slot the
         * exception was taken through is where its instruction's fault
         * belongs, in the context it was interrupted from */
        UsPoolTrapCPU *acc = trapAccount(cpu);

        if (acc != NULL) {
            acc->faulted++;
            acc->handedBack++;
        }
        if (usSlotOfSPSR(outer->spsr) == UsStubSlotEL0) {
            /* The instruction was the user's, so the fault has to read as one
             * the user took; the fault status and the direction are the same
             * either way */
            outer->esr = usESRAsLowerEL(outer->esr);
        }
        gActive[cpu].loading = false;
        if (!landingFor(outer, vbar)) {
            return NULL;
        }
    } else {
        /*
         * Something of ours, not the interrupted instruction: that is a fault
         * inside a handler, which the kernel treats as fatal, and it is
         * reported where it happened - at our code - rather than blamed on
         * the instruction the handler was standing in for
         */
        UsPayloadConfig *cfg = usPayloadConfig();
        UsPool *pool;

        outer->elr = currentELR();
        outer->spsr = currentSPSR();
        pool = cfg->poolBase != 0 ? (UsPool *)(uintptr_t)cfg->poolBase : NULL;
        if (pool != NULL) {
            pool->entry.nestedFaults++;
            pool->entry.nestedESR = outer->esr;
            pool->entry.nestedFAR = outer->far;
            pool->entry.nestedELR = outer->elr;
            pool->entry.nestedSelfVA = cfg->selfVA;
            pool->entry.nestedPoolBase = cfg->poolBase;
            pool->entry.nestedStackTop = cfg->stackTop[cpu];
            pool->entry.nestedCPU = (uint64_t)(int64_t)cpu;
        }
        gActive[cpu].loading = false;
        if (!usPayloadSlotTail(vbar, outer->spsr, &outer->landing)) {
            return NULL;
        }
    }
    /* What was handed back, like every other way out of the handler. The
     * frame is finished with: the entry branches away from here and never
     * returns to the handler that owned it */
    usPANEnterHandler(outer->spsr);
    usPayloadLeave(cpu);
    usRecordHandback(outer);
    return outer;
}

/*
 * The last answer, when there is none
 *
 * The entry reaches this when it cannot go on at all: no destination for the
 * exception, or a CPU it has no stack for. Both end in the entry's own halt,
 * which from outside is a machine that stopped with nothing to say, so what
 * was known at that moment is written where the host can read it
 */
void usPayloadStuck(uint64_t kind) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;

    if (cfg->poolBase == 0) {
        return;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    pool->entry.stuck++;
    pool->entry.stuckKind = kind;
    pool->entry.stuckESR = currentESR();
    pool->entry.stuckELR = currentELR();
    pool->entry.stuckSPSR = currentSPSR();
    pool->entry.stuckVBAR = currentVBAR();
}
