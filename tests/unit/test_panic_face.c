/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The face of a panic report, drawn on the host so that it can be looked at
 *
 * The report is drawn into the frame buffer by the payload, and the payload
 * runs where nothing else does: after the kernel's tables are in force, on a
 * machine that has already stopped. There is no way to iterate on how it looks
 * there, so the face -- which is the part that is looked at rather than read --
 * is drawn here. The pen and the primitives are the payload's own, so what is
 * drawn is what a machine would draw and not a second version of it; what is
 * checked is what a photograph would show: where the paper ends, where the
 * picture is, where the text is, and what colour the two things a reader looks
 * for came out
 *
 * The screens are the ones a machine of this kind comes with, because the
 * resolution is not known until the firmware says so and the layout has to
 * hold at any of them
 *
 *   ./test_panic_face                    the checks, at every screen below
 *   ./test_panic_face p out.ppm [w h]    the face as a file, to look at
 */

#include <stdio.h>
#include <stdlib.h>

#include "common/font.h"
#include "core/pixel.h"
#include "payload/panic.h"
#include "payload/panelface.h"
#include "payload/panicimage.h"

#define US_CHECK_NAME "test_panic_face"
#include "check.h"

/*
 * The screens this is looked at on: a small panel, the one the previews have
 * used all along, and the two a desktop monitor comes in. The buffer is the
 * largest of them, because a run draws at whichever it was asked for
 *
 * The largest is a constant and not the last entry's width, so that the buffer
 * below is a fixed size rather than something the compiler has to work out
 * from the table
 */
enum {
    MAX_WIDTH = 3840,
    MAX_HEIGHT = 2160,
};

static const struct {
    uint32_t width;
    uint32_t height;
} kScreens[] = {
    { 640U, 480U },
    { 1024U, 768U },
    { 1920U, 1080U },
    { (uint32_t)MAX_WIDTH, (uint32_t)MAX_HEIGHT },
};
#define SCREEN_COUNT (sizeof(kScreens) / sizeof(kScreens[0]))

/*
 * A guard row before the frame buffer and a guard row after it, so that a
 * write above the top or below the bottom lands somewhere that can be noticed.
 * The frame buffer has to be one contiguous block, which is why this is a flat
 * array and not a two dimensional one with a margin: the drawer walks it by
 * the stride it was given
 */
#define GUARD 0x5A5A5A5AU

static uint32_t arena[(MAX_HEIGHT + 2U) * MAX_WIDTH];
static uint32_t *frame = &arena[MAX_WIDTH];
static int failures;

/* White either way round, which is why this one needs no conversion */
#define PAPER 0x00FFFFFFU

/*
 * A colour as a pixel of this buffer, which is not the order the header states
 * it in
 *
 * The face names its colours as 0xRRGGBB, and a pixel is those three bytes in
 * whichever order the firmware said. Black and white are the same word either
 * way, so a comparison that used the header's number directly would pass for
 * those two and fail for everything that is a colour -- which is what the
 * colours here are
 */
static uint32_t wordOf(uint32_t rgb) {
    return usPixelColour((uint8_t)(rgb >> 16U), (uint8_t)(rgb >> 8U),
                         (uint8_t)rgb, false);
}

/* The stride is the widest screen rather than the one asked for, so that one
 * buffer serves every size and a row that runs past the width still lands
 * inside the buffer where the guards can see it */
static UsSurface surfaceOf(uint32_t width, uint32_t height) {
    return (UsSurface){
        .base = frame,
        .width = width,
        .height = height,
        .stride = MAX_WIDTH,
        .blueFirst = false,
    };
}

static uint32_t at(uint32_t x, uint32_t y) {
    return frame[(uint64_t)y * MAX_WIDTH + x];
}

static void guardFill(void) {
    for (uint32_t i = 0; i < (MAX_HEIGHT + 2U) * MAX_WIDTH; i++) {
        arena[i] = GUARD;
    }
}

