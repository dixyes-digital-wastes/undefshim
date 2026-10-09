/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * A pixel, and the order the firmware puts its channels in
 *
 * A frame buffer's pixels are four bytes each and the format does not say
 * which byte is which: the firmware does, and machines differ. So a colour is
 * built one way and turned round on the way out if the machine wants the other
 * order
 *
 * It has to be built the same way everywhere and turned round by the same
 * rule, which is why this is one function rather than a swap written out at
 * each place that draws. Black and white are the same number either way, and
 * those were the only two colours for long enough that a swap that was missing
 * or backwards looked like nothing at all -- a colour that is neither is what
 * shows it, and a screenful of yellow where white was meant is what it looks
 * like when this is wrong
 */

#ifndef US_PIXEL_H
#define US_PIXEL_H

#include <stdbool.h>
#include <stdint.h>

/* The three bytes of a colour made into a word, red at the bottom */
static inline uint32_t usPixelPack(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)r | ((uint32_t)g << 8U) | ((uint32_t)b << 16U);
}

/* That word with its two ends swapped, which is the other way round */
static inline uint32_t usPixelSwap(uint32_t rgb) {
    return (rgb & 0xFF00FF00U) | ((rgb & 0xFFU) << 16U) | ((rgb >> 16U) & 0xFFU);
}

/* A colour, as the three bytes it is made of and the order to put them in */
static inline uint32_t usPixelColour(uint8_t r, uint8_t g, uint8_t b,
                                     bool blueFirst) {
    uint32_t v = usPixelPack(r, g, b);

    return blueFirst ? usPixelSwap(v) : v;
}

#endif
