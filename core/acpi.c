/*
 * Reading the processor list out of ACPI, see acpi.h.
 *
 * The layouts are from the ACPI 6.5 specification: the root pointer, the
 * table header every table starts with, and the two structures that matter
 * here -- the MADT and its GIC CPU Interface entries. Offsets are written
 * out with the field names from the tables rather than in a packed struct,
 * because the tables are firmware's and reading them through a struct the
 * compiler is free to pad is how a reader ends up one byte out.
 */

#include "core/acpi.h"

/* --- little endian reads, because the tables are not aligned ------------------ */

static uint8_t rd8(const uint8_t *p) {
    return p[0];
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool signatureIs(const uint8_t *p, const char *sig) {
    for (int i = 0; i < 4; i++) {
        if (p[i] != (uint8_t)sig[i]) {
            return false;
        }
    }
    return true;
}

/* The sum of every byte of a table, including the checksum, is zero. */
static bool checksumOk(const uint8_t *p, size_t len) {
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

/* --- the root pointer --------------------------------------------------------- */

#define RSDP_SIGNATURE "RSD PTR "
#define RSDP_V1_BYTES 20U   /* the part the original checksum covers */
#define RSDP_RSDT 16U       /* 32 bit address of the RSDT */
#define RSDP_REVISION 15U
#define RSDP_XSDT 24U       /* 64 bit address of the XSDT, revision 2 and up */
#define RSDP_V2_BYTES 36U

/* --- the header every table begins with -------------------------------------- */

#define TABLE_SIGNATURE 0U
#define TABLE_LENGTH 4U
#define TABLE_BYTES 36U     /* the header's own size, before any entries */

/* --- the MADT ----------------------------------------------------------------- */

#define MADT_ENTRIES 44U    /* where the controller structures start */

/* Entry types this knows about. The list is in table 5.21; anything else is
 * skipped by its length, which is what the length field is for. */
#define MADT_TYPE_GICC 0x0BU

/* One GIC CPU Interface entry, offsets from its start. */
#define GICC_LENGTH 1U
#define GICC_UID 8U
#define GICC_MPIDR 68U
#define GICC_MPIDR_END 76U

const void *usAcpiFindMadt(const void *rsdp) {
    const uint8_t *p = rsdp;
    const uint8_t *root;
    const uint8_t *end;
    uint32_t length;
    size_t entryBytes;
    bool extended;

    if (p == NULL) {
        return NULL;
    }
    for (int i = 0; i < 8; i++) {
        if (p[i] != (uint8_t)RSDP_SIGNATURE[i]) {
            return NULL;
        }
    }
    /* Revision 0 is the original, which has no XSDT; anything later has both
     * and the XSDT is the one to use, because the RSDT cannot describe a
     * table above 4 GB. */
    extended = rd8(p + RSDP_REVISION) >= 2;
    if (!checksumOk(p, extended ? RSDP_V2_BYTES : RSDP_V1_BYTES)) {
        return NULL;
    }

    root = (const uint8_t *)(uintptr_t)rd64(p + RSDP_XSDT);
    if (!extended) {
        root = (const uint8_t *)(uintptr_t)(uint64_t)rd32(p + RSDP_RSDT);
    }
    if (root == NULL) {
        return NULL;
    }
    entryBytes = extended ? 8U : 4U;
    length = rd32(root + TABLE_LENGTH);
    if (length < TABLE_BYTES || !checksumOk(root, length)) {
        return NULL;
    }

    end = root + length;
    for (const uint8_t *at = root + TABLE_BYTES; at + entryBytes <= end; at += entryBytes) {
        const uint8_t *table = (const uint8_t *)(uintptr_t)
            (entryBytes == 8 ? rd64(at) : (uint64_t)rd32(at));

        if (table == NULL) {
            continue;
        }
        /* The signature is read before anything else about the table, and the
         * length is not trusted until this says it is a table wanted here. */
        if (signatureIs(table, "APIC")) {
            uint32_t madtLength = rd32(table + TABLE_LENGTH);

            if (madtLength < MADT_ENTRIES || !checksumOk(table, madtLength)) {
                return NULL;
            }
            return table;
        }
    }
    return NULL;
}

UsAcpiCpus usAcpiCollectCpus(const void *madt) {
    UsAcpiCpus out = { 0 };
    const uint8_t *p = madt;
    const uint8_t *end;
    uint32_t length;

    if (p == NULL || !signatureIs(p, "APIC")) {
        return out;
    }
    length = rd32(p + TABLE_LENGTH);
    if (length < MADT_ENTRIES) {
        return out;
    }
    end = p + length;

    /*
     * Each entry carries its own length, so an entry this does not know is
     * skipped rather than ending the walk. A zero length would end it by
     * looping forever, so that is refused rather than followed.
     */
    for (const uint8_t *at = p + MADT_ENTRIES; at + 2 <= end;) {
        uint8_t type = rd8(at);
        uint8_t entryLength = rd8(at + GICC_LENGTH);

        if (entryLength < 2 || at + entryLength > end) {
            break;
        }

        if (type == MADT_TYPE_GICC && entryLength >= GICC_MPIDR_END) {
            if (out.count < US_ACPI_MAX_CPUS) {
                out.mpidr[out.count++] = rd64(at + GICC_MPIDR) & US_MPIDR_AFFINITY_MASK;
            } else {
                out.overflow = true;
            }
        }
        at += entryLength;
    }

    return out;
}

int usAcpiCpuIndex(const UsAcpiCpus *cpus, uint64_t mpidr) {
    uint64_t wanted;

    if (cpus == NULL) {
        return -1;
    }
    wanted = mpidr & US_MPIDR_AFFINITY_MASK;

    for (size_t i = 0; i < cpus->count; i++) {
        if (cpus->mpidr[i] == wanted) {
            return (int)i;
        }
    }
    return -1;
}