static void checkGuards(const char *what) {
    for (uint32_t i = 0; i < MAX_WIDTH; i++) {
        if (arena[i] != GUARD) {
            usCheckFail("%s: wrote above the frame buffer at column %u\n", what,
                        i);
            failures++;
            return;
        }
        if (arena[(MAX_HEIGHT + 1U) * MAX_WIDTH + i] != GUARD) {
            usCheckFail("%s: wrote below the frame buffer at column %u\n", what,
                        i);
            failures++;
            return;
        }
    }
}

/* Whether a band of the screen is paper and nothing else */
static void checkBlank(const char *what, uint32_t width, uint32_t y0,
                       uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 0; x < width; x++) {
            if (at(x, y) != PAPER) {
                usCheckFail("%s: (%u,%u) is %08x and not paper\n", what, x, y,
                            at(x, y));
                failures++;
                return;
            }
        }
    }
}

/* Where the ink is in a band, as the first and last column that holds any */
static bool inkColsIn(uint32_t width, uint32_t y0, uint32_t y1,
                      uint32_t *left, uint32_t *right) {
    bool any = false;

    *left = width;
    *right = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 0; x < width; x++) {
            if (at(x, y) != PAPER) {
                any = true;
                if (x < *left) {
                    *left = x;
                }
                if (x > *right) {
                    *right = x;
                }
            }
        }
    }
    return any;
}

/* How many pixels of a colour a band holds, which is how a colour is checked */
static uint32_t countColour(uint32_t y0, uint32_t y1, uint32_t colour) {
    uint32_t seen = 0;

    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 0; x < MAX_WIDTH; x++) {
            if (at(x, y) == colour) {
                seen++;
            }
        }
    }
    return seen;
}

/* The first and last row of the drawing, which is where the face put it */
static void inkRows(uint32_t width, uint32_t height, uint32_t *top,
                    uint32_t *bottom) {
    bool any = false;

    *top = 0;
    *bottom = 0;
    for (uint32_t y = 0; y < height; y++) {
        bool row = false;

        for (uint32_t x = 0; x < width; x++) {
            if (at(x, y) != PAPER) {
                row = true;
                break;
            }
        }
        if (row) {
            if (!any) {
                *top = y;
                any = true;
            }
            *bottom = y;
        }
    }
}

/*
 * The face, and what a photograph of it would show
 *
 * What the layout promises is that the picture is a margin below the top and
 * centred, that the text begins below the picture rather than over it, and
 * that the block of it is centred too -- a block is what a table is, and one
 * with more paper on one side than the other is a table that has been pushed
 * off its own screen
 *
 * All of it is asked of every screen: a layout that holds on one and not on
 * another is a machine that reports differently depending on what was plugged
 * into it
 */
