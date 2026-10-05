/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The screen drawer, so far as it can be checked without looking at a screen
 *
 * What this checks is that nothing is written outside the frame buffer,
 * whatever it is asked to draw, and that what it draws lands inside the area
 * it claims. It also drives the real console into the real drawer, so that
 * what a line looks like - its colours, its layout - can be looked at without
 * a machine
 */

#include <uefi.h>

int printf(const char *format, ...);
int putchar(int c);
int atoi(const char *s);

/*
 * The file functions come from uefi.h, which declares them against its own
 * FILE. On the host that is the host's stdio behind the same signatures,
 * which is why the header can be used here at all
 */

#include "uefi/console.h"
#include "uefi/font.h"
#include "uefi/screen.h"

#define US_CHECK_NAME "test_screen"
#include "check.h"

/* usScreenInit looks the protocol up; this test sets the frame buffer itself
 * and never calls it, but the symbols have to exist for the linker */
efi_system_table_t *ST;
efi_boot_services_t *BS;

/* Forty characters across and eight lines down: enough for a line to wrap and
 * for more lines than fit, which is what the scrolling is for */
#define WIDTH 320U
#define HEIGHT (US_FONT_HEIGHT * 8U)

/* What SGR 31 comes out as, which an error line is written in */
#define RED_INK 0x00C00000U
/* A guard row before the frame buffer and a guard row after it, so that a
 * write above the top or below the bottom lands somewhere that can be
 * noticed. The frame buffer itself has to be one contiguous block, which is
 * why this is a flat array and not a two dimensional one with a margin: the
 * drawer walks it by the stride it was given */
#define GUARD 0x5A5A5A5AU

static uint32_t arena[(HEIGHT + 2U) * WIDTH];
static uint32_t *frame = &arena[WIDTH];

/*
 * A frame the size of a real one, for the preview: the checks above want a
 * small buffer so that more text than fits is a line or two away
 */
#define PREVIEW_WIDTH 1024U
#define PREVIEW_HEIGHT 768U

static uint32_t previewFrame[PREVIEW_WIDTH * PREVIEW_HEIGHT];

static int failures;

static void fputsTo(FILE *out, const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    fwrite(s, 1, n, out);
}

static void putUintTo(FILE *out, uint32_t value) {
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

static void checkGuards(const char *what) {
    for (uint32_t i = 0; i < WIDTH; i++) {
        if (arena[i] != GUARD) {
            usCheckFail("%s: wrote above the frame buffer at column %u\n", what, i);
            failures++;
            return;
        }
        if (arena[(HEIGHT + 1U) * WIDTH + i] != GUARD) {
            usCheckFail("%s: wrote below the frame buffer at column %u\n", what, i);
            failures++;
            return;
        }
    }
}

static void fillGuards(void) {
    for (uint32_t i = 0; i < (HEIGHT + 2U) * WIDTH; i++) {
        arena[i] = GUARD;
    }
}

/* Some ink, and no ink, in the area the drawer owns */
static void checkDrawn(const char *what) {
    uint32_t ink = 0;
    uint32_t paper = 0;
    uint32_t other = 0;

    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t v = frame[y * WIDTH + x];

            if (v == 0x00000000U) {
                ink++;
            } else if (v == 0x00FFFFFFU) {
                paper++;
            } else {
                other++;
            }
        }
    }
    usCheckNote("%s: ink %u, paper %u, other %u\n", what, ink, paper, other);
    if (other != 0) {
        usCheckFail("%s: %u pixels are neither ink nor paper\n", what, other);
        failures++;
    }
    if (ink == 0) {
        usCheckFail("%s: nothing was drawn\n", what);
        failures++;
    }
}

/* How many pixels of ink a block of lines holds */
static uint32_t inkInLines(uint32_t first, uint32_t lines) {
    uint32_t ink = 0;

    for (uint32_t y = first * US_FONT_HEIGHT;
         y < (first + lines) * US_FONT_HEIGHT && y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            if (frame[y * WIDTH + x] == 0x00000000U) {
                ink++;
            }
        }
    }
    return ink;
}

