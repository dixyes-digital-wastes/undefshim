/*
 * The PL011, as little of it as reporting needs
 *
 * Only two registers are used. The data register is where a byte goes, and
 * the flag register says whether there is room, which has to be waited for or
 * bytes are dropped when the receiver is not being read
 */

#include <stddef.h>

#include "payload/uart.h"

/* PL011 registers, as byte offsets */
#define US_PL011_DR 0x00U
#define US_PL011_FR 0x18U
#define US_PL011_FR_TXFF (1U << 5)  /* transmit FIFO full */
#define US_PL011_FR_BUSY (1U << 3)  /* transmit in progress */

/* 8250 registers, as byte offsets; a 32 bit bus spaces them a word apart */
#define US_8250_THR 0x00U
#define US_8250_LSR 0x05U
#define US_8250_LSR_THRE (1U << 5)  /* holding register empty */

static uintptr_t gBase;
static uint32_t gKind;
static uint32_t gWidth = 32U;
static uint32_t gShift;

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

void usUARTInit(uint64_t base, uint32_t kind, uint32_t width) {
    gBase = (uintptr_t)base;
    gKind = kind;
    gWidth = width == 8U ? 8U : 32U;
    gShift = (kind == US_UART_8250 && gWidth == 32U) ? 2U : 0U;
}

void usUARTPutc(char c) {
    uint32_t spins;

    if (gBase == 0) {
        return;
    }
    if (c == '\n') {
        usUARTPutc('\r');
    }

    /*
     * Wait for room. The count is bounded on purpose: if the port is not
     * there, or its clock is not running, the flags read as permanently busy
     * and an unbounded wait turns a report into a hang
     */
    for (spins = 0; spins < 1000000U; spins++) {
        if (gKind == US_UART_8250) {
            if ((readReg(US_8250_LSR) & US_8250_LSR_THRE) != 0) {
                break;
            }
        } else if ((readReg(US_PL011_FR) & (US_PL011_FR_TXFF | US_PL011_FR_BUSY)) == 0) {
            break;
        }
    }
    writeReg(gKind == US_UART_8250 ? US_8250_THR : US_PL011_DR, (uint32_t)(uint8_t)c);
}

void usUARTPuts(const char *s) {
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        usUARTPutc(*s++);
    }
}

void usUARTPutHex(uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char out[16];
    int n = 0;

    usUARTPuts("0x");
    if (value == 0) {
        usUARTPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(out)) {
        out[n++] = kDigits[value & 0xFU];
        value >>= 4;
    }
    while (n > 0) {
        usUARTPutc(out[--n]);
    }
}

void usUARTPutDec(uint64_t value) {
    char out[20];
    int n = 0;

    if (value == 0) {
        usUARTPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(out)) {
        out[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        usUARTPutc(out[--n]);
    }
}
