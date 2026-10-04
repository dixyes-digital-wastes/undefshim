/*
 * Serial output, see console.h for why it is not just printf, and for why a
 * port that was not configured stays silent.
 */

#include <uefi.h>

#include "uefi/src/console.h"
#include "uefi/src/screen.h"

/*
 * Both kinds are described by byte offsets from the port, which is how the
 * datasheets state them. The shift below is a separate thing: an 8250 on a 32
 * bit bus spaces its byte-wide registers a word apart, and nothing else does.
 *
 * The PL011's data register is where a byte goes and the flag register says whether
 * there is room, which has to be waited for or bytes are dropped when the
 * receiver is not being read.
 */
#define US_PL011_DR 0x00U
#define US_PL011_FR 0x18U
#define US_PL011_IBRD 0x24U
#define US_PL011_FBRD 0x28U
#define US_PL011_LCRH 0x2cU
#define US_PL011_CR 0x30U
#define US_PL011_ICR 0x44U

#define US_PL011_FR_TXFF (1U << 5)

/* The 8250's registers, a byte apart, or a word apart on a 32 bit bus. */
#define US_8250_THR 0x00U
#define US_8250_IER 0x01U
#define US_8250_FCR 0x02U
#define US_8250_LCR 0x03U
#define US_8250_MCR 0x04U
#define US_8250_LSR 0x05U

#define US_8250_LSR_THRE (1U << 5)

static UsUartKind gKind = UsUartOff;
static uintptr_t gBase;
static uint32_t gShift;
static uint32_t gWidth = 32U;

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
    if (gKind == UsUartPl011) {
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
     * writing one would break a port the firmware already set up correctly.
     */
    writeReg(US_8250_IER, 0x00U);
    writeReg(US_8250_LCR, 0x03U); /* eight bits, no parity, one stop, no divisor latch */
    writeReg(US_8250_FCR, 0x07U); /* FIFOs on, both cleared */
    writeReg(US_8250_MCR, 0x03U); /* terminal ready, request to send */
}

void usConsoleUse(UsUartKind kind, uint64_t base, uint32_t width) {
    gKind = kind;
    gBase = (uintptr_t)base;
    gWidth = width == 8U ? 8U : 32U;
    gShift = (kind == UsUartUart8250 && gWidth == 32U) ? 2U : 0U;
    if (kind != UsUartOff && gBase != 0) {
        bringUp();
    }
}

static bool roomToWrite(void) {
    if (gKind == UsUartPl011) {
        return (readReg(US_PL011_FR) & US_PL011_FR_TXFF) == 0U;
    }
    return (readReg(US_8250_LSR) & US_8250_LSR_THRE) != 0U;
}

void usConsolePutc(char c) {
    /*
     * The screen gets everything the serial port gets. It costs a few writes
     * to memory that is already mapped, and it is the only channel there is
     * on a machine that asked for no serial output.
     */
    usScreenPutc(c);
    if (gKind == UsUartOff || gBase == 0) {
        return;
    }
    /* Bounded spin: a wrong base address must not hang the boot. */
    for (uint32_t spin = 0; !roomToWrite() && spin < 1000000U; spin++) {
    }
    writeReg(gKind == UsUartPl011 ? US_PL011_DR : US_8250_THR, (uint32_t)(uint8_t)c);
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
