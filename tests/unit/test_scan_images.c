/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the memory scan that finds winload and the kernel
 *
 * The images are built here rather than taken from a corpus, so the test says
 * exactly which property makes an image recognisable: the test constructs the
 * header, the section table and the marker, and the scanner has to come to the
 * same conclusion. That also keeps this runnable without the Windows binaries
 */

#include <stdio.h>
#include <string.h>

#include "core/pe.h"
#include "core/scan.h"

#define US_CHECK_NAME "test_scan_images"
#include "check.h"

#define PAGE 4096U

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eqInt(const char *name, int got, int want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-40s want %d got %d\n", name, want, got);
    }
}

/* --- building a plausible image ----------------------------------------- */

typedef struct ImageSpec_t {
    const char *name;
    const char *const *sections;
    size_t sectionCount;
    const char *marker;   /* written as UTF-16 at the start of the first section */
    size_t totalSize;
} ImageSpec;

/* Writes a minimal but valid ARM64 PE image at base and returns its size in
 * pages. The point is validity, not fidelity: every field the scanner reads
 * has to be right, and nothing else matters */
static size_t buildImage(uint8_t *base, const ImageSpec *spec) {
    size_t size = spec->totalSize;
    uint32_t peOff = 0x80;
    uint32_t optOff = peOff + 24;
    uint32_t sectionsOff = optOff + 0xF0;
    size_t sectionBytes = spec->sectionCount * 40;
    uint32_t firstSectionRVA = 0x1000;

    memset(base, 0, size);

    /* DOS header */
    base[0] = 'M';
    base[1] = 'Z';
    *(uint32_t *)(base + 0x3C) = peOff;

    /* PE signature and file header */
    memcpy(base + peOff, "PE\0\0", 4);
    *(uint16_t *)(base + peOff + 4) = US_PE_MACHINE_ARM64;
    *(uint16_t *)(base + peOff + 6) = (uint16_t)spec->sectionCount;
    *(uint16_t *)(base + peOff + 20) = 0xF0;

    /* Optional header, 64 bit */
    *(uint16_t *)(base + optOff) = 0x20B;
    *(uint32_t *)(base + optOff + 16) = firstSectionRVA;
    *(uint64_t *)(base + optOff + 24) = 0x140000000ULL;
    *(uint32_t *)(base + optOff + 32) = PAGE;
    *(uint32_t *)(base + optOff + 36) = 0x200;
    *(uint32_t *)(base + optOff + 56) = (uint32_t)(sectionBytes + firstSectionRVA + PAGE);
    *(uint32_t *)(base + optOff + 60) = (uint32_t)(sectionsOff + sectionBytes);
    *(uint16_t *)(base + optOff + 68) = US_PE_SUBSYSTEM_NATIVE;

    /* Section table and section bodies */
    for (size_t i = 0; i < spec->sectionCount; i++) {
        uint8_t *sh = base + sectionsOff + i * 40;
        uint32_t rva = firstSectionRVA + (uint32_t)(i * PAGE);

        memcpy(sh, spec->sections[i], strlen(spec->sections[i]));
        *(uint32_t *)(sh + 8) = PAGE;      /* virtual size */
        *(uint32_t *)(sh + 12) = rva;      /* virtual address */
        *(uint32_t *)(sh + 16) = PAGE;     /* raw size */
        *(uint32_t *)(sh + 20) = rva;      /* raw offset, same as rva: identity */
        *(uint32_t *)(sh + 36) = US_PE_SECTION_EXECUTABLE;
    }

    if (spec->marker != NULL) {
        uint8_t *at = base + firstSectionRVA;
        for (size_t i = 0; spec->marker[i] != '\0'; i++) {
            at[i * 2] = (uint8_t)spec->marker[i];
        }
    }

    return size;
}

/* --- the region the scanner walks --------------------------------------- */

#define REGION_PAGES 32

static uint8_t gRegion[REGION_PAGES * PAGE] __attribute__((aligned(PAGE)));

/* Records what the visitor was shown, in order */
typedef struct Seen_t {
    UsImageKind kinds[8];
    uint32_t    rvas[8];
    size_t      count;
    size_t      stopAfter;
} Seen;