/*
 * One line of what was drawn, as text, for looking at. The checks above say
 * nothing is written outside the frame buffer and that something landed where
 * it should; whether the glyphs come out as letters is not a thing a check
 * can say, so a line can be printed as pixels and read
 */
static void printLineArt(uint32_t first) {
    for (uint32_t y = first * US_FONT_HEIGHT;
         y < (first + 1U) * US_FONT_HEIGHT && y < HEIGHT; y++) {
        printf("%2u ", y);
        for (uint32_t x = 0; x < WIDTH; x++) {
            putchar(frame[y * WIDTH + x] == 0x00000000U ? '#' : '.');
        }
        putchar('\n');
    }
}

/*
 * The colour of the first pixel that is not paper, which is the ink the last
 * glyph drawn in the top left cell was drawn with
 */
static uint32_t firstInk(void) {
    for (uint32_t y = 0; y < US_FONT_HEIGHT; y++) {
        for (uint32_t x = 0; x < US_FONT_WIDTH; x++) {
            uint32_t v = frame[y * WIDTH + x];

            if (v != 0x00FFFFFFU) {
                return v;
            }
        }
    }
    return 0x00FFFFFFU;
}

/*
 * The colour escapes, and what happens to sequences that are not them
 *
 * The console writes the escapes it wants the terminal to read, and this
 * drawer has to read the same bytes. A sequence it does not know is drawn as
 * the characters it was written as, so that a screen without colour support
 * still says what the log said rather than swallowing the line
 */
static void testColour(void) {
    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH, false);
    usScreenClear();
    usScreenPuts("X");
    if (firstInk() != 0x00000000U) {
        usCheckFail("colour: a plain line is not black\n");
        failures++;
    }

    usScreenClear();
    usScreenPuts("\x1b[31mX");
    if (firstInk() == 0x00000000U) {
        usCheckFail("colour: an escape was drawn instead of taken\n");
        failures++;
    }

    /* The colour stays until it is changed, and the reset puts it back */
    usScreenClear();
    usScreenPuts("\x1b[35mY\x1b[0m");
    if (firstInk() == 0x00000000U) {
        usCheckFail("colour: the reset came too late to be seen\n");
        failures++;
    }
    usScreenClear();
    usScreenPuts("\x1b[35m\x1b[0mX");
    if (firstInk() != 0x00000000U) {
        usCheckFail("colour: the reset did not put the ink back\n");
        failures++;
    }

    /* A sequence this does not know is drawn, minus the escape byte, which
     * has no glyph: an escape, a bracket and then letters reads as the
     * bracket and the letters */
    usScreenClear();
    usScreenPuts("\x1b[notsupp");
    {
        /* The first cell holds the bracket, and the second the n */
        uint32_t second = 0;

        for (uint32_t y = 0; y < US_FONT_HEIGHT; y++) {
            for (uint32_t x = US_FONT_WIDTH; x < US_FONT_WIDTH * 2U; x++) {
                if (frame[y * WIDTH + x] != 0x00FFFFFFU) {
                    second++;
                }
            }
        }
        if (firstInk() == 0x00FFFFFFU || second == 0) {
            usCheckFail("colour: an unknown sequence was swallowed\n");
            failures++;
        }
    }

    /* A newline inside a sequence still ends the line */
    usScreenClear();
    usScreenPuts("\x1b[3");
    usScreenPuts("\nX");
    if (inkInLines(0, 1) == 0 || inkInLines(1, 1) == 0) {
        usCheckFail("colour: a newline inside a sequence was lost\n");
        failures++;
    }
}

/*
 * The real console, drawing into the real drawer, so that a line can be
 * looked at as it will be drawn
 *
 * This is not a check: it is the only way to see whether the colours and the
 * layout are readable without a machine, and it is the same code the driver
 * runs. The console is pointed at no UART, which leaves the screen as its
 * only channel
 */
