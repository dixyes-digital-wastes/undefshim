/*
 * The stub that runs where the loader hands over to the kernel.
 *
 * It is copied into a spare slot in the loader's own code, and the branch that
 * used to go to the kernel is pointed at it. From there it calls into the
 * payload and then continues to the kernel exactly as the loader would have.
 *
 * This is the one moment where both the old and the new address space are
 * live: the pool is still reachable at its boot address, and the tables that
 * describe where it will end up have already been built. That is why the work
 * is done here and nowhere else.
 *
 * Registers.
 *
 * The kernel entry is about to be reached, and it receives whatever the
 * loader had arranged. Two of those are not recoverable if this stub damages
 * them: x0, which is the argument the loader prepared at the instruction
 * before the branch, and x20, which is the entry point itself and is what the
 * stub branches to when it is finished. Neither can be left to the calling
 * convention, because this is not a call.
 *
 *   saved here      x0-x18, x30   - the argument, and everything the payload
 *                                   is allowed to clobber, and the link
 *                                   register the call below overwrites
 *   not saved       x19-x29       - callee saved, so the payload preserves
 *                                   them by the convention it is compiled to
 *
 * Sixteen registers would not fit in the slot; twenty do. x30 is paired with
 * x18 rather than given its own slot, which is what makes the count come out
 * even.
 *
 * The target address is a literal at the end, filled in by the boot once it
 * knows where the payload has been placed. It is read through a register
 * rather than branched to directly: the payload is nowhere near the loader,
 * and a direct branch only reaches 128 MB.
 */

    .text

    .globl usTransferStub
    .type usTransferStub, %function
    .balign 16
usTransferStub:
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x30, [sp, #-16]!

    /* The payload entry, as placed. */
    ldr     x16, usTransferTarget
    mov     x0, x20                 /* the kernel entry, for the payload */
    blr     x16

    ldp     x18, x30, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16

    /* Exactly what was here before: the kernel's entry point, in x20. */
    br      x20

    /*
     * Where the payload is. The boot writes this after copying the stub, and
     * it is the only thing in the stub that is not decided here.
     */
    .balign 8
    .globl usTransferTarget
usTransferTarget:
    .quad   0

    .globl usTransferStubEnd
usTransferStubEnd:
    .size usTransferStub, . - usTransferStub
