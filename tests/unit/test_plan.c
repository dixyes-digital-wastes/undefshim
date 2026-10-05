/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the plan and the sites it is built from
 *
 * Two things are being checked. One is that the collection finds what is
 * there: sites have to be found, and a locator that matches twice has to be
 * refused rather than resolved to whichever came first. The other is that the
 * dump is stable, because a dump that changes order between runs cannot be
 * compared against anything, and comparing two dumps is the whole point
 *
 * The images are built here, so the test says exactly which property makes a
 * site a site, and it runs without the Windows binaries
 */

#include <stdio.h>
#include <string.h>

#include "core/plan.h"
#include "core/scan.h"

#define US_CHECK_NAME "test_plan"
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

static void eqSize(const char *name, size_t got, size_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-44s want %zu got %zu\n", name, want, got);
    }
}

static void eqStr(const char *name, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        usCheckFail("%-44s want \"%s\" got \"%s\"\n", name, want, got ? got : "(null)");
    }
}

/* --- building images ---------------------------------------------------- */

#define IMAGE_BYTES (32 * 4096)
#define TEXT_RVA 0x1000U
#define TEXT_SIZE 0x8000U
#define TEXT_RAW TEXT_RVA
/*
 * The image is examined as a memory view, where a section sits at its RVA and
 * the array offset is the RVA. Writing a section's contents at its RVA is
 * therefore what a loaded image looks like
 */
#define PDATA_RVA 0x9000U
#define PDATA_RAW PDATA_RVA

static uint8_t gWinload[IMAGE_BYTES];
static uint8_t gKernel[IMAGE_BYTES];

static void put32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

/* An instruction in the text section. The section is laid out where the bytes
 * are, so an RVA is also an offset into the array */
static void putInsn(uint8_t *image, uint32_t rva, uint32_t word) {
    put32(image + rva, word);
}

/*
 * The exception directory is sorted by start address, and the lookup is a
 * binary search, so the section is only ever as large as the entries that were
 * actually declared. An entry of zero past the last real one would break the
 * ordering and the search with it
 */
static size_t gFunctionCount;

static void setFunctionCount(uint8_t *image, size_t count) {
    const uint32_t sectionsOff = 0x80 + 24 + 0xF0;
    uint8_t *sh = image + sectionsOff + 40;

    put32(sh + 8, (uint32_t)(count * 8));
    put32(sh + 16, (uint32_t)(count * 8));
}

/*
 * The exception directory is part of the image this builds, because a loaded
 * image has one, but nothing in this test depends on it any more: the
 * transfer is a branch inside a function and is not looked up there
 */

static void buildImage(uint8_t *image, const char *section, uint16_t subsystem) {
    const uint32_t peOff = 0x80;
    const uint32_t optOff = peOff + 24;
    const uint32_t sectionsOff = optOff + 0xF0;
    uint8_t *sh;

    memset(image, 0, IMAGE_BYTES);

    image[0] = 'M';
    image[1] = 'Z';
    put32(image + 0x3C, peOff);

    memcpy(image + peOff, "PE\0\0", 4);
    *(uint16_t *)(image + peOff + 4) = US_PE_MACHINE_ARM64;
    *(uint16_t *)(image + peOff + 6) = 2;
    *(uint16_t *)(image + peOff + 20) = 0xF0;

    *(uint16_t *)(image + optOff) = 0x20B;
    put32(image + optOff + 16, TEXT_RVA);
    *(uint64_t *)(image + optOff + 24) = 0x140000000ULL;
    put32(image + optOff + 32, 0x1000);
    put32(image + optOff + 56, IMAGE_BYTES);
    put32(image + optOff + 60, 0x200);
    *(uint16_t *)(image + optOff + 68) = subsystem;

    sh = image + sectionsOff;
    memcpy(sh, section, strlen(section));
    put32(sh + 8, TEXT_SIZE);
    put32(sh + 12, TEXT_RVA);
    put32(sh + 16, TEXT_SIZE);
    put32(sh + 20, TEXT_RAW);
    put32(sh + 36, US_PE_SECTION_EXECUTABLE);

    sh = image + sectionsOff + 40;
    memcpy(sh, ".pdata", 6);
    put32(sh + 12, PDATA_RVA);
    put32(sh + 20, PDATA_RAW);
    put32(sh + 36, 0x40000040U);  /* initialised data, readable */
    gFunctionCount = 0;
    setFunctionCount(image, 0);
}

