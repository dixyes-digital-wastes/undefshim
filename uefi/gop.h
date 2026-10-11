/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The firmware's frame buffer, found and described, see screen.h for what is
 * drawn on it and why the screen is a channel of its own
 */

#ifndef US_UEFI_GOP_H
#define US_UEFI_GOP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The frame buffer as the firmware describes it
 *
 * The pixels are four bytes each, and blueFirst says which end of the word
 * the red is at: the firmware states this and it cannot be worked out from
 * the address, so a drawer that guessed would paint every colour except black
 * and white wrong
 */
typedef struct UsGOPFrame_t {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* pixels a row, which may exceed the width */
    bool     blueFirst;
} UsGOPFrame;

/*
 * Looks the graphics protocol up and answers whether there is a frame buffer
 * this driver can draw on. A machine without one has no screen output, which
 * is a machine whose only report is the port
 */
bool usGOPFind(UsGOPFrame *out);

#endif
