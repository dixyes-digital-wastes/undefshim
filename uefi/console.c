/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Serial output, see console.h for why it is not just printf, and for what a
 * line of the log is made of
 */

#include <uefi.h>

#include "uefi/console.h"
#include "uefi/screen.h"

/*
 * Both kinds are described by byte offsets from the port, which is how the
 * datasheets state them. The shift below is a separate thing: an 8250 on a 32
 * bit bus spaces its byte-wide registers a word apart, and nothing else does
 *
 * The PL011's data register is where a byte goes and the flag register says whether
 * there is room, which has to be waited for or bytes are dropped when the
 * receiver is not being read
 */
#define US_PL011_DR 0x00U
#define US_PL011_FR 0x18U
#define US_PL011_IBRD 0x24U
#define US_PL011_FBRD 0x28U
#define US_PL011_LCRH 0x2cU
#define US_PL011_CR 0x30U
#define US_PL011_ICR 0x44U

#define US_PL011_FR_TXFF (1U << 5)

/* The 8250's registers, a byte apart, or a word apart on a 32 bit bus */
#define US_8250_THR 0x00U
#define US_8250_IER 0x01U
#define US_8250_FCR 0x02U
#define US_8250_LCR 0x03U
#define US_8250_MCR 0x04U
#define US_8250_LSR 0x05U

#define US_8250_LSR_THRE (1U << 5)

/*
 * One line, formatted before any of it is written: long enough for the
 * longest the driver says, and no longer, because it lives on the stack of
 * whatever happened to be running when the line was written
 */
#define US_LINE_BYTES 256

static UsUARTKind gKind = UsUARTOff;
static uintptr_t gBase;
static uint32_t gShift;
static uint32_t gWidth = 32U;
static UsLogLevel gLevel = UsLogInfo;
static bool gColour = true;
/* Set while an escape sequence is going out, so that the serial port can skip
 * one whole rather than leave its tail in the log */
static bool gEscape;

static uintptr_t regAt(uint32_t index) {
    return gBase + ((uintptr_t)index << gShift);
}

static uint32_t readReg(uint32_t index) {
    if (gWidth == 8U) {
        return *(volatile uint8_t *)regAt(index);
    }
    return *(volatile uint32_t *)regAt(index);
}

static void writeReg(uint32_t index, uint32_t value) {
    if (gWidth == 8U) {
        *(volatile uint8_t *)regAt(index) = (uint8_t)value;
        return;
    }
    *(volatile uint32_t *)regAt(index) = value;
}

static void bringUp(void) {
    if (gKind == UsUARTPL011) {
        writeReg(US_PL011_CR, 0U);
        writeReg(US_PL011_ICR, 0x7FFU);
        writeReg(US_PL011_IBRD, 13U);
        writeReg(US_PL011_FBRD, 43U);
        writeReg(US_PL011_LCRH, 0x70U); /* eight bits, no parity, FIFOs on */
        writeReg(US_PL011_CR, 0x301U);  /* enabled, transmitting, receiving */
        return;
    }
    /*
     * The 8250 gets its line settings and its FIFOs, and no baud change: the
     * divisor depends on a clock the configuration does not state, and
     * writing one would break a port the firmware already set up correctly
     */
    writeReg(US_8250_IER, 0x00U);
    writeReg(US_8250_LCR, 0x03U); /* eight bits, no parity, one stop, no divisor latch */
    writeReg(US_8250_FCR, 0x07U); /* FIFOs on, both cleared */
    writeReg(US_8250_MCR, 0x03U); /* terminal ready, request to send */
}

void usConsoleUse(UsUARTKind kind, uint64_t base, uint32_t width) {
    gKind = kind;
    gBase = (uintptr_t)base;
    gWidth = width == 8U ? 8U : 32U;
    gShift = (kind == UsUART8250 && gWidth == 32U) ? 2U : 0U;
    if (kind != UsUARTOff && gBase != 0) {
        bringUp();
    }
}

static bool roomToWrite(void) {
    if (gKind == UsUARTPL011) {
        return (readReg(US_PL011_FR) & US_PL011_FR_TXFF) == 0U;
    }
    return (readReg(US_8250_LSR) & US_8250_LSR_THRE) != 0U;
}

/*
 * The serial port, and nothing else. Bounded spin: a wrong base address must
 * not hang the boot
 *
 * An escape sequence is skipped whole when the configuration turns the colour
 * off, and it is skipped here rather than by whoever wrote it: the escapes
 * belong to the message now, and a line may colour one address in the middle
 * of it without the console having been told
 */
static void emitSerial(char c) {
    if (gEscape) {
        gEscape = c != 'm';
        if (!gColour) {
            return;
        }
    } else if (c == '\x1b') {
        gEscape = true;
        if (!gColour) {
            return;
        }
    }
    if (gKind == UsUARTOff || gBase == 0) {
        return;
    }
    for (uint32_t spin = 0; !roomToWrite() && spin < 1000000U; spin++) {
    }
    writeReg(gKind == UsUARTPL011 ? US_PL011_DR : US_8250_THR, (uint32_t)(uint8_t)c);
}

/*
 * Both outputs, in step
 *
 * There is no filter here: whether a line is worth saying is decided by the
 * one call that writes it, before any of it is formatted, so nothing is ever
 * written and then taken back
 */
static void emit(char c) {
    usScreenPutc(c);
    emitSerial(c);
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

void usConsoleColour(bool enabled) {
    gColour = enabled;
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
    char digits[16];
    int n = 0;

    emitString("0x");
    if (value == 0) {
        emit('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        uint32_t nibble = (uint32_t)(value & 0xF);
        digits[n++] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
        value >>= 4;
    }
    while (n > 0) {
        emit(digits[--n]);
    }
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
