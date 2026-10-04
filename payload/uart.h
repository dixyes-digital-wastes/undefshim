/*
 * Writing to the serial port, with no firmware behind it.
 *
 * The console the firmware provides stops being usable partway through a
 * boot, and the whole point of the payload is to still be reporting after
 * that, so the UART is driven by MMIO.
 */

#ifndef US_UART_H
#define US_UART_H

#include <stdint.h>

/*
 * Sets the port to write to. Everything below is silent until this is done,
 * and a base of zero means it stays silent.
 *
 * The kind is 1 for a PL011 and 2 for an 8250; the width is the access size
 * that port needs. Nothing is programmed here - the driver brought the port
 * up before the kernel ran, and the handler runs on someone else's stack with
 * no business writing to a UART's control registers.
 */
#define US_UART_PL011 1U
#define US_UART_8250 2U

void usUartInit(uint64_t base, uint32_t kind, uint32_t width);

void usUartPutc(char c);

/*
 * Text, and the two numbers. There is no formatter in the payload: it is
 * entered on a stack of a known size, and a formatter is the one thing whose
 * stack use cannot be read off the page.
 */
void usUartPuts(const char *s);
void usUartPutHex(uint64_t value);
void usUartPutDec(uint64_t value);

#endif
