/*
 * Serial output
 *
 * The console the firmware provides stops being usable partway through a
 * boot, and the whole point of this driver is to still be reporting at that
 * point, so the UART is driven directly. Everything the driver wants to say
 * after the early stages goes here rather than through printf
 *
 * Where it writes is not decided here: the configuration says which port it
 * is and how wide its registers are read, and leaving that out is how a
 * machine asks for no serial output at all
 */

#ifndef US_CONSOLE_H
#define US_CONSOLE_H

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

void usConsolePutc(char c);
void usConsolePuts(const char *s);

/*
 * Numbers, without the formatter
 *
 * printf cannot be used here: it needs tens of kilobytes of stack, and these
 * are called from hooks that run on someone else's. These two use a sixteen
 * byte buffer and nothing else, so they are safe anywhere the console is
 */
void usConsolePutHex(uint64_t value);
void usConsolePutDec(uint64_t value);

#endif
