/*
 * The code that carries a branch out of an image.
 *
 * A vector table slot can only hold a branch that reaches within its own
 * image, and what it has to reach is not there: the payload is in a pool
 * whose address the kernel image cannot know at link time, and which changes
 * when the address space is rebuilt. So the image gets a thunk of its own, a
 * handful of instructions placed in a run of zero words inside one of its
 * mapped executable sections, and the slot branches to that.
 *
 * The address is carried as four immediates rather than as a literal loaded
 * from memory. That is what lets the whole thunk be written at boot time,
 * when the address is already known, with nothing left to fill in later.
 * Whether the image is writable once the kernel is running is exactly the
 * kind of thing this project has learned not to assume.
 */

#ifndef US_THUNK_H
#define US_THUNK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * str x16, [sp, #-16]!, movz, three movk, br. Six instructions.
 *
 * The push is not optional and is not tidiness. The only register free to
 * carry the address is one the interrupted code is using, and an exception
 * arrives in the middle of whatever that code was doing, so there is no
 * convention that says x16 may be destroyed: on entry every register is live.
 * Sixteen bytes of the interrupted stack are the cheapest place to keep it,
 * and the entry knows they are there and reads it back.
 */
#define US_THUNK_WORDS 6U
#define US_THUNK_BYTES (US_THUNK_WORDS * 4U)

/*
 * Writes the thunk. The address may be anything; the encoding covers the
 * full 64 bits because the immediates are four sixteen bit halves.
 */
void usEncodeThunk(uint32_t out[US_THUNK_WORDS], uint64_t target);

/*
 * arm64's unconditional branch: a 26 bit word offset, in instructions, so it
 * reaches 128 MB either way and only between addresses that agree in their
 * bottom two bits.
 *
 * Returns false when the target is out of reach. A caller that ignored this
 * and encoded anyway would produce a branch to somewhere else entirely, which
 * is a fault in code that has nothing to do with the mistake.
 */
#define US_BRANCH_RANGE (1U << 27)
/* The instruction a branch encodes to with a zero offset: b . */
#define US_BRANCH_OPCODE 0x14000000U

bool usEncodeBranch(uint32_t from, uint32_t to, uint32_t *out);

#endif
