/*
 * The machine's processor list, from the firmware's own description
 *
 * The payload indexes everything it keeps per CPU by an index, and the
 * obvious source for one -- the low byte of MPIDR_EL1 -- is wrong: that byte
 * is the core within a cluster, so it repeats across clusters. ACPI describes
 * each processor and carries its MPIDR, which is what this reads
 *
 * The reading itself is in core/acpi.c, where it is a pure function over a
 * table and can be tested without a machine. All that is here is finding the
 * table: the firmware publishes a pointer to it in the configuration table,
 * under one of two guids depending on how old it is
 */

#include <uefi.h>

#include "core/acpi.h"
#include "uefi/src/acpi.h"
#include "uefi/src/console.h"

/*
 * The root pointer the firmware published. The newer guid is looked for
 * first: its table can describe a table anywhere, while the older one's
 * entries cannot reach past four gigabytes
 */
static const void *findRSDP(void) {
    static const efi_guid_t newer = ACPI_20_TABLE_GUID;
    static const efi_guid_t older = ACPI_TABLE_GUID;

    if (ST == NULL || ST->ConfigurationTable == NULL) {
        return NULL;
    }

    for (uintn_t i = 0; i < ST->NumberOfTableEntries; i++) {
        const efi_configuration_table_t *t = &ST->ConfigurationTable[i];

        if (t->VendorTable == NULL) {
            continue;
        }
        if (memcmp(&t->VendorGuid, &newer, sizeof(newer)) == 0) {
            return t->VendorTable;
        }
    }
    for (uintn_t i = 0; i < ST->NumberOfTableEntries; i++) {
        const efi_configuration_table_t *t = &ST->ConfigurationTable[i];

        if (t->VendorTable != NULL
            && memcmp(&t->VendorGuid, &older, sizeof(older)) == 0) {
            return t->VendorTable;
        }
    }
    return NULL;
}

UsACPICPUs usACPIProbeCPUs(void) {
    const void *rsdp = findRSDP();
    const void *madt = usACPIFindMADT(rsdp);

    if (madt == NULL) {
        usConsolePuts("acpi: no processor list, falling back to one CPU\n");
        return (UsACPICPUs){ 0 };
    }
    return usACPICollectCPUs(madt);
}
