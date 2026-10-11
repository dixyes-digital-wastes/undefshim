/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What the log lets the rest of the console ask it, see console.h for the log
 * itself
 *
 * A line of the log is written here and read in three places, so the one thing
 * the others need is the answer to a question only this side has: whether the
 * escapes that colour the line are wanted at all
 */

#ifndef US_UEFI_LOG_H
#define US_UEFI_LOG_H

#include <stdbool.h>

/*
 * Whether the escapes that colour a line are to be written to the port
 *
 * The port asks rather than being told. The switch is the log's, because the
 * log is the thing that knows whether what it writes is read back by a
 * terminal or compared byte for byte, and the port is the only part that has
 * to act on the answer. The screen parses the escapes into ink either way
 */
bool usConsoleColourWanted(void);

#endif
