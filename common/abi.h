/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The facts the boot and the payload both name
 *
 * The two halves are separate programs: the boot is a PE image linked for the
 * firmware and the payload is a blob linked at zero, and neither can call the
 * other. What they share is a handful of small encodings - a serial port kind,
 * a reason a machine stopped, a run of bytes - which each side used to write
 * out again for itself, with a comment saying to keep the copy in step. That
 * is a fact with two homes, and the two homes drift.
 *
 * So the encodings live here, where both sides include them and neither can
 * be the only one that changed. The layouts stay where they are: the pool is
 * common/layout.h, and the payload's configuration block is payload/payload.h,
 * because those carry host-side readers and generated offsets of their own
 *
 * A number here is part of an interface and not an implementation detail: a
 * note, a script or a report on a screen matches on it, so a value that is
 * already written down somewhere does not move
 */

#ifndef US_ABI_H
#define US_ABI_H

/*
 * Which serial port the payload writes to
 *
 * The boot decides this while the firmware can still be asked, and tells the
 * payload, which has no firmware and no ACPI to ask. Zero is a machine with
 * no port, and the payload stays silent for it
 *
 * The boot's own kind is an enumeration of its own; the two agree on these
 * numbers rather than on a type, which is what lets either side add a kind it
 * alone knows without changing this
 */
#define US_UART_KIND_SILENT 0U
#define US_UART_KIND_PL011 1U
#define US_UART_KIND_8250 2U

/*
 * Why a machine stopped
 *
 * Zero is "nothing went wrong", which is what a record holds on a machine
 * that is still running. Injected means the configuration asked for the stop:
 * the reporting path is only reached when something has already gone wrong,
 * so a stop that can be asked for is the only way to find out whether the
 * report works
 */
#define US_PANIC_CODE_NONE 0U
#define US_PANIC_CODE_INJECTED 0xF000U

#endif
