/*
 * Cache maintenance, see cache.h for why this is not optional.
 *
 * The line size is read from the system registers rather than assumed: it is
 * implementation defined, and cleaning the wrong granularity leaves stale
 * instructions behind.
 */

#include "core/cache.h"

static uint32_t dataCacheLineSize(void) {
    uint64_t ctr;

    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    /* CTR_EL0.DminLine is bits 19:16, as a log2 of words. */
    return 4U << ((ctr >> 16) & 0xFU);
}

static uint32_t instructionCacheLineSize(void) {
    uint64_t ctr;

    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    /* CTR_EL0.IminLine is bits 3:0. */
    return 4U << (ctr & 0xFU);
}

void usCacheFlushRange(const void *addr, size_t len) {
    uintptr_t start;
    uintptr_t end;
    uint32_t dline;
    uint32_t iline;

    if (len == 0) {
        return;
    }

    dline = dataCacheLineSize();
    iline = instructionCacheLineSize();

    start = (uintptr_t)addr & ~(uintptr_t)(dline - 1);
    end = ((uintptr_t)addr + len + dline - 1) & ~(uintptr_t)(dline - 1);
    for (uintptr_t p = start; p < end; p += dline) {
        /* Clean and invalidate to the point of coherence: the new bytes have
         * to leave this core's cache before another core, or the instruction
         * fetcher, can be trusted to see them. */
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb ish" ::: "memory");

    start = (uintptr_t)addr & ~(uintptr_t)(iline - 1);
    end = ((uintptr_t)addr + len + iline - 1) & ~(uintptr_t)(iline - 1);
    for (uintptr_t p = start; p < end; p += iline) {
        __asm__ __volatile__("ic ivau, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb ish" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

void usCacheSync(void) {
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}