/* --- a sink that writes into a buffer ----------------------------------- */

typedef struct Buffer_t {
    char   text[16384];
    size_t len;
} Buffer;

static void bufPuts(void *ctx, const char *s) {
    Buffer *b = ctx;

    while (*s != '\0' && b->len + 1 < sizeof(b->text)) {
        b->text[b->len++] = *s++;
    }
    b->text[b->len] = '\0';
}

static void bufHex(void *ctx, uint64_t v) {
    char tmp[19];
    int i = 18;

    tmp[i] = '\0';
    if (v == 0) {
        tmp[--i] = '0';
    }
    while (v != 0) {
        uint64_t d = v & 0xF;
        tmp[--i] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    }
    bufPuts(ctx, "0x");
    bufPuts(ctx, tmp + i);
}

static void bufDec(void *ctx, uint64_t v) {
    char tmp[21];
    int i = 20;

    tmp[i] = '\0';
    do {
        tmp[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    bufPuts(ctx, tmp + i);
}

static void makeSink(UsSink *sink, Buffer *buf) {
    sink->ctx = buf;
    sink->puts = bufPuts;
    sink->hex = bufHex;
    sink->dec = bufDec;
}

static size_t countLines(const char *text) {
    size_t n = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            n++;
        }
    }
    return n;
}

/*
 * The transfer to the kernel, as the locator looks for it: the loader moves
 * the kernel's stack into SP, keeps the kernel entry point in a register, and
 * finally branches to it. The call in the middle goes somewhere different in
 * every version, so its target is not part of the test
 *
 * Returns the offset of the branch, which is what a patch would replace
 */
static uint32_t putLeaf(uint8_t *image, uint32_t rva) {
    putInsn(image, rva + 0x00, 0x9100005F);  /* mov sp, x2 */
    putInsn(image, rva + 0x04, 0xAA0003F3);  /* mov x19, x0 */
    putInsn(image, rva + 0x08, 0xAA0103F4);  /* mov x20, x1 */
    putInsn(image, rva + 0x0C, 0x94001234);  /* bl  anywhere */
    putInsn(image, rva + 0x10, 0xAA1303E0);  /* mov x0, x19 */
    putInsn(image, rva + 0x14, 0xD61F0280);  /* br  x20 */
    return rva + 0x14;
}

/* --- the checks --------------------------------------------------------- */

