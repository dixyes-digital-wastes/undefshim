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
 * The vector-slot stub filters the exception class and instruction before
 * entering the payload. Only the four RCpc load widths leave the stub;
 * other exceptions rebuild x18 and run the slot's original two-word tail
 *
 * The slot a fault inside the payload is taken through - the synchronous one
 * at offset 0x200, which is the one the saving form is used for - also lets a
 * data abort through, because that is what a fault on the emulated access
 * looks like and the payload is the only thing that can tell it from a fault
 * of its own and pass it on as the interrupted instruction's
 *
 * Neither form touches memory. x18 is the one register spent, and in kernel
 * mode it is the per-CPU block, which TPIDR_EL1 names: the stub rebuilds it
 * on the way out exactly as the kernel's own handlers do. Saving it in the
 * interrupted stack's red zone was tried instead and is wrong: an exception
 * taken at EL1h can arrive with a stale SP_EL1 - the kernel's own EL1t
 * handler runs there for its first instructions before deriving its stack
 * from SP_EL0 - and the push would then fault where the exception was, with
 * the faulting store's address unchanged, which is an endless loop

 * x18 is the only register the stub spends, and it is the one register whose
 * value every vector can have back: on EL0 and EL1h there is a stack, and the
 * `save` form pushes it into the ABI red zone first; on EL1t the kernel
 * rebuilds it from TPIDR_EL1, which is what its own handler does. Nothing is
 * pushed on EL1t, because there the SP the CPU left is not a stack the
 * interrupted code was using.
 */
#define US_SLOT_STUB_WORDS 20U
#define US_SLOT_STUB_BYTES (US_SLOT_STUB_WORDS * 4U)
#define US_SLOT_TARGET_WORDS 5U
#define US_SLOT_TARGET_BYTES (US_SLOT_TARGET_WORDS * 4U)
#define US_SLOT_RUNTIME_WORDS (US_SLOT_STUB_WORDS + US_SLOT_TARGET_WORDS)
#define US_SLOT_RUNTIME_BYTES (US_SLOT_RUNTIME_WORDS * 4U)

/* Stage this sequence after the stub before publishing a branch to it. */
void usEncodeSlotTarget(uint32_t out[US_SLOT_TARGET_WORDS], uint64_t target);

/* Atomically replace this first MOVZ with B after staging/cache maintenance. */
uint32_t usSlotStubTargetIndex(bool save);

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, bool save);

/*
 * The index of the first of the two tail words. A caller that has to encode a
 * branch into them needs it, because a relative branch is relative to its own
 * address.
 */
uint32_t usSlotStubTailIndex(bool save);

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
