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
 *
 * The rest of this is the frame buffer as somewhere to draw: one pixel, a run
 * of them, a glyph, and a picture. It is what the two drawers have in common
 * and it is here rather than in either of them so that the second one is not a
 * second spelling of the same arithmetic
 */

#ifndef US_PIXEL_H
#define US_PIXEL_H

#include <stdbool.h>
#include <stdint.h>

#include "common/font.h"
#include "common/hex.h"

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

/*
 * A frame buffer, as somewhere to draw
 *
 * There are two drawers -- the firmware's console and the payload's report --
 * and what they have in common is not where the memory is but how a pixel and
 * a glyph go into it. That is what this is, so that the second drawer is not
 * a second spelling of the same arithmetic
 *
 * A position is in the screen's own pixels, and the scale says how large one
 * of the things being drawn is: a glyph is eight by sixteen of them and a
 * picture is as many as it is wide, and each of those is a scale by scale
 * square of the screen. Everything drawn at a scale of two is the same drawing
 * twice as large, which is the only kind of enlargement a bitmap survives:
 * three would give rows of three pixels and rows of two, and that is a blurred
 * glyph rather than a larger one
 *
 * The scale is an argument rather than a field, because the two things being
 * drawn are not always the same size: a report wants its picture as large as
 * the screen allows and its text at a size that still fits across, and those
 * two are not the same number
 *
 * The stores are bounded, and by the width and the height rather than by the
 * stride: a row is stride pixels wide and may be wider than the part of it
 * that is shown, and writing the margin is writing off the end of the picture
 */
typedef struct UsSurface_t {
    volatile uint32_t *base; /* the top left pixel, at the alias it is mapped */
    uint32_t width;          /* pixels across, which is what is shown */
    uint32_t height;
    uint32_t stride;    /* pixels a row, which may exceed the width */
    bool blueFirst;     /* which end of a pixel is blue */
} UsSurface;

/* A colour as this surface wants it, which is the order its firmware stated */
static inline uint32_t usSurfaceColour(const UsSurface *surface, uint8_t r,
                                       uint8_t g, uint8_t b) {
    return usPixelColour(r, g, b, surface->blueFirst);
}

/*
 * How many of the things being drawn fit across at a scale
 *
 * A screen whose width is not a whole number of them has a strip at the right
 * that is too narrow to hold one, and it stays the paper
 */
static inline uint32_t usSurfaceColumns(const UsSurface *surface,
                                        uint32_t scale) {
    return scale == 0 ? 0U : surface->width / scale;
}

/*
 * One of the things being drawn, at a place on the screen
 *
 * One of them is a scale by scale square of the screen and the place is where
 * that square's top left corner goes, so a position is a screen pixel like any
 * other and the scale is only how much of the screen each one takes. A square
 * that hangs over the edge is drawn up to the edge rather than not at all:
 * what the picture is made of is known and what is beside it on the screen is
 * not
 *
 * Bounded on purpose. What is drawn here is written from a length and a
 * position, either of which can be wrong in a way that only the screen would
 * show, and a store past the end of a frame buffer is a fault on a path that
 * has nothing left to recover with
 */
static inline void usSurfacePut(const UsSurface *surface, uint32_t x,
                                uint32_t y, uint32_t scale, uint32_t colour) {
    for (uint32_t dy = 0; dy < scale && y + dy < surface->height; dy++) {
        volatile uint32_t *row =
            surface->base + (uint64_t)(y + dy) * surface->stride;

        for (uint32_t dx = 0; dx < scale && x + dx < surface->width; dx++) {
            row[x + dx] = colour;
        }
    }
}

/*
 * A whole run of one colour, which is the paper a report starts from
 *
 * The surface and not the thing being drawn: this fills every pixel that is
 * shown, margin and all
 */
static inline void usSurfaceFill(const UsSurface *surface, uint32_t colour) {
    for (uint32_t y = 0; y < surface->height; y++) {
        volatile uint32_t *row = surface->base + (uint64_t)y * surface->stride;

        for (uint32_t x = 0; x < surface->width; x++) {
            row[x] = colour;
        }
    }
}

/*
 * One glyph, at a position in the pixels of the face itself
 *
 * The glyph is the eight bytes a character is in common/font.h: one byte a
 * row, most significant bit on the left, and a set bit is drawn in the ink and
 * a clear one in the paper
 */
static inline void usSurfaceGlyph(const UsSurface *surface, uint32_t x,
                                  uint32_t y, uint32_t scale,
                                  const uint8_t *glyph, uint32_t ink,
                                  uint32_t paper) {
    for (uint32_t r = 0; r < US_FONT_HEIGHT; r++) {
        for (uint32_t c = 0; c < US_FONT_WIDTH; c++) {
            usSurfacePut(surface, x + c * scale, y + r * scale, scale,
                         (glyph[r] & (0x80U >> c)) != 0 ? ink : paper);
        }
    }
}

/*
 * A picture, at a position, two bits a pixel
 *
 * Four pixels a byte with the left one in the highest bits, which is the order
 * the generator wrote them in and the order they are drawn in, so a row is a
 * whole number of bytes and a byte is read the way it was packed. A level is
 * expanded by multiplying it by 85, so that the last of the four is white
 * rather than the three a shift would give
 *
 * The picture is drawn whole, its own background included: what it is drawn on
 * is not consulted, so the same call is right over white paper and over
 * anything else
 */