static void preview(const char *path) {
    FILE *out;

    usScreenUseFrameBuffer(previewFrame, PREVIEW_WIDTH, PREVIEW_HEIGHT,
                           PREVIEW_WIDTH, false);
    usConsoleUse(UsUARTOff, 0, 8);
    usConsoleLevel(UsLogDebug);

    usLogI("undefshim", "0.1.0\n");
    usLogI("config", "loaded\n");
    usLogV("config", "level=" US_VALUE("%u") " rewrite=" US_VALUE("%u")
           " debug=" US_VALUE("%u") " patches=" US_VALUE("%u") "\n",
           (unsigned)UsLogInfo, 1U, 0U, 0U);
    usLogW("config", "no descriptor base, the kernel's own pages will not be rewritten\n");
    usLogV("config", "a detail that is only worth saying when asked for\n");
    usLogD("config", "and one that is only for debugging what went wrong\n");
    usConsoleMilestone("M2 done");
    usLogI("pool", "pa=" US_VALUE("%#llx") " bytes=" US_VALUE("%#llx")
           " slots=" US_VALUE("%u") "\n", 0x1380e0000ULL, 0x22000ULL, 8U);
    usLogI("pool", "stack[0]=" US_VALUE("%#llx") "\n", 0x1380e6000ULL);
    usConsoleMilestone("M6 payload placed");
    usLogV("gmm", "winload found at " US_VALUE("%#llx") "\n", 0x40a55000ULL);
    usLogV("gmm", "ntoskrnl found at " US_VALUE("%#llx") "\n", 0x100000000ULL);
    usLogV("arm", "handover +" US_VALUE("%#x") " -> slot +" US_VALUE("%#x")
           " -> payload " US_VALUE("%#llx") "\n",
           0x109cU, 0x2790U, 0x13bc01948ULL);
    usLogV("arm", "ntoskrnl tables=" US_VALUE("%u") " stubs at +" US_VALUE("%#x")
           " room=" US_VALUE("%u") "\n", 1U, 0x60c05cU, 4004U);
    usLogE("patch", "refusing the volume root as a list directory\n");
    usLogI("patch", "all-ldapr.txt applied " US_VALUE("%u") " refused "
           US_VALUE("%u") "\n", 1576U, 2U);
    usConsoleMilestone("M6.5 armed");
    usLogD("progress", "marking the way\n");
    usConsoleProgress('g');
    usConsoleProgress('1');
    usConsoleProgress('2');
    usConsolePuts("\n");
    usConsoleMilestone("M7 rewritten");
    usLogI("rewrite", "ntoskrnl holds " US_VALUE("%u") "\n", 1931U);

    out = fopen(path, "wb");
    if (out == NULL) {
        usCheckFail("preview: cannot write %s\n", path);
        failures++;
        return;
    }
    /* The header, a number at a time: small enough to build by hand, and it
     * keeps the file's writer to the pixels */
    fputsTo(out, "P6\n");
    putUintTo(out, PREVIEW_WIDTH);
    fputsTo(out, " ");
    putUintTo(out, PREVIEW_HEIGHT);
    fputsTo(out, "\n255\n");
    for (uint32_t i = 0; i < PREVIEW_WIDTH * PREVIEW_HEIGHT; i++) {
        uint32_t v = previewFrame[i];
        unsigned char rgb[3];

        rgb[0] = (unsigned char)((v >> 16) & 0xFFU);
        rgb[1] = (unsigned char)((v >> 8) & 0xFFU);
        rgb[2] = (unsigned char)(v & 0xFFU);
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
    printf("wrote %s\n", path);
}

/*
 * The level filter, on the real console
 *
 * A line above the level is dropped whole: what is checked is that nothing of
 * it reaches the screen, including the escapes, and that the line after it
 * still does
 */
static void testLevel(void) {
    uint32_t loudOnly;

    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH, false);
    usConsoleUse(UsUARTOff, 0, 8);
    usConsoleLevel(UsLogInfo);

    /* On its own, a line below the level puts nothing on the screen at all:
     * not the text, not the escape that would have coloured it */
    usScreenClear();
    usLogD("quiet", "not worth saying\n");
    if (inkInLines(0, 2) != 0) {
        usCheckFail("level: a line below the level was written\n");
        failures++;
    }

    usScreenClear();
    usLogI("loud", "worth saying\n");
    loudOnly = inkInLines(0, 1);
    if (loudOnly == 0) {
        usCheckFail("level: a line at the level was dropped\n");
        failures++;
    }

    /* With the quiet line in front of it, the loud one is drawn in exactly
     * the same place: a dropped line does not take a row */
    usScreenClear();
    usLogD("quiet", "not worth saying\n");
    usLogI("loud", "worth saying\n");
    if (inkInLines(0, 1) != loudOnly || inkInLines(1, 2) != 0) {
        usCheckFail("level: the dropped line left something behind\n");
        failures++;
    }

    usConsoleLevel(UsLogDebug);
    usScreenClear();
    usLogD("quiet", "worth saying now\n");
    if (inkInLines(0, 1) == 0) {
        usCheckFail("level: a line was dropped below its level\n");
        failures++;
    }

    /* Nothing at all is written when the level is off, numbers included */
    usConsoleLevel(UsLogOff);
    usScreenClear();
    usLogE("x", "gone " US_VALUE("%u") "\n", 42U);
    if (inkInLines(0, 2) != 0) {
        usCheckFail("level: something was written with the level off\n");
        failures++;
    }
    usConsoleLevel(UsLogDebug);
}