static UsImageVisit note(const UsImage *img, UsImageKind kind, void *ctx) {
    Seen *seen = ctx;

    if (seen->count < 8) {
        seen->kinds[seen->count] = kind;
        seen->rvas[seen->count] = img->entryRVA;
        seen->count++;
    }
    return seen->count >= seen->stopAfter ? UsImageVisitStop : UsImageVisitContinue;
}

static void reset(void) {
    memset(gRegion, 0, sizeof(gRegion));
}

static void testKernel(void) {
    /* The kernel is recognised by the section names its linker script emits,
     * which no boot loader has */
    static const char *const kernelSections[] = { ".text", "PAGE", "INITKDBG" };
    ImageSpec spec = { .name = "kernel", .sections = kernelSections, .sectionCount = 3,
                       .marker = NULL, .totalSize = 8 * PAGE };
    Seen seen = { .stopAfter = 8 };

    reset();
    buildImage(gRegion + 3 * PAGE, &spec);

    eqInt("kernel found", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
    eqInt("kernel reported once", (int)seen.count, 1);
    if (seen.count == 1) {
        ok("kernel classified as the kernel", seen.kinds[0] == UsImageNtoskrnl);
    }
}

static void testWinload(void) {
    /* winload is recognised by a load option it must carry, which no other
     * stage does */
    static const char *const winloadSections[] = { ".text", ".data" };
    ImageSpec spec = { .name = "winload", .sections = winloadSections, .sectionCount = 2,
                       .marker = "OSLOADER.XSL", .totalSize = 8 * PAGE };
    Seen seen = { .stopAfter = 8 };

    reset();
    buildImage(gRegion + 2 * PAGE, &spec);

    eqInt("winload found", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
    eqInt("winload reported once", (int)seen.count, 1);
    if (seen.count == 1) {
        ok("winload classified as winload", seen.kinds[0] == UsImageWinload);
    }
}

static void testBothAndOrder(void) {
    static const char *const kernelSections[] = { ".text", "INITKDBG" };
    static const char *const winloadSections[] = { ".text", ".data" };
    ImageSpec kernel = { .name = "kernel", .sections = kernelSections, .sectionCount = 2,
                         .marker = NULL, .totalSize = 8 * PAGE };
    ImageSpec winload = { .name = "winload", .sections = winloadSections, .sectionCount = 2,
                          .marker = "OSLOADER.XSL", .totalSize = 8 * PAGE };
    Seen seen = { .stopAfter = 8 };

    reset();
    buildImage(gRegion + 2 * PAGE, &winload);
    buildImage(gRegion + 12 * PAGE, &kernel);

    eqInt("two images found", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 2);
    eqInt("two images reported", (int)seen.count, 2);
    if (seen.count == 2) {
        /* Address order, because that is the order memory is written in */
        ok("winload comes first", seen.kinds[0] == UsImageWinload);
        ok("kernel comes second", seen.kinds[1] == UsImageNtoskrnl);
    }
}

static void testVisitorCanStop(void) {
    static const char *const kernelSections[] = { ".text", "INITKDBG" };
    static const char *const winloadSections[] = { ".text", ".data" };
    ImageSpec kernel = { .name = "kernel", .sections = kernelSections, .sectionCount = 2,
                         .marker = NULL, .totalSize = 8 * PAGE };
    ImageSpec winload = { .name = "winload", .sections = winloadSections, .sectionCount = 2,
                          .marker = "OSLOADER.XSL", .totalSize = 8 * PAGE };
    Seen seen = { .stopAfter = 1 };

    reset();
    buildImage(gRegion, &winload);
    buildImage(gRegion + 12 * PAGE, &kernel);

    /* Stopping is how a caller that only wants the first match avoids paying
     * for the rest of the scan */
    eqInt("scan stops on request", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
    eqInt("only the first was reported", (int)seen.count, 1);
}

static void testIgnoresNonImages(void) {
    static const char *const kernelSections[] = { ".text", "INITKDBG" };
    ImageSpec kernel = { .name = "kernel", .sections = kernelSections, .sectionCount = 2,
                         .marker = NULL, .totalSize = 8 * PAGE };
    Seen seen = { .stopAfter = 8 };

    /* A header near the end of the region: there is not room for the image it
     * claims, so it must be rejected rather than read past the end */
    reset();
    buildImage(gRegion + 3 * PAGE, &kernel);
    eqInt("truncated image rejected",
          usPEScanRegion(gRegion, 4 * PAGE, note, &seen), 0);
    eqInt("nothing reported", (int)seen.count, 0);

    /* Only MZ at a page boundary counts */
    reset();
    gRegion[PAGE] = 'M';
    gRegion[PAGE + 1] = 'Z';
    eqInt("a bare MZ is not an image", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);

    /* An image that is neither stage is not noise: it is a PE, but it has
     * neither the linker script section names nor the load option string */
    reset();
    buildImage(gRegion + 2 * PAGE, &kernel);
    {
        /* Wipe both section names. The header stays valid, so the image is
         * well formed and simply not one we are looking for */
        uint8_t *sh = gRegion + 2 * PAGE + 0x80 + 24 + 0xF0;
        memset(sh, 'X', 8);
        memset(sh + 40, 'X', 8);
    }
    eqInt("an unclassifiable image is skipped",
          usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);
}

static void testEmptyRegion(void) {
    Seen seen = { .stopAfter = 8 };

    reset();
    eqInt("a region with nothing in it", usPEScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);
    eqInt("null base is refused", usPEScanRegion(NULL, 4096, note, &seen), 0);
    eqInt("a region too small to hold a header",
          usPEScanRegion(gRegion, 16, note, &seen), 0);
}

/* --- the descriptor base ------------------------------------------------ */

/*
 * An image carrying the kernel's descriptor-base lookup
 *
 * The words are the assembler's, and every one of them is registered in
 * check_encodings.py so that what they mean stays checked rather than
 * asserted. The body is the kernel's translate function as the compiler
 * emitted it:
 *
 *   ubfx x9, x0, #12, #36      the index of the descriptor inside its table
 *   ldr  x8, <literal>         the base, sixteen bytes further on
 *   add  x0, x8, x9, lsl #3    base plus index times eight
 *   ret
 *
 * and the literal that follows is the base itself, which the second half of
 * the search has to find a page-relative load of
 */
static const uint32_t kPteHelper[4] = {
    0xD34CBC09U, /* ubfx x9, x0, #12, #36 */
    0x58000068U, /* ldr  x8, <literal> */
    0x8B090D00U, /* add  x0, x8, x9, lsl #3 */
    0xD65F03C0U, /* ret */
};

/* adrp x8, <the page of the body>; ldr x8, [x8, #0x10] */
static const uint32_t kPteLoad[2] = { 0x90000008U, 0xF9400908U };
/* the same, reading a second variable at #0x20 */
static const uint32_t kPteLoadOther[2] = { 0x90000008U, 0xF9401108U };
/* ubfx x9, x0, #21, #30: the page-directory form of the same body, which the
 * shift and the width keep out */
static const uint32_t kPdeHelper[4] = {
    0xD355C809U,
    0x58000068U,
    0x8B090D00U,
    0xD65F03C0U,
};
/* add x0, x8, x9, lsr #3: a shift the body never has */
static const uint32_t kPteHelperShifted[4] = {
    0xD34CBC09U,
    0x58000068U,
    0x8B490D00U,
    0xD65F03C0U,
};

#define PTE_SELF_MAP UINT64_C(0xFFFFF68000000000)

/* Offsets inside the image's first section, which starts at RVA 0x1000 */
#define PTE_HELPER_AT 0x000U
#define PTE_LITERAL_AT 0x010U
#define PTE_OTHER_AT 0x020U
#define PTE_SITE_AT 0x100U

static void putWord(uint8_t *section, size_t at, uint32_t word) {
    for (size_t i = 0; i < 4U; i++) {
        section[at + i] = (uint8_t)(word >> (i * 8U));
    }
}

static void putU64(uint8_t *section, size_t at, uint64_t value) {
    for (size_t i = 0; i < 8U; i++) {
        section[at + i] = (uint8_t)(value >> (i * 8U));
    }
}

static void putWords(uint8_t *section, size_t at, const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; i++) {
        putWord(section, at + i * 4U, words[i]);
    }
}

/* Writes the body, its literal and one load of it */
static void buildPteImage(uint8_t *section) {
    putWords(section, PTE_HELPER_AT, kPteHelper, 4);
    putU64(section, PTE_LITERAL_AT, PTE_SELF_MAP);
    putWords(section, PTE_SITE_AT, kPteLoad, 2);
}

/* An image whose first section is the body's, as a loaded image */
static UsImage pteImage(uint8_t *at, size_t pages) {
    static const char *const sections[] = { ".text" };
    ImageSpec spec = { .name = "pte", .sections = sections, .sectionCount = 1,
                       .marker = NULL, .totalSize = pages * PAGE };
    UsImage img = { 0 };

    buildImage(at, &spec);
    ok("the image built for the descriptor base is valid",
       usImageInitMemory(&img, at, pages * PAGE));
    return img;
}

/* The section body, which a memory view reaches at the same offset as its RVA
 * because the builder gives every section an identity raw offset */
#define PTE_SECTION_AT PAGE

static void testPteBaseFound(void) {
    uint8_t *at = gRegion + PAGE;
    UsImage img;
    UsPteBaseSite site;

    reset();
    img = pteImage(at, 8);
    buildPteImage(at + PTE_SECTION_AT);

    site = usLocatePteBase(&img);
    ok("the descriptor base is found", site.found);
    eqInt("one body was seen", (int)site.helpers, 1);
    eqInt("one load named the literal", (int)site.candidates, 1);
    eqInt("the body is where it was written", (int)site.helperRVA, 0x1000);
    eqInt("the variable is where it was written", (int)site.rva, 0x1010);
    ok("the literal is the base", site.base == PTE_SELF_MAP);
}

static void testPteBaseRefusals(void) {
    uint8_t *at = gRegion + PAGE;
    uint8_t *section = at + PTE_SECTION_AT;
    UsImage img;
    UsPteBaseSite site;

    /* Two bodies: which one the kernel calls is not something the bytes say,
     * so neither is the answer */
    reset();
    img = pteImage(at, 8);
    buildPteImage(section);
    putWords(section, 0x800U, kPteHelper, 4);
    putU64(section, 0x810U, PTE_SELF_MAP);
    site = usLocatePteBase(&img);
    ok("two bodies are refused", !site.found);
    eqInt("both were counted", (int)site.helpers, 2);

    /* Two variables holding the same literal */
    reset();
    img = pteImage(at, 8);
    buildPteImage(section);
    putU64(section, PTE_OTHER_AT, PTE_SELF_MAP);
    putWords(section, PTE_SITE_AT + 8U, kPteLoadOther, 2);
    site = usLocatePteBase(&img);
    ok("two variables are refused", !site.found);
    eqInt("both loads were counted", (int)site.candidates, 2);

    /* The page-directory form of the body, which differs in the shift and the
     * width and reads the same literal */
    reset();
    img = pteImage(at, 8);
    buildPteImage(section);
    putWords(section, PTE_HELPER_AT, kPdeHelper, 4);
    site = usLocatePteBase(&img);
    ok("the page-directory body is not this one", !site.found);
    eqInt("nothing was counted as a body", (int)site.helpers, 0);

    /* A body whose add shifts the wrong way */
    reset();
    img = pteImage(at, 8);
    buildPteImage(section);
    putWords(section, PTE_HELPER_AT, kPteHelperShifted, 4);
    site = usLocatePteBase(&img);
    ok("an add that does not shift left is refused", !site.found);

    /* An image with no body at all, which is every image that is not a kernel */
    reset();
    img = pteImage(at, 8);
    putU64(section, PTE_LITERAL_AT, PTE_SELF_MAP);
    putWords(section, PTE_SITE_AT, kPteLoad, 2);
    site = usLocatePteBase(&img);
    ok("no body means no answer", !site.found);
    ok("nor is a null image answered", !usLocatePteBase(NULL).found);
}

int main(void) {
    testKernel();
    testWinload();
    testBothAndOrder();
    testVisitorCanStop();
    testIgnoresNonImages();
    testEmptyRegion();
    testPteBaseFound();
    testPteBaseRefusals();

    return usCheckSummary(checks, failures);
}
