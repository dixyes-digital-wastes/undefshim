/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Laying out the runtime pool, see pool.h for why this is separate
 */

// us-build: both

#include "core/pool.h"

/*
 * Zeroed a word at a time rather than with memset, because this file is on
 * both sides of the handover: the boot has a libc, the payload has none, and a
 * header that is not there is what makes that a build error in the one place
 * that does not have it. The loop is what the compiler would emit anyway
 */
static void zeroPool(UsPool *pool) {
    uint64_t *words = (uint64_t *)(void *)pool;

    for (size_t i = 0; i < sizeof(*pool) / sizeof(*words); i++) {
        words[i] = 0;
    }
}

bool usPoolInitLayout(UsPool *pool, uint64_t baseVA, uint64_t basePA,
                      uint32_t stackSlots) {
    if (pool == NULL) {
        return false;
    }
    if ((baseVA & (US_PAGE_SIZE - 1)) != 0 || (basePA & (US_PAGE_SIZE - 1)) != 0) {
        return false;
    }
    /*
     * At least the processor this is running on, and no more than the header
     * has room to describe. A pool with no stacks is one nothing can be run
     * on, and a count past the array is a header that claims slots that are
     * not in it
     */
    if (stackSlots == 0 || stackSlots > US_MAX_CPUS) {
        return false;
    }

    zeroPool(pool);
    pool->magic = US_POOL_MAGIC;
    pool->stackSlots = stackSlots;
    pool->selfPA = basePA;
    pool->selfVA = baseVA;

    for (uint32_t i = 0; i < stackSlots; i++) {
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

bool usPoolRecordFB(UsPool *pool, uint64_t pa, uint64_t bytes, uint32_t width,
                    uint32_t height, uint32_t stride, bool blueFirst) {
    if (pool == NULL || pa == 0 || bytes == 0 || width == 0 || height == 0
        || stride < width) {
        return false;
    }
    pool->fb = (UsPoolFB){
        .magic = US_POOL_FB_MAGIC,
        .pa = pa,
        .bytes = bytes,
        .width = width,
        .height = height,
        .stride = stride,
        .blueFirst = blueFirst ? 1U : 0U,
    };
    return true;
}

void usPoolFBSet(UsPool *pool, uint64_t aliasVA, uint64_t blocks,
                 uint64_t attrIndex) {
    if (pool == NULL || pool->fb.magic != US_POOL_FB_MAGIC) {
        return;
    }
    pool->fb.aliasVA = aliasVA;
    pool->fb.blocks = blocks;
    pool->fb.attrIndex = attrIndex;
    pool->fb.reason = US_FB_OK;
    pool->fb.detail = 0;
}

/*
 * Says why there is no mapping, in the record the host can read
 *
 * A refusal on this path is silent by nature: nothing can be reported at the
 * time it happens, because the thing being built is the only way left to
 * report anything. So the reason is written where a reader will look, and it
 * is written even on the paths that had no frame buffer to begin with, so that
 * "the boot never looked" and "this build refused" stay different answers
 */
void usPoolFBRefuse(UsPool *pool, uint64_t reason, uint64_t detail) {
    if (pool == NULL || pool->fb.magic != US_POOL_FB_MAGIC) {
        return;
    }
    pool->fb.reason = reason;
    pool->fb.detail = detail;
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
