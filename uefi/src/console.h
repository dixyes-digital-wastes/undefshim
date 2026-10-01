/*
 * Serial output.
 *
 * The console the firmware provides stops being usable partway through a
 * boot, and the whole point of this driver is to still be reporting at that
 * point, so the UART is driven directly. Everything the driver wants to say
 * after the early stages goes here rather than through printf.
 */

#ifndef US_CONSOLE_H
#define US_CONSOLE_H

/*
 * Brings the UART up. The firmware may not have left it enabled, and with it
 * disabled the FIFO flags report permanently full, so this has to run before
 * anything is written.
 */
void usConsoleInit(void);

void usConsolePutc(char c);
void usConsolePuts(const char *s);

/*
 * Numbers, without the formatter.
 *
 * printf cannot be used here: it needs tens of kilobytes of stack, and these
 * are called from hooks that run on someone else's. These two use a sixteen
 * byte buffer and nothing else, so they are safe anywhere the console is.
 */
void usConsolePutHex(uint64_t value);
void usConsolePutDec(uint64_t value);

#endif
