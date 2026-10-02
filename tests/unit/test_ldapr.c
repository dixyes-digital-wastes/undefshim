/*
 * Checks for the instruction decoder.
 *
 * The encodings here are the real ones, taken from the architecture rather
 * than from the decoder itself: a test built from the code under test agrees
 * with it by construction and says nothing. Each is written as the
 * instruction word, and what has to come back out is the width and the two
 * register numbers a handler needs to carry it out.
 */

#include <stdio.h>

#include "core/ldapr.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eqInt(const char *name, int got, int want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-44s want %d got %d\n", name, want, got);
    }
}

/* --- the four loads ----------------------------------------------------- */

static void testWidths(void) {
    /* ldaprb w2, [x1]   ldaprh w3, [x4]   ldapr w5, [x6]   ldapr x7, [x8] */
    UsLdaprInsn b = usLdaprDecode(0x38BFC022U);
    UsLdaprInsn h = usLdaprDecode(0x78BFC083U);
    UsLdaprInsn w = usLdaprDecode(0xB8BFC0C5U);
    UsLdaprInsn x = usLdaprDecode(0xF8BFC107U);

    eqInt("a byte load is a byte", b.kind, UsLdaprByte);
    eqInt("a halfword load is a halfword", h.kind, UsLdaprHalf);
    eqInt("a word load is a word", w.kind, UsLdaprWord);
    eqInt("an xword load is an xword", x.kind, UsLdaprXword);

    eqInt("byte: destination", b.rt, 2);
    eqInt("byte: base", b.rn, 1);
    eqInt("half: destination", h.rt, 3);
    eqInt("half: base", h.rn, 4);
    eqInt("word: destination", w.rt, 5);
    eqInt("word: base", w.rn, 6);
    eqInt("xword: destination", x.rt, 7);
    eqInt("xword: base", x.rn, 8);
}

/* The patterns the scanner searches for have to be the ones the decoder
 * accepts, or the count and the emulation disagree about what an LDAPR is. */
static void testScannerAgrees(void) {
    uint32_t rt = 0;
    uint32_t rn = 0;

    for (rn = 0; rn < 31; rn++) {
        for (rt = 0; rt < 32; rt++) {
            uint32_t base = (rn << 5) | rt;
            UsLdaprInsn got;

            got = usLdaprDecode(0x38BFC000U | base);
            if (got.kind != UsLdaprByte || got.rn != rn || got.rt != rt) {
                printf("FAIL ldaprb rn=%u rt=%u\n", rn, rt);
                failures++;
                checks++;
                return;
            }
            got = usLdaprDecode(0x78BFC000U | base);
            if (got.kind != UsLdaprHalf || got.rn != rn || got.rt != rt) {
                printf("FAIL ldaprh rn=%u rt=%u\n", rn, rt);
                failures++;
                checks++;
                return;
            }
            got = usLdaprDecode(0xB8BFC000U | base);
            if (got.kind != UsLdaprWord || got.rn != rn || got.rt != rt) {
                printf("FAIL ldapr rn=%u rt=%u\n", rn, rt);
                failures++;
                checks++;
                return;
            }
            got = usLdaprDecode(0xF8BFC000U | base);
            if (got.kind != UsLdaprXword || got.rn != rn || got.rt != rt) {
                printf("FAIL ldapr x rn=%u rt=%u\n", rn, rt);
                failures++;
                checks++;
                return;
            }
        }
    }
    checks += 4;
}

/*
 * Everything else has to be refused. An emulator that claims an instruction
 * it does not understand returns to the wrong place with the wrong value,
 * which is worse than not being there: the fault it produces is somewhere
 * else entirely.
 */
static void testRefusals(void) {
    /* ldar w9, [x10] and stlr w11, [x12], assembled rather than recalled:
     * the difference between these and an LDAPR is a field or two, and a
     * decoder that accepted one of them would corrupt the value it loads. */
    ok("ldar is not ldapr", usLdaprDecode(0x88DFFD49U).kind == UsLdaprNone);
    ok("the store form is refused", usLdaprDecode(0x889FFD8BU).kind == UsLdaprNone);
    /* The same opcode with the fixed low bits cleared. */
    ok("a different opcode is refused", usLdaprDecode(0x38BF0022U).kind == UsLdaprNone);
    ok("zero is refused", usLdaprDecode(0U).kind == UsLdaprNone);
    /* A brk, which is what the debug probes use. */
    ok("a breakpoint is not a load", usLdaprDecode(0xD4200000U).kind == UsLdaprNone);
}

int main(void) {
    testWidths();
    testScannerAgrees();
    testRefusals();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
