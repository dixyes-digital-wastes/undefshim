/* Save and resume one synchronous exception on the CPU's own stack */

#include "layout_defs.inc"
#include "core/stackgen.h"

/* The encoding of `msr pan, #1`, for the reason core/pan.h gives. */
#define US_MSR_PAN_ON 0xd500419f

    .text
    .balign 16
    .globl usSyncEntry
    .globl usSyncEntrySp0
    .type usSyncEntry, %function
    .type usSyncEntrySp0, %function
usSyncEntry:
usSyncEntrySp0:
    msr     daifset, #0xF
    b       usStackLookup

    .globl usSyncStackReady
usSyncStackReady:
    /*
     * Whether the exception was taken while the payload was already running.
     *
     * The entry switches this CPU's own stack in, so a fault inside the
     * payload arrives with SP already inside this CPU's slot. Nothing else
     * runs there, so the comparison is what tells the two apart, and it has
     * to be made before the frame is built: building it would write over the
     * frame of the exception the payload is in the middle of handling.
     *
     * x18 is the one register the entry may spend here. Every other one is
     * the interrupted context and is about to be stored into the frame.
     */
    sub     x18, x18, #US_STACK_SIZE
    cmp     sp, x18
    /* The add does not touch the flags, so the comparison survives it and
     * x18 is the stack top again on every path. */
    add     x18, x18, #US_STACK_SIZE
    b.lo    usSyncOwnStack
    cmp     sp, x18
    b.lo    usSyncFaultInside

