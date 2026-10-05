/*
 * Getting the runtime pool from firmware memory
 *
 * The pool has to survive the handover to the kernel, which rules out the
 * memory types the OS is free to reuse. Runtime services memory is the class
 * the firmware and the OS both agree to keep, so that is what is asked for.
 * The OS maps it non-executable, which is correct for data and stacks and is
 * only a problem for code, which is why code gets its own allocation
 *
 * Only the allocation happens here; the layout is worked out in core/pool.c,
 * which is where it can be tested without a firmware
 */

#ifndef US_POOL_ALLOC_H
#define US_POOL_ALLOC_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"

typedef struct UsPoolAlloc_t {
    UsPool  *pool;
    uint64_t basePA;
    uint64_t baseVA;
    uint64_t bytes;
} UsPoolAlloc;

/*
 * Allocates and lays out the pool where the page tables map it identity, and
 * reports both addresses so the injector can map it again later
 *
 * Returns false when the firmware refuses, which the caller must treat as
 * fatal: without a pool there is no stack to run on, and no handler
 */
bool usPoolAllocate(UsPoolAlloc *out);

#endif
