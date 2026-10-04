/*
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

#define PAGE 4096U

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eqInt(const char *name, int got, int want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-40s want %d got %d\n", name, want, got);
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
    uint32_t firstSectionRva = 0x1000;

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
    *(uint32_t *)(base + optOff + 16) = firstSectionRva;
    *(uint64_t *)(base + optOff + 24) = 0x140000000ULL;
    *(uint32_t *)(base + optOff + 32) = PAGE;
    *(uint32_t *)(base + optOff + 36) = 0x200;
    *(uint32_t *)(base + optOff + 56) = (uint32_t)(sectionBytes + firstSectionRva + PAGE);
    *(uint32_t *)(base + optOff + 60) = (uint32_t)(sectionsOff + sectionBytes);
    *(uint16_t *)(base + optOff + 68) = US_PE_SUBSYSTEM_NATIVE;

    /* Section table and section bodies */
    for (size_t i = 0; i < spec->sectionCount; i++) {
        uint8_t *sh = base + sectionsOff + i * 40;
        uint32_t rva = firstSectionRva + (uint32_t)(i * PAGE);

        memcpy(sh, spec->sections[i], strlen(spec->sections[i]));
        *(uint32_t *)(sh + 8) = PAGE;      /* virtual size */
        *(uint32_t *)(sh + 12) = rva;      /* virtual address */
        *(uint32_t *)(sh + 16) = PAGE;     /* raw size */
        *(uint32_t *)(sh + 20) = rva;      /* raw offset, same as rva: identity */
        *(uint32_t *)(sh + 36) = US_PE_SECTION_EXECUTABLE;
    }

    if (spec->marker != NULL) {
        uint8_t *at = base + firstSectionRva;
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
        seen->rvas[seen->count] = img->entryRva;
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

    eqInt("kernel found", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
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

    eqInt("winload found", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
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

    eqInt("two images found", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 2);
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
    eqInt("scan stops on request", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 1);
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
          usPeScanRegion(gRegion, 4 * PAGE, note, &seen), 0);
    eqInt("nothing reported", (int)seen.count, 0);

    /* Only MZ at a page boundary counts */
    reset();
    gRegion[PAGE] = 'M';
    gRegion[PAGE + 1] = 'Z';
    eqInt("a bare MZ is not an image", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);

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
          usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);
}

static void testEmptyRegion(void) {
    Seen seen = { .stopAfter = 8 };

    reset();
    eqInt("a region with nothing in it", usPeScanRegion(gRegion, sizeof(gRegion), note, &seen), 0);
    eqInt("null base is refused", usPeScanRegion(NULL, 4096, note, &seen), 0);
    eqInt("a region too small to hold a header",
          usPeScanRegion(gRegion, 16, note, &seen), 0);
}

int main(void) {
    testKernel();
    testWinload();
    testBothAndOrder();
    testVisitorCanStop();
    testIgnoresNonImages();
    testEmptyRegion();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
