/*
 * Serial output, see console.h for why it is not just printf.
 */

#include <uefi.h>

#include "uefi/src/console.h"

#ifndef US_UART_BASE
#error "US_UART_BASE must be defined by the build"
#endif

/* PL011 register offsets. */
#define US_UART_DR 0x00U
#define US_UART_FR 0x18U
#define US_UART_IBRD 0x24U
#define US_UART_FBRD 0x28U
#define US_UART_LCRH 0x2CU
#define US_UART_CR 0x30U
#define US_UART_ICR 0x44U

#define US_UART_FR_TXFF (1U << 5)
#define US_UART_LCRH_8N1_FIFO 0x70U
#define US_UART_CR_UARTEN (1U << 0)
#define US_UART_CR_TXE (1U << 8)
#define US_UART_CR_RXE (1U << 9)

static void writeReg(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)((uintptr_t)US_UART_BASE + offset) = value;
}

static uint32_t readReg(uint32_t offset) {
    return *(volatile uint32_t *)((uintptr_t)US_UART_BASE + offset);
}

void usConsoleInit(void) {
    writeReg(US_UART_CR, 0);
    writeReg(US_UART_ICR, 0x7FF);
    writeReg(US_UART_IBRD, 13);
    writeReg(US_UART_FBRD, 43);
    writeReg(US_UART_LCRH, US_UART_LCRH_8N1_FIFO);
    writeReg(US_UART_CR, US_UART_CR_UARTEN | US_UART_CR_TXE | US_UART_CR_RXE);
}

void usConsolePutc(char c) {
    /* Bounded spin: a wrong base address must not hang the boot. */
    for (uint32_t spin = 0; (readReg(US_UART_FR) & US_UART_FR_TXFF) != 0 && spin < 1000000U; spin++) {
    }
    writeReg(US_UART_DR, (uint32_t)(uint8_t)c);
}

void usConsolePuts(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            usConsolePutc('\r');
        }
        usConsolePutc(*s);
    }
}

void usConsolePutHex(uint64_t value) {
    /* Sixteen characters and a counter: no frame to speak of, which is the
     * whole point of not using printf here. */
    char digits[16];
    int n = 0;

    usConsolePuts("0x");
    if (value == 0) {
        usConsolePutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        uint32_t nibble = (uint32_t)(value & 0xF);
        digits[n++] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
        value >>= 4;
    }
    while (n > 0) {
        usConsolePutc(digits[--n]);
    }
}

void usConsolePutDec(uint64_t value) {
    char digits[20];
    int n = 0;

    if (value == 0) {
        usConsolePutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        usConsolePutc(digits[--n]);
    }
}
