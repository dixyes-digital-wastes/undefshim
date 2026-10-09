/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * How much of the frame buffer the mapping has to cover
 *
 * The payload maps the screen into an address of its own choosing so that it
 * can still draw on it once the kernel's tables are in force, and it does that
 * in two megabyte blocks. The frame buffer is not aligned to a block, so the
 * number of them is the rest of the block it starts in plus as many whole ones
 * as its length takes -- and getting that wrong is silent in the way a screen
 * is: the entry is written, every field of the record is right, and the last
 * rows of the screen simply are not there.
 *
 * That is not hypothetical. A machine was checked with a frame buffer of 800
 * by 600 at a physical address 192 kilobytes into its block, one entry was
 * mapped where two were needed, and the six rows past the end of that block
 * faulted as soon as something filled the screen -- which is a fault inside a
 * panic report, on the one path that has nothing left to recover with
 *
 * What is checked here is the arithmetic and the property it has to have, over
 * the screen sizes this is used with. The mapping itself needs page tables and
 * a machine, and is checked where it runs
 */

#include <stdio.h>

#include "payload/fb.h"

#define US_CHECK_NAME "test_fb"
#include "check.h"

static int failures;

/* The screens this is used with, and the one that was actually measured */
static const struct {
    uint32_t width;
    uint32_t height;
    uint64_t pa;
} kScreens[] = {
    { 640U, 480U, 0x138430000ULL },
    { 800U, 600U, 0x138430000ULL },  /* the one that was measured */
    { 1024U, 768U, 0x138430000ULL },
    { 1920U, 1080U, 0x138430000ULL },
    { 3840U, 2160U, 0x138430000ULL },
};

#define SCREEN_COUNT (sizeof(kScreens) / sizeof(kScreens[0]))

/* What the frame buffer is, given the screen and how a row is padded */
static uint64_t bytesOf(uint32_t height, uint32_t stride) {
    return (uint64_t)stride * height * 4U;
}

/*
 * The number of blocks, against the count worked out here
 *
 * Written as a loop rather than as the same formula again: a check that
 * repeats the expression it is checking passes for any expression
 */
static uint64_t blocksByCounting(uint64_t pa, uint64_t bytes) {
    uint64_t first = pa & ~(US_GRANULE_2M - 1U);
    uint64_t seen = 0;

    for (uint64_t at = first; at < pa + bytes; at += US_GRANULE_2M) {
        seen++;
    }
    return seen;
}

static void testCount(void) {
    /* The measured case: it takes two, and one was what the mapping took */
    if (usFBBlocksFor(0x138430000ULL, bytesOf(600U, 800U)) != 2U) {
        usCheckFail("count: an 800x600 screen off the start of its block is "
                    "%llu blocks and not 2\n",
                    (unsigned long long)usFBBlocksFor(
                        0x138430000ULL, bytesOf(600U, 800U)));
        failures++;
    }
    /* Aligned and exactly one block is the one case that is one */
    if (usFBBlocksFor(0x138000000ULL, US_GRANULE_2M) != 1U) {
        usCheckFail("count: a block-aligned screen of one block is not one "
                    "block\n");
        failures++;
    }
    /* One byte past a block boundary starts another */
    if (usFBBlocksFor(0x138000000ULL, US_GRANULE_2M + 1U) != 2U) {
        usCheckFail("count: one byte past a block does not need another\n");
        failures++;
    }
    /* Nothing is nothing, which is what the caller refuses on */
    if (usFBBlocksFor(0x138430000ULL, 0U) != 0U) {
        usCheckFail("count: a frame buffer of no length asked for an entry\n");
        failures++;
    }
}

/*
 * The property, at every screen: what is mapped covers the whole of it
 *
 * This is the one that would have caught the fault. It is asked of the
 * arithmetic rather than of a machine, so it is asked of every screen rather
 * than of the one that happened to be plugged in
 */
static void testCoverage(void) {
    for (size_t i = 0; i < SCREEN_COUNT; i++) {
        /* A row that is padded is a wider frame buffer, and one of these
         * machines pads: the arithmetic has to hold for a padded one too */
        for (uint32_t stride = kScreens[i].width;
             stride <= kScreens[i].width + 32U; stride += 16U) {
            uint64_t bytes = bytesOf(kScreens[i].height, stride);
            uint64_t blocks;
            uint64_t offset;
            uint64_t covered;

            blocks = usFBBlocksFor(kScreens[i].pa, bytes);
            offset = kScreens[i].pa & (US_GRANULE_2M - 1U);
            covered = blocks * US_GRANULE_2M - offset;
            if (covered < bytes) {
                usCheckFail("coverage: %ux%u stride %u needs %llu bytes and "
                            "%llu blocks cover %llu\n",
                            kScreens[i].width, kScreens[i].height, stride,
                            (unsigned long long)bytes,
                            (unsigned long long)blocks,
                            (unsigned long long)covered);
                failures++;
            }
            if (blocks != blocksByCounting(kScreens[i].pa, bytes)) {
                usCheckFail("coverage: %ux%u covers %llu blocks and counts "
                            "%llu\n", kScreens[i].width, kScreens[i].height,
                            (unsigned long long)blocks,
                            (unsigned long long)blocksByCounting(kScreens[i].pa,
                                                                 bytes));
                failures++;
            }
            /* And not more than one block of slack: a mapping longer than the
             * screen is physical memory mapped that nothing asked to see */
            if (covered >= bytes + 2U * US_GRANULE_2M) {
                usCheckFail("coverage: %ux%u is covered with more than a block "
                            "to spare\n", kScreens[i].width,
                            kScreens[i].height);
                failures++;
            }
        }
    }
}

int main(void) {
    testCount();
    testCoverage();
    if (failures == 0) {
        return usCheckPassed("all checks passed");
    }
    printf("fb: %d failures\n", failures);
    return 1;
}
