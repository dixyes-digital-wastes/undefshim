/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The face of a panic report, see panelface.h
 *
 * A machine that has stopped is a photograph: someone points a camera at it,
 * and the report is read off that photograph afterwards. What is on the screen
 * before the report is whatever was there when it stopped, which is the
 * kernel's own output and a desktop that was being drawn -- none of it
 * addressed to a reader, and all of it noise around the few lines that matter.
 *
 * So the report is given a screen of its own: white paper, a picture saying
 * which program this is, and the text under it. White rather than black
 * because the paper is what a camera exposes for, and because the text is read
 * as much off a dim photograph as a bright one
 */

#include "payload/panelface.h"

#include "payload/panicimage.h"

/*
 * Doubled until the picture is more than a third of the width, and never so
 * far that it would be more than half the height
 *
 * The width is the thing being aimed at and the height is the limit: the
 * report goes under the picture, so a picture taller than half the screen is
 * one that has left its own report no room. A small screen that can never
 * reach a third of its own width gets the largest picture its height allows
 * rather than none
 */
uint32_t usPanicFacePictureScale(uint32_t width, uint32_t height) {
    uint32_t scale = 1U;

    while (US_PANIC_IMAGE_WIDTH * scale <= width * 3U / 10U
           && US_PANIC_IMAGE_HEIGHT * scale * 2U <= height / 2U) {
        scale *= 2U;
    }
    return scale;
}

uint32_t usPanicFaceTextScale(const UsSurface *surface, uint32_t pictureScale,
                              uint32_t columns, uint32_t lines) {
    const uint32_t pictureBottom =
        US_PANIC_MARGIN + US_PANIC_IMAGE_HEIGHT * pictureScale;
    const uint32_t across = surface->width - 2U * US_PANIC_MARGIN;
    uint32_t scale = 1U;

    /*
     * The two questions in the order they are asked of a reader: a line that
     * does not fit across is a report that has to be stitched back together,
     * and one that does not fit down is a report with its end missing. Either
     * one ends the enlargement at the size that was already there
     */
    for (uint32_t next = 2U; next <= US_PANIC_TEXT_MAX_SCALE; next *= 2U) {
        if (columns * US_FONT_WIDTH * next > across) {
            break;
        }
        if (pictureBottom + US_PANIC_MARGIN + lines * US_FONT_HEIGHT * next
                + US_PANIC_MARGIN
            > surface->height) {
            break;
        }
        scale = next;
    }
    return scale;
}

UsPanicFace usPanicFaceDraw(const UsSurface *surface, uint32_t columns,
                            uint32_t lines) {
    uint32_t pictureScale =
        usPanicFacePictureScale(surface->width, surface->height);
    UsPanicFace face = {
        .textScale = usPanicFaceTextScale(surface, pictureScale, columns,
                                          lines),
        .column = 0,
        /* The row follows the picture, so it is worked out where the picture
         * is drawn rather than written down twice */
        .row = US_PANIC_MARGIN + US_PANIC_IMAGE_HEIGHT * pictureScale
               + US_PANIC_MARGIN,
    };
    uint32_t unit = US_FONT_WIDTH * face.textScale;
    uint32_t across = usSurfaceColumns(surface, unit);

    usSurfaceFill(surface, usSurfaceColour(surface, 0xFF, 0xFF, 0xFF));
    /*
     * The report is centred as a block, and in whole characters: a block whose
     * left edge falls between two of them is one with half a character of
     * paper on one side and half of one less on the other, which is the same
     * as not being centred
     */
    if (across > columns) {
        face.column = (across - columns) / 2U;
    }
    /*
     * A picture wider than the screen is not drawn at all
     *
     * It would fit if it were clipped, and clipping is the wrong answer: what
     * a reader has to be able to tell from across a room is that this machine
     * stopped on purpose, and the report under it says so. Half a picture is
     * noise above a message rather than a sign, and the subtraction below is
     * what a narrower screen would otherwise be told to do backwards
     */
    if (US_PANIC_IMAGE_WIDTH * pictureScale > surface->width) {
        return face;
    }
    /* Centred in the screen's own pixels, so that the leftover of a screen
     * that is not a whole number of pictures wide is half on each side */
    usSurfacePicture(surface, kPanicImage, US_PANIC_IMAGE_WIDTH,
                     US_PANIC_IMAGE_HEIGHT,
                     (surface->width - US_PANIC_IMAGE_WIDTH * pictureScale)
                         / 2U,
                     US_PANIC_MARGIN, pictureScale);
    return face;
}
