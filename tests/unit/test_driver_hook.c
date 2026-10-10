/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the driver load hook's locator, and for the two PE facts it is
 * built on
 *
 * The locator answers by name and by shape, so what makes it right is not a
 * count of matches: it is that one entry of a sequence of calls resolves to
 * an exported name. Every check here is one of the ways that can fail, and
 * the image is built here, so the test says exactly which property makes the
 * shape a shape and it runs without any Windows binary
 *
 * Two of the refusals matter more than the others, and they are the reason
 * this is a test rather than a reading of the real kernel: a witness that is
 * in the wrong position, and a witness that the body calls twice. Both would
 * let a locator pick the wrong call while still finding something, and the
 * machine would then be armed at an address that has nothing to do with
 * loading a driver
 */

#include <stdio.h>
#include <string.h>

#include "core/pe.h"
#include "core/scan.h"

#define US_CHECK_NAME "test_driver_hook"
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

static void eqHex(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-46s want %#llx got %#llx\n", name,
                    (unsigned long long)want, (unsigned long long)got);
    }
}

static void eqSize(const char *name, size_t got, size_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-46s want %zu got %zu\n", name, want, got);
    }
}

/* --- building an image -------------------------------------------------- */

#define IMAGE_BYTES (64 * 4096)

/* A function the table describes that the anchor sits inside the end of,
 * which is what the unframed wrappers of the newer builds are */
#define OUTER_RVA 0x800U
#define TEXT_RVA 0x1000U
#define ANCHOR_RVA 0x1000U
#define WORKER_RVA 0x2000U
#define WITNESS_RVA 0x3000U
#define FILLER_RVA 0x3800U
#define UNSEEN_RVA 0x3900U

#define PDATA_RVA 0x8000U
#define PDATA_ENTRIES 5U
#define PDATA_BYTES (PDATA_ENTRIES * US_PE_PDATA_ENTRY_BYTES)
#define EDATA_RVA 0x9000U

static uint8_t gImage[IMAGE_BYTES];

static void put32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *at, uint16_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
}

static void putInsn(uint32_t rva, uint32_t word) {
    put32(gImage + rva, word);
}

/* bl, from one RVA to another. The two are in the same section, so the
 * displacement is what the assembler would compute */
static uint32_t blTo(uint32_t from, uint32_t to) {
    int32_t delta = (int32_t)to - (int32_t)from;

    return 0x94000000U | (((uint32_t)(delta >> 2)) & 0x03FFFFFFU);
}

static uint32_t bTo(uint32_t from, uint32_t to) {
    int32_t delta = (int32_t)to - (int32_t)from;

    return 0x14000000U | (((uint32_t)(delta >> 2)) & 0x03FFFFFFU);
}

/* What the builder lets a test vary, so that each refusal is one property */
typedef struct Shape_t {
    bool     anchorExported;
    bool     witnessExported;
    bool     anchorUsesBranch;  /* a tail branch rather than a call */
    /* The table describes no function at the anchor, so the anchor reads as
     * part of the function before it */
    bool     anchorHasNoOwnEntry;
    /* The handover lands past the worker's entry, inside the function but not
     * at its start */
    bool     anchorBranchesIntoFunction;
    /* Which call of the worker's body goes to the witness, and whether there
     * is a second one as well */
    uint32_t witnessAt;
    bool     secondWitness;
    bool     witnessIsForwarder;
} Shape;

/*
 * A name, with its terminator. A count written out at the call site is a
 * count that can be one short, and one short is a name that runs into the
 * next one and matches nothing
 */
#define NAME_BYTES(s) (sizeof(s))

static const Shape kShape = {
    .anchorExported = true,
    .witnessExported = true,
    .witnessAt = 19U,
};

/* The export directory, its four arrays and the two names, laid out inside
 * one section so that every RVA the parser follows is a real one */
#define EDATA_FUNCTIONS (EDATA_RVA + 0x40U)
#define EDATA_NAMES (EDATA_RVA + 0x80U)
#define EDATA_ORDINALS (EDATA_RVA + 0xC0U)
#define EDATA_STRINGS (EDATA_RVA + 0x100U)

