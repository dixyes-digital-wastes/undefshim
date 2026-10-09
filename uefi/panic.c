/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Stopping the boot on purpose, see panic.h
 *
 * Nothing here knows what a session is. The stop is decided where the
 * configuration is readable and carried out here, which is what keeps the
 * console -- the thing that calls this at every milestone -- from depending on
 * the whole of the boot
 */

#include <stddef.h>

#include "uefi/console.h"
#include "uefi/panic.h"

_Noreturn void usPanicHaltEFI(uint64_t code, const char *what, uint64_t detail) {
    /*
     * The console, which at this point is the firmware's and reaches both the
     * serial port and the screen. Saying it on both is the point: which of the
     * two a stop is noticed on is the question this exists to answer
     */
    usConsolePuts("PANIC code=");
    usConsolePutHex(code);
    usConsolePuts(" what=");
    usConsolePuts(what != NULL ? what : "?");
    usConsolePuts(" detail=");
    usConsolePutHex(detail);
    usConsolePuts("\n");

    /*
     * And a line that says the report ended
     *
     * A harness watching the serial port stops reading the moment it sees
     * what it was waiting for, so a pattern that matches anywhere inside the
     * report cuts the report off there and the rest is never read. This is
     * written last and nothing follows it, so a pattern can name the whole of
     * the report's end and lose none of it
     */
    usConsolePuts("PANIC-END\n");

    /*
     * Every interrupt masked and then a wait that cannot be woken. The masking
     * comes first and stays: an interrupt arriving here would run a firmware
     * handler on a machine that has announced it is stopped
     *
     * On the target. This file is also built into a host test, which links the
     * console in to check what it draws, and the halt there is the same loop
     * without the masking: it is never reached, since a test has no
     * configuration asking for a stop, and a host has no DAIF to write
     */
#if defined(__aarch64__)
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
#endif
    for (;;) {
#if defined(__aarch64__)
        __asm__ volatile("wfi" ::: "memory");
#endif
    }
}
