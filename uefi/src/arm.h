/*
 * Taking over the handover to the kernel
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
 * know where the kernel will end up
 *
 * What this does NOT do is decide the payload's address after the switch:
 * that is the payload's own job, done while it runs here. See transfer.c on
 * the payload side
 */

#ifndef US_ARM_H
#define US_ARM_H

#include <stdbool.h>

#include "uefi/src/session.h"

/*
 * Takes over the handover: the branch the loader enters the kernel with is
 * redirected to a stub, and the stub enters the payload first
 *
 * Returns false when the loader has no such branch, or no room for the stub
 */
bool usArmTransfer(UsSession *session);

/*
 * Draws the exception path into the payload
 *
 * The loader's vector table is what is in force when the kernel first
 * executes an instruction this hardware does not have, so its synchronous
 * slot is where the shim has to be reachable from. Tables are found by
 * following what the writes of VBAR_EL1 load, and only a slot that is still
 * a branch to itself is written: anything else is a handler that works
 *
 * Returns false when no slot was taken over, which means the shim cannot be
 * reached and the boot will fail the same way it did before
 */
bool usArmVectorTable(UsSession *session);

#endif
