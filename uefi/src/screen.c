/*
 * Drawing on the screen, see screen.h for why it is not just the serial port.
 */

#include <uefi.h>

#include "uefi/src/font8x16.h"
#include "uefi/src/screen.h"

/*
 * The graphics output protocol, which the firmware headers here do not
 * declare: they carry the pixel formats and the GUID and say the rest is not
 * used. Only the mode is needed, and only the fields of it that say where the
 * frame buffer is and how wide it is.
 */
typedef struct {
    uint32_t               Version;
    uint32_t               HorizontalResolution;
    uint32_t               VerticalResolution;
    efi_gop_pixel_format_t PixelFormat;
    efi_gop_pixel_bitmask_t PixelInformation;
    uint32_t               PixelsPerScanLine;
} usGopModeInfo;

typedef struct {
    uint32_t       MaxMode;
    uint32_t       Mode;
    usGopModeInfo *Info;
    uint64_t       SizeOfInfo;
    uint64_t       FrameBufferBase;
    uint64_t       FrameBufferSize;
} usGopMode;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    usGopMode *Mode;
} usGop;

/* Two, so that a line of eighty characters fills a 1280 wide screen and the
 * text is readable without leaning in. */
#define US_SCREEN_SCALE 2U
#define US_SCREEN_LINE (US_FONT_HEIGHT * US_SCREEN_SCALE)

/*
 * Black text on white, rather than the other way round: the firmware draws
 * its own console on the same screen, so what this driver says has to be
 * distinguishable at a glance from what everything else says. It also has the
 * useful property that both colours look the same whichever order the
 * channels are in, so the two 8 bit formats need no telling apart.
 */
#define US_SCREEN_INK 0x00000000U
#define US_SCREEN_PAPER 0x00FFFFFFU

static usGop *gGop;
static uint8_t *gFrame;
static uint32_t gWidth;
static uint32_t gHeight;
static uint32_t gStride;  /* pixels a row, which may exceed the width */
static uint32_t gTop;     /* the first row of the area this driver owns */
static uint32_t gColumn;
static uint32_t gRow;
static bool gReady;

static void putPixel(uint32_t x, uint32_t y, uint32_t value) {
    /* Bounded on purpose: a frame buffer smaller than the mode claims would
     * otherwise be written past its end, which is a fault and not a picture.
     * That is not hypothetical - an earlier version of this drew a line below
     * the bottom of the screen and the machine stopped. */
    if (x >= gWidth || y >= gHeight) {
        return;
    }
    *(volatile uint32_t *)(gFrame + (y * gStride + x) * 4U) = value;
}

static void fillBlock(uint32_t x, uint32_t y, uint32_t value) {
    for (uint32_t dy = 0; dy < US_SCREEN_SCALE; dy++) {
        for (uint32_t dx = 0; dx < US_SCREEN_SCALE; dx++) {
            putPixel(x + dx, y + dy, value);
        }
    }
}

static void drawGlyph(char c, uint32_t x, uint32_t y) {
    const uint8_t *glyph;

    if (c < US_FONT_FIRST || c > US_FONT_LAST) {
        c = ' ';
    }
    glyph = kFont8x16[(int)c - US_FONT_FIRST];
    for (uint32_t row = 0; row < US_FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];

        for (uint32_t bit = 0; bit < US_FONT_WIDTH; bit++) {
            fillBlock(x + bit * US_SCREEN_SCALE, y + row * US_SCREEN_SCALE,
                      (bits & (0x80U >> bit)) != 0 ? US_SCREEN_INK : US_SCREEN_PAPER);
        }
    }
}

/*
 * When the area is full, everything moves up a line and the last one is
 * cleared.
 *
 * Clearing instead would be simpler and is what this did first, and it is
 * wrong for the same reason the whole screen exists: the message that matters
 * on an error path is the one just written, and a clear that runs after it
 * takes it away again. That is not hypothetical either - it is why the first
 * messages this driver wrote were missing from the screen while the last one
 * was there.
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
    gRow += US_SCREEN_LINE;
    if (gRow + US_SCREEN_LINE > gHeight) {
        scrollUp();
        gRow = gHeight - US_SCREEN_LINE;
    }
}

bool usScreenReady(void) {
    return gReady;
}

void usScreenClear(void) {
    if (!gReady) {
        return;
    }
    /* Only the area this driver owns: the firmware's own text is above it and
     * is none of our business. */
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
     * The bottom half is this driver's. The firmware's console draws from the
     * top down, and sharing the top means the two keep overwriting each
     * other: a machine whose error is on a line that was drawn over is a
     * machine with nothing to say.
     */
    gTop = gHeight / 2U;
    gColumn = 0;
    gRow = gTop;
    gReady = pixels != NULL && width != 0 && height != 0 && stride != 0;
    if (gReady) {
        usScreenClear();
    }
}

bool usScreenInit(void) {
    efi_guid_t guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    if (gReady) {
        return true;
    }
    gGop = NULL;
    if (EFI_ERROR(BS->LocateProtocol(&guid, NULL, (void **)&gGop)) || gGop == NULL
        || gGop->Mode == NULL || gGop->Mode->Info == NULL) {
        return false;
    }
    /* Only the two formats whose channels are eight bits wide are drawn on: a
     * bit mask or a blt-only mode would need a translation this does not
     * have, and guessing at one would put unreadable pixels on the screen. */
    if (gGop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor
        && gGop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
        return false;
    }
    if (gGop->Mode->FrameBufferBase == 0 || gGop->Mode->Info->PixelsPerScanLine == 0
        || gGop->Mode->Info->HorizontalResolution == 0
        || gGop->Mode->Info->VerticalResolution == 0) {
        return false;
    }
    usScreenUseFrameBuffer((void *)(uintptr_t)gGop->Mode->FrameBufferBase,
                           gGop->Mode->Info->HorizontalResolution,
                           gGop->Mode->Info->VerticalResolution,
                           gGop->Mode->Info->PixelsPerScanLine);
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
         * return means the start of the line, not a glyph nobody has. */
        gColumn = 0;
        return;
    }
    drawGlyph(c, gColumn * US_FONT_WIDTH * US_SCREEN_SCALE, gRow);
    gColumn += US_FONT_WIDTH * US_SCREEN_SCALE;
    if (gColumn + US_FONT_WIDTH * US_SCREEN_SCALE > gWidth) {
        newline();
    }
}

void usScreenPuts(const char *s) {
    for (; s != NULL && *s != '\0'; s++) {
        usScreenPutc(*s);
    }
}

void usScreenPutHex(uint64_t value) {
    char digits[16];
    int n = 0;

    usScreenPuts("0x");
    if (value == 0) {
        usScreenPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        uint32_t nibble = (uint32_t)(value & 0xF);
        digits[n++] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
        value >>= 4;
    }
    while (n > 0) {
        usScreenPutc(digits[--n]);
    }
}

void usScreenPutDec(uint64_t value) {
    char digits[20];
    int n = 0;

    if (value == 0) {
        usScreenPutc('0');
        return;
    }
    while (value != 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (n > 0) {
        usScreenPutc(digits[--n]);
    }
}
