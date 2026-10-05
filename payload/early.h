#ifndef US_EARLY_H
#define US_EARLY_H

#include <stdint.h>

/* Migrate only the runtime regions supplied by the UEFI notification */
bool usPayloadEarly(void);

/* Point every stub at the payload's runtime address, from the handover. The
 * kernel's runtime base is the loader block's, or zero when it named none */
void usPayloadPublishAll(uint64_t kernelBase);

/* Publish pending stubs in the image identified by the current VBAR */
bool usPayloadPublish(uint64_t vbar);

/*
 * Where the tail of the slot an exception came through is: the slot's
 * original instruction followed by the branch past it, which together are the
 * entry of the handler the kernel wrote there. Branching to it hands the
 * exception to that handler in the state a real entry would have left
 *
 * The slot is the one the interrupted SPSR names, and the stub it is found
 * through is the one the boot left at the end of that slot's chain
 */
bool usPayloadSlotTail(uint64_t vbar, uint64_t spsr, uint64_t *tail);

#endif
