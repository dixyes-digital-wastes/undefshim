/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Drawing on the screen, see screen.h for why it is not just the serial port,
 * gop.h for where the pixels come from and ansi.h for the escapes it reads
 */

#include <stddef.h>

#include "common/font.h"
#include "core/pixel.h"
#include "uefi/ansi.h"
#include "uefi/gop.h"
#include "uefi/screen.h"

/*
 * A glyph is drawn at its own size, so a row of text is as tall as the font
 * and a character is as wide as one. Nothing here scales the font: the face
 * was chosen for its size, and scaling it would blur the very pixels that make
 * it worth using
 */
#define US_SCREEN_LINE US_FONT_HEIGHT

/*
 * Black text on white, rather than the other way round: the firmware draws
 * its own console on the same screen, so what this driver says has to be
 * distinguishable at a glance from what everything else says
 */
#define US_SCREEN_INK 0x00000000U
#define US_SCREEN_PAPER 0x00FFFFFFU

static uint8_t *gFrame;
static uint32_t gWidth;
static uint32_t gHeight;
static uint32_t gStride;  /* pixels a row, which may exceed the width */
static uint32_t gTop;     /* the first row of the area this driver owns */
static uint32_t gColumn;  /* characters, not pixels */
static uint32_t gRow;
static bool gReady;
static bool gBlueFirst;   /* the frame buffer's channels, blue at the bottom */
static uint32_t gInk = US_SCREEN_INK;

/*
 * The colour escapes, read back on this side
 *
 * The console writes to two places that do not know about each other and the
 * same bytes have to make sense at both. A terminal reads them for itself; a
 * frame buffer does not, so the ones that say what colour to use are parsed
 * and everything else is drawn as the characters it is
 */
static UsANSIScan gANSI;

/* How many characters fit across the frame buffer */
static uint32_t columns(void) {
    return gWidth / US_FONT_WIDTH;
}

static void putPixel(uint32_t x, uint32_t y, uint32_t value) {
    /* Bounded on purpose: a frame buffer smaller than the mode claims would
     * otherwise be written past its end, which is a fault and not a picture */
    if (x >= gWidth || y >= gHeight) {
        return;
    }
    /* The format says which end of the word the red is at */
    if (gBlueFirst) {
        value = usPixelSwap(value);
    }
    *(volatile uint32_t *)(gFrame + (y * gStride + x) * 4U) = value;
}

static void drawGlyph(char c, uint32_t x, uint32_t y) {
    const uint8_t *glyph;

    if (c < US_FONT_FIRST || c > US_FONT_LAST) {
        c = ' ';
    }
    glyph = kFont[(int)c - US_FONT_FIRST];
    for (uint32_t row = 0; row < US_FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];

        for (uint32_t bit = 0; bit < US_FONT_WIDTH; bit++) {
            putPixel(x + bit, y + row,
                     (bits & (0x80U >> bit)) != 0 ? gInk : US_SCREEN_PAPER);
        }
    }
}

/*
 * The colours the console names with SGR, as pixels
 *
 * Black ink on white paper is what the rest of this draws, so a colour has to
 * be dark enough to read against white - which rules out the bright half of
 * the palette, whose whole point is being legible on black. The two that the
 * console uses from that half are given darker versions here rather than
 * being left to fall through, which would draw them in the default ink and
 * lose the difference between them and everything else
 */
static uint32_t screenColour(uint32_t sgr) {
    switch (sgr) {
    case 31:
        return 0x00C00000U; /* red */
    case 32:
        return 0x0000A000U; /* green */
    case 33:
        return 0x00A0A000U; /* yellow */
    case 34:
        return 0x000000C0U; /* blue */
    case 35:
        return 0x00A000A0U; /* magenta */
    case 36:
        return 0x0000A0A0U; /* cyan */
    case 90:
        return 0x00606060U; /* grey */
    case 96:
        return 0x0000C0C0U; /* bright cyan, darkened to be read on white */
    default:
        return US_SCREEN_INK;
    }
}

/*
 * When the area is full, everything moves up a line and the last one is
 * cleared
 *
 * Clearing instead would be simpler and is what this did first, and it is
 * wrong for the same reason the whole screen exists: the message that matters
 * on an error path is the one just written, and a clear that runs after it
 * takes it away again. That is not hypothetical either - it is why the first
 * messages this driver wrote were missing from the screen while the last one
 * was there
 */
