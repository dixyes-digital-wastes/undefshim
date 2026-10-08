/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * PAN, the state that keeps EL1 out of the pages EL0 is allowed to touch
 *
 * It matters here because an exception entry to EL1 sets it and nothing about
 * the interrupted code's own value survives that: SCTLR_EL1.SPAN was found to
 * be zero, so every exception to EL1 leaves PAN set, whatever the code that
 * was interrupted had. The emulation of an LDAPR has to run with the
 * interrupted code's PAN, or a load of a user page that the kernel was
 * deliberately making succeeds instead comes back as a permission fault that
 * never happened on the hardware this is standing in for
 *
 * PAN is readable, by the `MRS Xt, PAN` the architecture defines, and it is
 * read here for the case the SPSR cannot answer: the payload also runs as a
 * plain call, entered from the middle of the kernel rather than from an
 * exception, and there is no SPSR then. What the call needs is not the
 * interrupted code's value but its own caller's, and the only way to have it
 * is to read it out before changing it and put it back afterwards
 *
 * The mnemonics cannot be written here: the payload is assembled for a generic
 * aarch64 target and PAN is an ARMv8.1 pstate field, so the encodings are
 * spelled out. These are the words those three assemble to under
 * -march=armv8.1-a:
 *
 *   mrs x0, PAN   1101 0101 0011 1000 0100 0010 0110 0000   0xd5384260
 *   msr PAN, #1   1101 0101 0000 0000 0100 0001 1001 1111   0xd500419f
 *   msr PAN, #0   1101 0101 0000 0000 0100 0000 1001 1111   0xd500409f
 */

#ifndef US_PAN_H
#define US_PAN_H

#include <stdint.h>

/* SPSR_EL1.PAN: the state the interrupted code was in */
#define US_SPSR_PAN (UINT64_C(1) << 22)

static inline void usPANOn(void) {
    __asm__ volatile(".inst " "0xd500419f" ::: "memory");
}

static inline void usPANOff(void) {
    __asm__ volatile(".inst " "0xd500409f" ::: "memory");
}

/* Whether PAN is on, which is the state to leave it in when this is done */
static inline bool usPANIsOn(void) {
    uint64_t value;

    __asm__ volatile(".inst " "0xd5384260" "\n\t"
                     "mov %0, x0"
                     : "=r"(value)
                     :
                     : "x0", "memory");
    return (value & 1U) != 0;
}

/* Puts PAN back to what it was, which is not the same as putting it on: the
 * payload is reachable as a call, and its caller's state is the one to keep */
static inline void usPANRestore(bool wasOn) {
    if (wasOn) {
        usPANOn();
    } else {
        usPANOff();
    }
}

#endif
