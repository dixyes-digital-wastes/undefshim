/*
 * A stand-in for the kernel
 *
 * The real check of this shim needs Windows, and Windows takes ten minutes
 * and a person looking at a screen to say whether it worked. This does the
 * same job in a few seconds and answers on the serial port: it loads values
 * through the instructions the target's processor cannot execute, and reports
 * whether each one produced what it should have
 *
 * It is named ntoskrnl on purpose. The driver decides what an image is by its
 * name, so naming it this way means the whole kernel path runs: the vectors
 * are armed into it, and the patch lists meant for it are applied to it
 *
 * Every load here is a separate function, and there is more than one form of
 * it, because the shim has to get the width right: a load of a word replaced
 * by a load of a doubleword reads the wrong memory and the value says so
 */

#include <uefi.h>

/* The values the checks expect. Odd numbers, so a truncated load is not the
 * same value by accident */
static volatile uint64_t gWide = 0x0123456789abcdefULL;
static volatile uint32_t gWord = 0xfedcba98U;
static volatile uint16_t gHalf = 0xbeefU;
static volatile uint8_t gByte = 0xa5U;

/* Each of these is one site: the instructions the shim exists for */
static __attribute__((noinline)) uint64_t loadWide(const void *p) {
    uint64_t v;

    __asm__ volatile("ldapr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static __attribute__((noinline)) uint32_t loadWord(const void *p) {
    uint32_t v;

    __asm__ volatile("ldapr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static __attribute__((noinline)) uint16_t loadHalf(const void *p) {
    uint32_t v;

    __asm__ volatile("ldaprh %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return (uint16_t)v;
}

static __attribute__((noinline)) uint8_t loadByte(const void *p) {
    uint32_t v;

    __asm__ volatile("ldaprb %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return (uint8_t)v;
}

static int failed;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) {
        printf("FAKEK: %s ok %lx\n", what, (unsigned long)got);
        return;
    }
    printf("FAKEK: %s WRONG got %lx want %lx\n", what, (unsigned long)got,
           (unsigned long)want);
    failed++;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /*
     * A real kernel installs its vectors by writing VBAR_EL1, and the driver
     * finds where to put its stubs by following exactly that write. Writing
     * the value back is what makes this image armable, and it is the one
     * place where the fake has to imitate the kernel rather than simply use
     * the instructions it cannot execute
     */
    {
        uint64_t vbar;

        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        __asm__ volatile("msr vbar_el1, %0" :: "r"(vbar));
    }

    printf("FAKEK: up\n");
    check("wide", loadWide((const void *)&gWide), gWide);
    check("word", loadWord((const void *)&gWord), gWord);
    check("half", loadHalf((const void *)&gHalf), gHalf);
    check("byte", loadByte((const void *)&gByte), gByte);

    /* Twice, so a site that is only handled on its first exception is still
     * seen to work the second time */
    check("wide again", loadWide((const void *)&gWide), gWide);
    check("word again", loadWord((const void *)&gWord), gWord);

    printf("FAKEK: %s\n", failed == 0 ? "PASS" : "FAIL");

    /*
     * Park rather than return. The shell that started this exits when its
     * script ends, the firmware then finds nothing else to boot and the
     * machine quits - which is fine for the automated check, whose monitor
     * has already seen the line above, but not for a run that is being looked
     * at: a machine that has quit has no screen to look at
     */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