static void buildImage(const Shape *shape) {
    const uint32_t peOff = 0x80;
    const uint32_t optOff = peOff + 24;
    const uint32_t sectionsOff = optOff + 0xF0;
    uint8_t *sh;
    uint32_t nameAt = EDATA_STRINGS;
    uint32_t anchorName = 0;
    uint32_t witnessName = 0;
    uint32_t functions = 0;
    uint32_t names = 0;
    uint32_t target;

    memset(gImage, 0, IMAGE_BYTES);

    gImage[0] = 'M';
    gImage[1] = 'Z';
    put32(gImage + 0x3C, peOff);

    memcpy(gImage + peOff, "PE\0\0", 4);
    put16(gImage + peOff + 4, US_PE_MACHINE_ARM64);
    put16(gImage + peOff + 6, 3);
    put16(gImage + peOff + 20, 0xF0);
    /* The build's name, which is what a peHash list is checked against */
    put32(gImage + peOff + 8, 0x1149c62dU);

    put16(gImage + optOff, 0x20B);
    put32(gImage + optOff + 16, ANCHOR_RVA);
    *(uint64_t *)(gImage + optOff + 24) = 0x140000000ULL;
    put32(gImage + optOff + 32, 0x1000);
    put32(gImage + optOff + 56, IMAGE_BYTES);
    put32(gImage + optOff + 60, 0x200);
    put16(gImage + optOff + 68, US_PE_SUBSYSTEM_NATIVE);
    put32(gImage + optOff + 108, 16);

    /* .text, .pdata and .edata, at their own RVAs: the view is a memory one,
     * so an offset in the array is the RVA */
    sh = gImage + sectionsOff;
    memcpy(sh, ".text", 5);
    put32(sh + 8, 0x4000);
    put32(sh + 12, TEXT_RVA);
    put32(sh + 16, 0x4000);
    put32(sh + 20, TEXT_RVA);
    put32(sh + 36, US_PE_SECTION_EXECUTABLE);

    sh = gImage + sectionsOff + 40;
    memcpy(sh, ".pdata", 6);
    put32(sh + 12, PDATA_RVA);
    put32(sh + 20, PDATA_RVA);
    put32(sh + 36, 0x40000040U);

    sh = gImage + sectionsOff + 80;
    memcpy(sh, ".edata", 6);
    put32(sh + 12, EDATA_RVA);
    put32(sh + 20, EDATA_RVA);
    put32(sh + 36, 0x40000040U);

    /* The exception table: five functions, in order, eight bytes an entry.
     * The count of entries is the count that were written; a declaration of
     * entries that are all zero would not be a table */
    put32(gImage + PDATA_RVA + 0,
          shape->anchorHasNoOwnEntry ? OUTER_RVA : ANCHOR_RVA);
    put32(gImage + PDATA_RVA + 8, WORKER_RVA);
    put32(gImage + PDATA_RVA + 16, WITNESS_RVA);
    put32(gImage + PDATA_RVA + 24, FILLER_RVA);
    put32(gImage + PDATA_RVA + 32, UNSEEN_RVA);
    /* The two directories. The sizes are the ones a reader would bound
     * itself by: six entries, five entries */
    put32(gImage + optOff + 112 + 0 * 8, EDATA_RVA);
    put32(gImage + optOff + 112 + 0 * 8 + 4, 0x200);
    put32(gImage + optOff + 112 + 3 * 8, PDATA_RVA);
    put32(gImage + optOff + 112 + 3 * 8 + 4, PDATA_BYTES);

    /* The anchor: one handover to the worker, which is what makes it a
     * wrapper. It is a call on some builds and a tail branch on others, and
     * both have to be followed */
    target = shape->anchorBranchesIntoFunction ? WORKER_RVA + 4U : WORKER_RVA;
    putInsn(ANCHOR_RVA, shape->anchorUsesBranch ? bTo(ANCHOR_RVA, target)
                                                : blTo(ANCHOR_RVA, target));
    putInsn(ANCHOR_RVA + 4, 0xD65F03C0U);   /* ret */

    /* The worker's body: 44 calls, one of which goes to the witness */
    for (uint32_t i = 0; i < 44U; i++) {
        uint32_t at = WORKER_RVA + i * 4U;

        target = (i == shape->witnessAt || (shape->secondWitness && i == 30U))
                     ? WITNESS_RVA
                     : FILLER_RVA;
        putInsn(at, blTo(at, target));
    }
    putInsn(WORKER_RVA + 44U * 4U, 0xD65F03C0U);

    putInsn(WITNESS_RVA, 0xD65F03C0U);
    putInsn(FILLER_RVA, 0xD65F03C0U);
    putInsn(UNSEEN_RVA, 0xD65F03C0U);

    /*
     * The export directory. Its arrays hold as many entries as it claims and
     * no more, because a reader that trusts the count would otherwise walk
     * past the section
     */
    functions = 4;
    names = 0;
    if (shape->anchorExported) {
        anchorName = nameAt;
        memcpy(gImage + nameAt, US_DRIVER_LOAD_ANCHOR, NAME_BYTES(US_DRIVER_LOAD_ANCHOR));
        nameAt += NAME_BYTES(US_DRIVER_LOAD_ANCHOR);
    }
    if (shape->witnessExported) {
        witnessName = nameAt;
        memcpy(gImage + nameAt, US_DRIVER_LOAD_WITNESS, NAME_BYTES(US_DRIVER_LOAD_WITNESS));
        nameAt += NAME_BYTES(US_DRIVER_LOAD_WITNESS);
    }

    /* Function 0 is the anchor, 1 the witness, 2 an export nothing looks for */
    put32(gImage + EDATA_FUNCTIONS + 0 * 4, ANCHOR_RVA);
    put32(gImage + EDATA_FUNCTIONS + 1 * 4,
          shape->witnessIsForwarder ? EDATA_RVA + 0x180U : WITNESS_RVA);
    put32(gImage + EDATA_FUNCTIONS + 2 * 4, UNSEEN_RVA);
    put32(gImage + EDATA_FUNCTIONS + 3 * 4, 0);

    if (shape->anchorExported) {
        put32(gImage + EDATA_NAMES + names * 4, anchorName);
        put16(gImage + EDATA_ORDINALS + names * 2, 0);
        names++;
    }
    if (shape->witnessExported) {
        put32(gImage + EDATA_NAMES + names * 4, witnessName);
        put16(gImage + EDATA_ORDINALS + names * 2, 1);
        names++;
    }
    /* A name that is present but not the one being looked for, so that the
     * walk has to skip something rather than finding the first entry */
    put32(gImage + EDATA_NAMES + names * 4, EDATA_STRINGS + 0x1A0U);
    memcpy(gImage + EDATA_STRINGS + 0x1A0U, "NtSomethingElse", NAME_BYTES("NtSomethingElse"));
    put16(gImage + EDATA_ORDINALS + names * 2, 2);
    names++;

    put32(gImage + EDATA_RVA + 12, EDATA_STRINGS + 0x1B0U);  /* the module's own name */
    memcpy(gImage + EDATA_STRINGS + 0x1B0U, "ntoskrnl.exe", NAME_BYTES("ntoskrnl.exe"));
    put32(gImage + EDATA_RVA + 16, 1);           /* ordinal base */
    put32(gImage + EDATA_RVA + 20, functions);
    put32(gImage + EDATA_RVA + 24, names);
    put32(gImage + EDATA_RVA + 28, EDATA_FUNCTIONS);
    put32(gImage + EDATA_RVA + 32, EDATA_NAMES);
    put32(gImage + EDATA_RVA + 36, EDATA_ORDINALS);
}