static void testSiteCollection(void) {
    UsSiteList list;
    UsImage img;

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    usSiteListInit(&list);

    ok("collecting from nothing finds nothing", usCollectSites(&list, NULL, 0) == 0);
    eqSize("and adds nothing", list.total, 0);

    /* Three vector table installations, in an order the collector does not
     * see: the dump sorts, so the input order must not matter */
    putInsn(gWinload, TEXT_RVA + 0x100, 0xD518C010);  /* msr vbar_el1, x16 */
    putInsn(gWinload, TEXT_RVA + 0x040, 0xD518C000);  /* msr vbar_el1, x0 */
    putInsn(gWinload, TEXT_RVA + 0x280, 0xD518C001);  /* msr vbar_el1, x1 */

    /* Not a vector table write, and must not be taken for one: msr spsel */
    putInsn(gWinload, TEXT_RVA + 0x300, 0xD50041BF);

    /* LDAPR instructions, which are counted rather than collected */
    putInsn(gWinload, TEXT_RVA + 0x400, 0xB8BFC000);  /* ldapr w0, [x0] */
    putInsn(gWinload, TEXT_RVA + 0x404, 0x38BFC021);  /* ldaprb w1, [x1] */

    ok("the image is valid", usImageInitMemory(&img, gWinload, sizeof(gWinload)));
    eqSize("collects the three writes", usCollectSites(&list, &img, UsImageWinload), 3);
    eqSize("total found", list.total, 3);
    eqSize("stored", list.count, 3);

    usSiteListSort(&list);
    eqSize("first write is the lowest rva", list.sites[0].rva, TEXT_RVA + 0x040);
    eqSize("second write", list.sites[1].rva, TEXT_RVA + 0x100);
    eqSize("third write", list.sites[2].rva, TEXT_RVA + 0x280);
    eqSize("register of the first", list.sites[0].auxiliary, 0);
    eqSize("register of the second", list.sites[1].auxiliary, 16);
    eqSize("register of the third", list.sites[2].auxiliary, 1);

    {
        UsLDAPRCounts c = usCountLDAPR(NULL);
        eqSize("ldapr of nothing is nothing", c.total, 0);
    }
    {
        UsLDAPRCounts c = usCountLDAPR(&img);
        eqSize("one ldapr word", c.word, 1);
        eqSize("one ldapr byte", c.byte, 1);
        eqSize("two ldapr in total", c.total, 2);
    }

    eqStr("site kind name", usSiteKindName(UsSiteVBARWrite), "vbar-write");
    eqStr("image kind name", usImageKindName(UsImageNtoskrnl), "ntoskrnl");
}

/*
 * The transfer is a branch inside a function, not a function entry, so the
 * exception directory is not consulted and must not be
 *
 * What the patch point has to be is the branch itself. The four bytes before
 * it are the last thing the loader does with the kernel's entry point
 */
static void testLeafPatchPointIsTheBranch(void) {
    UsImage img;
    UsSiteList list;
    uint32_t branch;

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    branch = putLeaf(gWinload, TEXT_RVA + 0x1000);
    usImageInitMemory(&img, gWinload, sizeof(gWinload));

    {
        UsLeafSite leaf = usLocateTransferLeaf(&img);
        ok("the sequence is found", leaf.found);
        eqSize("and it is unique", leaf.matches, 1);
        eqSize("the sequence starts where it was written", leaf.rva, TEXT_RVA + 0x1000);
        eqSize("the patch point is the branch", leaf.patchRVA, branch);
    }

    usSiteListInit(&list);
    usCollectSites(&list, &img, UsImageWinload);
    eqSize("it becomes a site", list.count, 1);
    eqSize("as a transfer leaf", list.sites[0].kind, UsSiteTransferLeaf);
    eqSize("at the branch", list.sites[0].rva, branch);
}

/*
 * A sequence that is almost the same but does not end in the branch is not
 * the transfer, and must not be taken for it: everything here is checked
 * because getting it wrong means patching a place that does nothing
 */
static void testLeafRefusesNearMisses(void) {
    UsImage img;

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    putLeaf(gWinload, TEXT_RVA + 0x1000);
    /* Change the branch into a return */
    putInsn(gWinload, TEXT_RVA + 0x1014, 0xD65F03C0);
    usImageInitMemory(&img, gWinload, sizeof(gWinload));
    ok("a return instead of the branch is not the transfer",
       !usLocateTransferLeaf(&img).found);

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    putLeaf(gWinload, TEXT_RVA + 0x1000);
    /* Change the register the branch goes through */
    putInsn(gWinload, TEXT_RVA + 0x1014, 0xD61F0260);
    usImageInitMemory(&img, gWinload, sizeof(gWinload));
    ok("a branch through another register is not the transfer",
       !usLocateTransferLeaf(&img).found);
}

