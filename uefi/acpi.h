/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The machine, as the firmware describes it. See acpi.c
 */

#ifndef US_UEFI_ACPI_H
#define US_UEFI_ACPI_H

#include "core/acpi.h"

/*
 * Where the root pointer is, for anything that wants to read the tables
 * itself. NULL when the firmware published none
 */
const void *usACPIFindRSDP(void);

/*
 * The processor list, read once. An empty list is what "no table to read"
 * looks like, and the caller has to treat it as "one processor, the one we
 * are on" rather than as "no processors": the boot carries on either way,
 * and an empty list taken literally would leave every CPU without a stack
 */
UsACPICPUs usACPIProbeCPUs(void);

/* Which table a configuration asked for its serial port in */
typedef enum UsACPIUARTTable_e {
    UsACPIUARTTableSPCR = 0,
    UsACPIUARTTableDBG2,
    UsACPIUARTTableDSDT,
} UsACPIUARTTable;

/*
 * The console the firmware describes, or a kind of UsACPIUARTNone when no
 * table describes one this can drive
 *
 * 'path' is only for the DSDT, where a device is named rather than listed
 */
UsACPIUART usACPIProbeUART(UsACPIUARTTable table, const char *path);

/* The DSDT on its own, because it is the one that has to be named */
UsACPIUART usACPIDSDTUART(const char *path);

#endif