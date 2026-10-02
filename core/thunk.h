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
 * mrs, lsr, cbnz, movz, three movk, br, and the branch back. Nine
 * instructions.
 *
 * Every one of them works on a register, a system register or an immediate,
 * and none of them touches memory: see the note below about what is true at
 * the moment this runs.
 */
#define US_THUNK_WORDS 6U
#define US_THUNK_BYTES (US_THUNK_WORDS * 4U)

void usEncodeThunk(uint32_t out[US_THUNK_WORDS], uint64_t target);

/*
 * The stub that goes in a vector slot, or in a hole the slot branches to.
 *
 *   mrs  x16, esr_el1            the exception class
 *   lsr  x16, x16, #26
 *   cbnz x16, tail               not an undefined instruction: not ours
 *   mrs  x16, elr_el1            the instruction that faulted
 *   ldr  w17, [x16]
 *   and  w17, w17, #0x3ffffc00   everything about an RCpc load but its width
 *   movz w16, #0xc000
 *   movk w16, #0x38bf, lsl #16
 *   cmp  w17, w16
 *   b.ne tail                    an undefined instruction, but not one of ours
 *   movz x16, ...                four halves of the payload's address
 *   br   x16
 * tail:
 *   the slot's own behaviour, two instructions
 *
 * The instruction is examined rather than only the class, and that is what
 * makes the tail safe to reach. Windows uses `udf` as a trap of its own, and
 * an undefined instruction that is not an RCpc load has to go to the kernel's
 * handler; sending it to the payload instead means the payload has to find its
 * way back with the interrupted registers restored, which is a second exit
 * from the entry and a lot of places to get it wrong. Four instructions of
 * masking avoid all of it: only the four loads ever leave this stub.
 *
 * The width is not tested. The four loads differ only in the two bits the
 * mask clears, so one comparison covers all of them.
 */
#define US_SLOT_STUB_WORDS 17U
#define US_SLOT_STUB_BYTES (US_SLOT_STUB_WORDS * 4U)

/* Where the four halves of the payload's address are, and the two words the
 * slot's own behaviour is written into. */
#define US_SLOT_STUB_ADDRESS 10U
#define US_SLOT_STUB_CONTINUATION 15U

void usEncodeSlotStub(uint32_t out[US_SLOT_STUB_WORDS], uint64_t target,
                      uint32_t tail0, uint32_t tail1);

/*
 * arm64's unconditional branch: a 26 bit word offset, in instructions, so it
 * reaches 128 MB either way and only between addresses that agree in their
 * bottom two bits.
 *
 * Returns false when the target is out of reach. A caller that ignored this
 * would produce a branch to somewhere else entirely, which is a fault in code
 * that has nothing to do with the mistake.
 */
#define US_BRANCH_RANGE (1U << 27)
#define US_BRANCH_OPCODE 0x14000000U

bool usEncodeBranch(uint32_t from, uint32_t to, uint32_t *out);

/* The instruction that does nothing, for a tail whose slot needs no
 * instruction replayed. */
#define US_NOP 0xD503201FU

#endif
