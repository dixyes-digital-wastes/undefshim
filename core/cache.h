/* AArch64 instruction publication, shared by boot and runtime code */
#ifndef US_CACHE_H
#define US_CACHE_H

#include <stddef.h>
#include <stdint.h>

void usCacheFlushRange(const void *addr, size_t len);
void usCacheSync(void);

#endif
