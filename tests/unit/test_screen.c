/*
 * The screen drawer, so far as it can be checked without looking at a screen
 *
 * What this checks is that nothing is written outside the frame buffer,
 * whatever it is asked to draw, and that what it draws lands inside the area
 * it claims
 */

#include <uefi.h>

int printf(const char *format, ...);

#include "uefi/font8x16.h"
#include "uefi/screen.h"

/* usScreenInit looks the protocol up; this test sets the frame buffer itself
 * and never calls it, but the symbols have to exist for the linker */
efi_system_table_t *ST;
efi_boot_services_t *BS;

#define WIDTH 320U
#define HEIGHT 256U
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

/* Some ink, and no ink, in the lower half where this driver draws */
static void checkDrawn(const char *what) {
    uint32_t ink = 0;
    uint32_t paper = 0;
    uint32_t other = 0;
    uint32_t oddX = 0;
    uint32_t oddY = 0;

    for (uint32_t y = HEIGHT / 2U; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t v = frame[y * WIDTH + x];

            if (v == 0x00000000U) {
                ink++;
                oddX += x & 1U;
                oddY += y & 1U;
            } else if (v == 0x00FFFFFFU) {
                paper++;
            } else {
                other++;
            }
        }
    }
    printf("%s: ink %u (odd x %u, odd y %u), paper %u, other %u\n", what, ink, oddX,
           oddY, paper, other);
    if (ink == 0) {
        printf("FAIL %s: nothing was drawn\n", what);
        failures++;
    }
}

int main(void) {
    fillGuards();
    usScreenUseFrameBuffer(frame, WIDTH, HEIGHT, WIDTH);
    checkGuards("after starting up");

    /* Long enough to wrap, and more lines than fit: the drawer has to clear
     * rather than run off the bottom */
    for (int i = 0; i < 20; i++) {
        usScreenPuts("the quick brown fox jumps over the lazy dog\n");
    }
    checkGuards("after more lines than fit");
    checkDrawn("after more lines than fit");

    if (failures == 0) {
        printf("screen: all checks passed\n");
        return 0;
    }
    printf("screen: %d failures\n", failures);
    return 1;
}
