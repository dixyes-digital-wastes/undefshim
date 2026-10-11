/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The serial port's registers, see uart_hw.h for the byte path and console.h
 * for where the port is and how it is opened
 */

#include "uefi/ansi.h"
#include "uefi/console.h"
#include "uefi/log.h"
#include "uefi/uart_hw.h"

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

static UsUARTKind gKind = UsUARTOff;
static uintptr_t gBase;
static uint32_t gShift;
static uint32_t gWidth = 32U;

/*
 * The sequence the port is in the middle of
 *
 * The port is the reader that has to know: a log written without colour is
 * compared byte for byte, and dropping a sequence whole means knowing where it
 * ends. The same bytes go to the screen, which parses them for the ink it
 * draws with, and the two parsers are fed in step because both are handed
 * every byte of the line
 */
static UsANSIScan gEscape;

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

static void bringUp(uint64_t clock, uint32_t baud) {
    if (gKind == UsUARTPL011) {
        writeReg(US_PL011_CR, 0U);
        writeReg(US_PL011_ICR, 0x7FFU);
        /*
         * The line rate is programmed only when both halves of it are known,
         * because a divisor is a function of the clock and nothing in ACPI
         * states one. Left alone, the port keeps what the firmware put there,
         * which is what it was printing the boot with
         *
         * Divisor = clk / (16 * baud): the PL011's bit clock is sixteen times
         * the line rate, the integer part of the quotient goes in IBRD and
         * the remaining sixty-fourths in FBRD
         */
        if (clock != 0 && baud != 0) {
            uint32_t divisor = (uint32_t)((clock * 4U) / baud);

            writeReg(US_PL011_IBRD, divisor >> 6);
            writeReg(US_PL011_FBRD, divisor & 0x3FU);
        }
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

void usConsoleUse(UsUARTKind kind, uint64_t base, uint32_t width,
                  uint64_t clock, uint32_t baud) {
    gKind = kind;
    gBase = (uintptr_t)base;
    gWidth = width == 8U ? 8U : 32U;
    gShift = (kind == UsUART8250 && gWidth == 32U) ? 2U : 0U;
    if (kind != UsUARTOff && gBase != 0) {
        bringUp(clock, baud);
    }
}

static bool roomToWrite(void) {
    if (gKind == UsUARTPL011) {
        return (readReg(US_PL011_FR) & US_PL011_FR_TXFF) == 0U;
    }
    return (readReg(US_8250_LSR) & US_8250_LSR_THRE) != 0U;
}

/*
 * One byte, and nothing else. Bounded spin: a wrong base address must not hang
 * the boot
 */
static void writeByte(char c) {
    if (gKind == UsUARTOff || gBase == 0) {
        return;
    }
    for (uint32_t spin = 0; !roomToWrite() && spin < 1'000'000U; spin++) {
    }
    writeReg(gKind == UsUARTPL011 ? US_PL011_DR : US_8250_THR, (uint32_t)(uint8_t)c);
}

void usUARTPutc(char c) {
    UsANSIVerdict verdict = usANSIFeed(&gEscape, c);

    /*
     * With the colour on, the escapes are what the far end reads for itself,
     * so every byte goes out as it arrived - the ones inside a sequence
     * included, which is why the text of one is never written a second time
     */
    if (verdict == UsANSIPlain || usConsoleColourWanted()) {
        writeByte(c);
        return;
    }
    /*
     * With it off, a sequence that names a colour is dropped whole, and one
     * that only looks like a sequence is text: it is written as the characters
     * it was written as, and so is the byte that ended it when that byte is
     * not the start of another
     */
    if (verdict == UsANSIUnknown || verdict == UsANSIAbandoned) {
        for (uint32_t i = 0; i < gEscape.length; i++) {
            writeByte(gEscape.text[i]);
        }
        if (verdict == UsANSIUnknown) {
            writeByte(c);
        }
    }
}
