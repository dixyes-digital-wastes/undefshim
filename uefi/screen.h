/*
 * Drawing on the screen
 *
 * The serial port is where this driver usually reports, but it is not always
 * there: a machine that does not name a UART in its configuration gets no
 * serial output at all, and a configuration that fails to parse is exactly
 * the case where something needs to be said. The screen is the other channel,
 * and it does not depend on the configuration having been read
 *
 * Text is drawn by writing pixels, with the glyphs in font.h. The colour
 * escapes the console writes are parsed here and turn into the ink a glyph is
 * drawn in; a sequence that is not one of those is drawn as the characters it
 * is, so a reader of the screen never sees less than what was said. Nothing
 * is drawn until usScreenInit has found a frame buffer, and everything here
 * is safe to call when it has not
 */

#ifndef US_SCREEN_H
#define US_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Finds the frame buffer and starts drawing at its top left. Called once,
 * early, because the point of it is to still be reporting when the
 * configuration cannot be read
 */
bool usScreenInit(void);

/*
 * The same, for a caller that already has the frame buffer - a test, or
 * anything that was handed the address rather than finding it. The pixels are
 * four bytes each, and blueFirst says which end of the word the red is at:
 * the firmware states this and it cannot be worked out from the address
 */
void usScreenUseFrameBuffer(void *pixels, uint32_t width, uint32_t height,
                            uint32_t stride, bool blueFirst);

/* Also clears what has been written so far */
void usScreenClear(void);

void usScreenPutc(char c);
void usScreenPuts(const char *s);

#endif
