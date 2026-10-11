/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The console for a build that has no serial port, see console.h for the
 * console itself
 *
 * A line is written by log.c and the port behind it by uart_hw.c, and the two
 * are separate modules because the port is reached through registers while a
 * line is reached from everywhere. The port is therefore the part that can be
 * left out, and this file is what that build links against
 */

#include "uefi/console.h"

/*
 * The port's own calls, defined here as the weaker ones
 *
 * There is one build that has no port: the host tests, which drive the
 * console into a frame buffer and have no machine to write to. They link this
 * file, so the names the log calls have to exist here; a driver build links
 * uart_hw.c as well, whose strong definitions are the ones the linker picks,
 * because a weak definition only ever answers for a symbol nothing stronger
 * defines
 */
__attribute__((weak)) void usConsoleUse(UsUARTKind kind, uint64_t base,
                                        uint32_t width, uint64_t clock,
                                        uint32_t baud) {
    (void)kind;
    (void)base;
    (void)width;
    (void)clock;
    (void)baud;
}

__attribute__((weak)) void usUARTPutc(char c) {
    (void)c;
}
