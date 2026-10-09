/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * How a panic report looks
 *
 * The layout is here rather than in the report because it is a picture and not
 * a message: a margin, the picture above, the text below, and all three follow
 * from the screen so that the same face comes out of any of them
 */

#ifndef US_PANEL_FACE_H
#define US_PANEL_FACE_H

#include "core/pixel.h"

/*
 * The margin, in the screen's own pixels, above the picture and beside the
 * text
 *
 * A photograph of a screen is cropped, at an angle, and taken of a machine
 * that may be lying any way up, so what is written near an edge is what is
 * lost first. Everything the face has to say is inside this
 */
#define US_PANIC_MARGIN 24U

/*
 * The largest the text may be drawn at
 *
 * The text is what is read, so it is enlarged to be readable, and then it
 * stops: past four times its own size a screen is not a telephone photograph
 * any more, and the report is being read out of a dump
 */
#define US_PANIC_TEXT_MAX_SCALE 4U

/*
 * The three colours the face is drawn in
 *
 * The heading and the values are the two things a reader of the report looks
 * for, and the console the driver writes to already shows them in red and in
 * magenta -- US_RED and US_VALUE in uefi/console.h. This is the other channel
 * of the same report, so it is read the same way, and the numbers here are
 * what those escapes come out as on a screen. Everything else is black on
 * white, which is what the boot's own screen is
 */
#define US_PANIC_INK 0x00000000U   /* everything that is just text */
#define US_PANIC_ALERT 0x00C00000U /* the heading: what went wrong */
#define US_PANIC_VALUE 0x00A000A0U /* the numbers, which are looked up */

/*
 * The size the picture is drawn at
 *
 * The picture is a bitmap and the same size in pixels wherever it is drawn, so
 * on a large screen it is a small part of it. It is doubled until it takes
 * more than a third of the width, which is a sign rather than a stamp, or
 * until doubling again would take it past half the height, which is where it
 * would leave the report under it no room
 *
 * Powers of two only: a picture drawn at three times the size has runs of
 * three pixels and of two, and that is a blurred picture rather than a larger
 * one. The font is a bitmap for the same reason, and neither survives an
 * enlargement that is not a whole number
 */
uint32_t usPanicFacePictureScale(uint32_t width, uint32_t height);

/*
 * The size the text is drawn at, given what has to be drawn
 *
 * Doubled while the whole report still fits, up to the ceiling above, and that
 * is the difference from the picture: the text is not enlarged to fill the
 * screen but to stay readable, so what limits it is the report itself. A block
 * of it is a table, and a table that runs off the right of the screen or under
 * its bottom edge is worse than a smaller one
 *
 * So it is the largest of 1, 2 and 4 that leaves the widest line inside the
 * margins and the whole face inside the screen. On a small screen that is the
 * size the text already is, which is the answer there: there is no room to
 * make it larger, and losing a line of the report to gain a few pixels of
 * height is not a trade
 */
uint32_t usPanicFaceTextScale(const UsSurface *surface, uint32_t pictureScale,
                              uint32_t columns, uint32_t lines);

/*
 * Where the report is drawn: at what size, and from which column and line
 *
 * The column is a column of characters and the row is a pixel, which is how
 * the frame buffer's pen counts them. The reader of a panic is looking for one
 * number in a table, so the block is centred as a block -- the picture above
 * it is centred too, so the two line up as one face rather than the text
 * hanging off one side of it
 */
typedef struct UsPanicFace_t {
    uint32_t textScale;
    uint32_t column;
    uint32_t row;
} UsPanicFace;

/*
 * Draws the face and says where its text goes
 *
 * White paper, then the picture centred under the top margin, and then the
 * place the report begins. The two sizes are worked out here because they
 * follow from the screen, and the caller writes its own lines at the size it
 * is told
 *
 * The text is not drawn here: the report is written by the side that has the
 * numbers, and this stays a layout. What it has to be told is how wide the
 * widest line is and how many lines there are, because that is what the size
 * of the text follows from -- and both are facts about the report, which is
 * written on the other side
 *
 * Nothing is assumed about the surface beyond its bounds, and the picture is
 * skipped rather than clipped when the screen is too narrow for it: half a
 * logo says less than none of it
 */
UsPanicFace usPanicFaceDraw(const UsSurface *surface, uint32_t columns,
                            uint32_t lines);

#endif
