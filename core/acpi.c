/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Reading the processor list out of ACPI, see acpi.h
 *
 * The layouts are from the ACPI 6.5 specification: the root pointer, the
 * table header every table starts with, and the two structures that matter
 * here -- the MADT and its GIC CPU Interface entries. Offsets are written
 * out with the field names from the tables rather than in a packed struct,
 * because the tables are firmware's and reading them through a struct the
 * compiler is free to pad is how a reader ends up one byte out
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

/* The sum of every byte of a table, including the checksum, is zero */
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

#define TABLE_LENGTH 4U
#define TABLE_BYTES 36U     /* the header's own size, before any entries */

/* --- the MADT ----------------------------------------------------------------- */

#define MADT_ENTRIES 44U    /* where the controller structures start */

/* Entry types this knows about. The list is in table 5.21; anything else is
 * skipped by its length, which is what the length field is for */
#define MADT_TYPE_GICC 0x0BU

/* One GIC CPU Interface entry, offsets from its start */
#define GICC_LENGTH 1U
#define GICC_MPIDR 68U
#define GICC_MPIDR_END 76U

const void *usACPIFindMADT(const void *rsdp) {
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
     * table above 4 GB */
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
         * length is not trusted until this says it is a table wanted here */
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

UsACPICPUs usACPICollectCPUs(const void *madt) {
    UsACPICPUs out = { 0 };
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
     * looping forever, so that is refused rather than followed
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

/* --- the serial port ---------------------------------------------------------- */

/*
 * The two tables that name a console. Their offsets are from the ACPI
 * specification, written out with the field names the tables use rather than
 * read through a struct, for the reason every other reader here is: a struct
 * the compiler may pad is how a reader ends up a byte out
 */

#define SPCR_INTERFACE_TYPE 36U
#define SPCR_BASE_ADDRESS 40U        /* a Generic Address Structure */
#define SPCR_BASE_BYTES 52U          /* to the end of that structure */

#define GAS_ADDRESS_SPACE 0U
#define GAS_ACCESS_SIZE 3U
#define GAS_ADDRESS 4U
#define GAS_BYTES 12U

/* AddressSpaceId: the port is in memory. Anything else -- IO space, a PCI
 * configuration space -- is a port this boot has no way to reach */
#define GAS_SPACE_SYSTEM_MEMORY 0U

/* AccessSize, as the specification numbers them */
#define GAS_SIZE_BYTE 1U
#define GAS_SIZE_DWORD 3U

/* InterfaceType, from the specification's list */
#define SPCR_IF_16550 0x00U
#define SPCR_IF_16450 0x01U
#define SPCR_IF_ARM_PL011 0x03U
#define SPCR_IF_NVIDIA_16550 0x05U
#define SPCR_IF_ARM_SBSA_2X 0x0DU
#define SPCR_IF_ARM_SBSA 0x0EU
#define SPCR_IF_16550_GAS 0x12U

#define DBG2_DEVICE_INFO_OFFSET 36U
#define DBG2_DEVICE_INFO_COUNT 40U
#define DBG2_ENTRIES 44U

/* One Debug Device Information structure, offsets from its own start */
#define DDI_LENGTH 1U
#define DDI_REGISTER_COUNT 3U
#define DDI_NAMESPACE_LENGTH 4U
#define DDI_NAMESPACE_OFFSET 6U
#define DDI_PORT_TYPE 12U
#define DDI_PORT_SUBTYPE 14U
#define DDI_BASE_OFFSET 18U
#define DDI_BASE_OFFSET_END 20U
#define DDI_FIXED_BYTES 22U          /* up to the address register */

#define DBG2_PORT_TYPE_SERIAL 0x8000U
#define DBG2_SUBTYPE_PL011 0x0003U
#define DBG2_SUBTYPE_SBSA 0x0004U

static UsACPIUARTKind kindOfInterface(uint8_t interfaceType) {
    switch (interfaceType) {
    case SPCR_IF_ARM_PL011:
    case SPCR_IF_ARM_SBSA:
    case SPCR_IF_ARM_SBSA_2X:
        return UsACPIUARTPl011;
    case SPCR_IF_16550:
    case SPCR_IF_16450:
    case SPCR_IF_NVIDIA_16550:
    case SPCR_IF_16550_GAS:
        return UsACPIUART16550;
    default:
        return UsACPIUARTNone;
    }
}

/*
 * A Generic Address Structure, which is how both tables point at the port.
 * The width is the access size the table asks for, and only a byte or a word
 * is taken literally: everything else has meant 32 bits in practice
 */
static bool parseGAS(const uint8_t *gas, UsACPIUART *out) {
    if (rd8(gas + GAS_ADDRESS_SPACE) != GAS_SPACE_SYSTEM_MEMORY) {
        return false;
    }
    out->base = rd64(gas + GAS_ADDRESS);
    out->width = rd8(gas + GAS_ACCESS_SIZE) == GAS_SIZE_BYTE ? 8U : 32U;
    if (rd8(gas + GAS_ACCESS_SIZE) == GAS_SIZE_DWORD) {
        out->width = 32U;
    }
    return out->base != 0;
}

UsACPIUART usACPIParseSPCR(const void *spcr) {
    UsACPIUART out = { 0 };
    const uint8_t *p = spcr;

    if (p == NULL || !signatureIs(p, "SPCR") || rd32(p + TABLE_LENGTH) < SPCR_BASE_BYTES) {
        return out;
    }
    out.kind = kindOfInterface(rd8(p + SPCR_INTERFACE_TYPE));
    if (out.kind == UsACPIUARTNone || !parseGAS(p + SPCR_BASE_ADDRESS, &out)) {
        return (UsACPIUART){ 0 };
    }
    return out;
}

/*
 * The path is an ACPI namespace path, and what a debug device names itself
 * with is the last part of one: the device, without the scope it is in. So
 * the comparison is on the tail, which is what makes "\_SB.COM0" and "COM0"
 * the same answer
 */
static bool nameMatches(const uint8_t *namespaceString, uint16_t length,
                        const char *path) {
    const char *tail = path;

    if (length == 0 || path == NULL) {
        return false;
    }
    for (const char *at = path; *at != '\0'; at++) {
        if (*at == '\\' || *at == '.') {
            tail = at + 1;
        }
    }
    for (uint16_t i = 0; i < length; i++) {
        char a = (char)namespaceString[i];

        if (a == '\0') {
            return tail[i] == '\0';
        }
        if (a != tail[i]) {
            return false;
        }
    }
    return tail[length] == '\0';
}

UsACPIUART usACPIParseDBG2(const void *dbg2, const char *name) {
    UsACPIUART out = { 0 };
    const uint8_t *p = dbg2;
    uint32_t tableLength;
    uint32_t first;
    uint32_t count;

    if (p == NULL || !signatureIs(p, "DBG2") || rd32(p + TABLE_LENGTH) < DBG2_ENTRIES) {
        return out;
    }
    tableLength = rd32(p + TABLE_LENGTH);
    first = rd32(p + DBG2_DEVICE_INFO_OFFSET);
    count = rd32(p + DBG2_DEVICE_INFO_COUNT);
    if (first < DBG2_ENTRIES || first > tableLength || count > tableLength) {
        return out;
    }

    /*
     * Each device carries its own length, so one this does not know is
     * stepped over rather than ending the walk, and a length that would run
     * past the table ends it: what comes after is another table's memory
     */
    for (uint32_t index = 0, at = first;
         index < count && at + DDI_FIXED_BYTES <= tableLength;) {
        const uint8_t *ddi = p + at;
        uint16_t length = (uint16_t)(rd8(ddi + DDI_LENGTH)
                                     | ((uint16_t)rd8(ddi + DDI_LENGTH + 1U) << 8));
        uint16_t subtype = (uint16_t)(rd8(ddi + DDI_PORT_SUBTYPE)
                                      | ((uint16_t)rd8(ddi + DDI_PORT_SUBTYPE + 1U) << 8));
        uint16_t baseOffset = (uint16_t)(rd8(ddi + DDI_BASE_OFFSET)
                                         | ((uint16_t)rd8(ddi + DDI_BASE_OFFSET + 1U) << 8));

        if (length < DDI_FIXED_BYTES || at + length > tableLength) {
            break;
        }
        if (rd8(ddi + DDI_PORT_TYPE) == 0
            && (uint16_t)(rd8(ddi + DDI_PORT_TYPE + 1U) << 8) == DBG2_PORT_TYPE_SERIAL
            && baseOffset + GAS_BYTES <= length
            && parseGAS(ddi + baseOffset, &out)) {
            if (subtype == DBG2_SUBTYPE_PL011) {
                out.kind = UsACPIUARTPl011;
            } else if (subtype == DBG2_SUBTYPE_SBSA) {
                out.kind = UsACPIUARTPl011;
            } else if (subtype == 0U) {
                out.kind = UsACPIUART16550;
            } else {
                out.kind = UsACPIUARTNone;
            }
            if (out.kind != UsACPIUARTNone) {
                bool wanted = true;

                if (name != NULL) {
                    uint16_t nameLength = (uint16_t)(rd8(ddi + DDI_NAMESPACE_LENGTH)
                                                     | ((uint16_t)rd8(ddi + DDI_NAMESPACE_LENGTH + 1U) << 8));
                    uint16_t nameOffset = (uint16_t)(rd8(ddi + DDI_NAMESPACE_OFFSET)
                                                     | ((uint16_t)rd8(ddi + DDI_NAMESPACE_OFFSET + 1U) << 8));

                    wanted = nameOffset + nameLength <= length
                             && nameMatches(ddi + nameOffset, nameLength, name);
                }
                if (wanted) {
                    return out;
                }
            }
        }
        at += length;
        index++;
    }
    return (UsACPIUART){ 0 };
}

