/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The sizes the uACPI port is built around. See uacpi_kernel.c
 */

#ifndef US_UACPI_KERNEL_H
#define US_UACPI_KERNEL_H

/*
 * One line of uACPI's log, formatted by uACPI itself before it is handed
 * over. It is a buffer on the stack of whatever called in, which is our own
 * boot stack, so it is the length of a line and no more
 */
#define US_UACPI_LOG_BYTES 192U

/*
 * An ACPI name path with its leading marker put back on. A file cannot spell
 * one -- the configuration's parser hands out strings with no backslash in
 * them -- so the path arrives without it and is completed here
 */
#define US_ACPI_PATH_BYTES 64U

/*
 * The heap, taken from the firmware in one piece the first time anything
 * asks for memory. A namespace is what it is for: every object in the DSDT
 * and the SSDTs becomes a node, and the board's own tables are tens of
 * kilobytes of AML
 */
#define US_UACPI_HEAP_BYTES (32U * 1024U * 1024U)

#endif
