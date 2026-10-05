/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Serial output
 *
 * The console the firmware provides stops being usable partway through a
 * boot, and the whole point of this driver is to still be reporting at that
 * point, so the UART is driven directly
 *
 * Where it writes is not decided here: the configuration says which port it
 * is and how wide its registers are read, and leaving that out is how a
 * machine asks for no serial output at all
 */

#ifndef US_CONSOLE_H
#define US_CONSOLE_H

#include <stdint.h>

#include "common/log.h"

typedef enum {
    UsUARTOff = 0,   /* nothing is written */
    UsUARTPL011,     /* ARM's own, 32 bits per register */
    UsUART8250,  /* the one that turns up on PC-derived boards */
} UsUARTKind;

/*
 * Points the console at a port and brings it up. Until this is called nothing
 * is written. The width is the access size the port needs - 32 for a PL011,
 * 8 or 32 for an 8250, whose registers are a byte apart when read a byte at a
 * time and a word apart when read as words
 */
void usConsoleUse(UsUARTKind kind, uint64_t base, uint32_t width);

/*
 * How much is worth saying. A line above this level is not written at all,
 * and is not formatted either
 */
void usConsoleLevel(UsLogLevel level);

/*
 * Whether a line at this level would be written. The level decides before
 * anything is formatted, which is enough for a line that costs nothing to
 * assemble; this is for the ones that cost something before they can be
 * assembled at all
 */
bool usConsoleWants(UsLogLevel level);

/*
 * Whether the serial output carries the colour escapes. On unless asked
 * otherwise: they are what a terminal reading the log back shows. Off is for
 * a log that is going to be compared byte for byte, or read by something that
 * would rather not see them
 *
 * Only the serial port: the screen parses the escapes into the ink it draws
 * with and is coloured either way
 */
void usConsoleColour(bool enabled);

/*
 * The colours a line can put its own parts in, as the escape sequences the
 * terminal and the screen both understand
 *
 * The screen reads them back into the ink it draws with, and the serial port
 * drops them when the configuration says the log should carry no colour, so a
 * line may colour one address without knowing where it will be read
 */
#define US_RESET "\x1b[0m"
#define US_RED "\x1b[31m"
#define US_GREEN "\x1b[32m"
#define US_YELLOW "\x1b[33m"
#define US_BLUE "\x1b[34m"
#define US_MAGENTA "\x1b[35m"
#define US_CYAN "\x1b[36m"
#define US_GREY "\x1b[90m"
#define US_LIGHT_CYAN "\x1b[96m"

/*
 * A number inside a line: addresses and counts get a colour of their own, so
 * that they stand out from the tag and from the words around them. It wraps a
 * conversion rather than a value, which is what makes it usable in a format
 */
#define US_VALUE(fmt) US_MAGENTA fmt US_RESET

/*
 * One line of the log: the tag, a colon, and the message the format writes
 *
 * The level decides the colour of the tag and whether the line is written at
 * all, so the tag and the level are stated together. The message is a format
 * rather than a run of calls, which is what makes a line with values in it
 * one line of source
 *
 * The format goes through the same formatter the rest of the driver uses, so
 * it costs a couple of hundred bytes of stack and nothing else. It is written
 * from hooks that run on the firmware's stack, where the printing that was
 * there before this came back to tens of kilobytes
 */
void usLog(UsLogLevel level, const char *tag, const char *fmt, ...);

/* The level is the one thing the five differ in, so it is the name */
#define usLogE(tag, ...) usLog(UsLogError, tag, __VA_ARGS__)
#define usLogW(tag, ...) usLog(UsLogWarn, tag, __VA_ARGS__)
#define usLogI(tag, ...) usLog(UsLogInfo, tag, __VA_ARGS__)
#define usLogV(tag, ...) usLog(UsLogVerbose, tag, __VA_ARGS__)
#define usLogD(tag, ...) usLog(UsLogDebug, tag, __VA_ARGS__)

/*
 * A milestone: one whole line, named for the stage a script is waiting for.
 * These are the markers the deploy scripts stop and start on, so they are
 * written as one call and cannot be assembled from parts
 *
 * Its colour is its own rather than a level's, because a milestone is not a
 * kind of message: it is the machine saying it reached a stage
 */
void usConsoleMilestone(const char *what);

/*
 * A character that marks progress through something long, written as it goes
 * rather than at the end: a machine that stopped partway leaves the mark it
 * reached. Debug level, so an ordinary boot does not have to carry them, and
 * one byte each, because these are written from hooks that may have very
 * little stack
 */
void usConsoleProgress(char mark);

/*
 * Raw output, in pieces and without a tag
 *
 * The plan dump is a block of text that a script compares byte for byte
 * against what the same code produced on the host, so it is written through
 * a sink of three callbacks rather than as tagged lines. Everything that is
 * a line of the log goes through usLog instead
 */
void usConsolePuts(const char *s);
void usConsolePutHex(uint64_t value);
void usConsolePutDec(uint64_t value);

#endif