static void testPlanDump(void) {
    UsImage winload;
    UsImage kernel;
    UsPlan plan;
    Buffer buf;
    UsSink sink;

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    buildImage(gKernel, ".text", US_PE_SUBSYSTEM_NATIVE);
    usImageInitMemory(&winload, gWinload, sizeof(gWinload));
    usImageInitMemory(&kernel, gKernel, sizeof(gKernel));

    putInsn(gWinload, TEXT_RVA + 0x040, 0xD518C000);
    putInsn(gKernel, TEXT_RVA + 0x100, 0xD518C010);

    ok("a plan without a leaf is not complete", !usPlanBuild(&plan, &winload, &kernel));
    eqSize("two images", plan.imageCount, 2);
    eqSize("two writes", plan.sites.count, 2);
    eqSize("no ldapr", plan.ldapr.total, 0);
    eqSize("pool pages are stated", plan.poolPages, US_POOL_PAGES);
    eqSize("pool bytes are stated", plan.poolBytes, US_POOL_BYTES);

    memset(&buf, 0, sizeof(buf));
    makeSink(&sink, &buf);
    usPlanEmit(&plan, &sink);

    ok("the dump is not empty", buf.len > 0);
    ok("images line",
       strstr(buf.text, "plan: images 2 ntoskrnl=0x20000 winload=0x20000") != NULL);
    ok("ldapr line", strstr(buf.text, "plan: ldapr w=0 x=0 b=0 h=0 total=0") != NULL);
    ok("site count line", strstr(buf.text, "plan: sites 2 of 2") != NULL);
    ok("the kernel site comes first",
       strstr(buf.text, "plan: site vbar-write ntoskrnl +0x1100 x16") != NULL);
    ok("then the loader's", strstr(buf.text, "plan: site vbar-write winload +0x1040") != NULL);
    ok("the thunk is placed in the kernel",
       strstr(buf.text, "plan: thunk ntoskrnl +0x") != NULL);
    ok("the vector tables are reported",
       strstr(buf.text, "plan: vbar sites=1 unresolved=1 tables=0") != NULL);
    ok("incompleteness is stated", strstr(buf.text, "plan: complete=0") != NULL);
    eqSize("dump is line based", countLines(buf.text), 9);

    /* The same plan printed twice has to be identical, byte for byte */
    {
        Buffer again;
        UsSink sink2;

        memset(&again, 0, sizeof(again));
        makeSink(&sink2, &again);
        usPlanEmit(&plan, &sink2);
        ok("the dump is stable", strcmp(buf.text, again.text) == 0);
    }
}

static void testCompletePlan(void) {
    UsImage winload;
    UsImage kernel;
    UsPlan plan;

    buildImage(gWinload, ".text", US_PE_SUBSYSTEM_EFI_APPLICATION);
    buildImage(gKernel, ".text", US_PE_SUBSYSTEM_NATIVE);

    putLeaf(gWinload, TEXT_RVA + 0x1000);

    putInsn(gWinload, TEXT_RVA + 0x2000, 0xD5182000);  /* msr ttbr0_el1, x0 */
    putInsn(gWinload, TEXT_RVA + 0x2004, 0xD5033FDF);  /* isb */
    putInsn(gWinload, TEXT_RVA + 0x2008, 0xD5182021);  /* msr ttbr1_el1, x1 */
    putInsn(gWinload, TEXT_RVA + 0x200C, 0xD518A202);  /* msr mair_el1, x2 */
    putInsn(gWinload, TEXT_RVA + 0x2010, 0xD5182043);  /* msr tcr_el1, x3 */
    putInsn(gWinload, TEXT_RVA + 0x2014, 0xD5181004);  /* msr sctlr_el1, x4 */

    usImageInitMemory(&winload, gWinload, sizeof(gWinload));
    usImageInitMemory(&kernel, gKernel, sizeof(gKernel));

    {
        UsLeafSite leaf = usLocateTransferLeaf(&winload);
        ok("the leaf is found", leaf.found);
        eqSize("the leaf is unique", leaf.matches, 1);
        eqSize("the leaf starts at the sequence", leaf.rva, TEXT_RVA + 0x1000);
        eqSize("and patches the branch", leaf.patchRVA, TEXT_RVA + 0x1014);
    }
    {
        UsHandoffSite handoff = usLocateTTBRHandoff(&winload);
        ok("the handoff is found", handoff.found);
        eqSize("the handoff is at the ttbr0 write", handoff.rva, TEXT_RVA + 0x2000);
    }

    ok("a plan with both is complete", usPlanBuild(&plan, &winload, &kernel));
    eqSize("two sites in all", plan.sites.count, 2);
    eqSize("one is the leaf", plan.sites.sites[0].kind, UsSiteTransferLeaf);
    eqSize("one is the handoff", plan.sites.sites[1].kind, UsSiteTTBRHandoff);

    /* And without the kernel there is nothing to plan against */
    ok("a plan without the kernel is not complete", !usPlanBuild(&plan, &winload, NULL));
    ok("a plan with neither is not complete", !usPlanBuild(&plan, NULL, NULL));
    eqSize("and covers no images", plan.imageCount, 0);
}

