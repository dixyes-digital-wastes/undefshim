/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * How a number is spelled in hexadecimal
 *
 * There are four places that write one out: the boot's console, the payload's
 * serial port, the payload's frame buffer, and the panic report. They are four
 * because they write to four different things, but the spelling is one fact,
 * and it was four copies of the same loop - which is how a report written to
 * the screen and the same report written to the port come to disagree about
 * where a column starts
 *
 * The rule: no leading zeros, and as many digits as the value needs. Every
 * reader is a person looking for the digit that is wrong, so a fixed width
 * would only put padding where the eye expects a digit
 *
 * Only <stdint.h>, on purpose. This header is reached from the driver, from
 * the payload, and from host tests that compile the driver's own sources
 * against a stand-in libc; <stddef.h> is the one that is not the same header
 * in all three, and it is the one that collides with the firmware library's
 * own wchar_t. Lengths here are uint32_t, which is a length and not an
 * address, and no caller passes more than a line of text
 *
 * Callers that need the digits somewhere other than at the end of a buffer
 * use usHexInto, which does not terminate; the ones that want text use usHex,
 * which does
 */

#ifndef US_HEX_H
#define US_HEX_H

#include <stdint.h>

/* Digits a 64 bit value can take. Not the length of anything written: the
 * leading zeros are dropped, so this is a buffer size and not a width */
#define US_HEX_DIGITS 16U
#define US_HEX_MAX US_HEX_DIGITS

/*
 * Writes the spelling of value from at, without a terminator and without ever
 * writing more than room characters. Answers how many characters it wrote, or
 * zero when even "0" does not fit
 */
static inline uint32_t usHexInto(char *at, uint32_t room, uint64_t value) {
    char     digits[US_HEX_DIGITS];
    uint32_t n = 0;

    while (value != 0 && n < US_HEX_DIGITS) {
        uint32_t nibble = (uint32_t)(value & 0xFU);

        digits[n++] = (char)(nibble < 10U ? '0' + nibble : 'a' + (nibble - 10U));
        value >>= 4;
    }
    if (n == 0) {
        digits[n++] = '0';
    }
    if (room < n) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        at[i] = digits[n - 1U - i];
    }
    return n;
}

#define US_HEX_TEXT 18U /* room for "0x" and every digit there can be */

/*
 * The same with the 0x in front and a terminator. room describes the whole
 * buffer, so it wants US_HEX_TEXT + 1 at least; the answer excludes the
 * terminator, and is zero when it did not fit
 */
static inline uint32_t usHex(char *out, uint32_t room, uint64_t value) {
    uint32_t n;

    if (room < 3U) {
        return 0;
    }
    n = usHexInto(out + 2, room - 2U, value);
    if (n == 0) {
        return 0;
    }
    out[0] = '0';
    out[1] = 'x';
    out[2U + n] = '\0';
    return 2U + n;
}

#endif
