/* Save and resume one synchronous exception on the CPU's own stack */

#include "layout_defs.inc"
#include "core/stackgen.h"

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
    and     x17, x17, #0xF
    cmp     x17, #5
    b.eq    usSyncSavedX18
    cbz     x17, usSyncSavedX18

    mrs     x18, tpidr_el1
    and     x18, x18, #0xfffffffffffff000
    ldr     x17, [x16, #US_FRAME_STRIDE]
    b       usSyncHaveX18

usSyncSavedX18:
    /* The stub saved x18 below the entry SP for EL0 and EL1h */
    ldr     x17, [x16, #US_FRAME_STRIDE]
    ldr     x18, [x17]
    add     x17, x17, #16
    str     x17, [x16, #US_FRAME_STRIDE]

usSyncHaveX18:
    str     x18, [x16, #US_FRAME_X18]
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
    ldr     x18, [x9, #US_FRAME_X18]
    ldp     x19, x20, [x9, #US_FRAME_X19]
    ldp     x21, x22, [x9, #US_FRAME_X21]
    ldp     x23, x24, [x9, #US_FRAME_X23]
    ldp     x25, x26, [x9, #US_FRAME_X25]
    ldp     x27, x28, [x9, #US_FRAME_X27]
    ldp     x29, x30, [x9, #US_FRAME_X29]

    /* Restore the entry bank independently of the interrupted SP */
    ldr     x10, [x9, #US_FRAME_STRIDE]
    mov     sp, x10
    ldp     x9, x10, [x9, #US_FRAME_X9]
    eret

    .globl usSyncNoStack
usSyncNoStack:
    wfi
    b       usSyncNoStack
usSyncNoForward:
    wfi
    b       usSyncNoForward

    .size usSyncEntry, . - usSyncEntry
    .size usSyncEntrySp0, . - usSyncEntrySp0

    .balign 16
    .globl usStackLookup
    .type usStackLookup, %object
usStackLookup:
    .fill US_STACK_LOOKUP_WORDS, 4, 0xd503201f
    .size usStackLookup, . - usStackLookup
