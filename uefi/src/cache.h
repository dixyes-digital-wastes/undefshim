/*
 * Cache maintenance.
 *
 * On AArch64 the instruction and data caches are not coherent with each
 * other, so writing bytes that are about to be executed requires more than
 * storing them: the data must reach the point of coherence, and the
 * instruction cache must be told the old contents are stale. Getting this
 * wrong produces code that runs correctly until it suddenly does not.
 */

#ifndef US_CACHE_H
#define US_CACHE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Makes a range of written bytes visible to the instruction fetcher. The
 * range does not have to be aligned; the enclosing cache lines are what get
 * cleaned, which is why the whole image does not have to be touched.
 */
void usCacheFlushRange(const void *addr, size_t len);

/* Ensures every prior write is visible before anything that follows. */
void usCacheSync(void);

#endif
