/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The colour escapes, parsed, see ansi.h for why this is one parser and not
 * one per reader
 */

#include <stdbool.h>

#include "uefi/ansi.h"

/*
 * Whether what has been collected is a bracket and then a parameter
 *
 * The bracket is kept in the text so that a sequence which turns out not to
 * be a colour can be handed back the way it was written, and so that the
 * digits start after it
 */
static bool isParameter(const UsANSIScan *scan) {
    if (scan->length < 1U || scan->text[0] != '[') {
        return false;
    }
    for (uint32_t i = 1; i < scan->length; i++) {
        if (scan->text[i] < '0' || scan->text[i] > '9') {
            return false;
        }
    }
    return true;
}

/* The parameter the collected digits spell. No digits is the default, which
 * is zero, and a caller reads that as the reset */
static uint32_t parameterOf(const UsANSIScan *scan) {
    uint32_t value = 0;

    for (uint32_t i = 1; i < scan->length; i++) {
        value = value * 10U + (uint32_t)(scan->text[i] - '0');
    }
    return value;
}

UsANSIVerdict usANSIFeed(UsANSIScan *scan, char c) {
    if (scan->state == UsANSIGround) {
        if (c == '\x1b') {
            scan->state = UsANSIAfterEscape;
            return UsANSITaken;
        }
        return UsANSIPlain;
    }
    if (scan->state == UsANSIAfterEscape) {
        if (c != '[') {
            /*
             * An escape of another kind. It has no glyph of its own and is
             * dropped, and this byte is text, which is what a terminal does
             * with it as well
             */
            scan->state = UsANSIGround;
            return UsANSIPlain;
        }
        scan->state = UsANSIParams;
        scan->length = 0;
        scan->text[scan->length++] = '[';
        return UsANSITaken;
    }
    if (c == '\x1b') {
        /* Another sequence starts here, so this one ended at the byte before */
        scan->state = UsANSIAfterEscape;
        return UsANSIAbandoned;
    }
    if ((uint8_t)c < 0x20U || (uint8_t)c == 0x7FU) {
        /*
         * A control character ends a sequence and is then the caller's own
         * business -- a newline in the middle of one still ends the line
         */
        scan->state = UsANSIGround;
        return UsANSIUnknown;
    }
    if ((uint8_t)c >= 0x40U && (uint8_t)c <= 0x7EU && c != '[') {
        /* The byte that ends the sequence. A colour is the one this knows */
        scan->state = UsANSIGround;
        if (c != 'm' || !isParameter(scan)) {
            return UsANSIUnknown;
        }
        scan->param = parameterOf(scan);
        return UsANSIColour;
    }
    if (scan->length >= US_ANSI_TEXT) {
        /*
         * Longer than anything this writes, so what is left is not part of a
         * sequence: what has been collected is text, and so is this byte,
         * which is how a reader that does not know the sequence sees it too
         */
        scan->state = UsANSIGround;
        return UsANSIUnknown;
    }
    scan->text[scan->length++] = c;
    return UsANSITaken;
}
