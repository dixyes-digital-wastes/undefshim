/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the entry a diverted kernel call arrives at
 *
 * Three things are being checked and they are all about the answer being
 * visible rather than assumed. That the entry passes the call through
 * unchanged - same argument, same return value - because to the kernel nothing
 * happened. That it leaves x18 alone, which cannot be checked here and is
 * checked by the encoding instead: what is checked here is that it reads the
 * configuration rather than any register state. And that what it records names
 * a real image, because the record is the only thing that can tell a hook that
 * is working from a hook that was never reached
 */

#include <stdio.h>
#include <string.h>

#include "payload/drvhook.h"
#include "payload/payload.h"

#define US_CHECK_NAME "test_drvhook"
#include "check.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eq64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-46s want %#llx got %#llx\n", name,
                    (unsigned long long)want, (unsigned long long)got);
    }
}

static UsPayloadConfig gCfg;
/*
 * A whole pool header, sized by the layout rather than by a number written
 * here: the fields the entry records are near the end of the entry, and a
 * buffer that stops short of them would let the writes land outside it - which
 * reads as a counter that does not count
 */
static _Alignas(US_PAGE_SIZE) uint8_t gPoolStorage[US_POOL_HEADER_SIZE];
static _Alignas(8) uint8_t gImage[0x1000];

/* The payload's own way of reaching its configuration block, which on the
 * target is a symbol in the blob and here is the test's own */
UsPayloadConfig *usPayloadConfig(void) {
    return &gCfg;
}

/* Standing in for the kernel's RtlImageNtHeader: what it is called with is
 * the whole of what the entry has to get right */
static void *gCalledWith;
static int gCalls;
static void *gAnswer = (void *)0x1234;

static void *fakeTarget(void *imageBase) {
    gCalledWith = imageBase;
    gCalls++;
    return gAnswer;
}

static void put16(uint8_t *at, uint16_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

/* A minimal 64 bit PE header, enough for the entry to describe it */
static void buildImage(uint32_t sizeOfImage, uint32_t entryRVA) {
    const uint32_t peOff = 0x80;
    const uint32_t optOff = peOff + 24;

    memset(gImage, 0, sizeof(gImage));
    gImage[0] = 'M';
    gImage[1] = 'Z';
    put32(gImage + 0x3C, peOff);
    memcpy(gImage + peOff, "PE\0\0", 4);
    put16(gImage + peOff + 4, 0xAA64);
    put16(gImage + peOff + 6, 2);
    put16(gImage + peOff + 20, 0xF0);
    put16(gImage + optOff, 0x20B);
    put32(gImage + optOff + 16, entryRVA);
    put32(gImage + optOff + 56, sizeOfImage);
    put32(gImage + optOff + 60, 0x200);
}

static void init(void) {
    memset(&gCfg, 0, sizeof(gCfg));
    memset(gPoolStorage, 0, sizeof(gPoolStorage));
    gCfg.poolBase = (uintptr_t)gPoolStorage;
    gCfg.driverLoadTarget = (uintptr_t)fakeTarget;
    gCalledWith = NULL;
    gAnswer = (void *)0x1234;
    gCalls = 0;
    buildImage(0x1b4000, 0x1234);
}

/* The record itself, reached the way the entry reaches it rather than by an
 * offset written out here: the pool's header comes first, so an offset would be
 * a second copy of the layout that nothing checks */
#define POOL_ENTRY (&((UsPool *)(void *)gPoolStorage)->entry)

int main(void) {
    /* The call goes through unchanged: to the kernel nothing happened */
    init();
    ok("the answer is the target's", usDriverLoadEntry(gImage) == gAnswer);
    eq64("the target was called once", (uint64_t)gCalls, 1);
    ok("with the argument it was given", gCalledWith == (void *)gImage);
    eq64("the call was counted", POOL_ENTRY->driverLoads, 1);
    eq64("the image was recorded", POOL_ENTRY->driverImage, (uint64_t)(uintptr_t)gImage);
    eq64("its size came from its own header", POOL_ENTRY->driverSize, 0x1b4000);
    eq64("and so did its entry point", POOL_ENTRY->driverEntry, 0x1234);

    /* A second one is counted as well: drivers arrive one at a time and each
     * arrival is the thing being counted */
    init();
    usDriverLoadEntry(gImage);
    usDriverLoadEntry(gImage);
    eq64("every arrival is counted", POOL_ENTRY->driverLoads, 2);

    /*
     * Something that is not an image. The call still goes through, because the
     * entry is not a validator and refusing here would break a boot over a
     * report - but nothing that is not an image is recorded as one, so the
     * record cannot be read as proof the hook fired when it did not
     */
    init();
    gImage[0] = 'X';
    ok("a pointer that is not an image still gets the call",
       usDriverLoadEntry(gImage) == gAnswer);
    eq64("and is recorded as an arrival", POOL_ENTRY->driverLoads, 1);
    eq64("but with no size claimed for it", POOL_ENTRY->driverSize, 0);

    /* A header that is well formed up to the point it is not */
    init();
    memcpy(gImage + 0x80, "PX\0\0", 4);
    usDriverLoadEntry(gImage);
    eq64("a header that is not a PE has no size", POOL_ENTRY->driverSize, 0);

    /* The pool is not there yet on some paths: that is a report with nowhere
     * to go, not a reason to stop the call */
    init();
    gCfg.poolBase = 0;
    ok("with no pool the call still goes through",
       usDriverLoadEntry(gImage) == gAnswer);
    eq64("and the target still ran", (uint64_t)gCalls, 1);

    /* The target is what the handover filled in. Unreachable in practice, and
     * answering with nothing is the one failure the caller can see */
    init();
    gCfg.driverLoadTarget = 0;
    ok("with no target nothing is claimed", usDriverLoadEntry(gImage) == NULL);

    return usCheckSummary(checks, failures);
}