static const UsImage *imageOf(UsImage *img) {
    ok("the built image is valid", usImageInitMemory(img, gImage, sizeof(gImage)));
    return img;
}

/* --- the two PE facts --------------------------------------------------- */

static void testExports(void) {
    UsImage img;
    uint32_t rva = 0;

    buildImage(&kShape);
    imageOf(&img);

    ok("the anchor's name resolves", usImageExportRVA(&img, "MmLoadSystemImage", &rva));
    eqHex("and resolves to its RVA", rva, ANCHOR_RVA);
    ok("the witness's name resolves", usImageExportRVA(&img, "RtlImageNtHeader", &rva));
    eqHex("and resolves to its RVA", rva, WITNESS_RVA);
    ok("a name that is not there does not resolve",
       !usImageExportRVA(&img, "MmLoadSystemImageEx", &rva));
    ok("and an empty name does not either", !usImageExportRVA(&img, "", &rva));
    ok("nor does nothing at all", !usImageExportRVA(&img, NULL, &rva));

    /* A forwarded export is a string naming another module. Every caller here
     * wants an address, so the locator has to refuse rather than follow it */
    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .witnessAt = 19U, .witnessIsForwarder = true });
    imageOf(&img);
    ok("a forwarded export is refused rather than followed",
       !usImageExportRVA(&img, "RtlImageNtHeader", &rva));

    eqHex("the COFF stamp is read, not computed", img.timeDateStamp, 0x1149c62dU);
}

