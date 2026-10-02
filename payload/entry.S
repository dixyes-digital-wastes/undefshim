/*
 * The exception entry.
 *
 * This is the part of the payload that cannot be written in C. The CPU enters
 * a vector table with the interrupted stack pointer still in SP, so the
 * registers have to be saved before anything is allowed to touch them, and
 * there is nowhere to save them until an address has been computed.
 *
 * The offsets come from layout_defs.inc, generated from the C structures, so
 * the two halves cannot drift apart.
 *
 * Three registers are borrowed before there is anywhere to put them, and the
 * borrow is the part worth reading twice. Working out where the landing pad
 * is takes two registers (x9 for the address, x10 for the stride) and a third
 * to hold the CPU index while that is done, and none of the three can be
 * stored anywhere until the pad address is known. So all three go on the
 * interrupted stack and come back into the frame afterwards. An earlier
 * version borrowed only x18 and left x9 and x10 holding the pad address and
 * the stride, which meant every exception returned to the code it interrupted
 * with two of its registers replaced by ours. It went unnoticed while the
 * payload was only entered by a call; the first time an exception was claimed
 * and resumed, the kernel faulted two instructions later.
 */

#include "layout_defs.inc"

/* Where the frame layout starts, for readability below. */
#define X0 US_FRAME_X0

    .text

/*
 * Entered at EL1 with SP set to the interrupted stack.
 *
 * Three things happen, in this order, and the order is the point:
 *
 *   1. interrupts are masked, so nothing arrives while the state is half saved
 *   2. the interrupted context goes into this CPU's landing pad
 *   3. the stack is switched to one the payload owns, before anything is called
 *
 * The third step is what makes the handler analysable. On the interrupted
 * stack, how much room is left is unknown, and a handler that overflows it
 * corrupts whatever it interrupted in a way that surfaces far away. The pads
 * are per CPU because the exception can arrive on any of them.
 *
 * Everything is addressed PC relatively, which is all a position independent
 * blob may use.
 *
 * There are two of these, one per synchronous vector, because the vectors
 * differ in the one thing that decides whether the registers this code spends
 * can be given back: whether there is a stack.
 *
 *   SP0  execution was using SP_EL0, so the exception left that stack in SP,
 *        and the stub that reached here has already pushed x16, x17 and x18
 *        onto it. This form reads them back and returns them.
 *   SPx  SP holds whatever SP_EL1 did, which is not a stack to push on. The
 *        stub pushed nothing, so there is nothing to read and the frame
 *        records zeros for those three.
 *
 * The three are the ones this code spends, and on the SP0 vector they are the
 * kernel's: its breakpoint services carry their number in x16 and the handler
 * this hands to reads it from there. Spending them without giving them back
 * turns every one of those into a service that does not exist.
 */
    .macro UsSyncEntry name, save
    .globl \name
    .type \name, %function
    .balign 16
\name:
    msr     daifset, #0xF

    /*
     * This code runs before there is a stack, and that is not a figure of
     * speech. At the vector it was entered through, SP holds whatever the
     * kernel left in it -- not a stack the kernel was using -- and once the
     * address space has been rebuilt that value is not mapped. A push here is
     * a fault taken inside the exception it was meant to handle, and the
     * fault arrives at the same instruction, until the machine resets.
     *
     * So: registers only, until a stack of our own has been switched to.
     *
     * x16, x17 and x18 are the bootstrap. None of them can be live across one
     * of these exceptions, and the proof is the platform's own handler for
     * the same vector: it writes x18 before saving anything and calls
     * functions, which destroys x16 and x17. A register the platform's own
     * path does not preserve cannot be holding anything that has to survive
     * one. The same is true of SP, which that handler discards.
     *
     * The first memory touched is the landing pad, and its address is worked
     * out without a stack: everything below is PC relative.
     */
    adrp    x16, usPayloadConfigBlock
    add     x16, x16, #:lo12:usPayloadConfigBlock
    add     x16, x16, #US_CONFIG_CPUS

    /*
     * Which processor this is.
     *
     * Not the low byte of MPIDR_EL1: that is Aff0, the core within a cluster,
     * so the first core of every cluster has the same one and two processors
     * would share a landing pad. The list comes from the firmware and gives
     * each processor a position of its own.
     *
     * The register is normalised first. MPIDR_EL1 carries a bit saying the
     * value is multiprocessor capable, a bit saying the cluster is
     * multithreaded, and a reserved bit that reads as one; ACPI's copy of the
     * value has all three as zero, so the comparison is made on what both
     * sides agree on. The instruction clears bits 24 to 31, which is where
     * all three live.
     *
     * The list ends with an entry whose index is all ones, which is not a
     * position any processor can have.
     */
    mrs     x17, mpidr_el1
    and     x17, x17, #US_MPIDR_MASK
