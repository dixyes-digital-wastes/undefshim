/*
 * Serial output, see console.h for why it is not just printf, and for why a
 * port that was not configured stays silent
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

static UsUARTKind gKind = UsUARTOff;
static uintptr_t gBase;
static uint32_t gShift;
static uint32_t gWidth = 32U;
static UsLogLevel gLevel = UsLogInfo;
static bool gColour = true;
/* Set while a line above the level is being written, so that it is dropped
 * whole rather than at each byte */
static bool gDropping;

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

/* The serial port, and nothing else. Bounded spin: a wrong base address must
 * not hang the boot */
static void emitSerial(char c) {
    if (gKind == UsUARTOff || gBase == 0) {
        return;
    }
    for (uint32_t spin = 0; !roomToWrite() && spin < 1000000U; spin++) {
    }
    writeReg(gKind == UsUARTPL011 ? US_PL011_DR : US_8250_THR, (uint32_t)(uint8_t)c);
}

/*
 * The only way anything is written, and the place the level filter sits
 *
 * A line that is not worth saying is dropped from the call that would have
 * started it to its newline, which is why the filter is here and not in each
 * of the writers: a half written line is worse than none, and there would be
 * as many chances to get that wrong as there are lines
 */
static void emit(char c) {
    usScreenPutc(c);
    emitSerial(c);
}

/*
 * A colour says the level, not the subject: the tag after it says which part
 * of the boot is talking
 */
#define US_SGR_RESET 0U
#define US_SGR_RED 31U
#define US_SGR_YELLOW 33U
#define US_SGR_GREEN 32U
#define US_SGR_GREY 90U
#define US_SGR_LIGHT_CYAN 96U

/*
 * The colour of a level, or the reset for one that is drawn in the default
 * ink
 *
 * Info is the default: most of what is said is neither a failure nor a
 * detail, and colouring all of it would make the ones that matter harder to
 * pick out rather than easier
 */
static uint32_t levelColour(UsLogLevel level) {
    switch (level) {
    case UsLogError:
        return US_SGR_RED;
    case UsLogWarn:
        return US_SGR_YELLOW;
    case UsLogVerbose:
        return US_SGR_GREY;
    case UsLogDebug:
        return US_SGR_LIGHT_CYAN;
    default:
        return US_SGR_RESET;
    }
}

/*
 * A colour, as the escape sequence a terminal understands and the screen
 * parses
 *
 * The screen always gets it, because the screen is what turns it into a
 * colour and it has no other way of knowing which one. The serial port gets
 * it only when the configuration says so: that is about the log, which may
 * be read by something that would rather not see the escapes in it, and the
 * two channels are asked separately for that reason
 */
static void putSgr(uint32_t code) {
    char digits[4];
    int n = 0;
    uint32_t value = code;

    while (value != 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }

    usScreenPutc('\x1b');
    usScreenPutc('[');
    for (int i = n - 1; i >= 0; i--) {
        usScreenPutc(digits[i]);
    }
    usScreenPutc('m');

    if (!gColour) {
        return;
    }
    emitSerial('\x1b');
    emitSerial('[');
    for (int i = n - 1; i >= 0; i--) {
        emitSerial(digits[i]);
    }
    emitSerial('m');
}

/*
 * Colours a tag and puts the colour back, or writes the tag plainly when the
 * level has no colour of its own
 *
 * The reset has to be written whenever a colour was: leaving it out is not
 * the same as saying the default, it leaves everything after the tag in the
 * tag's colour - and on a terminal, every line after it as well
 */
static void putTag(const char *tag, UsLogLevel level) {
    uint32_t colour = levelColour(level);

    if (colour != US_SGR_RESET) {
        putSgr(colour);
    }
    usConsolePuts(tag);
    if (colour != US_SGR_RESET) {
        putSgr(US_SGR_RESET);
    }
    usConsolePuts(": ");
}

/*
 * The colour of a tag, which is the level's business rather than the
 * caller's: a failure is red wherever it is said from
 *
 * Only the tag is coloured. Colouring the message as well would make a long
 * line one block of colour, and the tag is what a reader scans for - the
 * message is what they read once they have found it
 */
void usConsoleLevel(UsLogLevel level) {
    gLevel = level;
}

/*
 * Whether the serial output carries the colour escapes
 *
 * Not the screen's business: the screen parses the escapes into the ink it
 * draws with, and it keeps doing that whichever way this is set. Only the
 * bytes that reach the serial port are affected
 */
void usConsoleColour(bool enabled) {
    gColour = enabled;
}

void usConsoleLog(const char *tag, UsLogLevel level) {
    if ((int)level > (int)gLevel) {
        gDropping = true;
        return;
    }
    gDropping = false;
    putTag(tag, level);
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
        gDropping = true;
        return;
    }
    gDropping = false;
    putSgr(US_SGR_GREEN);
    usConsolePuts("milestone");
    putSgr(US_SGR_RESET);
    usConsolePuts(": ");
    usConsolePuts(what);
    usConsolePuts("\n");
}

void usConsoleProgress(char mark) {
    if ((int)UsLogDebug > (int)gLevel) {
        return;
    }
    /*
     * One byte, and no colour: this is written from hooks where the stack is
     * not ours, and an escape sequence is fourteen bytes of work for a mark
     * that is read as its own letter anyway. It goes through the same writer
     * as everything else, so a line that is being dropped swallows this too
     */
    usConsolePutc(mark);
}

void usConsolePutc(char c) {
    if (gDropping) {
        /* The newline is what ends the line that is not being written */
        if (c == '\n') {
            gDropping = false;
        }
        return;
    }
    emit(c);
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
     * whole point of not using printf here */
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
