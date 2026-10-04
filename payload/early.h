#ifndef US_EARLY_H
#define US_EARLY_H

#include <stdint.h>

/* Migrate only the runtime regions supplied by the UEFI notification */
bool usPayloadEarly(void);

/* Publish pending stubs in the image identified by the current VBAR */
bool usPayloadPublish(uint64_t vbar);

/*
 * Where the tail of the slot an exception came through is: the slot's
 * original instruction followed by the branch past it, which together are the
 * entry of the handler the kernel wrote there. Branching to it hands the
 * exception to that handler in the state a real entry would have left.
 *
 * Which slot is the one the exception came through is in the SPSR it
 * interrupted: EL1h is the slot at offset 0x200 and everything else is the
 * one at offset zero.
 */
bool usPayloadSlotTail(uint64_t vbar, uint64_t spsr, uint64_t *tail);

#endif
