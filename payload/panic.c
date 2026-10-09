/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Stopping on purpose, and saying why, see panic.h
 *
 * The rule that runs through all of it: nothing here may fault. Every caller
 * has already decided it cannot continue, so a fault taken inside the report
 * is a stop with nothing said at all -- the worst of the ways this can end.
 * That is why the serial port is asked about rather than written to, and why
 * every drawing call checks its own mapping first
 */

#include "common/layout.h"
#include "core/translate.h"
#include "payload/fb.h"
#include "payload/panic.h"
#include "payload/payload.h"
#include "payload/regs.h"
#include "payload/uart.h"
#include "payload/vamap.h"

/*
 * Whether the serial port can be written to from here
 *
 * The boot says the port stops being reachable once the kernel's tables are in
 * force, and the payload is told so rather than finding out. Asking is better
 * than being told, though, and costs one instruction: the port is at a
 * physical address the firmware handed over, so reaching it from here means
 * that address still translates to itself
 *
 * This matters because the port is not left uninitialised. The self test runs
 * before the kernel and sets the port up, so a plain write here would go to an
 * address that no longer translates and fault inside the report. An `at`
 * cannot fault: a failed translation is written into PAR_EL1 and nothing else
 * happens, so the question is always safe to ask
 */
static bool serialReachable(UsPayloadConfig *cfg) {
    uint64_t pa = 0;

    return cfg->uartBase != 0 && cfg->uartKind != 0
           && usTranslateAddress(cfg->uartBase, true, &pa)
           && pa == cfg->uartBase;
}

/*
 * Three pairs on one line, to both channels that are left
 *
 * The serial port is one of them only when it is actually there, and the frame
 * buffer only when the boot found one and the payload managed to map it. The
 * same text goes to each rather than one being a summary of the other: the two
 * are read at different times by different means, and neither reader can see
 * the other's copy
 */
static void line(bool serial, const char *a, uint64_t aValue, const char *b,
                 uint64_t bValue, const char *c, uint64_t cValue) {
    if (serial) {
        usUARTPuts(a);
        usUARTPutHex(aValue);
        usUARTPuts(b);
        usUARTPutHex(bValue);
        usUARTPuts(c);
        usUARTPutHex(cValue);
        usUARTPuts("\n");
    }

    usFBText(a);
    usFBHex(aValue);
    usFBText(b);
    usFBHex(bValue);
    usFBText(c);
    usFBHex(cValue);
    usFBText("\n");
}

_Noreturn void usPanicHalt(uint64_t code, const char *what, uint64_t detail) {
    /*
     * The record in memory first, before anything that can fail
     *
     * The two channels below are both unreliable -- the serial port may not be
     * mapped any more and the screen may never have been mapped at all -- while
     * this one cannot fail: the pool is memory this code already owns and has
     * just written to. Ordering it here is what makes the stop explainable on
     * a machine where both of the others are gone, which is exactly the
     * machine a panic happens on
     *
     * The flag goes last so that a host sampling memory while this runs never
     * sees the flag without the fields it explains
     */
    {
        UsPayloadConfig *cfg = usPayloadConfig();

        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;

            pool->entry.panicCode = code;
            pool->entry.panicDetail = detail;
            pool->entry.panicELR = currentELR();
            pool->entry.panicESR = currentESR();
            pool->entry.panicFAR = currentFAR();
            pool->entry.panicSPSR = currentSPSR();
            pool->entry.panicEntries = pool->entry.entries;
            __atomic_store_n(&pool->entry.panic, US_POOL_PANIC_MAGIC,
                             __ATOMIC_RELEASE);
        }
    }

    /*
     * Then the two channels, each of which is asked whether it is there rather
     * than written to and hoped about
     */
    UsPayloadConfig *cfg = usPayloadConfig();
    bool serial = serialReachable(cfg);

    if (serial) {
        usUARTPuts("PANIC code=");
        usUARTPutHex(code);
        usUARTPuts(" what=");
        usUARTPuts(what != NULL ? what : "?");
        usUARTPuts("\n");
    }

    usFBText("PANIC code=");
    usFBHex(code);
    usFBText(" what=");
    usFBText(what != NULL ? what : "?");
    usFBText("\n");

    /*
     * The registers that describe the exception, then where the two moving
     * parts of this project were. The addresses on their own say nothing:
     * neither the code nor the pool stays where it was linked, so an address
     * only becomes a place in a source file once those two are subtracted
     * from it
     */
    line(serial, "elr=", currentELR(), " esr=", currentESR(), " far=",
         currentFAR());
    line(serial, "spsr=", currentSPSR(), " detail=", detail, " vbar=",
         currentVBAR());
    line(serial, "payload=", usVAMapRecord.payloadAfter, " pool=",
         usVAMapRecord.poolAfter, " self=", usVAMapRecord.payloadBefore);

    /*
     * And a line that says the report ended, written last and followed by
     * nothing. A harness watching the serial port stops reading the moment it
     * sees what it waited for, so a pattern matching inside the report would
     * cut the rest of it off
     */
    if (serial) {
        usUARTPuts("PANIC-END\n");
    }
    usFBText("PANIC-END");

    /*
     * Every interrupt masked, then a wait that cannot be woken. The masking
     * comes first and stays: an interrupt arriving between the report and the
     * stop would run the kernel's own handler, which is a machine that carried
     * on after saying it had stopped
     */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

/* One more than before, and what the count is now */
static uint64_t bump(uint64_t *counter) {
    uint64_t seen = *counter + 1U;

    *counter = seen;
    return seen;
}

void usPanicCountTrap(void) {
    UsPayloadConfig *cfg = usPayloadConfig();

    if (cfg->panicAfterTraps == 0) {
        return;
    }
    if (bump(&cfg->panicTraps) == cfg->panicAfterTraps) {
        usPanicHalt(US_PANIC_INJECTED, "trap", cfg->panicTraps);
    }
}

void usPanicCountRewrite(void) {
    UsPayloadConfig *cfg = usPayloadConfig();

    if (cfg->panicAfterRewrites == 0) {
        return;
    }
    if (bump(&cfg->panicRewrites) == cfg->panicAfterRewrites) {
        usPanicHalt(US_PANIC_INJECTED, "rewrite", cfg->panicRewrites);
    }
}
