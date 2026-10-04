/*
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
        printf("FAIL %-46s want %d got %d\n", name, want, got);
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
static uint8_t *gMadt;
static uint8_t *gRsdp;

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
    gRsdp = mapLow(0x0000, 0x100);
    gRoot = mapLow(0x1000, 0x1000);
    gMadt = mapLow(0x2000, 0x1000);
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

static size_t gMadtLength;

/* Builds a MADT with the given GIC CPU Interface entries, and a root table
 * that points at it */
static void buildTables(const CpuSpec *cpus, size_t count, bool extended) {
    size_t at = 44;

    memset(gMadt, 0, 0x1000);
    memcpy(gMadt, "APIC", 4);
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = gMadt + at;

        e[0] = 0x0B;        /* GICC */
        e[1] = 82;          /* the length ACPI 6.5 gives it */
        put32(e + 4, (uint32_t)i);
        put32(e + 8, cpus[i].uid);
        put32(e + 12, 1);   /* Enabled */
        put64(e + 68, cpus[i].mpidr);
        at += 82;
    }
    gMadtLength = at;
    put32(gMadt + 4, (uint32_t)gMadtLength);
    seal(gMadt, gMadtLength);

    memset(gRoot, 0, 0x1000);
    if (extended) {
        memcpy(gRoot, "XSDT", 4);
        put32(gRoot + 4, 36 + 8);
        put64(gRoot + 36, (uint64_t)(uintptr_t)gMadt);
        seal(gRoot, 36 + 8);
    } else {
        memcpy(gRoot, "RSDT", 4);
        put32(gRoot + 4, 36 + 4);
        put32(gRoot + 36, (uint32_t)(uintptr_t)gMadt);
        seal(gRoot, 36 + 4);
    }

    memset(gRsdp, 0, 0x100);
    memcpy(gRsdp, "RSD PTR ", 8);
    gRsdp[15] = extended ? 2 : 0;
    put32(gRsdp + 16, (uint32_t)(uintptr_t)gRoot);
    put64(gRsdp + 24, (uint64_t)(uintptr_t)gRoot);
    {
        uint8_t sum = 0;
        size_t len = extended ? 36 : 20;

        for (size_t i = 0; i < len; i++) {
            sum = (uint8_t)(sum + gRsdp[i]);
        }
        gRsdp[8] = (uint8_t)(0 - sum);
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
    UsAcpiCpus got;

    buildTables(cpus, 8, true);
    madt = usAcpiFindMadt(gRsdp);
    ok("the MADT is found through the XSDT", madt != NULL);

    got = usAcpiCollectCpus(madt);
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
    UsAcpiCpus got;

    buildTables(cpus, 2, false);
    ok("the MADT is found through the RSDT too", usAcpiFindMadt(gRsdp) != NULL);
    got = usAcpiCollectCpus(usAcpiFindMadt(gRsdp));
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
    ok("a good table is accepted", usAcpiFindMadt(gRsdp) != NULL);

    gMadt[20] ^= 0xFF;
    ok("a damaged MADT is refused", usAcpiFindMadt(gRsdp) == NULL);
    gMadt[20] ^= 0xFF;

    gRoot[40] ^= 0xFF;
    ok("a damaged root table is refused", usAcpiFindMadt(gRsdp) == NULL);
    gRoot[40] ^= 0xFF;

    gRsdp[10] ^= 0xFF;
    ok("a damaged root pointer is refused", usAcpiFindMadt(gRsdp) == NULL);
    gRsdp[10] ^= 0xFF;

    ok("and a good one is accepted again", usAcpiFindMadt(gRsdp) != NULL);
}

static void testRobustness(void) {
    static const CpuSpec cpus[] = {
        { 0, 0 }, { 1, 1 },
    };

    ok("a null root pointer is refused", usAcpiFindMadt(NULL) == NULL);

    buildTables(cpus, 2, true);
    gRsdp[0] = 'X';
    ok("a wrong signature is refused", usAcpiFindMadt(gRsdp) == NULL);
    gRsdp[0] = 'R';

    /* An entry of an unknown type has to be skipped by its length rather than
     * ending the walk, or a machine with one would report no processors */
    {
        uint8_t *madt = gMadt;
        size_t old = gMadtLength;

        /* Insert a distributor entry before the first GICC */
        memmove(madt + 44 + 24, madt + 44, old - 44);
        memset(madt + 44, 0, 24);
        madt[44] = 0x0C;    /* GICD */
        madt[45] = 24;
        gMadtLength = old + 24;
        put32(madt + 4, (uint32_t)gMadtLength);
        seal(madt, gMadtLength);

        {
            UsAcpiCpus got = usAcpiCollectCpus(madt);

            eqInt("an unknown entry is skipped", (int)got.count, 2);
        }
    }
}

int main(void) {
    setUpTables();
    testRealShape();
    testOlderRoot();
    testChecksums();
    testRobustness();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
