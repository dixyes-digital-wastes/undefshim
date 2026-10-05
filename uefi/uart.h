/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Where the console is, and opening it. See console.h for what it is once it
 * is open
 */

#ifndef US_UART_H
#define US_UART_H

#include "core/cfg.h"
#include "uefi/session.h"

/*
 * Opens the console the configuration asks for
 *
 * Three answers, and the interesting one is the default: a configuration may
 * name a port and how to drive it, name a type and let the driver find the
 * address, or name neither and have the whole of it read out of the
 * firmware's own description of the machine. The last is what a file that
 * says nothing gets, and it is the right answer for a board whose tables
 * describe its console: the description travels with the machine, a copy of
 * it in a file does not, and a port the firmware brought up is one whose
 * line rate is already right
 *
 * It takes the session because the search may run an AML interpreter, which
 * runs on the session's stack rather than on whatever called this
 *
 * Never fatal. A machine with no console to find and none named has no serial
 * output, which is a machine saying it does not want any
 */
void usConsoleOpen(const UsConfig *cfg, UsSession *session);

#endif
