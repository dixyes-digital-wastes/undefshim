/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The machine's own description of itself, as the firmware published it
 *
 * Reading a table's contents is in core/acpi.c, where it is a pure function
 * over bytes and can be checked without a machine. What is here is what
 * needs one: finding the tables, and running the AML in them.
 *
 * uACPI does both. It knows the root pointer's two layouts, the XSDT before
 * the RSDT, the list of tables and their checksums; and it has an interpreter
 * for the DSDT, which is the only way to reach a device that describes itself
 * in a method rather than in a table of its own
 */

#include <uefi.h>

#pragma push_macro("_MSC_VER")
#undef _MSC_VER
#define UACPI_OVERRIDE_TYPES
#include "uacpi/uacpi.h"
#include "uacpi/tables.h"
#include "uacpi/namespace.h"
#include "uacpi/resources.h"
#pragma pop_macro("_MSC_VER")

#include "core/acpi.h"
#include "uefi/acpi.h"
#include "uefi/console.h"
#include "uefi/uacpi_kernel.h"

/*
 * The root pointer the firmware published. The newer guid is looked for
 * first: its table can describe a table anywhere, while the older one's
 * entries cannot reach past four gigabytes
 */
const void *usACPIFindRSDP(void) {
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

/* --- bringing uACPI up --------------------------------------------------------- */

/*
 * uACPI's own record of the tables, from before it has a heap. It is static
 * rather than on a stack because it stays in use for as long as uACPI is
 * only being asked for tables, which is every run that does not name the
 * DSDT
 */
static uint8_t gTableBuffer[4096];
static bool    gTablesUp;
static bool    gNamespaceUp;

/*
 * Enough to look a table up by signature, which is all the processor list
 * and the serial tables need. Nothing is allocated at this stage: uACPI's
 * own porting notes are explicit that it calls no kernel API besides logging
 */
static bool tableAccessUp(void) {
    if (gTablesUp) {
        return true;
    }
    if (usACPIFindRSDP() == NULL) {
        return false;
    }
    gTablesUp = uacpi_setup_early_table_access(gTableBuffer, sizeof(gTableBuffer))
                == UACPI_STATUS_OK;
    return gTablesUp;
}

/* uACPI's view of a table, or NULL when there is not one */
static const void *findTable(const char *signature) {
    uacpi_table table;

    if (!tableAccessUp()) {
        return NULL;
    }
    if (uacpi_table_find_by_signature(signature, &table) != UACPI_STATUS_OK) {
        return NULL;
    }
    return table.ptr;
}

UsACPICPUs usACPIProbeCPUs(void) {
    const void *madt = usACPIFindMADT(usACPIFindRSDP());

    if (madt == NULL) {
        return (UsACPICPUs){ 0 };
    }
    return usACPICollectCPUs(madt);
}

/* --- the serial port ----------------------------------------------------------- */

UsACPIUART usACPIProbeUART(UsACPIUARTTable table, const char *path) {
    switch (table) {
    case UsACPIUARTTableSPCR: {
        const void *spcr = findTable("SPCR");

        return spcr != NULL ? usACPIParseSPCR(spcr) : (UsACPIUART){ 0 };
    }
    case UsACPIUARTTableDBG2: {
        const void *dbg2 = findTable("DBG2");

        return dbg2 != NULL ? usACPIParseDBG2(dbg2, path) : (UsACPIUART){ 0 };
    }
    case UsACPIUARTTableDSDT:
        return usACPIDSDTUART(path);
    default:
        return (UsACPIUART){ 0 };
    }
}

/* --- the DSDT ------------------------------------------------------------------ */

/*
 * A device that describes its port as a memory range. The range is the first
 * memory resource the device's _CRS returns: a serial device describes its
 * registers as one and everything else -- an interrupt, a GPIO -- as another
 * kind, so the first memory one is the port
 */
static uacpi_iteration_decision tookMemoryResource(void *user, uacpi_resource *resource) {
    UsACPIUART *found = user;

    switch (resource->type) {
    case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        found->base = resource->fixed_memory32.address;
        break;
    case UACPI_RESOURCE_TYPE_MEMORY32:
        found->base = resource->memory32.minimum;
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS32:
        found->base = (uint64_t)resource->address32.minimum;
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS64:
    case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
        found->base = (uint64_t)resource->address64.minimum;
        break;
    default:
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

/*
 * The whole interpreter, for a port that is described by a method rather
 * than by a table of its own
 *
 * This is the expensive path and it is only taken when the configuration
 * names it: loading a namespace runs the DSDT and every SSDT, which needs
 * the heap and is a different order of work from finding a table by
 * signature
 *
 * The path arrives without its leading backslash. That is not a convention
 * chosen here: the configuration's own parser hands out strings that contain
 * none, so a file cannot spell one, and what a file writes is the path from
 * the root with the marker left off. Putting it back is this function's
 * business, and it is what makes the answer the same path every other ACPI
 * reader would use
 */
UsACPIUART usACPIDSDTUART(const char *path) {
    uacpi_namespace_node *device = NULL;
    char absolute[US_ACPI_PATH_BYTES];
    UsACPIUART found = { 0 };

    if (path == NULL || path[0] == '\0' || !tableAccessUp()) {
        return found;
    }
    if (path[0] != '\\') {
        size_t length = 0;

        while (path[length] != '\0' && length + 2U < sizeof(absolute)) {
            absolute[length + 1U] = path[length];
            length++;
        }
        absolute[0] = '\\';
        absolute[length + 1U] = '\0';
        path = absolute;
    }
    if (!gNamespaceUp) {
        if (uacpi_initialize(0) != UACPI_STATUS_OK
            || uacpi_namespace_load() != UACPI_STATUS_OK) {
            return found;
        }
        gNamespaceUp = true;
    }
    /* NULL for the scope: the path is absolute, so it is the root's */
    if (uacpi_namespace_node_find(NULL, path, &device) != UACPI_STATUS_OK
        || device == NULL) {
        return found;
    }
    if (uacpi_for_each_device_resource(device, "_CRS", tookMemoryResource, &found)
        != UACPI_STATUS_OK) {
        return (UsACPIUART){ 0 };
    }
    /*
     * A device on this class of machine that describes one memory range and
     * is a serial console is a PL011. Which of the two it is would be in the
     * table the configuration did not ask for, and the width would be in its
     * access size, so what is left is the answer for the port and the width
     * the board is known to want
     */
    if (found.base != 0) {
        found.kind = UsACPIUARTPl011;
        found.width = 32U;
    }
    return found;
}

