/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The stages a boot goes through, and the one line each of them says
 *
 * A milestone is a whole log line that something outside the machine waits
 * for: the deploy scripts stop a run at one, and a person reading a serial log
 * finds where it got to by looking for one. That makes the text of these lines
 * an interface, and it makes a stage a thing the code has rather than a string
 * it happens to print
 *
 * So both live here. The boot says usStageMilestone(UsStageM6Armed) and the
 * spelling is decided in one place, which is what keeps a name the scripts
 * grep for from being edited at one of the call sites
 *
 * A stage with more than one outcome has an entry per outcome, because the
 * outcome is what a reader is looking for: "M4 failed" and "M4 setup" are two
 * different machines, and a script that waits for one of them would wait
 * forever for the other
 *
 * The names are not free to change: the deploy scripts match them, and the M
 * numbering is the one the runbooks use. What a stage means, and what has to
 * be true when it is said, is in design.md
 */

#ifndef US_STAGE_H
#define US_STAGE_H

/* <stdint.h> and not <stddef.h>: the entry is reached from a host test that
 * compiles this file against the firmware library, and that is the one header
 * which is not the same header in all three worlds */
#include <stdint.h>

typedef enum UsStage_e {
    /* The configuration was read and understood */
    UsStageM2,
    UsStageM2Failed,
    /* The driver is armed: hooks in place, nothing taken over yet */
    UsStageM4,
    UsStageM4Failed,
    /* The configuration's own patch table was written into an image */
    UsStageM4Patched,
    /* A scan of the loaded images produced a plan */
    UsStageM5Planned,
    UsStageM5Incomplete,
    /* The payload is in memory and the boot side can reach it */
    UsStageM6PayloadPlaced,
    UsStageM6Failed,
    /* The handover stub went into the loader */
    UsStageM6TransferDone,
    UsStageM6TransferFailed,
    UsStageM6TransferSkipped,
    /* The vector slots answer to the payload, however they were armed */
    UsStageM6Armed,
    UsStageM6ArmFailed,
    UsStageM6ArmedSkipped,
    /* The kernel's driver load call is diverted */
    UsStageM6DriverHookDone,
    UsStageM6DriverHookFailed,
    UsStageM6DriverHookSkipped,
    /* The images were rewritten in place */
    UsStageM7Rewritten,
    UsStageM7Nothing,
    UsStageM7Skipped,

    UsStageCount,
} UsStage;

/* The line, through the console and the screen, and the one thing anyone
 * outside the machine reads */
void usStageMilestone(UsStage stage);

/* The same name without anything printed: what usStageMilestone would say */
const char *usStageName(UsStage stage);

#endif
