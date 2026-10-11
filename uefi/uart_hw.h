/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The serial port, as the rest of the console sees it
 *
 * Where it writes is not decided here: usConsoleUse states the port and how
 * its registers are reached, and that declaration is in console.h with the
 * rest of what a caller outside the console may use. What is here is the one
 * call the log makes while a line is being written
 */

#ifndef US_UEFI_UART_HW_H
#define US_UEFI_UART_HW_H

/*
 * One byte, whether it is text or part of an escape
 *
 * The escapes arrive with the text and are the port's business rather than
 * the caller's: a log written without colour is compared byte for byte, and
 * dropping a colour sequence whole means knowing where it ends. The bytes of
 * a line are handed over one at a time because that is how they are written,
 * and the port waits for room before each one
 */
void usUARTPutc(char c);

#endif
