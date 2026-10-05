/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The machine's processors, as the firmware describes them. See acpi.c
 */

#ifndef US_UEFI_ACPI_H
#define US_UEFI_ACPI_H

#include "core/acpi.h"

/*
 * Reads the processor list out of the firmware's ACPI tables
 *
 * Returns an empty list when there is no table to read, which the caller has
 * to treat as "one processor, the one we are on" rather than as "no
 * processors": the boot carries on either way, and an empty list that was
 * taken literally would leave every CPU without a stack
 */
UsACPICPUs usACPIProbeCPUs(void);

#endif