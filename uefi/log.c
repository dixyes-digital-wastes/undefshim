/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * One line of the log and the two channels it goes to, see console.h for why
 * it is not just printf and for what a line is made of
 *
 * What the line says is decided here and nowhere else: the level decides
 * before anything is formatted, the tag is coloured from it, and the bytes of
 * the finished line are handed to the screen and to the port in step
 */

#include <uefi.h>

#include "common/hex.h"
#include "uefi/console.h"
#include "uefi/log.h"
#include "uefi/panic.h"
#include "uefi/screen.h"
#include "uefi/uart_hw.h"

/*
 * One line, formatted before any of it is written: long enough for the
 * longest the driver says, and no longer, because it lives on the stack of
 * whatever happened to be running when the line was written
 */
#define US_LINE_BYTES 256

static UsLogLevel gLevel = UsLogInfo;

/*
 * The milestone the configuration asked to stop after, if any
 *
 * Two fields rather than a string, because the configuration hands back a
 * pointer into its own document and a length rather than a terminated string.
 * Set once when the console is opened, and read at every milestone, which is
 * the one place that has to know: a check at each site would be a list to keep
 * in step with the messages, and the ones that were forgotten would be exactly
 * the ones that matter
 */
static const char *gPanicAfter;
static int gPanicAfterLen;
static bool gColour = true;
static bool gScreen = true;

void usConsoleStopAfter(const char *what, int whatLen) {
    gPanicAfter = what;
    gPanicAfterLen = whatLen;
}

/*
 * Both outputs, in step
 *
 * There is no filter here: whether a line is worth saying is decided by the
 * one call that writes it, before any of it is formatted, so nothing is ever
 * written and then taken back
 */
static void emit(char c) {
    if (gScreen) {
        usScreenPutc(c);
    }
    usUARTPutc(c);
}

static void emitString(const char *s) {
    for (; *s != '\0'; s++) {
        emit(*s);
    }
}

static void emitNewline(void) {
    emit('\r');
    emit('\n');
}

/*
 * The colour of a level, or nothing for one that is drawn in the default ink
 *
 * Info is the default: most of what is said is neither a failure nor a
 * detail, and colouring all of it would make the ones that matter harder to
 * pick out rather than easier
 */
static const char *levelColour(UsLogLevel level) {
    switch (level) {
    case UsLogError:
        return US_RED;
    case UsLogWarn:
        return US_YELLOW;
    case UsLogVerbose:
        return US_GREY;
    case UsLogDebug:
        return US_LIGHT_CYAN;
    default:
        return NULL;
    }
}

/*
 * Colours a tag and puts the colour back, or writes the tag plainly when the
 * level has none
 *
 * The reset has to be written whenever a colour was: leaving it out is not
 * the same as saying the default, it leaves everything after the tag in the
 * tag's colour - and on a terminal, every line after it as well
 */
static void putTag(const char *tag, UsLogLevel level) {
    const char *colour = levelColour(level);

    if (colour != NULL) {
        emitString(colour);
    }
    emitString(tag);
    if (colour != NULL) {
        emitString(US_RESET);
    }
    emitString(": ");
}

void usConsoleLevel(UsLogLevel level) {
    gLevel = level;
}

bool usConsoleWants(UsLogLevel level) {
    return (int)level <= (int)gLevel;
}

void usConsoleColour(bool enabled) {
    gColour = enabled;
}

void usConsoleScreen(bool enabled) {
    gScreen = enabled;
}

bool usConsoleColourWanted(void) {
    return gColour;
}

void usLog(UsLogLevel level, const char *tag, const char *fmt, ...) {
    char line[US_LINE_BYTES];
    va_list args;
    int length;

    /* Above the level, and nothing is even formatted: a machine that is not
     * listening should not pay for the line */
    if ((int)level > (int)gLevel) {
        return;
    }
    va_start(args, fmt);
    length = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    /* The formatter answers with the length it would have written, which is
     * past the end of the buffer when the line did not fit */
    if (length >= (int)sizeof(line)) {
        length = (int)sizeof(line) - 1;
    }
    putTag(tag, level);
    for (int i = 0; i < length; i++) {
        emit(line[i]);
    }
}

/*
 * A milestone: one whole line, named for the stage a script waits for
 *
 * Its colour is its own rather than a level's, because a milestone is not a
 * kind of message: it is the machine saying it reached a stage, and the
 * scripts stop and start on it
 */
void usConsoleMilestone(const char *what) {
    if ((int)UsLogInfo > (int)gLevel) {
        return;
    }
    emitString(US_GREEN "milestone" US_RESET ": ");
    emitString(what);
    emitNewline();
    if (usPanicNameMatches(gPanicAfter, gPanicAfterLen, what)) {
        /*
         * The detail is zero: what identifies this stop is the milestone,
         * which is already in the line above, and a number would be a second
         * way of naming the same thing
         */
        usPanicHaltEFI(US_PANIC_CODE_INJECTED, what, 0U);
    }
}

void usConsoleProgress(char mark) {
    if ((int)UsLogDebug > (int)gLevel) {
        return;
    }
    /*
     * One byte, and no colour: this is written from hooks where the stack is
     * not ours, and an escape sequence is a dozen bytes of work for a mark
     * that is read as its own letter anyway
     */
    emit(mark);
}

void usConsolePuts(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            emit('\r');
        }
        emit(*s);
    }
}

/*
 * Numbers, without the formatter
 *
 * These are for the few places that are not a log line and cannot be one --
 * the plan dump above all -- and they use a small buffer rather than the
 * formatter, so they are safe on any stack the console is reachable from
 */
void usConsolePutHex(uint64_t value) {
    char out[US_HEX_TEXT + 1U];

    usHex(out, (uint32_t)sizeof(out), value);
    emitString(out);
}

void usConsolePutDec(uint64_t value) {
    char digits[20];
    int n = 0;

    if (value == 0) {
        emit('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        emit(digits[--n]);
    }
}
