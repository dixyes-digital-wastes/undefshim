/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the name a stop is asked for by
 *
 * The name comes from a configuration file, which hands back a pointer into
 * its own document and a length rather than a terminated string, so the two
 * sides of the comparison do not have the same shape. That is the interesting
 * part and it is why this exists: an off-by-one here either stops the boot at
 * a milestone nobody asked for or never stops it at all, and neither looks
 * like a bug in this function
 *
 * The other half is that the comparison is whole rather than by prefix. Four
 * of the milestones begin with "M6.5", so a prefix would stop at whichever
 * came first -- which is a machine that stopped somewhere other than where it
 * was told to, and looks exactly like the right behaviour until it is read
 */

#include <stdio.h>
#include <string.h>

#include "uefi/panic.h"

#define US_CHECK_NAME "test_panic"
#include "check.h"

static int failures;
static int checks;

/* The configuration's shape: a pointer and a length, with the byte after the
 * value being whatever the document has there rather than a terminator */
static void matchesText(const char *name, const char *want, int wantLen,
                        int expected) {
    int got = usPanicNameMatches(want, wantLen, name);

    checks++;
    if (got != expected) {
        failures++;
        usCheckFail("%-46s want %d got %d\n", name, expected, got);
    }
}

int main(void) {
    const char *want = "M6.5 armed";
    int len = (int)strlen(want);

    /* The ordinary case */
    matchesText("M6.5 armed", want, len, 1);

    /* A milestone that begins the same way is not the one named. This is the
     * reason the comparison is whole: three others share this prefix */
    matchesText("M6.5 arm failed", want, len, 0);
    matchesText("M6.5 armed skipped", want, len, 0);
    matchesText("M6.5 transfer done", want, len, 0);
    matchesText("M6", want, len, 0);

    /* The name has to end where the milestone does: a prefix of it is not
     * it, and neither is a longer name that begins with all of it */
    matchesText("M6.5 arme", want, len, 0);
    matchesText("M6.5 armedx", want, len, 0);

    /* Nothing asked for, and nothing to compare against */
    matchesText("M2 done", NULL, 0, 0);
    matchesText("M2 done", want, 0, 0);
    matchesText(NULL, want, len, 0);

    /* An empty milestone only matches a name with an empty length */
    matchesText("", want, 0, 1);

    /* Every milestone the boot prints, against a name that is one of them,
     * so that the list and the comparison are checked against each other */
    {
        static const char *const names[] = {
            "M2 done",
            "M6 payload placed",
            "M4 setup",
            "M5 planned",
            "M6.5 transfer skipped",
            "M6.5 transfer done",
            "M6.5 transfer failed",
            "M6.5 armed skipped",
            "M6.5 armed",
            "M6.5 arm failed",
            "M6.5 driver hook skipped",
            "M6.5 driver hook done",
            "M6.5 driver hook failed",
            "M7 rewritten",
            "M7 nothing to rewrite",
            "M7 skip rewrite",
        };
        static const char *const wanted[] = { "M7 rewritten", "M2 done",
                                              "M6.5 armed" };

        for (size_t w = 0; w < sizeof(wanted) / sizeof(wanted[0]); w++) {
            int hits = 0;

            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (usPanicNameMatches(wanted[w], (int)strlen(wanted[w]),
                                       names[i])) {
                    hits++;
                }
            }
            checks++;
            if (hits != 1) {
                failures++;
                usCheckFail("%-46s matched %d milestones\n", wanted[w], hits);
            }
        }
    }

    return usCheckSummary(checks, failures);
}
