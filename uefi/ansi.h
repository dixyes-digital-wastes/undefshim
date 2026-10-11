/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The colour escapes, read back
 *
 * The log writes one byte stream and three things read it: a terminal, which
 * wants the escapes, the screen, which has to turn them into the ink it
 * draws with, and the serial port, which has to drop a sequence whole when
 * the log is written without colour. The screen and the port are handed the
 * same bytes and neither can see what the other makes of them, so the reading
 * is one parser here and both of them use it
 *
 * It is fed a byte at a time because that is how the text arrives, and it
 * holds what it has not decided about. A sequence it does not know is handed
 * back as the characters it was written as, which is what a terminal that
 * does not understand it shows as well
 */

#ifndef US_UEFI_ANSI_H
#define US_UEFI_ANSI_H

#include <stdint.h>

/*
 * How much of a sequence is kept
 *
 * Long enough for the longest one this project writes and no longer, because
 * a scan lives on the stack of whatever is logging. A sequence longer than
 * this is given back as text
 */
#define US_ANSI_TEXT 16U

/*
 * Where the parser is. This is the parser's own business: a caller sets a
 * whole scan to zero to start one and reads none of it afterwards
 */
typedef enum UsANSIState_e {
    UsANSIGround = 0,  /* not in a sequence */
    UsANSIAfterEscape, /* the byte after the escape */
    UsANSIParams,      /* inside the brackets, collecting the parameter */
} UsANSIState;

/*
 * One parser, and what it has collected
 *
 * The screen and the port have one each rather than sharing one: both are fed
 * the same bytes and either may be the only reader there is
 */
typedef struct UsANSIScan_t {
    UsANSIState state;
    /*
     * Everything that followed the escape, the bracket included: what the
     * sequence was written as, which is what is drawn when it turns out not
     * to be one of ours. Valid until the next sequence starts
     */
    char        text[US_ANSI_TEXT];
    uint32_t    length;
    /*
     * The parameter the sequence that just ended named. Read it on the colour
     * verdict and on no other: a colour with no parameter names zero, which
     * is the reset
     */
    uint32_t    param;
} UsANSIScan;

/* What one byte turned out to be, which is what the caller acts on */
typedef enum UsANSIVerdict_e {
    UsANSIPlain = 0, /* text, and the byte is the caller's to write */
    UsANSITaken,     /* part of a sequence: nothing of the byte is written */
    UsANSIColour,    /* the sequence ended at this byte and named a colour */
    UsANSIUnknown,   /* it ended here and is not one of ours: the text held in
                      * the scan, and then the byte, which is text as well */
    UsANSIAbandoned, /* it gave way to another escape: the text held in the
                      * scan, and nothing of the byte */
} UsANSIVerdict;

UsANSIVerdict usANSIFeed(UsANSIScan *scan, char c);

#endif
