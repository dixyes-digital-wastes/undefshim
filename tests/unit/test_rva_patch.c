/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the RVA patch writer
 *
 * The patch table is what gets a breakpoint into a stage that is not otherwise
 * instrumented, so what matters here is that the bytes land the right way
 * round at the address the RVA names. A patch written back to front still
 * writes successfully and only shows up as something inexplicable much later
 *
 * The image is built here rather than taken from a corpus, so the test runs
 * without the Windows binaries
 */

#include <stdio.h>
#include <string.h>

#include "core/pe.h"
#include "core/rva_patch.h"

#define US_CHECK_NAME "test_rva_patch"
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

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-34s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

#define IMAGE_SIZE 0x4000U
#define SECTION_RVA 0x1000U

static uint8_t gImage[IMAGE_SIZE];

/* A minimal but valid ARM64 PE, in memory view, so the writer has something
 * to write into. Only the fields the reader looks at are set */
static void buildImage(void) {
    const uint32_t peOff = 0x80;
    const uint32_t optOff = peOff + 24;
    const uint32_t sectionsOff = optOff + 0xF0;

    memset(gImage, 0, sizeof(gImage));

    gImage[0] = 'M';
    gImage[1] = 'Z';
    *(uint32_t *)(gImage + 0x3C) = peOff;

    memcpy(gImage + peOff, "PE\0\0", 4);
    *(uint16_t *)(gImage + peOff + 4) = US_PE_MACHINE_ARM64;
    *(uint16_t *)(gImage + peOff + 6) = 1;      /* one section */
    *(uint16_t *)(gImage + peOff + 20) = 0xF0;  /* optional header size */

    *(uint16_t *)(gImage + optOff) = 0x20B;
    *(uint32_t *)(gImage + optOff + 16) = SECTION_RVA;
    *(uint64_t *)(gImage + optOff + 24) = 0x140000000ULL;
    *(uint32_t *)(gImage + optOff + 32) = 0x1000;
    *(uint32_t *)(gImage + optOff + 56) = IMAGE_SIZE;
    *(uint32_t *)(gImage + optOff + 60) = 0x200;
    *(uint16_t *)(gImage + optOff + 68) = US_PE_SUBSYSTEM_NATIVE;

    memcpy(gImage + sectionsOff, ".text", 5);
    *(uint32_t *)(gImage + sectionsOff + 8) = IMAGE_SIZE - SECTION_RVA;
    *(uint32_t *)(gImage + sectionsOff + 12) = SECTION_RVA;
    *(uint32_t *)(gImage + sectionsOff + 16) = IMAGE_SIZE - SECTION_RVA;
    *(uint32_t *)(gImage + sectionsOff + 20) = SECTION_RVA;
    *(uint32_t *)(gImage + sectionsOff + 36) = US_PE_SECTION_EXECUTABLE;
}

static void testWrites(void) {
    UsImage img;
    UsPatchSpec spec = { 0 };
    UsPatchRange range;

    ok("image is valid", usImageInitMemory(&img, gImage, sizeof(gImage)));

    /* Four bytes, the width a branch needs */
    spec.rva = SECTION_RVA;
    spec.value = 0x14000000;  /* b . */
    spec.width = 4;
    eqU64("four byte patch result", usPatchApply(&img, &spec, &range), UsPatchOk);
    eqU64("four byte patch address", (uint64_t)(uintptr_t)range.addr,
          (uint64_t)(uintptr_t)(gImage + SECTION_RVA));
    eqU64("four byte patch size", range.bytes, 4);
    eqU64("four byte patch is little endian", *(uint32_t *)(gImage + SECTION_RVA),
          0x14000000U);

    /* Each width has to write exactly the bytes it claims and no more */
    memset(gImage + SECTION_RVA, 0xAA, 8);

    spec.rva = SECTION_RVA;
    spec.value = 0x12;
    spec.width = 1;
    eqU64("one byte patch", usPatchApply(&img, &spec, &range), UsPatchOk);
    eqU64("one byte value", gImage[SECTION_RVA], 0x12);
    eqU64("one byte leaves the rest alone", gImage[SECTION_RVA + 1], 0xAA);

    spec.rva = SECTION_RVA + 2;
    spec.value = 0x1234;
    spec.width = 2;
    eqU64("two byte patch", usPatchApply(&img, &spec, &range), UsPatchOk);
    eqU64("two byte value is little endian",
          (uint64_t)(gImage[SECTION_RVA + 2] | (gImage[SECTION_RVA + 3] << 8)), 0x1234);

    spec.rva = SECTION_RVA + 8;
    spec.value = 0x0123456789ABCDEFULL;
    spec.width = 8;
    eqU64("eight byte patch", usPatchApply(&img, &spec, &range), UsPatchOk);
    eqU64("eight byte value is little endian",
          *(uint64_t *)(gImage + SECTION_RVA + 8), 0x0123456789ABCDEFULL);
}

static void testRefusals(void) {
    UsImage img;
    UsPatchSpec spec = { 0 };
    UsPatchRange range;

    usImageInitMemory(&img, gImage, sizeof(gImage));

    /* Widths the writer does not implement must be refused rather than
     * guessed at */
    spec.rva = SECTION_RVA;
    spec.width = 3;
    eqU64("three byte width is refused", usPatchApply(&img, &spec, &range), UsPatchBadWidth);
    spec.width = 0;
    eqU64("zero width is refused", usPatchApply(&img, &spec, &range), UsPatchBadWidth);
    spec.width = 16;
    eqU64("sixteen byte width is refused", usPatchApply(&img, &spec, &range), UsPatchBadWidth);

    /* Past the end of the image, including a patch that would only fit by
     * running over it */
    spec.width = 4;
    spec.rva = IMAGE_SIZE;
    eqU64("rva past the image is refused", usPatchApply(&img, &spec, &range),
          UsPatchOutOfRange);
    spec.rva = IMAGE_SIZE - 2;
    eqU64("rva without room for the width is refused", usPatchApply(&img, &spec, &range),
          UsPatchOutOfRange);
    spec.rva = 0xFFFFFFF0U;
    eqU64("rva that overflows is refused", usPatchApply(&img, &spec, &range),
          UsPatchOutOfRange);

    /* A memory view has no header area to speak of: address zero is not a
     * place an image can be written at */
    spec.rva = 0;
    eqU64("rva zero is refused", usPatchApply(&img, &spec, &range), UsPatchOutOfRange);
}

int main(void) {
    buildImage();
    testWrites();
    testRefusals();

    return usCheckSummary(checks, failures);
}
