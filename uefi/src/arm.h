/*
 * Taking over the handover to the kernel.
 *
 * The loader branches to the kernel from a fixed place, and the last thing it
 * does with the kernel's entry point is put it in a register. Replacing that
 * branch is what puts our code between the two, which is the only moment at
 * which everything needed is true at once:
 *
 *   - the kernel's image is loaded but nothing of it has run
 *   - the tables that will describe the rest of the boot are already built
 *   - physical memory is still reachable at its own address, so those tables
 *     can be read
 *
 * The replacement is a branch to padding the loader leaves in its own code,
 * where the stub goes. That padding exists and is reachable, which is what
 * makes this work without a trampoline somewhere else and without needing to
 * know where the kernel will end up.
 *
 * What this does NOT do is decide the payload's address after the switch:
 * that is the payload's own job, done while it runs here. See transfer.c on
 * the payload side.
 */

#ifndef US_ARM_H
#define US_ARM_H

#include <stdbool.h>

#include "uefi/src/session.h"

/*
 * Puts the handover stub in place, with the loader's branch redirected to it.
 *
 * Returns false when the loader cannot be prepared, which is a fact worth
 * reporting: nothing later will work, and the boot will otherwise look
 * normal while nothing of ours ever runs.
 */
bool usArmTransfer(UsSession *session);

#endif
