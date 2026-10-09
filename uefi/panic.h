/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Stopping the boot on purpose, and saying why
 *
 * The payload has its own panic and this is not that one. The two exist for
 * the same reason and report the same way, but they run in different worlds:
 * the payload has no firmware, no console, and only the channels it built for
 * itself, while this runs in the boot where the console is the firmware's and
 * still works
 *
 * What this is for is checking that a stop is noticed at all. A stop that
 * prints and halts is a still screen with a message on it; a stop that is
 * never reached, or one whose halt does not halt, looks like a machine that is
 * merely slow, and the two are told apart by whether that message is there
 */

#ifndef US_PANIC_H
#define US_PANIC_H

#include <stdbool.h>
#include <stdint.h>

/* The same codes the payload uses, so a reader does not have two tables */
#define US_PANIC_NONE 0U
#define US_PANIC_INJECTED 0xF000U

/*
 * Whether a name is the one that was asked for
 *
 * Compared whole rather than by prefix, because the milestones share their
 * beginnings -- four of them start with "M6.5" -- so a prefix would stop at
 * whichever came first, which is a machine that stopped somewhere other than
 * where it was told to
 *
 * want comes from the configuration, which hands back a pointer into the
 * document rather than a terminated string, so its length is given rather than
 * measured
 *
 * In the header because it is the whole of what a caller other than the halt
 * needs, and putting it beside the halt would make every caller of it link the
 * console
 */
static inline bool usPanicNameMatches(const char *want, int wantLen,
                                      const char *what) {
    int i = 0;

    if (want == NULL || what == NULL || wantLen < 0) {
        return false;
    }
    for (; i < wantLen && want[i] != '\0' && what[i] != '\0'; i++) {
        if (want[i] != what[i]) {
            return false;
        }
    }
    /* Both have to end together, so a name that is only the beginning of this
     * one is not a match */
    return i == wantLen && what[i] == '\0';
}

/*
 * Says what happened and stops, for good
 *
 * Never returns. Interrupts are masked first and stay masked, so nothing runs
 * on this processor again: a boot that carried on after printing this would be
 * indistinguishable from one whose report was written and then lost
 */
_Noreturn void usPanicHaltEFI(uint64_t code, const char *what, uint64_t detail);

#endif