static inline void usSurfacePicture(const UsSurface *surface,
                                    const uint8_t *pixels, uint32_t width,
                                    uint32_t height, uint32_t x0, uint32_t y0,
                                    uint32_t scale) {
    uint32_t perRow = width / 4U;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t byte = pixels[(uint64_t)y * perRow + x / 4U];
            uint8_t level = (uint8_t)((byte >> (8U - 2U * (x % 4U + 1U))) & 0x3U);
            uint8_t value = (uint8_t)(level * 85U);

            usSurfacePut(surface, x0 + x * scale, y0 + y * scale, scale,
                         usSurfaceColour(surface, value, value, value));
        }
    }
}

/*
 * A pen: where the next character goes, how large it is, and what colour
 *
 * A report is written a fragment at a time -- a name, then a value, then the
 * next name -- and each of those has to carry on where the one before it
 * stopped and in the colour it was asked for. That state is this, and it is
 * here rather than beside either caller because there are two of them: the
 * frame buffer the payload writes into, and the host test that draws the same
 * report to be looked at. One of them spelling the wrap rule differently from
 * the other is a preview of something that is not what runs
 *
 * The column is a column of characters and the row is a pixel, which is how a
 * line is laid out: the font is a whole number of characters wide and sixteen
 * pixels tall, and the two are counted in the units they are whole in
 *
 * left is what a new line returns to, and it is not zero when the report is a
 * block in the middle of the screen: a report whose first line starts at the
 * margin and whose second starts at the edge of the screen is a report a
 * reader has to reassemble
 */
typedef struct UsPen_t {
    uint32_t column;
    uint32_t row;
    uint32_t left;
    uint32_t scale;
    uint32_t ink; /* 0xRRGGBB, and zero is black */
} UsPen;

/* Puts the pen at a column and a row, which is also where its lines return */
static inline void usPenAt(UsPen *pen, uint32_t column, uint32_t row) {
    pen->column = column;
    pen->left = column;
    pen->row = row;
}

/* How many screen pixels one pixel of the drawing is */
static inline uint32_t usPenScale(const UsPen *pen) {
    return pen->scale == 0 ? 1U : pen->scale;
}

static inline void usPenSize(UsPen *pen, uint32_t scale) {
    pen->scale = scale == 0 ? 1U : scale;
}

/* The colour the text is drawn in, which is the ink and not the paper */
static inline void usPenInk(UsPen *pen, uint32_t rgb) {
    pen->ink = rgb;
}

/* One character, which does not advance the pen: the callers below do that */
static inline void usPenGlyph(const UsSurface *surface, const UsPen *pen,
                              char c) {
    uint32_t scale = usPenScale(pen);
    uint32_t ink = usSurfaceColour(surface, (uint8_t)(pen->ink >> 16U),
                                   (uint8_t)(pen->ink >> 8U),
                                   (uint8_t)pen->ink);
    uint32_t paper = usSurfaceColour(surface, 0xFF, 0xFF, 0xFF);

    if (c < US_FONT_FIRST || c > US_FONT_LAST) {
        c = '?';
    }
    usSurfaceGlyph(surface, pen->column * US_FONT_WIDTH * scale, pen->row,
                   scale, kFont[(int)c - US_FONT_FIRST], ink, paper);
}

/*
 * Text, wrapped to the surface
 *
 * A newline ends the line, and so does a character that would run off the
 * right: that character is not drawn rather than being drawn in the margin,
 * and the line after it begins where this one began. Nothing is scrolled --
 * what is drawn here is read once, off a photograph
 */
static inline void usPenText(const UsSurface *surface, UsPen *pen,
                             const char *text) {
    uint32_t scale = usPenScale(pen);

    for (const char *p = text; *p != '\0'; p++) {
        char c = *p;

        if (c == '\n'
            || (pen->column + 1U) * US_FONT_WIDTH * scale > surface->width) {
            if (c != '\n') {
                p--;
            }
            pen->column = pen->left;
            pen->row += US_FONT_HEIGHT * usPenScale(pen);
            continue;
        }
        usPenGlyph(surface, pen, c);
        pen->column++;
    }
}

/*
 * A number, as 0x and as many digits as it needs
 *
 * The leading zeros are dropped because the report is a column of values read
 * by eye: a fixed width would put the digits of one value under the padding of
 * the one above it, and the reader is looking for a digit that is not where it
 * should be
 */
static inline void usPenHex(const UsSurface *surface, UsPen *pen,
                            uint64_t value) {
    char out[US_HEX_TEXT + 1U];

    usHex(out, (uint32_t)sizeof(out), value);
    usPenText(surface, pen, out);
}

/* And one in decimal, which is what a count wants */
static inline void usPenDec(const UsSurface *surface, UsPen *pen,
                            uint64_t value) {
    char out[21];
    uint32_t at = sizeof(out) - 1U;

    out[at] = '\0';
    do {
        out[--at] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0);
    usPenText(surface, pen, &out[at]);
}

#endif
