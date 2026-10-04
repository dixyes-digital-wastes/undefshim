/*
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
 * PAN cannot be read. The way to learn the interrupted value is the SPSR of
 * the exception, bit 22; the way to set it is MSR PAN. The mnemonic cannot be
 * written here: the payload is assembled for a generic aarch64 target and PAN
 * is an ARMv8.1 pstate field, so the encoding is spelled out. These are the
 * words `msr pan, #1` and `msr pan, #0` assemble to under -march=armv8.1-a:
 *
 *   msr PAN, #1   1101 0101 0000 0000 0100 0001 1001 1111   0xd500419f
 *   msr PAN, #0   1101 0101 0000 0000 0100 0000 1001 1111   0xd500409f
 */

#ifndef US_PAN_H
#define US_PAN_H

#include <stdint.h>

/* SPSR_EL1.PAN: the state the interrupted code was in */
#define US_SPSR_PAN (UINT64_C(1) << 22)

#define US_PAN_SET_ON 0xD500419FU
#define US_PAN_SET_OFF 0xD500409FU

static inline void usPanOn(void) {
    __asm__ volatile(".inst " "0xd500419f" ::: "memory");
}

static inline void usPanOff(void) {
    __asm__ volatile(".inst " "0xd500409f" ::: "memory");
}

#endif