usSyncOwnStack:
    /* Exchange the entry SP with the stack top without another scratch */
    add     sp, sp, x18
    sub     x18, sp, x18
    sub     sp, sp, x18
    str     x18, [sp, #-16]!
    sub     sp, sp, #US_FRAME_STRIDE

    stp     x0, x1, [sp, #US_FRAME_X0]
    stp     x2, x3, [sp, #US_FRAME_X2]
    stp     x4, x5, [sp, #US_FRAME_X4]
    stp     x6, x7, [sp, #US_FRAME_X6]
    stp     x8, x9, [sp, #US_FRAME_X8]
    stp     x10, x11, [sp, #US_FRAME_X10]
    stp     x12, x13, [sp, #US_FRAME_X12]
    stp     x14, x15, [sp, #US_FRAME_X14]
    stp     x16, x17, [sp, #US_FRAME_X16]
    stp     x19, x20, [sp, #US_FRAME_X19]
    stp     x21, x22, [sp, #US_FRAME_X21]
    stp     x23, x24, [sp, #US_FRAME_X23]
    stp     x25, x26, [sp, #US_FRAME_X25]
    stp     x27, x28, [sp, #US_FRAME_X27]
    stp     x29, x30, [sp, #US_FRAME_X29]
    mov     x16, sp

    mrs     x17, spsr_el1
    str     x17, [x16, #US_FRAME_SPSR]

    /*
     * Where the interrupted x18 comes from depends on the exception level.
     *
     * In kernel mode it is the per-CPU block, and TPIDR_EL1 holds its
     * address: rebuilding it is exactly what the kernel's own handlers do,
     * costs no stack, and is what makes an entry whose SP_EL1 is stale
     * harmless. At EL0 it is user state that cannot be rebuilt, so the stub
     * for that vector pushed it below the entry SP_EL1 - which is a kernel
     * stack there, as the kernel's own entry for that vector also assumes -
     * and this reads it back and puts SP the way it found it.
     */
    mrs     x17, spsr_el1
    and     x17, x17, #0xF
    cbz     x17, usSyncSavedX18
    mrs     x18, tpidr_el1
    and     x18, x18, #0xfffffffffffff000
    b       usSyncHaveX18

usSyncSavedX18:
    ldr     x17, [x16, #US_FRAME_STRIDE]
    ldr     x18, [x17]
    add     x17, x17, #16
    str     x17, [x16, #US_FRAME_STRIDE]

usSyncHaveX18:
    str     x18, [x16, #US_FRAME_X18]

    /*
     * The interrupted stack: at EL1h that is the entry's own SP_EL1, and
     * anywhere else it is SP_EL0, which is what the frame records for the
     * emulation to use.
     */
    ldr     x17, [x16, #US_FRAME_STRIDE]
    mrs     x18, spsr_el1
    and     x18, x18, #0xF
    cmp     x18, #5
    b.eq    usSyncHaveSp
    mrs     x17, sp_el0
usSyncHaveSp:
    str     x17, [x16, #US_FRAME_SP]

    mrs     x17, elr_el1
    str     x17, [x16, #US_FRAME_ELR]
    mrs     x17, esr_el1
    str     x17, [x16, #US_FRAME_ESR]
    mrs     x17, far_el1
    str     x17, [x16, #US_FRAME_FAR]

    mov     x19, x16
    mov     x0, x19
    bl      usPayloadHandle
    cbz     x0, usSyncNoForward
    /* Zero flag set means the frame is not to be resumed but handed to the
     * handler that was at the slot's original address; see usSyncLanding. */
    cmp     x0, #2
    b       usSyncRestore

    /*
     * A fault on the payload's own stack. The payload decides whether it is
     * the emulated access that faulted - which is the exception the kernel
     * has to finish, in the context it was originally taken from - or
     * something of ours, which stays a fault inside a handler. Either way it
     * answers with the frame to restore.
     */
usSyncFaultInside:
    /*
     * The handler may clobber the caller-saved registers, and one of its
     * answers is "carry on where you were": that answer has to leave the
     * handler's own registers as they were, so they are spilled first. The
     * payload's stack is the one in use here, and this is a bounded amount of
     * it.
     */
    sub     sp, sp, #US_FAULT_SPILL_BYTES
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    str     x18, [sp, #144]
    str     x30, [sp, #152]

    bl      usPayloadFault
    cmp     x0, #1
    b.eq    usSyncProbeAnswer

    add     sp, sp, #US_FAULT_SPILL_BYTES
    cbz     x0, usSyncNoForward
    mov     x19, x0
    /* A real entry to EL1 leaves PAN set, and the branch below does not go
     * through eret, where the interrupted state lives. */
    .inst   US_MSR_PAN_ON
    cmp     xzr, xzr
    b       usSyncRestore

    /*
     * A fault the handler asked to be told about: it has already moved ELR
     * past the instruction that faulted, and the payload continues there with
     * its own registers, since the exception was taken with them still live.
     */
usSyncProbeAnswer:
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldr     x18, [sp, #144]
    ldr     x30, [sp, #152]
    add     sp, sp, #US_FAULT_SPILL_BYTES
    eret

    /*
     * One restore, two exits: eret resumes the interrupted code, and the
     * branch hands the exception to the kernel's own handler with every
     * register as the interrupted code left it. Which exit is taken is the
     * zero flag, because it is the only state the restore itself does not
     * touch - every instruction below is a load, a move or a system register
     * write.
     */
usSyncRestore:
    mov     x9, x19
    ldr     x10, [x9, #US_FRAME_ELR]
    msr     elr_el1, x10
    ldr     x10, [x9, #US_FRAME_SPSR]
    msr     spsr_el1, x10

    ldp     x0, x1, [x9, #US_FRAME_X0]
    ldp     x2, x3, [x9, #US_FRAME_X2]
    ldp     x4, x5, [x9, #US_FRAME_X4]
    ldp     x6, x7, [x9, #US_FRAME_X6]
    ldr     x8, [x9, #US_FRAME_X8]
    ldr     x11, [x9, #US_FRAME_X11]
    ldp     x12, x13, [x9, #US_FRAME_X12]
    ldp     x14, x15, [x9, #US_FRAME_X14]
    ldp     x16, x17, [x9, #US_FRAME_X16]
    ldp     x19, x20, [x9, #US_FRAME_X19]
    ldp     x21, x22, [x9, #US_FRAME_X21]
    ldp     x23, x24, [x9, #US_FRAME_X23]
    ldp     x25, x26, [x9, #US_FRAME_X25]
    ldp     x27, x28, [x9, #US_FRAME_X27]
    ldp     x29, x30, [x9, #US_FRAME_X29]

    /* Restore the entry bank independently of the interrupted SP */
    ldr     x10, [x9, #US_FRAME_STRIDE]
    mov     sp, x10
    b.eq    usSyncLanding
    ldr     x18, [x9, #US_FRAME_X18]
    ldp     x9, x10, [x9, #US_FRAME_X9]
    eret

    /*
     * The tail of the slot the exception was taken through: its original
     * instruction and the branch past it. Reaching it this way is the same
     * state the handler would have been entered in, except that the branch
     * does not change the exception level, which is why the zero flag is used
     * rather than a return.
     *
     * x18 carries the address. That register is the one the kernel rebuilds
     * for itself at both of its entries (the thread's stack from sp_el0, or
     * the per-CPU block from tpidr_el1), so it is the one that can be spent.
     */
usSyncLanding:
    ldr     x18, [x9, #US_FRAME_LANDING]
    ldp     x9, x10, [x9, #US_FRAME_X9]
    br      x18

    /*
     * Both of these end in a halt, and a halt with nothing written down is a
     * machine that stopped for no visible reason. What the entry knew is left
     * for the host first.
     */
    .globl usSyncNoStack
usSyncNoStack:
    mov     x0, #2
    bl      usPayloadStuck
    wfi
    b       usSyncNoStack
usSyncNoForward:
    mov     x0, #1
    bl      usPayloadStuck
    wfi
    b       usSyncNoForward

    .size   usSyncEntry, . - usSyncEntry
    .size   usSyncEntrySp0, . - usSyncEntrySp0

    .balign 16
    .globl usStackLookup
    .type usStackLookup, %object
usStackLookup:
    .fill US_STACK_LOOKUP_WORDS, 4, 0xd503201f
    .size usStackLookup, . - usStackLookup