\name\()Cpu:
    ldr     x18, [x16, #US_CPU_INDEX]
    cmn     x18, #1
    b.eq    \name\()NoStack
    ldr     x18, [x16]
    cmp     x18, x17
    b.eq    \name\()Found
    add     x16, x16, #US_CPU_STRIDE
    b       \name\()Cpu

\name\()Found:
    /* x18 is this processor's index, and x16 is where it was found. */
    ldr     x18, [x16, #US_CPU_INDEX]
    adrp    x16, usPayloadLanding
    add     x16, x16, #:lo12:usPayloadLanding
    mov     x17, #US_FRAME_STRIDE
    madd    x16, x18, x17, x16

    /*
     * The frame. Every register that is still the interrupted code's is saved;
     * the three the bootstrap used are not, because by now they hold its
     * values rather than the interrupted code's, and the platform does not
     * preserve them across this vector anyway.
     */
    stp     x0, x1, [x16, #US_FRAME_X0]
    stp     x2, x3, [x16, #US_FRAME_X2]
    stp     x4, x5, [x16, #US_FRAME_X4]
    stp     x6, x7, [x16, #US_FRAME_X6]
    stp     x8, x9, [x16, #US_FRAME_X8]
    stp     x10, x11, [x16, #US_FRAME_X10]
    stp     x12, x13, [x16, #US_FRAME_X12]
    stp     x14, x15, [x16, #US_FRAME_X14]
    stp     x19, x20, [x16, #US_FRAME_X19]
    stp     x21, x22, [x16, #US_FRAME_X21]
    stp     x23, x24, [x16, #US_FRAME_X23]
    stp     x25, x26, [x16, #US_FRAME_X25]
    stp     x27, x28, [x16, #US_FRAME_X27]
    str     x29, [x16, #US_FRAME_X29]
    str     x30, [x16, #US_FRAME_X30]

    /*
     * The three this code spends. Where the stub pushed them they are read
     * back out of its area, which is still where it left them because nothing
     * since has touched that memory. Where it did not, there is nothing to
     * read and the frame records zeros, so that the omission is visible rather
     * than accidental.
     */
    .if \save
    ldr     x17, [sp, #US_STUB_SAVE_X18]
    str     x17, [x16, #US_FRAME_X18]
    ldr     x17, [sp, #US_STUB_SAVE_X16]
    str     x17, [x16, #US_FRAME_X16]
    ldr     x17, [sp, #US_STUB_SAVE_X17]
    str     x17, [x16, #US_FRAME_X17]
    .else
    str     xzr, [x16, #US_FRAME_X16]
    str     xzr, [x16, #US_FRAME_X17]
    str     xzr, [x16, #US_FRAME_X18]
    .endif

    mrs     x17, elr_el1
    str     x17, [x16, #US_FRAME_ELR]
    mrs     x17, spsr_el1
    str     x17, [x16, #US_FRAME_SPSR]
    mrs     x17, esr_el1
    str     x17, [x16, #US_FRAME_ESR]
    mrs     x17, far_el1
    str     x17, [x16, #US_FRAME_FAR]

    /*
     * The interrupted SP, read straight out of the register before it is
     * replaced. The CPU does not save SP on the way in, so this is the only
     * way to have it back, and the exception may have interrupted code that
     * was using it -- at the vector this arrives on the kernel's own handler
     * discards it, but that is the kernel's business and not something to
     * assume on its behalf.
     */
    .if \save
    add     x17, sp, #US_STUB_SAVE_BYTES
    .else
    mov     x17, sp
    .endif
    str     x17, [x16, #US_FRAME_SP]

    /* Now a stack of our own, chosen by this processor's index. */
    adrp    x17, usPayloadConfigBlock
    add     x17, x17, #:lo12:usPayloadConfigBlock
    add     x17, x17, #US_CONFIG_STACK_TOP
    ldr     x17, [x17, x18, lsl #3]
    cbz     x17, \name\()NoStack
    mov     sp, x17

    mov     x0, x16
    bl      usPayloadHandle
    cbnz    x0, \name\()Resume

    /*
     * Not ours. Handing it back is what keeps the payload out of the way when
     * it has nothing to say. There is nowhere to hand it to without a
     * forwarding address, and continuing would re-run the instruction that
     * faulted.
     */
    adrp    x10, usPayloadConfigBlock
    add     x10, x10, #:lo12:usPayloadConfigBlock
    ldr     x11, [x10, #US_CONFIG_FORWARD]
    cbz     x11, \name\()NoForward
    mov     x0, x16
    br      x11

\name\()Resume:
    /* The call is free to have used every register, so the pad is found
     * again rather than assumed, by the same register-only lookup. */
    adrp    x16, usPayloadConfigBlock
    add     x16, x16, #:lo12:usPayloadConfigBlock
    add     x16, x16, #US_CONFIG_CPUS
    mrs     x17, mpidr_el1
    and     x17, x17, #US_MPIDR_MASK
\name\()ResumeCpu:
    ldr     x18, [x16, #US_CPU_INDEX]
    cmn     x18, #1
    b.eq    \name\()NoStack
    ldr     x18, [x16]
    cmp     x18, x17
    b.eq    \name\()ResumeFound
    add     x16, x16, #US_CPU_STRIDE
    b       \name\()ResumeCpu

\name\()ResumeFound:
    ldr     x9, [x16, #US_CPU_INDEX]
    adrp    x16, usPayloadLanding
    add     x16, x16, #:lo12:usPayloadLanding
    mov     x17, #US_FRAME_STRIDE
    madd    x16, x9, x17, x16
    mov     x9, x16

    ldr     x10, [x9, #US_FRAME_ELR]
    msr     elr_el1, x10
    ldr     x10, [x9, #US_FRAME_SPSR]
    msr     spsr_el1, x10

    /*
     * x9 addresses the frame, so it is the last register restored. Everything
     * else comes back first, and SP is put back before the final load: from
     * then on the payload's stack is gone and only the interrupted one is
     * addressable, which is where the landing pad still is.
     *
     * x10 is restored twice, and the second time is the one that counts: the
     * first is overwritten by the SP restore, which needs a register of its
     * own and can only use one that has already been restored. Leaving it at
     * the first restore returned every exception with x10 holding the
     * interrupted SP.
     */
    ldp     x0, x1, [x9, #US_FRAME_X0]
    ldp     x2, x3, [x9, #US_FRAME_X2]
    ldp     x4, x5, [x9, #US_FRAME_X4]
    ldp     x6, x7, [x9, #US_FRAME_X6]
    ldr     x8, [x9, #US_FRAME_X8]
    ldp     x12, x13, [x9, #US_FRAME_X12]
    ldp     x14, x15, [x9, #US_FRAME_X14]
    /*
     * The three the entry spends come back where there was anywhere to keep
     * them, which is the vector whose stub pushed them. On the other one the
     * frame holds zeros and nothing is restored, because there was no stack to
     * have put them on.
     */
    .if \save
    ldr     x16, [x9, #US_FRAME_X16]
    ldr     x17, [x9, #US_FRAME_X17]
    ldr     x18, [x9, #US_FRAME_X18]
    .endif
    ldp     x19, x20, [x9, #US_FRAME_X19]
    ldp     x21, x22, [x9, #US_FRAME_X21]
    ldp     x23, x24, [x9, #US_FRAME_X23]
    ldp     x25, x26, [x9, #US_FRAME_X25]
    ldp     x27, x28, [x9, #US_FRAME_X27]
    ldr     x29, [x9, #US_FRAME_X29]
    ldr     x30, [x9, #US_FRAME_X30]

    ldr     x10, [x9, #US_FRAME_SP]
    mov     sp, x10
    ldp     x9, x10, [x9, #US_FRAME_X9]

    eret

/*
 * Where the payload cannot go on: a CPU with no stack of its own, or an
 * exception with nowhere to be forwarded to. Stopped, rather than carried on
 * with: running the handler on a stack whose size is not known is the failure
 * all of this exists to avoid.
 *
 * The mark is written only when the console is usable. In kernel context it
 * is not, and a write to it faults -- from inside an exception handler, which
 * means the fault arrives here again, and again, until the machine resets.
 * That is a loud failure turning into a silent one, which is the opposite of
 * what the mark is for.
 */
\name\()NoStack:
    /*
     * A mark goes into the pool before stopping. Without it this path is
     * indistinguishable from the machine simply dying, and "no stack for this
     * processor" is a different fault from "nothing was ever entered" -- the
     * one says the lookup failed, the other says the payload was never
     * reached at all.
     */
    adrp    x10, usPayloadConfigBlock
    add     x10, x10, #:lo12:usPayloadConfigBlock
    ldr     x11, [x10, #US_CONFIG_POOL]
    cbz     x11, \name\()Marked
    movz    x12, #0x5354
    movk    x12, #0x4143, lsl #16
    movk    x12, #0x4f4e, lsl #32
    movk    x12, #0x4f4e, lsl #48
    str     x12, [x11, #8]
\name\()Marked:
    ldr     x11, [x10, #US_CONFIG_QUIET]
    cbnz    x11, \name\()Halt
    ldr     x11, [x10, #US_CONFIG_UART]
    cbz     x11, \name\()Halt
    mov     w12, #'!'
    str     w12, [x11]
    b       \name\()Halt

\name\()NoForward:
    /* Nothing to forward to. Stopping here is deliberate: returning would
     * re-run the instruction that faulted. */
\name\()Halt:
    wfi
    b       \name\()Halt

    .size \name, . - \name
    .endm

    /*
     * The SPx one keeps the name the rest of the payload refers to it by, and
     * the SP0 one is the second instantiation of the same code.
     */
    UsSyncEntry usSyncEntry, 0
    UsSyncEntry usSyncEntrySp0, 1
