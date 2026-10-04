/*
 * Running a piece of work on our own stack
 *
 * Hooks installed in the firmware's tables are called on the caller's stack,
 * and the caller may be anything with any amount of stack left. Doing real
 * work there is how a boot hangs: a scan of an image needs a few kilobytes of
 * frame that the caller may not have to spare
 *
 * We already own stacks, one per CPU in the pool. This switches to one of
 * them, runs the work, and switches back, which turns "the caller's stack must
 * be big enough" into something we control
 *
 * The same switch is what the payload does on entry to the synchronous
 * exception handler, for the same reason
 */

#ifndef US_STACK_H
#define US_STACK_H

#include <stdint.h>

/*
 * Sets the stack pointer to stackTop and calls fn(arg)
 *
 * stackTop must be sixteen byte aligned, and must point at the first byte past
 * the usable stack: stacks grow down
 *
 * Safe to call from any context, including one that is currently running on a
 * stack we do not own. It uses no stack of its own, only the one it is given
 */
void usStackRunOn(uint64_t stackTop, void (*fn)(void *), void *arg);

/* The stack pointer, for checking that a switch really happened */
uint64_t usStackCurrent(void);

#endif
