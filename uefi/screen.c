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
                     (bits & (0x80U >> bit)) != 0 ? US_SCREEN_INK : US_SCREEN_PAPER);
        }
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

void usScreenClear(void) {
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
                            uint32_t stride) {
    gFrame = pixels;
    gWidth = width;
    gHeight = height;
    gStride = stride;
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
                           gGOP->Mode->Info->PixelsPerScanLine);
    return gReady;
}

void usScreenPutc(char c) {
    if (!gReady) {
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
