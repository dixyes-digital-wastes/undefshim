// Switching to one of our own stacks, see stack.h for why
//
// void usStackRunOn(uint64_t stackTop, void (*fn)(void *), void *arg)
//
// The caller's stack pointer and the return address are kept on the caller's
// own stack, so nothing has to be saved anywhere we do not already own. The
// registers this uses are all callee saved, which means the function that runs
// on our stack cannot disturb them

    .text
    .p2align 2
    .globl usStackRunOn
usStackRunOn:
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!

    mov     x19, x0                 // our stack top
    mov     x20, x1                 // the work
    mov     x21, x2                 // its argument
    mov     x22, sp                 // the caller's stack, to come back to

    mov     sp, x19
    mov     x0, x21
    blr     x20

    mov     sp, x22
    ldp     x29, x30, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ret