/*
 * The colour switch is the serial port's, not the screen's
 *
 * The screen turns the escapes into the ink it draws with and has no other
 * way of knowing which colour a line is; the switch is about the log, which
 * may be read by something that would rather not see the escapes. So turning
 * it off has to leave the screen exactly as it was
 */
static void testColourIsScreenIndependent(void) {
    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH, false);
    usConsoleUse(UsUARTOff, 0, 8);

    usConsoleColour(false);
    usScreenClear();
    usLogE("patch", "a failure\n");
    if (firstInk() != RED_INK) {
        usCheckFail("colour switch: the screen lost its colour with it off\n");
        failures++;
    }

    usConsoleColour(true);
    usScreenClear();
    usLogE("patch", "a failure\n");
    if (firstInk() != RED_INK) {
        usCheckFail("colour switch: the screen lost its colour with it on\n");
        failures++;
    }
}

int main(int argc, char **argv) {
    fillGuards();
    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH, false);
    checkGuards("after starting up");
    if (inkInLines(0, 1) != 0) {
        usCheckFail("starting up: a line was drawn before anything was said\n");
        failures++;
    }

    /* One character, and it has to be on the first line: the drawer starts at
     * the top left rather than wherever the console happens to be */
    usScreenPuts("X");
    checkGuards("after one character");
    if (inkInLines(0, 1) == 0) {
        usCheckFail("after one character: nothing on the first line\n");
        failures++;
    }
    if (inkInLines(1, 7) != 0) {
        usCheckFail("after one character: it was not the first line\n");
        failures++;
    }

    /* Exactly the characters a line holds stay on the first line; the next
     * one wraps */
    usScreenClear();
    for (uint32_t i = 0; i < WIDTH / US_FONT_WIDTH; i++) {
        usScreenPutc('X');
    }
    checkGuards("after one full line");
    if (inkInLines(0, 1) == 0 || inkInLines(1, 7) != 0) {
        usCheckFail("after one full line: the wrap came early or late\n");
        failures++;
    }
    usScreenPutc('X');
    if (inkInLines(1, 1) == 0) {
        usCheckFail("after one more character: it did not wrap\n");
        failures++;
    }

    /* Long enough to wrap, and more lines than fit: the drawer has to scroll
     * up rather than run off the bottom. The last line is written without a
     * newline, so that the text it holds is on the bottom line and can be
     * looked for there */
    usScreenClear();
    for (int i = 0; i < 20; i++) {
        usScreenPuts("the quick brown fox jumps over the lazy dog\n");
    }
    usScreenPuts("bottom");
    checkGuards("after more lines than fit");
    checkDrawn("after more lines than fit");
    if (inkInLines(7, 1) == 0) {
        usCheckFail("after more lines than fit: the last line is empty\n");
        failures++;
    }

    if (argc > 1) {
        if (argv[1][0] == 'p') {
            preview(argv[2]);
        } else {
            printLineArt((uint32_t)atoi(argv[1]));
        }
    } else {
        testColour();
        testColourIsScreenIndependent();
        testLevel();
        usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH, false);
    }
    if (failures == 0) {
        return usCheckPassed("all checks passed");
        return 0;
    }
    printf("screen: %d failures\n", failures);
    return 1;
}
