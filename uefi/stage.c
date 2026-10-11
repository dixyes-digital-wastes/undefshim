/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The stages a boot goes through, see stage.h
 */

#include "uefi/stage.h"

#include <stddef.h>

#include "uefi/console.h"

/*
 * The one place these strings are written
 *
 * Sized by the enumeration, so a stage added to stage.h without its line
 * added here is a null in this table and not a line that quietly was not
 * said. usStageName answers with a name that says so, and a check in
 * tests/unit walks the whole table rather than a sample of it
 */
static const char *const kStageNames[UsStageCount] = {
    [UsStageM2] = "M2 done",
    [UsStageM2Failed] = "M2 failed",
    [UsStageM4] = "M4 setup",
    [UsStageM4Failed] = "M4 failed",
    [UsStageM4Patched] = "M4 patched",
    [UsStageM5Planned] = "M5 planned",
    [UsStageM5Incomplete] = "M5 incomplete",
    [UsStageM6PayloadPlaced] = "M6 payload placed",
    [UsStageM6Failed] = "M6 failed",
    [UsStageM6TransferDone] = "M6.5 transfer done",
    [UsStageM6TransferFailed] = "M6.5 transfer failed",
    [UsStageM6TransferSkipped] = "M6.5 transfer skipped",
    [UsStageM6Armed] = "M6.5 armed",
    [UsStageM6ArmFailed] = "M6.5 arm failed",
    [UsStageM6ArmedSkipped] = "M6.5 armed skipped",
    [UsStageM6DriverHookDone] = "M6.5 driver hook done",
    [UsStageM6DriverHookFailed] = "M6.5 driver hook failed",
    [UsStageM6DriverHookSkipped] = "M6.5 driver hook skipped",
    [UsStageM7Rewritten] = "M7 rewritten",
    [UsStageM7Nothing] = "M7 nothing to rewrite",
    [UsStageM7Skipped] = "M7 skip rewrite",
};

const char *usStageName(UsStage stage) {
    if ((unsigned)stage >= (unsigned)UsStageCount || kStageNames[stage] == NULL) {
        return "M? unnamed";
    }
    return kStageNames[stage];
}

void usStageMilestone(UsStage stage) {
    usConsoleMilestone(usStageName(stage));
}
