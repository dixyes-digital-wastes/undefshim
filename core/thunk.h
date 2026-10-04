/*
 * The code that carries a branch out of an image
 *
 * A vector table slot can only hold a branch that reaches within its own
 * image, and what it has to reach is not there: the payload is in a pool
 * whose address the kernel image cannot know at link time, and which changes
 * when the address space is rebuilt. So the image gets a thunk of its own, a
 * handful of instructions placed in a run of zero words inside one of its
 * mapped executable sections, and the slot branches to that
 *
 * The address is carried as four immediates rather than as a literal loaded
 * from memory. That is what lets the whole thunk be written at boot time,
 * when the address is already known, with nothing left to fill in later
 */

#ifndef US_THUNK_H
#define US_THUNK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * str x16,[sp,#-16]!, then the target built into x16 by movz and three movk,
 * then br x16
 *
 * Every one of them works on a register or an immediate, and none of them
 * touches memory except the push that keeps the interrupted x16
 */
#define US_THUNK_WORDS 6U
#define US_THUNK_BYTES (US_THUNK_WORDS * 4U)

void usEncodeThunk(uint32_t out[US_THUNK_WORDS], uint64_t target);

/*
 * Which synchronous vector slot a stub is for
 *
 * The three differ in two ways that decide the shape of the stub: where the
 * interrupted code's x18 can be got back from, and whether the slot is the one
 * a fault inside the payload arrives at
 *
 * EL1t and EL1h are kernel mode, where x18 is the per-CPU block and TPIDR_EL1
 * names it, so the stub rebuilds it exactly as the kernel's own handlers do and
 * touches no memory at all. That is not a refinement: an exception taken at
 * EL1h can arrive with a stale SP_EL1 - the kernel's own EL1t handler runs
 * there for its first instructions, before deriving its stack from SP_EL0 - and
 * a push would then fault where the exception was, with the faulting store's
 * address unchanged, which is an endless loop
 *
 * An EL0 exception is different in both respects. Its x18 is user state and
 * cannot be rebuilt, and the kernel's own entry for that vector shows what may
 * be used to keep it: `sub sp, sp, #0x370` straight away, so SP_EL1 really is
 * the thread's kernel stack there. This stub pushes x18 into the red zone below
 * it and the entry reads it back; the entry puts SP back before anything else
 * runs, so the tail sees the SP the exception left
 *
 * Only the EL1h slot lets a data abort through: that is what a fault on the
 * emulated access looks like, and the payload is the only thing that can tell
 * it from a fault of its own. The other two send everything else to the tail,
 * which is the handler the slot originally held
 */
typedef enum UsStubSlot_e {
    UsStubSlotEl1t = 0, /* the synchronous slot at offset 0x000 */
    UsStubSlotEl1h = 1, /* at 0x200, the one the payload itself runs under */
    UsStubSlotEl0 = 2,  /* at 0x400, the kernel's lower EL entry */
} UsStubSlot;

/* How many of those there are, for tables that are indexed by one */
#define US_STUB_SLOT_COUNT 3U

#define US_SLOT_STUB_WORDS 20U
#define US_SLOT_STUB_BYTES (US_SLOT_STUB_WORDS * 4U)
#define US_SLOT_TARGET_WORDS 5U
#define US_SLOT_TARGET_BYTES (US_SLOT_TARGET_WORDS * 4U)
#define US_SLOT_RUNTIME_WORDS (US_SLOT_STUB_WORDS + US_SLOT_TARGET_WORDS)
#define US_SLOT_RUNTIME_BYTES (US_SLOT_RUNTIME_WORDS * 4U)

/* Stage this sequence after the stub before publishing a branch to it */
void usEncodeSlotTarget(uint32_t out[US_SLOT_TARGET_WORDS], uint64_t target);

/* Which word of a stub carries the branch to the payload's entry, and so the
 * one that is replaced once the rest of the stub is in place */
uint32_t usSlotStubTargetIndex(UsStubSlot slot);

/*
 * The landing this table and slot were given last time, when the table cannot
 * be identified now. The exception still has to go somewhere, and the place
 * it went for this table before is that place
 */
bool usPayloadSlotTailCached(uint64_t vbar, uint64_t spsr, uint64_t *tail);

/* The last one worked out for this slot, whatever table it was for: the answer
 * of last resort, used only where the alternative is a stopped processor */
bool usPayloadSlotTailLast(uint64_t spsr, uint64_t *tail);

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, UsStubSlot slot);

/*
 * The index of the first tail word, which is also where the entry branches when
 * the exception is handed back. A caller that has to encode a branch into the
 * tail needs the index and the length, because a relative branch is relative to
 * its own address
 *
 * The EL0 tail is one word longer than the others: the entry hands the frame
 * back through x18, and the kernel's own EL0 entry saves x18 into its trap
 * frame as user state rather than rebuilding it, so the first thing the tail
 * does is put the interrupted x18 back from the word the stub pushed
 */
uint32_t usSlotStubTailIndex(UsStubSlot slot);
uint32_t usSlotStubTailWords(UsStubSlot slot);

/*
 * arm64's unconditional branch: a 26 bit word offset, in instructions, so it
 * reaches 128 MB either way and only between addresses that agree in their
 * bottom two bits
 *
 * Returns false when the target is out of reach. A caller that ignored this
 * would produce a branch to somewhere else entirely, which is a fault in code
 * that has nothing to do with the mistake
 */
#define US_BRANCH_RANGE (1U << 27)
#define US_BRANCH_OPCODE 0x14000000U

bool usEncodeBranch(uint32_t from, uint32_t to, uint32_t *out);

/* The instruction that does nothing, for a tail whose slot needs no
 * instruction replayed */
#define US_NOP 0xD503201FU

#endif