static void testFace(uint32_t width, uint32_t height) {
    UsSurface surface = surfaceOf(width, height);
    uint32_t pictureScale = usPanicFacePictureScale(width, height);
    uint32_t picturePixels = US_PANIC_IMAGE_WIDTH * pictureScale;
    uint32_t pictureBottom = US_PANIC_MARGIN + picturePixels;
    UsPanicFace face = usPanicFaceDraw(&surface, US_PANIC_REPORT_COLUMNS,
                                       US_PANIC_REPORT_LINES);
    uint32_t unit = US_FONT_WIDTH * face.textScale;
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t left = 0;
    uint32_t right = 0;
    char what[64];

    snprintf(what, sizeof(what), "the face on %ux%u", width, height);
    guardFill();
    usPanicFaceDraw(&surface, US_PANIC_REPORT_COLUMNS, US_PANIC_REPORT_LINES);
    checkGuards(what);

    inkRows(width, height, &top, &bottom);
    /*
     * The picture is a margin below the top, and nothing is above it or below
     * the band it goes in: the margin is what a photograph loses first, so the
     * drawing starting exactly there and staying inside its own band is the
     * whole point of having one
     */
    checkBlank(what, width, 0, US_PANIC_MARGIN);
    if (top != US_PANIC_MARGIN) {
        usCheckFail("%s: the drawing starts at row %u and the margin is %u\n",
                    what, top, US_PANIC_MARGIN);
        failures++;
    }
    if (bottom >= pictureBottom) {
        usCheckFail("%s: the drawing ends at row %u and the picture's band ends "
                    "at %u\n", what, bottom, pictureBottom - 1U);
        failures++;
    }
    checkBlank(what, width, pictureBottom, height);

    /*
     * The picture is centred, and it is measured inside its own band: what the
     * reader sees is where it is on the screen, and the picture's own edges
     * are white in places, so it is the middle of what was drawn that has to
     * be the middle of the screen
     */
    if (!inkColsIn(width, US_PANIC_MARGIN, pictureBottom, &left, &right)) {
        usCheckFail("%s: nothing was drawn where the picture goes\n", what);
        failures++;
        return;
    }
    {
        uint32_t want = (width - 1U) / 2U;
        uint32_t got = (left + right) / 2U;

        /* Two pictures' worth of slack: the picture's own first and last
         * columns are white, so the ink is not symmetric about its box */
        if (got > want + pictureScale || got + pictureScale < want) {
            usCheckFail("%s: the middle of the picture is at %u and the middle "
                        "of the screen is at %u\n", what, got, want);
            failures++;
        }
    }

    /*
     * The text is under the picture with the margin between them, and the
     * block of it is centred: the paper on the left and on the right differ by
     * less than one character, which is the most a block of whole ones can
     * manage
     */
    if (face.row < pictureBottom + US_PANIC_MARGIN) {
        usCheckFail("%s: the text starts at row %u and the picture ends at "
                    "%u\n", what, face.row, pictureBottom - 1U);
        failures++;
    }
    if (face.row < pictureBottom) {
        usCheckFail("%s: the text starts inside the picture\n", what);
        failures++;
    }
    {
        uint32_t need = (face.column + US_PANIC_REPORT_COLUMNS) * unit;
        uint32_t paperLeft = face.column * unit;
        uint32_t paperRight = width - need;
        uint32_t ends =
            face.row + US_PANIC_REPORT_LINES * US_FONT_HEIGHT * face.textScale;

        if (need > width) {
            usCheckFail("%s: the text needs %u pixels and the screen is %u "
                        "across\n", what, need, width);
            failures++;
        } else if (paperLeft > paperRight + unit
                   || paperRight > paperLeft + unit) {
            usCheckFail("%s: the text has %u pixels of paper on the left and "
                        "%u on the right\n", what, paperLeft, paperRight);
            failures++;
        }
        if (ends > height) {
            usCheckFail("%s: the text ends at row %u and the screen ends at "
                        "%u\n", what, ends, height);
            failures++;
        }
    }

    /* One pixel of the picture against the level it came from, which is the
     * check the whole of the two bit path is: four pixels share a byte, the
     * left one in the highest bits, and a level is white at the last of them */
    {
        uint32_t column = US_PANIC_IMAGE_WIDTH / 2U;
        uint32_t row = US_PANIC_IMAGE_HEIGHT / 2U;
        uint8_t byte = kPanicImage[(uint64_t)row * (US_PANIC_IMAGE_WIDTH / 4U)
                                   + column / 4U];
        uint8_t level =
            (uint8_t)((byte >> (8U - 2U * (column % 4U + 1U))) & 0x3U);
        uint32_t want = (uint32_t)level * 85U;
        uint32_t got = at((width - picturePixels) / 2U + column * pictureScale,
                          US_PANIC_MARGIN + row * pictureScale);

        want = (want << 16U) | (want << 8U) | want;
        if (got != want) {
            usCheckFail("%s: the picture pixel at (%u,%u) is %08x, and its "
                        "level is %u\n", what, column, row, got, level);
            failures++;
        }
    }

    /*
     * A screen narrower than the picture leaves the paper, and nothing of it:
     * a machine that cannot show the sign still gets the report, and half a
     * logo over it says less than none
     */
    {
        uint32_t narrowWidth = US_PANIC_IMAGE_WIDTH - 1U;
        UsSurface narrow = surfaceOf(narrowWidth, height);

        guardFill();
        if (usPanicFacePictureScale(narrowWidth, height) != 1U) {
            usCheckFail("%s: a screen %u wide would enlarge the picture\n",
                        what, narrowWidth);
            failures++;
        }
        usPanicFaceDraw(&narrow, US_PANIC_REPORT_COLUMNS,
                        US_PANIC_REPORT_LINES);
        checkGuards("the face on a narrow screen");
        checkBlank("the face on a narrow screen", narrowWidth, 0, height);
    }
}

