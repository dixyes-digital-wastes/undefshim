/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for reading the processor list out of ACPI
 *
 * The tables are built here rather than taken from a machine, so the test says
 * what makes a table acceptable: the signature, the checksum, the lengths, and
 * the fields the parsing depends on. A corpus would exercise the same code
 * without saying which of those it was relying on
 *
 * The failure this is really about is the index. A CPU index that collides is
 * not a visible fault, it is two processors sharing one landing pad and one
 * stack, and the corruption surfaces somewhere else entirely
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "core/acpi.h"

#define US_CHECK_NAME "test_acpi"
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

static void eqInt(const char *name, int got, int want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-46s want %d got %d\n", name, want, got);
    }
}

/* --- building tables ---------------------------------------------------- */

/*
 * The tables live at a low address on purpose
 *
 * An RSDT holds 32 bit addresses, which is right for firmware: its tables are
 * at low physical addresses. A host process's own arrays are not, so a table
 * built there could not be described by an RSDT at all and the 32 bit path
 * would be untestable. Mapping the buffers low is what makes that path
 * reachable here, and mapping them at all is why this test needs a little
 * more than an array
 */
#define LOW_BASE 0x10000000U

static uint8_t *gRoot;
static uint8_t *gMADT;
static uint8_t *gRSDP;

