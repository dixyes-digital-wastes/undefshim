/*
 * Drawing on the screen, see screen.h for why it is not just the serial port
 */

#include <uefi.h>

#include "uefi/font.h"
#include "uefi/screen.h"

/*
 * The graphics output protocol, which the firmware headers here do not
 * declare: they carry the pixel formats and the GUID and say the rest is not
 * used. Only the mode is needed, and only the fields of it that say where the
 * frame buffer is and how wide it is
 */
typedef struct {
    uint32_t               Version;
    uint32_t               HorizontalResolution;
    uint32_t               VerticalResolution;
    efi_gop_pixel_format_t PixelFormat;
    efi_gop_pixel_bitmask_t PixelInformation;
    uint32_t               PixelsPerScanLine;
} UsGOPModeInfo;

typedef struct {
    uint32_t       MaxMode;
    uint32_t       Mode;
    UsGOPModeInfo *Info;
    uint64_t       SizeOfInfo;
    uint64_t       FrameBufferBase;
    uint64_t       FrameBufferSize;
} UsGOPMode;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    UsGOPMode *Mode;
} UsGOP;

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
 * distinguishable at a glance from what everything else says. It also has the
 * useful property that both colours look the same whichever order the
 * channels are in, so the two 8 bit formats need no telling apart
 */
#define US_SCREEN_INK 0x00000000U
#define US_SCREEN_PAPER 0x00FFFFFFU

