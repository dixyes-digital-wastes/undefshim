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

/* The branch back into what the slot used to do is the last of them, and is
 * where the exception classes that are not ours end up. */
#define US_STUB_WORDS 9U
#define US_STUB_BYTES (US_STUB_WORDS * 4U)
#define US_STUB_CONTINUATION 8U

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

/*
 * The code that goes in a vector table's synchronous slot.
 *
 * It writes nothing and pushes nothing. At the moment an exception arrives
 * there is no stack to use: at this vector SP holds whatever the kernel left
 * in it, which is not mapped once the address space has been rebuilt, and the
 * kernel's own handler discards it rather than reading it. A push here is a
 * fault taken inside the exception it was meant to handle, which repeats
 * until the machine resets.
 *
 * Nothing is preserved either, because nothing needs to be: x16 is one of the
 * two registers the platform reserves for this kind of use, and the kernel's
 * own handler clobbers it and x18 before saving anything. A register that the
 * platform's own path does not preserve cannot be holding anything across one
 * of these exceptions.
 *
 * The first thing it does is look at the exception class. Only an undefined
 * instruction is ours. Everything else takes the last instruction, which
 * carries on into whatever the slot used to do.
 *
 * That continuation is kept as a branch rather than as an address because a
 * branch is position relative: the image is relocated when the kernel builds
 * its own address space, and an address written here would be the address it
 * had before that.
 *
 * originalBranch is the slot's original word, which must be a branch. The
 * displacement is carried across, adjusted for the distance the instruction
 * moved inside the slot. A branch to itself is kept as one, because that is
 * what an unused slot holds and branching into this code would be worse than
 * stopping.
 */
void usEncodeVectorStub(uint32_t out[US_STUB_WORDS], uint64_t target,
                        uint32_t originalBranch);

#endif
