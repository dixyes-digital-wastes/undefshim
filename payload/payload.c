/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What the payload offers the machine around it
 *
 * See payload.h for the interface. The exception path is not here: the state
 * it works on is state.c, the probes are probe.c, the loads are emulate.c,
 * the entry is dispatch.c, and what it writes down is account.c. This file is
 * the block the boot writes and the hello that proves the copy runs
 */

#include "payload/payload.h"

#include "payload/selfmap.h"
#include "payload/state.h"
#include "payload/uart.h"

/*
 * Written by the boot after it copies the blob. Named without a leading
 * qualifier because the assembly entry addresses it directly, and the two
 * halves of the payload have to agree on where it is
 */
UsPayloadConfig usPayloadConfigBlock;

UsPayloadConfig *usPayloadConfig(void) {
    return &usPayloadConfigBlock;
}

/* Only used to prove the blob was copied and is executable before anything
 * depends on it */
void usPayloadSelfTest(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsSelfMap self;

    usUARTInit(cfg->uartBase, (uint32_t)cfg->uartKind, (uint32_t)cfg->uartWidth);
    usUARTPuts("payload: alive cpu=");
    usUARTPutDec((uint64_t)currentCPU());
    usUARTPuts(" frame=");
    usUARTPutDec(sizeof(UsFrame));
    usUARTPuts("\n");

    /*
     * Where the tables say this very code can be reached from
     *
     * There are none here. The firmware runs its own regime, and in this one
     * both translation base registers read as zero: an address is its own
     * physical address and no table describes it. So this reports the answer
     * for that case, and the search itself gets its exercise at the handover,
     * where the kernel's tables are in force and the question actually
     * matters
     */
    self = usSelfMapFind(cfg->selfVA, cfg->poolBytes, cfg->selfVA);
    usUARTPuts("payload: selfmap ");
    if (self.found) {
        usUARTPuts("va=");
        usUARTPutHex(self.va);
        usUARTPuts(" pa=");
        usUARTPutHex(self.pa);
        usUARTPuts(" size=");
        usUARTPutHex(self.size);
    } else {
        usUARTPuts("none");
    }
    usUARTPuts(" probes=");
    usUARTPutDec(self.probes);
    usUARTPuts(self.exhausted ? " truncated" : " complete");
    usUARTPuts("\n");
}
