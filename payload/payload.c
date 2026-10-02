/*
 * What runs when an exception arrives, and the state it needs to do it.
 *
 * See payload.h for the interface. This file is the C half; the assembly
 * entry that saves the frame and reaches here is entry.S.
 */

#include "common/layout.h"
#include "payload/payload.h"
#include "payload/selfmap.h"
#include "payload/uart.h"
#include "payload/us_mem.h"

/*
 * Written by the boot after it copies the blob. Named without a leading
 * qualifier because the assembly entry addresses it directly, and the two
 * halves of the payload have to agree on where it is.
 */
UsPayloadConfig usPayloadConfigBlock;

/*
 * One frame per CPU, filled by the entry before it switches stacks.
 *
 * The entry cannot use a stack it has not switched to yet, so the frame goes
 * here first. Interrupts are masked at entry and the handler does not
 * recurse, so one pad per CPU is enough, and the index comes from the CPU
 * affinity field rather than an assumption that this is running on the first
 * CPU.
 */
UsFrame usPayloadLanding[US_MAX_CPUS];

/*
 * Where the handler stores the frame it was given, so a nested report can
 * find it. Not used for anything else, and not the ABI: the ABI is the
 * pointer passed in.
 */
static UsFrame *gCurrentFrame;

static uint32_t currentCpu(void) {
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFFU);
}

UsPayloadConfig *usPayloadConfig(void) {
    return &usPayloadConfigBlock;
}

/*
 * The exceptions this is willing to take responsibility for.
 *
 * An undefined instruction is the whole point: on hardware without the
 * extension, every LDAPR is one. A breakpoint is accepted as well because it
 * is how the deployment checks get here without planting anything that would
 * corrupt a running kernel, and because answering "not mine" to a breakpoint
 * is worse than reporting it.
 */
static int isHandledClass(uint32_t ec) {
    return ec == US_EC_UNKNOWN || ec == US_EC_BRK64;
}

int usPayloadHandle(UsFrame *frame) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint32_t ec;

    gCurrentFrame = frame;
    usUartInit(cfg->uartBase);

    if (frame == NULL) {
        usUartPuts("US-PAYLOAD no-frame\n");
        return 0;
    }

    ec = (uint32_t)US_ESR_EC(frame->esr);
    usUartPuts("US-PAYLOAD ec=");
    usUartPutHex(ec);
    usUartPuts(" elr=");
    usUartPutHex(frame->elr);
    usUartPuts(" far=");
    usUartPutHex(frame->far);
    usUartPuts(" cpu=");
    usUartPutDec(currentCpu());
    usUartPuts("\n");

    if (!isHandledClass(ec)) {
        usUartPuts("US-PAYLOAD not-mine\n");
        return 0;
    }

    /*
     * Nothing is emulated yet, so the most that can be said is that the
     * exception arrived and was understood. Reporting it and refusing to
     * claim it is the honest answer, and the one that leaves the kernel to
     * deal with it exactly as it would have.
     */
    usUartPuts("US-PAYLOAD reached\n");
    return 0;
}

/* Only used to prove the blob was copied and is executable before anything
 * depends on it. */
void usPayloadSelfTest(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsSelfMap self;

    usUartInit(cfg->uartBase);
    usUartPuts("US-PAYLOAD alive cpu=");
    usUartPutDec(currentCpu());
    usUartPuts(" frame=");
    usUartPutDec(sizeof(UsFrame));
    usUartPuts("\n");

    /*
     * Where the tables say this very code can be reached from.
     *
     * There are none here. The firmware runs its own regime, and in this one
     * both translation base registers read as zero: an address is its own
     * physical address and no table describes it. So this reports the answer
     * for that case, and the search itself gets its exercise at the handover,
     * where the kernel's tables are in force and the question actually
     * matters.
     */
    self = usSelfMapFind(cfg->selfVa, US_POOL_BYTES, cfg->selfVa);
    usUartPuts("US-PAYLOAD selfmap ");
    if (self.found) {
        usUartPuts("va=");
        usUartPutHex(self.va);
        usUartPuts(" pa=");
        usUartPutHex(self.pa);
        usUartPuts(" size=");
        usUartPutHex(self.size);
    } else {
        usUartPuts("none");
    }
    usUartPuts(" probes=");
    usUartPutDec(self.probes);
    usUartPuts(self.exhausted ? " truncated" : " complete");
    usUartPuts("\n");
}
