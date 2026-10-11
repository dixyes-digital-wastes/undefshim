/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the stage names
 *
 * These strings are an interface with two readers that are not this code: the
 * deploy scripts, which stop a run on one of them, and a person reading a
 * serial log. So what is checked is that every stage has a line, and that the
 * lines are exactly the ones that were there before the table that holds them.
 * A rename is the one edit that would pass every other check in this tree and
 * quietly break the runbooks
 *
 * The scripts are read as text by check_structure.py rather than here: this
 * test asks what the table says, that one asks whether the table and the
 * scripts still agree
 */

#include <stdio.h>
#include <string.h>

#include "uefi/stage.h"

#define US_CHECK_NAME "test_stage"
#include "check.h"

/*
 * The console the stage table speaks through, stood in for here
 *
 * The table is what is under test, and the console it writes to needs the
 * firmware's own headers and a screen. What arrives is counted instead, so
 * that "the line was said" is something this test can see
 */
static const char *gLastSaid;
static long gSaid;

void usConsoleMilestone(const char *what) {
    gLastSaid = what;
    gSaid++;
}

/*
 * Every line, as it was before the table existed
 *
 * Written out rather than derived from anything, because the point is that
 * these exact spellings are what the scripts match on. A stage is free to
 * share a line with another - the two ways of arming the vectors say the same
 * thing - so this is a list of the lines and not a bijection with the enum
 */
static const char *const kExpected[] = {
    "M2 done",
    "M2 failed",
    "M4 setup",
    "M4 failed",
    "M4 patched",
    "M5 planned",
    "M5 incomplete",
    "M6 payload placed",
    "M6 failed",
    "M6.5 transfer done",
    "M6.5 transfer failed",
    "M6.5 transfer skipped",
    "M6.5 armed",
    "M6.5 arm failed",
    "M6.5 armed skipped",
    "M6.5 driver hook done",
    "M6.5 driver hook failed",
    "M6.5 driver hook skipped",
    "M7 rewritten",
    "M7 nothing to rewrite",
    "M7 skip rewrite",
};

#define EXPECTED_COUNT (sizeof(kExpected) / sizeof(kExpected[0]))

static int failures;
static long checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eqStr(const char *name, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        usCheckFail("%s: want \"%s\" got \"%s\"\n", name, want,
                    got != NULL ? got : "(null)");
    }
}

/* Every stage has a line, and no two stages have to share one unless they mean
 * the same thing */
static void testEveryStageHasItsLine(void) {
    int distinct = 0;

    for (unsigned s = 0; s < (unsigned)UsStageCount; s++) {
        const char *name = usStageName((UsStage)s);
        int seen = 0;

        checks++;
        if (name == NULL || name[0] == '\0') {
            failures++;
            usCheckFail("stage %u has no line\n", s);
            continue;
        }
        if (strcmp(name, "M? unnamed") == 0) {
            failures++;
            usCheckFail("stage %u has no line in the table\n", s);
        }
        for (unsigned e = 0; e < EXPECTED_COUNT; e++) {
            if (strcmp(kExpected[e], name) == 0) {
                seen = 1;
            }
        }
        if (!seen) {
            failures++;
            usCheckFail("stage %u says \"%s\", which is not one of the lines\n", s,
                        name);
        }
        /* Counted so that a table that collapsed to one entry is caught: the
         * stages do not all say the same thing */
        int first = 1;
        for (unsigned p = 0; p < s; p++) {
            if (strcmp(usStageName((UsStage)p), name) == 0) {
                first = 0;
            }
        }
        if (first) {
            distinct++;
        }
    }
    ok("at least a dozen distinct lines", distinct >= 12);
}

/* The spellings themselves, and the answer for a stage that is not one */
static void testSpellings(void) {
    eqStr("M2", usStageName(UsStageM2), "M2 done");
    eqStr("M2 failed", usStageName(UsStageM2Failed), "M2 failed");
    eqStr("M4", usStageName(UsStageM4), "M4 setup");
    eqStr("M4 patched", usStageName(UsStageM4Patched), "M4 patched");
    eqStr("M5 planned", usStageName(UsStageM5Planned), "M5 planned");
    eqStr("M5 incomplete", usStageName(UsStageM5Incomplete), "M5 incomplete");
    eqStr("payload placed", usStageName(UsStageM6PayloadPlaced),
          "M6 payload placed");
    eqStr("transfer done", usStageName(UsStageM6TransferDone),
          "M6.5 transfer done");
    eqStr("transfer skipped", usStageName(UsStageM6TransferSkipped),
          "M6.5 transfer skipped");
    eqStr("armed", usStageName(UsStageM6Armed), "M6.5 armed");
    eqStr("arm failed", usStageName(UsStageM6ArmFailed), "M6.5 arm failed");
    eqStr("driver hook done", usStageName(UsStageM6DriverHookDone),
          "M6.5 driver hook done");
    eqStr("rewritten", usStageName(UsStageM7Rewritten), "M7 rewritten");
    eqStr("nothing to rewrite", usStageName(UsStageM7Nothing),
          "M7 nothing to rewrite");
    eqStr("skip rewrite", usStageName(UsStageM7Skipped), "M7 skip rewrite");

    /* Out of range answers rather than reading past the table */
    eqStr("past the end", usStageName((UsStage)UsStageCount), "M? unnamed");
    eqStr("far past the end", usStageName((UsStage)0x4000), "M? unnamed");
}

/* Every stage name starts with the M a reader looks for, and none is empty */
static void testShape(void) {
    for (unsigned s = 0; s < (unsigned)UsStageCount; s++) {
        const char *name = usStageName((UsStage)s);

        ok("starts with M", name != NULL && name[0] == 'M');
        ok("fits the log line", name != NULL && strlen(name) < 40U);
    }
}

/* Saying a stage is what writes the line, so it has to reach the console with
 * the name the table holds rather than with anything else */
static void testSayingAStageSaysItsLine(void) {
    const char *want = usStageName(UsStageM6Armed);

    gLastSaid = NULL;
    gSaid = 0;
    usStageMilestone(UsStageM6Armed);
    ok("the console was written to once", gSaid == 1);
    ok("with the table's own line", gLastSaid == want);
    eqStr("which is the armed milestone", gLastSaid, "M6.5 armed");

    for (unsigned s = 0; s < (unsigned)UsStageCount; s++) {
        gLastSaid = NULL;
        usStageMilestone((UsStage)s);
        ok("every stage reaches the console", gLastSaid == usStageName((UsStage)s));
    }
}

int main(void) {
    testEveryStageHasItsLine();
    testSpellings();
    testShape();
    testSayingAStageSaysItsLine();

    return usCheckSummary(checks, failures);
}