static void testFunctionRange(void) {
    UsImage img;
    uint32_t start = 0;
    uint32_t end = 0;

    buildImage(&kShape);
    imageOf(&img);

    ok("an address in the worker is in the worker",
       usImageFunctionRange(&img, WORKER_RVA + 0x40U, &start, &end));
    eqHex("it starts at the worker", start, WORKER_RVA);
    eqHex("and ends at the next entry", end, WITNESS_RVA);
    eqHex("a function starts at itself",
          usImageFunctionRange(&img, WORKER_RVA, &start, &end) ? start : 0,
          WORKER_RVA);
    /* Entries are eight bytes: reading them as twelve would land in the
     * middle of the table and answer with a number that is not a function */
    eqHex("the entry after the first is the worker, not the second dword",
          usImageFunctionRange(&img, WITNESS_RVA, &start, &end) ? start : 0,
          WITNESS_RVA);
    ok("an address before the first entry is in nothing",
       !usImageFunctionRange(&img, TEXT_RVA - 4U, &start, &end));
    ok("the last entry reaches to the end of the image",
       usImageFunctionRange(&img, UNSEEN_RVA + 4U, &start, &end)
           && end == IMAGE_BYTES);
}

/* --- the locator -------------------------------------------------------- */

static void testFindsTheCall(void) {
    UsImage img;
    UsDriverLoadHook hook;

    buildImage(&kShape);
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);

    ok("the chain is found", hook.found);
    eqHex("the anchor is where its name says", hook.anchorRVA, ANCHOR_RVA);
    eqHex("the worker is what the anchor calls", hook.functionRVA, WORKER_RVA);
    /* A function ends where the next entry begins: the table says where they
     * start and nothing else, so the extent is read and not assumed */
    eqHex("the worker's extent is the next entry", hook.functionEnd, WITNESS_RVA);
    eqSize("the body's calls are all counted", hook.calls, 44U);
    eqHex("the call taken over is the twentieth", hook.callRVA,
          WORKER_RVA + 19U * 4U);
    eqSize("and it is at the index that identifies it", hook.callIndex, 19U);
    eqHex("it goes to the witness", hook.witnessRVA, WITNESS_RVA);
}

/*
 * The same shape with a tail branch in place of the call, which is what the
 * newer wrappers do. The hop says the same thing either way, so the
 * answer has to be the same, down to the call that is taken over
 */
static void testTailCallShape(void) {
    UsImage img;
    UsDriverLoadHook hook;

    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .anchorUsesBranch = true, .witnessAt = 19U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);

    ok("a tail branch to the worker is found", hook.found);
    eqHex("the worker is where it branches", hook.functionRVA, WORKER_RVA);
    eqHex("the worker's extent is the next entry", hook.functionEnd,
          WITNESS_RVA);
    eqSize("the body's calls are all counted", hook.calls, 44U);
    eqHex("and the call taken over is the same one", hook.callRVA,
          WORKER_RVA + 19U * 4U);
}

