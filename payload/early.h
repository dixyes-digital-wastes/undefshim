#ifndef US_EARLY_H
#define US_EARLY_H

#include <stdint.h>

/* Migrate only the runtime regions supplied by the UEFI notification */
bool usPayloadEarly(void);

/* Publish pending stubs in the image identified by the current VBAR */
bool usPayloadPublish(uint64_t vbar);

#endif