static void scrollUp(void) {
    uint32_t rows = gHeight - gTop - US_SCREEN_LINE;
    uint8_t *dst = gFrame + (uint64_t)gTop * gStride * 4U;
    const uint8_t *src = dst + US_SCREEN_LINE * gStride * 4U;

    for (uint32_t i = 0; i < rows * gStride * 4U; i++) {
        dst[i] = src[i];
    }
    for (uint32_t y = gHeight - US_SCREEN_LINE; y < gHeight; y++) {
        for (uint32_t x = 0; x < gWidth; x++) {
            putPixel(x, y, US_SCREEN_PAPER);
        }
    }
}

static void newline(void) {
    gColumn = 0;
    /*
     * A frame buffer shorter than two lines has nothing to scroll: the one
     * line it has is written over itself, which at least keeps the newest
     * text. The arithmetic below would otherwise be told to move a negative
     * number of rows, which as an unsigned count is an enormous copy
     */
    if (gHeight < 2U * US_SCREEN_LINE) {
        gRow = gTop;
        return;
    }
    gRow += US_SCREEN_LINE;
    if (gRow + US_SCREEN_LINE > gHeight) {
        scrollUp();
        gRow = gHeight - US_SCREEN_LINE;
    }
}

/* Also clears what has been written so far. The colour goes back to the one
 * a line with no escape in it is drawn in */
void usScreenClear(void) {
    gInk = US_SCREEN_INK;
    if (!gReady) {
        return;
    }
    for (uint32_t y = gTop; y < gHeight; y++) {
        for (uint32_t x = 0; x < gWidth; x++) {
            putPixel(x, y, US_SCREEN_PAPER);
        }
    }
    gColumn = 0;
    gRow = gTop;
}

void usScreenUseFrameBuffer(void *pixels, uint32_t width, uint32_t height,
                            uint32_t stride, bool blueFirst) {
    gFrame = pixels;
    gWidth = width;
    gHeight = height;
    gStride = stride;
    gBlueFirst = blueFirst;
    /*
     * From the very top. The firmware's console draws from the top down as
     * well, and the two would overwrite each other; leaving ours above it and
     * keeping the same lines on screen is worse than writing over it, because
     * a message that scrolled away is worse than one that was never there and
     * the console's own output is not what is being read
     */
    gTop = 0;
    gColumn = 0;
    gRow = gTop;
    gReady = pixels != NULL && width != 0 && height != 0 && stride != 0
             && columns() != 0;
    if (gReady) {
        usScreenClear();
    }
}

void usScreenFrameBuffer(UsScreenInfo *out) {
    if (out == NULL) {
        return;
    }
    *out = (UsScreenInfo){
        .base = gReady ? (uint64_t)(uintptr_t)gFrame : 0,
        .bytes = gReady ? (uint64_t)gStride * gHeight * 4U : 0,
        .width = gWidth,
        .height = gHeight,
        .stride = gStride,
        .blueFirst = gBlueFirst,
    };
}

bool usScreenInit(void) {
    UsGOPFrame frame;

    if (gReady) {
        return true;
    }
    if (!usGOPFind(&frame)) {
        return false;
    }
    usScreenUseFrameBuffer(frame.pixels, frame.width, frame.height, frame.stride,
                           frame.blueFirst);
    return gReady;
}

/* One byte of text, with the cursor it moves */
static void putText(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        /* The console writes one of these before every newline. Carriage
         * return means the start of the line, not a glyph nobody has */
        gColumn = 0;
        return;
    }
    drawGlyph(c, gColumn * US_FONT_WIDTH, gRow);
    gColumn++;
    if (gColumn >= columns()) {
        newline();
    }
}

void usScreenPutc(char c) {
    UsANSIVerdict verdict;

    if (!gReady) {
        return;
    }
    /* What the byte is - text, part of a sequence, or the end of one - is the
     * parser's answer, and an escape never takes a column of its own */
    verdict = usANSIFeed(&gANSI, c);
    if (verdict == UsANSITaken) {
        return;
    }
    if (verdict == UsANSIColour) {
        /* No parameter means the default, which is the reset */
        gInk = gANSI.param == 0U ? US_SCREEN_INK : screenColour(gANSI.param);
        return;
    }
    /*
     * A sequence this does not know is drawn rather than swallowed, which is
     * what a terminal that does not understand it does as well: the escape
     * byte has no glyph and is dropped, so what is left is everything from
     * the bracket on
     */
    if (verdict == UsANSIUnknown || verdict == UsANSIAbandoned) {
        for (uint32_t i = 0; i < gANSI.length; i++) {
            putText(gANSI.text[i]);
        }
        if (verdict == UsANSIAbandoned) {
            return;
        }
    }
    putText(c);
}

void usScreenPuts(const char *s) {
    for (; s != NULL && *s != '\0'; s++) {
        usScreenPutc(*s);
    }
}