static UsGOP *gGOP;
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
    /*
     * Black and white are the same number whatever order the channels are in,
     * which is why this went unnoticed while they were the only two colours.
     * A colour is not: the format says which end of the word the red is at
     */
    if (gBlueFirst) {
        value = (value & 0xFF00FF00U) | ((value & 0xFFU) << 16)
                | ((value >> 16) & 0xFFU);
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
 * The eight base colours and grey, and no bright ones: the escapes are read
 * by a terminal as well as by this, and a terminal is usually light text on a
 * dark background while this is the other way round. Only the base eight read
 * acceptably on both. A colour that is not named here is drawn in black,
 * which is what a terminal would do with a colour it had no opinion about
 */
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

bool usScreenInit(void) {
    efi_guid_t guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    if (gReady) {
        return true;
    }
    gGOP = NULL;
    if (EFI_ERROR(BS->LocateProtocol(&guid, NULL, (void **)&gGOP)) || gGOP == NULL
        || gGOP->Mode == NULL || gGOP->Mode->Info == NULL) {
        return false;
    }
    /* Only the two formats whose channels are eight bits wide are drawn on: a
     * bit mask or a blt-only mode would need a translation this does not
     * have, and guessing at one would put unreadable pixels on the screen */
    if (gGOP->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor
        && gGOP->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
        return false;
    }
    if (gGOP->Mode->FrameBufferBase == 0 || gGOP->Mode->Info->PixelsPerScanLine == 0
        || gGOP->Mode->Info->HorizontalResolution == 0
        || gGOP->Mode->Info->VerticalResolution == 0) {
        return false;
    }
    usScreenUseFrameBuffer((void *)(uintptr_t)gGOP->Mode->FrameBufferBase,
                           gGOP->Mode->Info->HorizontalResolution,
                           gGOP->Mode->Info->VerticalResolution,
                           gGOP->Mode->Info->PixelsPerScanLine,
                           gGOP->Mode->Info->PixelFormat
                               == PixelBlueGreenRedReserved8BitPerColor);
    return gReady;
}

/*
 * The colour escapes the console writes, read back on this side
 *
 * The console writes to two places that do not know about each other and the
 * same bytes have to make sense at both. A terminal reads them for itself; a
 * frame buffer does not, so the ones that say what colour to use are parsed
 * here and everything else is drawn as the characters it is
 *
 * The parser is a byte at a time because that is how the text arrives, and it
 * holds what it has not yet decided about. A sequence it does not recognise
 * is drawn as the characters that followed the escape, which is what a
 * terminal that does not understand it does too: the escape byte itself has
 * no glyph and is dropped
 */
typedef enum UsCSIState_e {
    UsCsiGround = 0,
    UsCsiEscape,   /* the byte after ESC */
    UsCsiParam,    /* inside the brackets, collecting the parameters */
} UsCSIState;

#define US_CSI_MAX 16U

static struct {
    uint8_t state;
    /* Everything that followed the escape, the bracket included: what the
     * sequence said, which is what is drawn when it turns out not to be one
     * of ours */
    char body[US_CSI_MAX];
    uint32_t length;
} gCSI;

/* Draws what the sequence held, as the characters it is. The state goes back
 * to ground first, because drawing re-enters the same path */
static void csiReject(void) {
    uint32_t length = gCSI.length;

    gCSI.length = 0;
    gCSI.state = UsCsiGround;
    for (uint32_t i = 0; i < length; i++) {
        usScreenPutc(gCSI.body[i]);
    }
}

/*
 * Whether the body is a colour's parameter: a bracket and then a number
 *
 * The bracket is kept in the body so that a sequence which turns out not to
 * be a colour can be drawn the way it was written, and so the number starts
 * after it
 */
static bool csiIsNumber(void) {
    if (gCSI.length < 1U || gCSI.body[0] != '[') {
        return false;
    }
    for (uint32_t i = 1; i < gCSI.length; i++) {
        if (gCSI.body[i] < '0' || gCSI.body[i] > '9') {
            return false;
        }
    }
    return true;
}

/*
 * The end of a sequence: the colour if it is one, and the characters it was
 * made of if it is not
 *
 * A sequence this does not recognise is drawn rather than swallowed, which is
 * what a terminal that does not understand it does as well. The escape byte
 * itself is dropped, because it has no glyph, so what is left is everything
 * from the bracket on
 */
static void csiFinish(char final) {
    bool colour = final == 'm' && csiIsNumber();
    uint32_t value = 0;
    uint32_t length = gCSI.length;

    gCSI.length = 0;
    gCSI.state = UsCsiGround;
    if (colour) {
        for (uint32_t i = 1; i < length; i++) {
            value = value * 10U + (uint32_t)(gCSI.body[i] - '0');
        }
        /* No parameter means the default, which is the reset */
        gInk = value == 0 ? US_SCREEN_INK : screenColour(value);
        return;
    }
    for (uint32_t i = 0; i < length; i++) {
        usScreenPutc(gCSI.body[i]);
    }
    usScreenPutc(final);
}

/*
 * One byte of an escape sequence, or false when there is none in progress and
 * the caller should draw the byte the ordinary way
 */
static bool csiFeed(char c) {
    if (gCSI.state == UsCsiGround) {
        if (c == '\x1b') {
            gCSI.state = UsCsiEscape;
            return true;
        }
        return false;
    }
    if (gCSI.state == UsCsiEscape) {
        if (c != '[') {
            /* An escape of some other kind. It has no glyph and is dropped,
             * and this byte is drawn, which is what a terminal does too */
            gCSI.state = UsCsiGround;
            return false;
        }
        gCSI.state = UsCsiParam;
        gCSI.length = 0;
        gCSI.body[gCSI.length++] = '[';
        return true;
    }
    if (c == '\x1b') {
        /* Another sequence starts, so this one is over */
        csiReject();
        gCSI.state = UsCsiEscape;
        return true;
    }
    if ((uint8_t)c < 0x20U || (uint8_t)c == 0x7FU) {
        /* A control character ends a sequence, and is then handled as
         * itself - a newline in the middle of one still ends the line */
        csiReject();
        return false;
    }
    if ((uint8_t)c >= 0x40U && (uint8_t)c <= 0x7EU && c != '[') {
        csiFinish(c);
        return true;
    }
    if (gCSI.length >= US_CSI_MAX) {
        /* Longer than anything this writes. Draw what there is and stop */
        csiReject();
        return true;
    }
    gCSI.body[gCSI.length++] = c;
    return true;
}

void usScreenPutc(char c) {
    if (!gReady) {
        return;
    }
    /* A colour escape is not a character and does not take a column */
    if (csiFeed(c)) {
        return;
    }
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

void usScreenPuts(const char *s) {
    for (; s != NULL && *s != '\0'; s++) {
        usScreenPutc(*s);
    }
}
