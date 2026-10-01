/*
 * The end of the payload, and the configuration block's position.
 *
 * The blob is copied as a byte range, so it needs a symbol that marks its
 * end: without one, the linker is free to put something after it and the copy
 * either misses it or takes memory that is not ours.
 */

    .text

    .globl usPayloadEnd
usPayloadEnd:
    .quad 0