/*
 * The shape a wrapper with no frame has: the table says nothing about it,
 * and the handover is a tail branch rather than a call.
 * Each of the two is a separate way the locator used to refuse, and the table
 * said nothing before the branch was ever read
 */
static void testUnframedWrapper(void) {
    UsImage img;
    UsDriverLoadHook hook;

    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .anchorHasNoOwnEntry = true,
                         .anchorUsesBranch = true, .witnessAt = 19U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);

    ok("a wrapper the table does not describe is found", hook.found);
    eqHex("the anchor is where its name says", hook.anchorRVA, ANCHOR_RVA);
    /* What the name resolved into, which is the function before it and not
     * the anchor: the field says so, and the scan starts at the anchor */
    eqHex("the name resolves into the function before it", hook.anchorStart,
          OUTER_RVA);
    eqHex("the worker is where it branches", hook.functionRVA, WORKER_RVA);
    eqSize("the body's calls are all counted", hook.calls, 44U);
    eqHex("and the call taken over is the same one", hook.callRVA,
          WORKER_RVA + 19U * 4U);
}

/*
 * Every refusal. Each is one property of the shape, and a locator that
 * answered anyway would arm the machine at an address that does no such thing
 */
static void testRefusals(void) {
    UsImage img;
    UsDriverLoadHook hook;

    /* The handover lands inside the worker rather than at its start. A call
     * into the middle of a function is not the entry the worker has, and a
     * locator that took it would count a chain that is not the one it is
     * looking for */
    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .anchorBranchesIntoFunction = true,
                         .witnessAt = 19U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);
    ok("a handover past the worker's start is refused", !hook.found);
    eqHex("and what was learned before the refusal is kept", hook.anchorRVA,
          ANCHOR_RVA);

    /* No witness to check the position against: nothing is left that could
     * tell one call of the body apart from another */
    buildImage(&(Shape){ .anchorExported = true, .witnessExported = false,
                         .witnessAt = 19U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);
    ok("without a witness nothing is answered", !hook.found);
    eqHex("the worker is still known", hook.functionRVA, WORKER_RVA);

    /* The witness is called, but not where the shape says it is. This is the
     * refusal that matters: something was found, and it is the wrong call */
    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .witnessAt = 18U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);
    ok("a witness in the wrong position is refused", !hook.found);
    eqHex("and the call it would have picked is not reported", hook.callRVA, 0);
    eqSize("the body is still counted", hook.calls, 44U);

    /* Called twice: which one is the driver load is then a guess, and a guess
     * here is a machine armed in the wrong place */
    buildImage(&(Shape){ .anchorExported = true, .witnessExported = true,
                         .witnessAt = 19U, .secondWitness = true });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);
    ok("a witness called twice is refused", !hook.found);

    /* Nothing to anchor on */
    buildImage(&(Shape){ .anchorExported = false, .witnessExported = true,
                         .witnessAt = 19U });
    imageOf(&img);
    hook = usLocateDriverLoadHook(&img);
    ok("an image without the anchor is refused", !hook.found);
    eqHex("and nothing is reported", hook.anchorRVA, 0);

    /* An image with no export directory at all: an image without one is the
     * normal case for a driver, and the answer is a refusal, not a crash */
    {
        UsImage bare;

        memset(gImage, 0, IMAGE_BYTES);
        ok("an image without a header is not valid",
           !usImageInitMemory(&bare, gImage, sizeof(gImage)));
        hook = usLocateDriverLoadHook(&bare);
        ok("and the locator refuses it", !hook.found);
        ok("as it does nothing", !usLocateDriverLoadHook(NULL).found);
    }
}

int main(void) {
    testExports();
    testFunctionRange();
    testFindsTheCall();
    testTailCallShape();
    testUnframedWrapper();
    testRefusals();

    return usCheckSummary(checks, failures);
}