static void *mapLow(size_t offset, size_t bytes) {
    void *p = mmap((void *)(uintptr_t)(LOW_BASE + offset), bytes,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (p == MAP_FAILED) {
        fprintf(stderr, "cannot map the test tables low\n");
        exit(2);
    }
    return p;
}

static void setUpTables(void) {
    gRSDP = mapLow(0x0000, 0x100);
    gRoot = mapLow(0x1000, 0x1000);
    gMADT = mapLow(0x2000, 0x1000);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* The checksum that makes a table sum to zero */
static void seal(uint8_t *p, size_t len) {
    uint8_t sum = 0;

    p[9] = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    p[9] = (uint8_t)(0 - sum);
}

typedef struct CpuSpec_t {
    uint64_t mpidr;
    uint32_t uid;
} CpuSpec;

static size_t gMADTLength;

/* Builds a MADT with the given GIC CPU Interface entries, and a root table
 * that points at it */
static void buildTables(const CpuSpec *cpus, size_t count, bool extended) {
    size_t at = 44;

    memset(gMADT, 0, 0x1000);
    memcpy(gMADT, "APIC", 4);
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = gMADT + at;

        e[0] = 0x0B;        /* GICC */
        e[1] = 82;          /* the length ACPI 6.5 gives it */
        put32(e + 4, (uint32_t)i);
        put32(e + 8, cpus[i].uid);
        put32(e + 12, 1);   /* Enabled */
        put64(e + 68, cpus[i].mpidr);
        at += 82;
    }
    gMADTLength = at;
    put32(gMADT + 4, (uint32_t)gMADTLength);
    seal(gMADT, gMADTLength);

    memset(gRoot, 0, 0x1000);
    if (extended) {
        memcpy(gRoot, "XSDT", 4);
        put32(gRoot + 4, 36 + 8);
        put64(gRoot + 36, (uint64_t)(uintptr_t)gMADT);
        seal(gRoot, 36 + 8);
    } else {
        memcpy(gRoot, "RSDT", 4);
        put32(gRoot + 4, 36 + 4);
        put32(gRoot + 36, (uint32_t)(uintptr_t)gMADT);
        seal(gRoot, 36 + 4);
    }

    memset(gRSDP, 0, 0x100);
    memcpy(gRSDP, "RSD PTR ", 8);
    gRSDP[15] = extended ? 2 : 0;
    put32(gRSDP + 16, (uint32_t)(uintptr_t)gRoot);
    put64(gRSDP + 24, (uint64_t)(uintptr_t)gRoot);
    {
        uint8_t sum = 0;
        size_t len = extended ? 36 : 20;

        for (size_t i = 0; i < len; i++) {
            sum = (uint8_t)(sum + gRSDP[i]);
        }
        gRSDP[8] = (uint8_t)(0 - sum);
    }
}

/* --- the tests ---------------------------------------------------------- */

/*
 * The machine this was written against: two clusters of four cores. Aff0
 * repeats across clusters, which is exactly why the low byte is not an index
 */
static void testRealShape(void) {
    static const CpuSpec cpus[] = {
        { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 },
        { 0x100, 4 }, { 0x101, 5 }, { 0x102, 6 }, { 0x103, 7 },
    };
    const void *madt;
    UsACPICPUs got;

    buildTables(cpus, 8, true);
    madt = usACPIFindMADT(gRSDP);
    ok("the MADT is found through the XSDT", madt != NULL);

    got = usACPICollectCPUs(madt);
    eqInt("eight processors", (int)got.count, 8);
    ok("no overflow", !got.overflow);

    ok("the first core of the first cluster is the first entry",
       got.mpidr[0] == 0);
    ok("the first core of the second cluster is the fifth",
       got.mpidr[4] == 0x100);
    ok("and the low byte is the same for both",
       (got.mpidr[0] & 0xFF) == (got.mpidr[4] & 0xFF));
}

static void testOlderRoot(void) {
    static const CpuSpec cpus[] = {
        { 0, 0 }, { 1, 1 },
    };
    UsACPICPUs got;

    buildTables(cpus, 2, false);
    ok("the MADT is found through the RSDT too", usACPIFindMADT(gRSDP) != NULL);
    got = usACPICollectCPUs(usACPIFindMADT(gRSDP));
    eqInt("two processors", (int)got.count, 2);
}

/*
 * A table that fails its checksum is refused rather than walked. The walk
 * follows lengths read out of the table, so a damaged one is a walk off the
 * end, and this runs too early for a fault to be reported as anything
 */
static void testChecksums(void) {
    static const CpuSpec cpus[] = {
        { 0, 0 },
    };

    buildTables(cpus, 1, true);
    ok("a good table is accepted", usACPIFindMADT(gRSDP) != NULL);

    gMADT[20] ^= 0xFF;
    ok("a damaged MADT is refused", usACPIFindMADT(gRSDP) == NULL);
    gMADT[20] ^= 0xFF;

    gRoot[40] ^= 0xFF;
    ok("a damaged root table is refused", usACPIFindMADT(gRSDP) == NULL);
    gRoot[40] ^= 0xFF;

    gRSDP[10] ^= 0xFF;
    ok("a damaged root pointer is refused", usACPIFindMADT(gRSDP) == NULL);
    gRSDP[10] ^= 0xFF;

    ok("and a good one is accepted again", usACPIFindMADT(gRSDP) != NULL);
}

static void testRobustness(void) {
    static const CpuSpec cpus[] = {
        { 0, 0 }, { 1, 1 },
    };

    ok("a null root pointer is refused", usACPIFindMADT(NULL) == NULL);

    buildTables(cpus, 2, true);
    gRSDP[0] = 'X';
    ok("a wrong signature is refused", usACPIFindMADT(gRSDP) == NULL);
    gRSDP[0] = 'R';

    /* An entry of an unknown type has to be skipped by its length rather than
     * ending the walk, or a machine with one would report no processors */
    {
        uint8_t *madt = gMADT;
        size_t old = gMADTLength;

        /* Insert a distributor entry before the first GICC */
        memmove(madt + 44 + 24, madt + 44, old - 44);
        memset(madt + 44, 0, 24);
        madt[44] = 0x0C;    /* GICD */
        madt[45] = 24;
        gMADTLength = old + 24;
        put32(madt + 4, (uint32_t)gMADTLength);
        seal(madt, gMADTLength);

        {
            UsACPICPUs got = usACPICollectCPUs(madt);

            eqInt("an unknown entry is skipped", (int)got.count, 2);
        }
    }
}

/* --- the serial tables --------------------------------------------------- */

/*
 * A SPCR, built field by field so the test says which fields the parsing
 * depends on. The port is the address, the interface type is what decides
 * which driver reads it, and the access size is how wide its registers are
 */
static size_t buildSPCR(uint8_t *out, uint8_t interfaceType, uint64_t base,
                        uint8_t accessSize) {
    size_t length = 60;

    memset(out, 0, length);
    memcpy(out, "SPCR", 4);
    put32(out + 4, (uint32_t)length);
    out[36] = interfaceType;
    /* A Generic Address Structure at 40: system memory, one register wide */
    out[40] = 0;            /* AddressSpaceId: memory */
    out[41] = 32;           /* RegisterBitWidth */
    out[42] = 0;            /* RegisterBitOffset */
    out[43] = accessSize;
    memcpy(out + 44, &base, 8);
    seal(out, length);
    return length;
}

static void testSPCR(void) {
    static uint8_t spcr[64];
    UsACPIUART got;

    buildSPCR(spcr, 0x03 /* ARM PL011 */, 0x94080000ULL, 3 /* DWORD */);
    got = usACPIParseSPCR(spcr);
    ok("a PL011 console is a PL011", got.kind == UsACPIUARTPl011);
    eqInt("and its address is the one written", (int)(got.base >> 32), 0);
    ok("all of it", got.base == 0x94080000ULL);
    eqInt("a DWORD wide register is 32 bits", (int)got.width, 32);

    /* The same table, one register a byte wide */
    buildSPCR(spcr, 0x03, 0x94080000ULL, 1 /* BYTE */);
    got = usACPIParseSPCR(spcr);
    eqInt("a BYTE wide register is 8 bits", (int)got.width, 8);

    /* The PC-derived console, which is the other kind that can be driven */
    buildSPCR(spcr, 0x00 /* 16550 */, 0x3F8ULL, 1);
    got = usACPIParseSPCR(spcr);
    ok("a 16550 console is an 8250", got.kind == UsACPIUART16550);
    ok("at its own address", got.base == 0x3F8ULL);

    /* Nothing to drive: a console this boot has no way to reach */
    buildSPCR(spcr, 0x0F /* DCC */, 0x94080000ULL, 3);
    got = usACPIParseSPCR(spcr);
    ok("a console that is not a serial port is none", got.kind == UsACPIUARTNone);

    /* A port in IO space rather than memory, which is not an address here */
    buildSPCR(spcr, 0x03, 0x3F8ULL, 1);
    spcr[40] = 1 /* IO space */;
    seal(spcr, 60);
    got = usACPIParseSPCR(spcr);
    ok("a port that is not in memory is none", got.kind == UsACPIUARTNone);

    /* A table too short to hold the fields: the address would be read past it */
    buildSPCR(spcr, 0x03, 0x94080000ULL, 3);
    put32(spcr + 4, 44);
    seal(spcr, 44);
    got = usACPIParseSPCR(spcr);
    ok("a truncated table is refused", got.kind == UsACPIUARTNone);

    ok("and a missing one is refused", usACPIParseSPCR(NULL).kind == UsACPIUARTNone);
}

/*
 * A DBG2, which is a list of debug devices rather than one. Each names
 * itself with a namespace string, and that string is the only thing that
 * tells two serial ports apart
 */
static size_t buildDBG2(uint8_t *out, const char *first, const char *second) {
    size_t at = 44;

    memset(out, 0, 256);
    memcpy(out, "DBG2", 4);
    put32(out + 36, 44);    /* where the devices start */
    put32(out + 40, second != NULL ? 2 : 1);

    {
        const char *names[2] = { first, second };
        uint32_t count = second != NULL ? 2 : 1;

        for (uint32_t i = 0; i < count; i++) {
            size_t nameLength = strlen(names[i]) + 1;
            size_t length = 22 + 12 + 4 + nameLength;
            uint8_t *ddi = out + at;

            ddi[0] = 0;                       /* revision */
            ddi[1] = (uint8_t)length;
            ddi[2] = (uint8_t)(length >> 8);
            ddi[3] = 1;                       /* one address register */
            ddi[4] = (uint8_t)nameLength;
            ddi[5] = (uint8_t)(nameLength >> 8);
            ddi[6] = 38;                      /* name offset */
            ddi[7] = 0;
            ddi[12] = 0x00;                   /* port type 0x8000, serial */
            ddi[13] = 0x80;
            ddi[14] = 0x03;                   /* subtype: ARM PL011 */
            ddi[15] = 0x00;
            ddi[18] = 22;                     /* where the address is */
            ddi[19] = 0;
            /* The device that is not the first one gets another address, so
             * that picking the wrong one is visible rather than lucky */
            {
                uint64_t base = i == 0 ? 0x94080000ULL : 0x94090000ULL;

                ddi[22] = 0;                  /* memory */
                ddi[23] = 32;
                ddi[24] = 0;
                ddi[25] = 3;                  /* DWORD */
                memcpy(ddi + 26, &base, 8);
            }
            memcpy(ddi + 38, names[i], nameLength);
            at += length;
        }
    }
    put32(out + 4, (uint32_t)at);
    seal(out, at);
    return at;
}

static void testDBG2(void) {
    static uint8_t dbg2[256];
    UsACPIUART got;

    buildDBG2(dbg2, "COM0", NULL);
    got = usACPIParseDBG2(dbg2, NULL);
    ok("a lone debug port is taken", got.kind == UsACPIUARTPl011);
    ok("at its address", got.base == 0x94080000ULL);

    /* A path is only the device: the scope it is in is not part of the name
     * the device gives itself, and a file writes neither */
    {
        static const char *const paths[] = {
            "COM0", "_SB.COM0", "\\_SB.COM0", "\\_SB.COM1",
        };

        ok("the device's own name matches", usACPIParseDBG2(dbg2, paths[0]).kind
           == UsACPIUARTPl011);
        ok("a path ending in it matches", usACPIParseDBG2(dbg2, paths[1]).kind
           == UsACPIUARTPl011);
        ok("with the leading marker too", usACPIParseDBG2(dbg2, paths[2]).kind
           == UsACPIUARTPl011);
        ok("a different device does not", usACPIParseDBG2(dbg2, paths[3]).kind
           == UsACPIUARTNone);
    }

    buildDBG2(dbg2, "COM0", "COM1");
    got = usACPIParseDBG2(dbg2, "COM1");
    ok("the second device is found by name", got.kind == UsACPIUARTPl011);
    ok("and it is its own address", got.base == 0x94090000ULL);

    got = usACPIParseDBG2(dbg2, NULL);
    ok("without a name the first is taken", got.base == 0x94080000ULL);

    ok("a missing table is refused", usACPIParseDBG2(NULL, "COM0").kind
       == UsACPIUARTNone);
}

int main(void) {
    setUpTables();
    testRealShape();
    testOlderRoot();
    testChecksums();
    testRobustness();
    testSPCR();
    testDBG2();

    return usCheckSummary(checks, failures);
}
