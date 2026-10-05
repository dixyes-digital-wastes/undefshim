/*
 * The screen drawer, so far as it can be checked without looking at a screen
 *
 * What this checks is that nothing is written outside the frame buffer,
 * whatever it is asked to draw, and that what it draws lands inside the area
 * it claims
 */

#include <uefi.h>

int printf(const char *format, ...);
int putchar(int c);
int atoi(const char *s);

#include "uefi/font.h"
#include "uefi/screen.h"

/* usScreenInit looks the protocol up; this test sets the frame buffer itself
 * and never calls it, but the symbols have to exist for the linker */
efi_system_table_t *ST;
efi_boot_services_t *BS;

/* Forty characters across and eight lines down: enough for a line to wrap and
 * for more lines than fit, which is what the scrolling is for */
#define WIDTH 320U
#define HEIGHT (US_FONT_HEIGHT * 8U)
/* A guard row before the frame buffer and a guard row after it, so that a
 * write above the top or below the bottom lands somewhere that can be
 * noticed. The frame buffer itself has to be one contiguous block, which is
 * why this is a flat array and not a two dimensional one with a margin: the
 * drawer walks it by the stride it was given */
#define GUARD 0x5A5A5A5AU

static uint32_t arena[(HEIGHT + 2U) * WIDTH];
static uint32_t *frame = &arena[WIDTH];

static int failures;

static void checkGuards(const char *what) {
    for (uint32_t i = 0; i < WIDTH; i++) {
        if (arena[i] != GUARD) {
            printf("FAIL %s: wrote above the frame buffer at column %u\n", what, i);
            failures++;
            return;
        }
        if (arena[(HEIGHT + 1U) * WIDTH + i] != GUARD) {
            printf("FAIL %s: wrote below the frame buffer at column %u\n", what, i);
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
    printf("%s: ink %u, paper %u, other %u\n", what, ink, paper, other);
    if (other != 0) {
        printf("FAIL %s: %u pixels are neither ink nor paper\n", what, other);
        failures++;
    }
    if (ink == 0) {
        printf("FAIL %s: nothing was drawn\n", what);
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

int main(int argc, char **argv) {
    fillGuards();
    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH);
    checkGuards("after starting up");
    if (inkInLines(0, 1) != 0) {
        printf("FAIL starting up: a line was drawn before anything was said\n");
        failures++;
    }

    /* One character, and it has to be on the first line: the drawer starts at
     * the top left rather than wherever the console happens to be */
    usScreenPuts("X");
    checkGuards("after one character");
    if (inkInLines(0, 1) == 0) {
        printf("FAIL after one character: nothing on the first line\n");
        failures++;
    }
    if (inkInLines(1, 7) != 0) {
        printf("FAIL after one character: it was not the first line\n");
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
        printf("FAIL after one full line: the wrap came early or late\n");
        failures++;
    }
    usScreenPutc('X');
    if (inkInLines(1, 1) == 0) {
        printf("FAIL after one more character: it did not wrap\n");
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
        printf("FAIL after more lines than fit: the last line is empty\n");
        failures++;
    }

    if (argc > 1) {
        printLineArt((uint32_t)atoi(argv[1]));
    }
    if (failures == 0) {
        printf("screen: all checks passed\n");
        return 0;
    }
    printf("screen: %d failures\n", failures);
    return 1;
}