/*
 * The report, drawn the way the payload draws it
 *
 * The pen, the glyphs and the numbers are the payload's own, out of
 * core/pixel.h, so this is what a machine would draw. What is here is the text
 * and the values, which the payload fills in from the registers it read
 */
static void field(const UsSurface *surface, UsPen *pen, const char *name,
                  uint64_t value) {
    uint32_t width = 0;

    while (name[width] != '\0') {
        width++;
    }
    usPenInk(pen, US_PANIC_INK);
    usPenText(surface, pen, name);
    while (width < US_PANIC_FIELD_NAME) {
        usPenText(surface, pen, " ");
        width++;
    }
    usPenText(surface, pen, "=");
    usPenInk(pen, US_PANIC_VALUE);
    usPenHex(surface, pen, value);
    usPenText(surface, pen, "\n");
}

static void drawReport(UsSurface *surface) {
    static const char *const names[] = { "elr",    "esr",    "far",   "spsr",
                                         "detail", "vbar",   "payload",
                                         "pool",   "self" };
    static const uint64_t values[] = {
        0xfffff801bfa7afe0ULL, 0x2000000ULL,          0xffffe5024722a000ULL,
        0x60000044ULL,         0x1ULL,                0xfffff801bfa00000ULL,
        0xfffff801c0000000ULL, 0xfffff801c0100000ULL, 0xfffff801c0200000ULL,
    };
    UsPanicFace face = usPanicFaceDraw(surface, US_PANIC_REPORT_COLUMNS,
                                       US_PANIC_REPORT_LINES);
    UsPen pen = { .ink = US_PANIC_INK };

    usPenAt(&pen, face.column, face.row);
    usPenSize(&pen, face.textScale);
    usPenInk(&pen, US_PANIC_ALERT);
    usPenText(surface, &pen, "PANIC");
    usPenInk(&pen, US_PANIC_INK);
    usPenText(surface, &pen, " code=");
    usPenInk(&pen, US_PANIC_VALUE);
    usPenHex(surface, &pen, US_PANIC_INJECTED);
    usPenInk(&pen, US_PANIC_INK);
    usPenText(surface, &pen, " what=trap\n");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        field(surface, &pen, names[i], values[i]);
    }
    usPenInk(&pen, US_PANIC_ALERT);
    usPenText(surface, &pen, "PANIC-END");
}

/*
 * The colours the two things a reader looks for are drawn in
 *
 * The console the driver writes to shows the heading in red and the values in
 * magenta, and this is the other channel of the same report: someone who has
 * learnt which is which on one has to find them the same way on the other, or
 * the colour is decoration. So both are looked for on the screen, and so is
 * the black that makes them stand out of the table
 */
