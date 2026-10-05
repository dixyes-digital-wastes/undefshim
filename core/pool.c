/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Laying out the runtime pool, see pool.h for why this is separate
 */

#include <string.h>

#include "core/pool.h"

bool usPoolInitLayout(UsPool *pool, uint64_t baseVA, uint64_t basePA) {
    if (pool == NULL) {
        return false;
    }
    if ((baseVA & (US_PAGE_SIZE - 1)) != 0 || (basePA & (US_PAGE_SIZE - 1)) != 0) {
        return false;
    }

    memset(pool, 0, sizeof(*pool));
    pool->magic = US_POOL_MAGIC;
    pool->stackSlots = US_MAX_CPUS;
    pool->selfPA = basePA;
    pool->selfVA = baseVA;

    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        /*
         * The top is the first byte past the stack, which is where a full
         * descending stack starts. Stacks grow down, so the top is aligned and
         * the first push lands inside the slot
         */
        uint64_t top = baseVA + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE;
        pool->stackTop[i] = top & ~(uint64_t)(US_STACK_ALIGN - 1);
    }

    return true;
}

uint64_t usPoolStackTop(const UsPool *pool, uint32_t slot) {
    if (pool == NULL || slot >= US_MAX_CPUS) {
        return 0;
    }
    return pool->stackTop[slot];
}

bool usPoolIsValid(const UsPool *pool) {
    if (pool == NULL || pool->magic != US_POOL_MAGIC) {
        return false;
    }
    if (pool->stackSlots == 0 || pool->stackSlots > US_MAX_CPUS) {
        return false;
    }
    if ((pool->selfPA & (US_PAGE_SIZE - 1)) != 0) {
        return false;
    }
    if (pool->selfVA == 0) {
        return false;
    }

    /*
     * Every advertised slot has to be exactly where the layout says it is.
     * Checking against the pool's own address rather than against each other
     * means a partially written or misrelocated header is caught rather than
     * accepted as long as it is monotonic
     */
    for (uint32_t i = 0; i < pool->stackSlots; i++) {
        uint64_t want = (pool->selfVA + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE)
                        & ~(uint64_t)(US_STACK_ALIGN - 1);

        if (pool->stackTop[i] != want) {
            return false;
        }
    }

    /* No stack may cover the header it is described by */
    if (pool->stackTop[0] < pool->selfVA + US_POOL_HEADER_SIZE) {
        return false;
    }

    return true;
}
