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
 * blob may use. Registers have to be spent on that address before there is
 * anywhere to store them, so thirty two bytes of the interrupted stack are
 * borrowed and given straight back. That is the smallest use of someone
 * else's stack that works.
 */
    .globl usSyncEntry
    .type usSyncEntry, %function
    .balign 16
usSyncEntry:
    msr     daifset, #0xF

    /*
     * Borrow on the interrupted stack, because nothing can be stored until
     * the frame has been found and finding it takes these three registers.
     *
     * The layout matters and is easy to get wrong: two pairs and a single do
     * not line up. Written this way the three land at sp+0, sp+8 and sp+16,
     * with nothing in between them, so the reads below are one offset apart
     * and there is no slot that is never written. An earlier version pushed
     * x18, x9 and then x10, which left x18 and x9 one slot further along than
     * the reads expected: x9 was read back holding x18, x18 was read back
     * holding whatever was under the stack, and the real x9 was never read at
     * all. Every exception returned with two registers of the interrupted
     * code replaced.
     *
     * The thunk below the vector slot has already pushed x16, sixteen bytes
     * below the interrupted SP, which is why the interrupts SP is sp+48. That
     * push is part of this arrangement, not a separate one: x16 is where the
     * thunk carries the payload's address, so it is the one register the
     * thunk destroys and this code is what puts it back.
     */
    stp     x9, x10, [sp, #-16]!
    str     x18, [sp, #-16]!

    mrs     x18, mpidr_el1
    and     x18, x18, #0xFF
    adrp    x9, usPayloadLanding
    add     x9, x9, #:lo12:usPayloadLanding
    mov     x10, #US_FRAME_STRIDE
    madd    x9, x18, x10, x9

    stp     x0, x1, [x9, #US_FRAME_X0]
    stp     x2, x3, [x9, #US_FRAME_X2]
    stp     x4, x5, [x9, #US_FRAME_X4]
    stp     x6, x7, [x9, #US_FRAME_X6]
    str     x8, [x9, #US_FRAME_X8]
    str     x11, [x9, #US_FRAME_X11]
    stp     x12, x13, [x9, #US_FRAME_X12]
    stp     x14, x15, [x9, #US_FRAME_X14]
    str     x17, [x9, #US_FRAME_X17]
    stp     x19, x20, [x9, #US_FRAME_X19]
    stp     x21, x22, [x9, #US_FRAME_X21]
    stp     x23, x24, [x9, #US_FRAME_X23]
    stp     x25, x26, [x9, #US_FRAME_X25]
    stp     x27, x28, [x9, #US_FRAME_X27]
    str     x29, [x9, #US_FRAME_X29]
    str     x30, [x9, #US_FRAME_X30]

    /* The borrowed three, from where they were put. Two pairs and a single do
     * not pack, so the offsets are what the pushes above actually produce:
     * x18 at sp+0, x9 at sp+16, x10 at sp+24, with a slot between them that
     * nothing writes and nothing reads. */
    ldr     x18, [sp]
    str     x18, [x9, #US_FRAME_X18]
    ldr     x11, [sp, #16]
    str     x11, [x9, #US_FRAME_X9]
    ldr     x10, [sp, #24]
    str     x10, [x9, #US_FRAME_X10]

    /* And x16, which the thunk saved on its way in. */
    ldr     x11, [sp, #32]
    str     x11, [x9, #US_FRAME_X16]

    /* The interrupted SP is the one thing the CPU did not keep for us, and
     * SP cannot be the source of a store, so it goes through a register.
     * What stands between it and this SP is the thunk's sixteen bytes and
     * this code's thirty two. */
    add     x11, sp, #48
    str     x11, [x9, #US_FRAME_SP]

    mrs     x10, elr_el1
    str     x10, [x9, #US_FRAME_ELR]
    mrs     x10, spsr_el1
    str     x10, [x9, #US_FRAME_SPSR]
    mrs     x10, esr_el1
    str     x10, [x9, #US_FRAME_ESR]
    mrs     x10, far_el1
    str     x10, [x9, #US_FRAME_FAR]

    /* Now the payload's own stack, chosen by this CPU's affinity. */
    mrs     x11, mpidr_el1
    and     x11, x11, #0xFF
    adrp    x10, usPayloadConfigBlock
    add     x10, x10, #:lo12:usPayloadConfigBlock
    add     x10, x10, #US_CONFIG_STACK_TOP
    ldr     x12, [x10, x11, lsl #3]
    cbz     x12, usSyncNoStack
    mov     sp, x12

    mov     x0, x9
    bl      usPayloadHandle
    cbnz    x0, usSyncResume

    /* Not ours. Handing it back is what keeps the payload out of the way when
     * it has nothing to say; there is nowhere to hand it to without a
     * forwarding address, and continuing would re-run the faulting
     * instruction and loop. */
    adrp    x10, usPayloadConfigBlock
    add     x10, x10, #:lo12:usPayloadConfigBlock
    ldr     x11, [x10, #US_CONFIG_FORWARD]
    cbz     x11, usSyncNoForward
    mov     x0, x9
    br      x11

usSyncResume:
    /* The call is free to have used every register, so the pad is found
     * again rather than assumed. */
    mrs     x11, mpidr_el1
    and     x11, x11, #0xFF
    adrp    x9, usPayloadLanding
    add     x9, x9, #:lo12:usPayloadLanding
    mov     x10, #US_FRAME_STRIDE
    madd    x9, x11, x10, x9

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
    ldp     x16, x17, [x9, #US_FRAME_X16]
    ldp     x18, x19, [x9, #US_FRAME_X18]
    ldp     x20, x21, [x9, #US_FRAME_X20]
    ldp     x22, x23, [x9, #US_FRAME_X22]
    ldp     x24, x25, [x9, #US_FRAME_X24]
    ldp     x26, x27, [x9, #US_FRAME_X26]
    ldp     x28, x29, [x9, #US_FRAME_X28]
    ldr     x30, [x9, #US_FRAME_X30]

    ldr     x10, [x9, #US_FRAME_SP]
    mov     sp, x10
    ldp     x9, x10, [x9, #US_FRAME_X9]

    eret

/*
 * Where the payload cannot go on. Reported on the serial port and then
 * stopped: there is no stack to unwind to, and carrying on would run the
 * handler somewhere whose size is not known, which is the failure all of this
 * exists to avoid. A silent stop would be worse than a loud one.
 */
usSyncNoStack:
    adrp    x10, usPayloadConfigBlock
    add     x10, x10, #:lo12:usPayloadConfigBlock
    ldr     x10, [x10, #US_CONFIG_UART]
    cbz     x10, usSyncHalt
    mov     x11, x10
    mov     w12, #'!'
    str     w12, [x11]
    b       usSyncHalt

usSyncNoForward:
    /* Nothing to forward to. Stopping here is deliberate: returning would
     * re-run the instruction that faulted. */
usSyncHalt:
    wfi
    b       usSyncHalt

    .size usSyncEntry, . - usSyncEntry