static void testColours(void) {
    UsSurface surface = surfaceOf(1024U, 768U);
    UsPanicFace face = usPanicFaceDraw(&surface, US_PANIC_REPORT_COLUMNS,
                                       US_PANIC_REPORT_LINES);
    uint32_t line = (uint32_t)US_FONT_HEIGHT * face.textScale;

    guardFill();
    drawReport(&surface);

    if (countColour(face.row, face.row + line, wordOf(US_PANIC_ALERT)) == 0) {
        usCheckFail("colours: the heading is not in red\n");
        failures++;
    }
    if (countColour(face.row, face.row + line, wordOf(US_PANIC_VALUE)) == 0) {
        usCheckFail("colours: the code is not in the value colour\n");
        failures++;
    }
    if (countColour(face.row, face.row + line, wordOf(US_PANIC_INK)) == 0) {
        usCheckFail("colours: the heading line has no black in it\n");
        failures++;
    }
    /* The line under the heading is a field, and a field is a black name and a
     * coloured value: both, or the table has lost one of its columns */
    if (countColour(face.row + line, face.row + 2U * line, wordOf(US_PANIC_VALUE)) == 0) {
        usCheckFail("colours: the first field has no value in the value "
                    "colour\n");
        failures++;
    }
    if (countColour(face.row + line, face.row + 2U * line, wordOf(US_PANIC_INK)) == 0) {
        usCheckFail("colours: the first field has no name in black\n");
        failures++;
    }
}

static void putUint(FILE *out, uint32_t value) {
    char digits[12];
    int n = 0;
    unsigned char c;

    if (value == 0) {
        c = '0';
        fwrite(&c, 1, 1, out);
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        c = (unsigned char)digits[--n];
        fwrite(&c, 1, 1, out);
    }
}

static void preview(const char *path, uint32_t width, uint32_t height) {
    UsSurface surface = surfaceOf(width, height);
    FILE *out;
    int whole = 0;

    guardFill();
    drawReport(&surface);
    checkGuards("the report");
    /* Everything the report drew is inside the frame buffer, and it is not
     * empty: a preview of a blank screen would be a picture of nothing */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            if (at(x, y) != PAPER) {
                whole++;
            }
        }
    }
    usCheckNote("the report on %ux%u: %d pixels are not paper\n", width, height,
                whole);
    if (whole == 0) {
        usCheckFail("the report on %ux%u: nothing was drawn\n", width, height);
        failures++;
    }

    out = fopen(path, "wb");
    if (out == NULL) {
        usCheckFail("the report: cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite("P6\n", 1, 3, out);
    putUint(out, width);
    fwrite(" ", 1, 1, out);
    putUint(out, height);
    fwrite("\n255\n", 1, 5, out);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t v = at(x, y);
            unsigned char rgb[3] = { (unsigned char)((v >> 16) & 0xFFU),
                                     (unsigned char)((v >> 8) & 0xFFU),
                                     (unsigned char)(v & 0xFFU) };

            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    printf("wrote %s: %ux%u\n", path, width, height);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'p') {
        uint32_t width = kScreens[1].width;
        uint32_t height = kScreens[1].height;

        if (argc > 2) {
            if (argc < 4) {
                fprintf(stderr, "usage: %s p <out.ppm> [width height]\n",
                        argv[0]);
                return 2;
            }
            width = (uint32_t)atoi(argv[3]);
            height = (uint32_t)atoi(argv[4]);
            if (width == 0 || width > MAX_WIDTH || height == 0
                || height > MAX_HEIGHT) {
                fprintf(stderr, "%ux%u is outside the buffer, which holds up "
                                "to %ux%u\n", width, height, MAX_WIDTH,
                        MAX_HEIGHT);
                return 2;
            }
        }
        preview(argv[2], width, height);
    } else {
        for (size_t i = 0; i < SCREEN_COUNT; i++) {
            testFace(kScreens[i].width, kScreens[i].height);
        }
        testColours();
    }
    if (failures == 0) {
        return usCheckPassed("all checks passed");
    }
    printf("panic face: %d failures\n", failures);
    return 1;
}