/*
 * Finding the vector table the loader installs
 *
 * Shape is not enough, and this is where that is written down: the table here
 * has its synchronous slot holding a branch to itself, exactly like the one
 * shape-based search would pick, and a second table is added that looks the
 * same. What tells them apart is the register the write site loads, which is
 * what this follows
 */
static void testVBARDiscovery(void) {
    UsImage kernel;
    UsVBARTables tables;

    buildImage(gKernel, ".text", US_PE_SUBSYSTEM_NATIVE);
    usImageInitMemory(&kernel, gKernel, sizeof(gKernel));

    /* A table at 0x2000 with an unused synchronous slot, and one at 0x3000
     * with the slot taken by something real */
    putInsn(gKernel, 0x2000 + 0x200, 0x14000000);
    putInsn(gKernel, 0x3000 + 0x200, 0xD503201F);

    /* adrp x8, 0x2000 ; add x8, x8, #0 ; msr vbar_el1, x8 */
    putInsn(gKernel, TEXT_RVA + 0x100, 0xB0000008);
    putInsn(gKernel, TEXT_RVA + 0x104, 0x91000108);
    putInsn(gKernel, TEXT_RVA + 0x108, 0xD518C008);

    /* adrp x9, 0x3000 ; add x9, x9, #0 ; msr vbar_el1, x9 */
    putInsn(gKernel, TEXT_RVA + 0x200, 0xD0000009);
    putInsn(gKernel, TEXT_RVA + 0x204, 0x91000129);
    putInsn(gKernel, TEXT_RVA + 0x208, 0xD518C009);

    /* ldr x10, [x0, #8] ; msr vbar_el1, x10 -- a runtime value */
    putInsn(gKernel, TEXT_RVA + 0x300, 0xF940040A);
    putInsn(gKernel, TEXT_RVA + 0x304, 0xD518C00A);

    tables = usFindVBARTables(&kernel);
    eqSize("three writes are seen", tables.sites, 3);
    eqSize("two tables are resolved", tables.count, 2);
    eqSize("and one is not", tables.unresolved, 1);
    ok("the first is the one adrp points at", tables.rvas[0] == 0x2000);
    ok("the second is the one the other adrp points at", tables.rvas[1] == 0x3000);

    {
        int32_t displacement = 0;

        ok("the first table's synchronous slot is a branch to itself",
           usVectorSlotBranch(&kernel, tables.rvas[0], UsVectorSlotEL1hSync,
                              &displacement) && displacement == 0);
    }

    /* A slot holding anything else is a handler written out in place, and
     * there is nothing there to keep for the exceptions that are not ours */
    ok("the second table's is not a branch",
       !usVectorSlotBranch(&kernel, tables.rvas[1], UsVectorSlotEL1hSync, NULL));
}

int main(void) {
    testSiteCollection();
    testLeafPatchPointIsTheBranch();
    testLeafRefusesNearMisses();
    testPlanDump();
    testCompletePlan();
    testVBARDiscovery();

    return usCheckSummary(checks, failures);
}
