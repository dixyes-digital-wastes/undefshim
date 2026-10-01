/*
 * The PL011, as little of it as reporting needs.
 *
 * Only two registers are used. The data register is where a byte goes, and
 * the flag register says whether there is room, which has to be waited for or
 * bytes are dropped when the receiver is not being read.
 */

#include <stddef.h>

#include "payload/uart.h"

/* Registers, by offset from the port base. */
#define US_PL011_DR 0x00U
#define US_PL011_FR 0x18U

/* Flag register bits. */
#define US_PL011_FR_TXFF (1U << 5)  /* transmit FIFO full */
#define US_PL011_FR_BUSY (1U << 3)  /* transmit in progress */

static volatile uint8_t *gBase;

static uint32_t readReg(uint32_t offset) {
    return *(volatile uint32_t *)(gBase + offset);
}

static void writeReg(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(gBase + offset) = value;
}

void usUartInit(uint64_t base) {
    gBase = (volatile uint8_t *)(uintptr_t)base;
}

void usUartPutc(char c) {
    uint32_t spins;

    if (gBase == NULL) {
        return;
    }
    if (c == '\n') {
        usUartPutc('\r');
    }

    /*
     * Wait for room. The count is bounded on purpose: if the port is not
     * there, or its clock is not running, the flags read as permanently busy
     * and an unbounded wait turns a report into a hang.
     */
    for (spins = 0; spins < 1000000U; spins++) {
        if ((readReg(US_PL011_FR) & (US_PL011_FR_TXFF | US_PL011_FR_BUSY)) == 0) {
            break;
        }
    }
    writeReg(US_PL011_DR, (uint32_t)(uint8_t)c);
}

void usUartPuts(const char *s) {
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        usUartPutc(*s++);
    }
}

void usUartPutHex(uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char out[16];
    int n = 0;

    usUartPuts("0x");
    if (value == 0) {
        usUartPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(out)) {
        out[n++] = kDigits[value & 0xFU];
        value >>= 4;
    }
    while (n > 0) {
        usUartPutc(out[--n]);
    }
}

void usUartPutDec(uint64_t value) {
    char out[20];
    int n = 0;

    if (value == 0) {
        usUartPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(out)) {
        out[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        usUartPutc(out[--n]);
    }
}
