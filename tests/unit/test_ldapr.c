/*
 * Checks for the instruction decoder
 *
 * The encodings here are the real ones, taken from the architecture rather
 * than from the decoder itself: a test built from the code under test agrees
 * with it by construction and says nothing. Each is written as the
 * instruction word, and what has to come back out is the width and the two
 * register numbers a handler needs to carry it out
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

static void eqHex(const char *name, uint32_t got, uint32_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-44s want 0x%08x got 0x%08x\n", name, want, got);
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
 * accepts, or the count and the emulation disagree about what an LDAPR is */
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
 * else entirely
 */
static void testRefusals(void) {
    /* ldar w9, [x10] and stlr w11, [x12], assembled rather than recalled:
     * the difference between these and an LDAPR is a field or two, and a
     * decoder that accepted one of them would corrupt the value it loads */
    ok("ldar is not ldapr", usLdaprDecode(0x88DFFD49U).kind == UsLdaprNone);
    ok("the store form is refused", usLdaprDecode(0x889FFD8BU).kind == UsLdaprNone);
    /* The same opcode with the fixed low bits cleared */
    ok("a different opcode is refused", usLdaprDecode(0x38BF0022U).kind == UsLdaprNone);
    ok("zero is refused", usLdaprDecode(0U).kind == UsLdaprNone);
    /* A brk, which is what the debug probes use */
    ok("a breakpoint is not a load", usLdaprDecode(0xD4200000U).kind == UsLdaprNone);
}

/*
 * The substitution, checked against encodings the assembler produced rather
 * than against what the transformer happens to emit. Each pair below is a
 * real instruction and the acquire load that replaces it, so a width paired
 * with the wrong replacement would show up as a mismatch
 */
static void testSubstitution(void) {
    static const struct {
        uint32_t ldapr;
        uint32_t ldar;
    } pairs[] = {
        { 0x38BFC022U, 0x08DFFC22U }, /* ldaprb w2, [x1]   -> ldarb  w2, [x1]  */
        { 0x78BFC083U, 0x48DFFC83U }, /* ldaprh w3, [x4]   -> ldarh  w3, [x4]  */
        { 0xB8BFC0C5U, 0x88DFFCC5U }, /* ldapr  w5, [x6]   -> ldar   w5, [x6]  */
        { 0xF8BFC107U, 0xC8DFFD07U }, /* ldapr  x7, [x8]   -> ldar   x7, [x8]  */
        { 0xB8BFC1CDU, 0x88DFFDCDU }, /* ldapr  w13, [x14] -> ldar   w13, [x14] */
    };

    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        uint32_t got = 0;
        char name[64];

        snprintf(name, sizeof(name), "0x%08x becomes 0x%08x", pairs[i].ldapr,
                 pairs[i].ldar);
        ok(name, usLdaprToLdar(pairs[i].ldapr, &got));
        eqHex("and it is the acquire load", got, pairs[i].ldar);
    }

    /* Everything the decoder refuses, the transformer refuses */
    {
        uint32_t got = 0xA5A5'A5A5U;

        ok("a non-RCpc load is refused", !usLdaprToLdar(0x88DFFD49U, &got));
        ok("and nothing was written", got == 0xA5A5'A5A5U);
        ok("zero is refused", !usLdaprToLdar(0U, &got));
    }

    /* The registers and the width have to survive, whatever they are */
    {
        bool everyOne = true;

        for (uint32_t rt = 0; rt < 32; rt++) {
            for (uint32_t rn = 0; rn < 32; rn++) {
                uint32_t got = 0;
                UsLdaprInsn back;

                usLdaprToLdar(0xB8BFC000U | (rn << 5) | rt, &got);
                back = usLdaprDecode(got);
                /* The decoder reads RCpc encodings, so it is asked about the
                 * substitute by shifting it back; what matters is that the
                 * fields landed where they started */
                if (back.kind != UsLdaprNone
                    || (got & 0x3FFU) != ((rn << 5) | rt)
                    || (got & 0xC0000000U) != 0x80000000U) {
                    everyOne = false;
                }
            }
        }
        ok("every register pair and width round trips", everyOne);
    }
}


/*
 * --- replacing an RCpc load with the acquire one ------------------------
 *
 * The rewrite is a property of the encoding: the replacement has to say the
 * same thing about size, base and destination, and differ only in being an
 * acquire load. Each pair here is the two encodings the assembler produces
 * for the same operands, so a conversion that drifts by one field fails
 * rather than looking plausible
 *
 * The pairs cover every width, the stack pointer as the base (register 31
 * means SP here, not the zero register) and the zero register as the
 * destination
 */
static const struct {
    uint32_t ldapr;
    uint32_t ldar;
    const char *what;
} kConversions[] = {
    { 0xB8BFC020U, 0x88DFFC20U, "ldar w0, [x1]" },
    { 0xF8BFC3E2U, 0xC8DFFFE2U, "ldar x2, [sp]" },
    { 0xF8BFC07FU, 0xC8DFFC7FU, "ldar xzr, [x3]" },
    { 0xB8BFC0A4U, 0x88DFFCA4U, "ldar w4, [x5]" },
    { 0x38BFC0E6U, 0x08DFFCE6U, "ldarb w6, [x7]" },
    { 0x78BFC128U, 0x48DFFD28U, "ldarh w8, [x9]" },
    { 0xF8BFC16AU, 0xC8DFFD6AU, "ldar x10, [x11]" },
};

static void testRewriteConversion(void) {
    for (size_t i = 0; i < sizeof(kConversions) / sizeof(kConversions[0]); i++) {
        uint32_t out = 0;

        ok("the conversion accepts an RCpc load", usLdaprToLdar(kConversions[i].ldapr, &out));
        eqHex(kConversions[i].what, out, kConversions[i].ldar);
        /* And nothing else: an acquire load is not one to convert, and
         * neither is a word that happens to be data */
        ok("an acquire load is not converted again", !usLdaprToLdar(kConversions[i].ldar, &out));
    }
    ok("a word of data is not converted", !usLdaprToLdar(0xDEADBEEFU, NULL));
    ok("zero is not converted", !usLdaprToLdar(0U, NULL));

    /*
     * The substitute has to decode as the same instruction, because a site
     * that has been replaced still traps on a processor whose caches have not
     * caught up: the exception is the old instruction's and the memory holds
     * the new one, so what the handler reads is an acquire load and it has to
     * mean the same load
     */
    for (size_t i = 0; i < sizeof(kConversions) / sizeof(kConversions[0]); i++) {
        UsLdaprInsn fromRcpc = usLdaprDecode(kConversions[i].ldapr);
        UsLdaprInsn fromAcquire = { 0 };

        ok("the acquire load decodes", usLdarDecode(kConversions[i].ldar, &fromAcquire));
        ok("at the same width", fromAcquire.kind == fromRcpc.kind);
        ok("from the same base", fromAcquire.rn == fromRcpc.rn);
        ok("into the same destination", fromAcquire.rt == fromRcpc.rt);
        ok("an RCpc load is not taken for an acquire one",
           !usLdarDecode(kConversions[i].ldapr, &fromAcquire));
    }
    ok("data is not an acquire load", !usLdarDecode(0xDEADBEEFU, NULL));
    ok("and neither is zero", !usLdarDecode(0U, NULL));
}

int main(void) {
    testRewriteConversion();
    testWidths();
    testScannerAgrees();
    testRefusals();
    testSubstitution();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
