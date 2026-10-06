/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
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

/* A destination of x31 is the zero register: the load is made and the value
 * goes nowhere. There is no value to check, so what this asks is that the
 * handler does not write a register it was told not to */
static __attribute__((noinline)) void loadDropped(const void *p) {
    __asm__ volatile("ldapr wzr, [%0]" ::"r"(p) : "memory");
}

/*
 * A base of x31 is the stack pointer rather than the zero register, and the
 * one register that cannot be reached through a constraint. Written out so
 * the base really is sp: the argument arrives in w0, and putting it where the
 * load can reach it with no index is the whole of the trick - no index is all
 * the RCpc loads allow
 */
extern uint32_t fakeLoadFromStack(uint32_t value);
__asm__(".text\n"
        ".balign 4\n"
        ".globl fakeLoadFromStack\n"
        "fakeLoadFromStack:\n"
        "    sub   sp, sp, #16\n"
        "    str   w0, [sp]\n"
        "    ldapr w0, [sp]\n"
        "    add   sp, sp, #16\n"
        "    ret\n");

/*
 * One site written out, so that the check can read it back
 *
 * A test that passes because the instruction was replaced rather than because
 * the exception was carried out is a test of the other mechanism. This site
 * is not in the exception directory, so no list ever names it and no pass
 * ever replaces it: reading it here says which of the two ran
 */
extern uint32_t fakeSiteWord(const volatile uint32_t *p);
/* The same address, named as the word it is, so the check can read it */
extern const uint32_t fakeSiteWordCode[];
__asm__(".text\n"
        ".balign 4\n"
        ".globl fakeSiteWord\n"
        ".globl fakeSiteWordCode\n"
        "fakeSiteWord:\n"
        "fakeSiteWordCode:\n"
        "    ldapr w0, [x0]\n"
        "    ret\n");

static int failed;

/*
 * The port itself, written to directly
 *
 * The tests run at EL1, and the firmware's console is not reachable from
 * there: it is a set of services that the firmware installed for calls made
 * at the level it is running at. What is left is the port, which is memory,
 * and this is a QEMU machine whose console is the PL011 the driver also opens
 */
#define FAKE_UART 0x09000000ULL
#define FAKE_UART_DR 0x00U
#define FAKE_UART_FR 0x18U
#define FAKE_UART_FR_TXFF 0x20U

static void outChar(char c) {
    volatile uint32_t *uart = (volatile uint32_t *)(uintptr_t)FAKE_UART;

    while ((uart[FAKE_UART_FR / 4] & FAKE_UART_FR_TXFF) != 0) {
    }
    uart[FAKE_UART_DR / 4] = (uint32_t)(uint8_t)c;
}

static void outStr(const char *s) {
    for (; *s != '\0'; s++) {
        outChar(*s);
    }
}

static void outHex(uint64_t value) {
    char digits[16];
    uint32_t n = 0;

    if (value == 0) {
        outChar('0');
        return;
    }
    while (value != 0 && n < sizeof(digits)) {
        uint32_t d = (uint32_t)(value & 0xFU);

        digits[n++] = (char)(d < 10U ? '0' + d : 'a' + (d - 10U));
        value >>= 4;
    }
    while (n > 0) {
        outChar(digits[--n]);
    }
}

static void check(const char *what, uint64_t got, uint64_t want) {
    outStr(got == want ? "FAKEK: " : "FAKEK: ");
    outStr(what);
    if (got == want) {
        outStr(" ok ");
        outHex(got);
        outChar('\n');
        return;
    }
    outStr(" WRONG got ");
    outHex(got);
    outStr(" want ");
    outHex(want);
    outChar('\n');
    failed++;
}

/*
 * Where the EL1 half gets its stack: the one in use now belongs to the level
 * being left, and a return to the other level would find it built over
 */
static uint64_t gEL1Stack[512];

static void el1Main(void);

/*
 * Down to EL1, which is where a kernel runs and therefore where the vectors
 * this project takes over are
 *
 * The firmware here runs at EL2 with VHE, so the EL1&0 translation regime is
 * already the one in force and there is nothing to build: the tables, the
 * memory attributes and the control register are all the ones this code is
 * already using, and nothing has to be copied down
 *
 * What does have to go is TGE. With it set the machine is in the "EL2 as the
 * host" shape, in which execution at EL1 is not permitted at all: an eret
 * with SPSR.M naming EL1 is an illegal exception return, and the firmware's
 * own report is what comes back. Clearing it is what makes EL1 a level this
 * can run at, and it costs nothing here because the mapping EL2 was using is
 * the one EL1 will use
 *
 * SPSR is EL1h with DAIF masked, because the vector entries for anything else
 * in the table are branches to themselves
 */
static void enterEL1(void) {
    uint64_t sp = (uint64_t)(uintptr_t)(gEL1Stack + sizeof(gEL1Stack) / 8);
    uint64_t hcr;

    __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
    hcr &= ~(UINT64_C(1) << 27); /* TGE: let EL1 be a level again */
    __asm__ volatile("msr hcr_el2, %0" ::"r"(hcr));
    __asm__ volatile("isb");

    __asm__ volatile("msr sp_el1, %0" ::"r"(sp));
    __asm__ volatile("msr elr_el2, %0" ::"r"((uint64_t)(uintptr_t)el1Main));
    /* EL1h, D A I F all masked */
    __asm__ volatile("mov x0, #0x3c5\n"
                     "msr spsr_el2, x0" ::: "x0");
    __asm__ volatile("dsb sy\n"
                     "isb\n"
                     "eret");
}

/*
 * A vector table of the fake's own, and a run of zero words beside it
 *
 * The driver finds where to put its stubs by following the value written to
 * VBAR_EL1, and what it finds has to be in this image: the branch it writes
 * into a slot is relative, so the thing it branches to has to move with the
 * image. A real kernel writes its own table here. Every entry is a branch to
 * itself, which is what an unused vector holds and what the driver treats as
 * a slot it may take over
 *
 * The hole is where the stubs go. It is zero words in an executable section
 * because that is what the driver looks for, and it has to be separate from
 * the table: a table of zeroes would be found first and the stubs would land
 * on top of the entries
 *
 * Both are in the image file, not built at run time, because the driver acts
 * when the image is loaded and that is before any of this code runs
 */
#define FAKE_VECTOR_SLOTS 16U
#define FAKE_VECTOR_SLOT_WORDS 32U
#define FAKE_HOLE_WORDS 512U

__attribute__((used, section(".text"), aligned(0x800)))
volatile uint32_t gFakeVectors[FAKE_VECTOR_SLOTS * FAKE_VECTOR_SLOT_WORDS] = {
    [0 ... FAKE_VECTOR_SLOTS * FAKE_VECTOR_SLOT_WORDS - 1] = 0x14000000u,
};

__attribute__((used, section(".text"), aligned(16)))
static uint32_t gStubHole[FAKE_HOLE_WORDS];

/*
 * One of the kernel's own section names, so that this is recognised as what
 * it stands in for
 *
 * The driver tells a kernel from a boot loader by the sections the kernel's
 * linker script emits - INITKDBG, PAGEDATA, ALMOSTRO - and never by a file's
 * name, because a name is not what makes an image a kernel. An image that is
 * to be armed has to carry the same mark, and so does one that a patch list
 * is to be matched against
 */
__attribute__((used, section("INITKDBG")))
static const uint8_t gKernelMark[16] = {
    0x75, 0x6e, 0x64, 0x65, 0x66, 0x73, 0x68, 0x69,   /* "undefshi" */
    0x6d, 0x20, 0x66, 0x61, 0x6b, 0x65, 0x00, 0x00,   /* "m fake" */
};

/*
 * Install it, written out rather than left to the compiler
 *
 * The driver reads the value out of the adrp/add pair that produced it, and
 * it reads the instructions backwards from the msr: anything it cannot place
 * in between - a store, a load - ends the search. Passing the address in a
 * register and letting the compiler arrange the three instructions around it
 * puts a store in there, because this is a call frame like any other. Three
 * adjacent instructions are the only way to say what a kernel says
 */
extern void fakeInstallVectors(void);
__asm__(".text\n"
        ".balign 4\n"
        ".globl fakeInstallVectors\n"
        "fakeInstallVectors:\n"
        "    adrp x8, gFakeVectors\n"
        "    add  x8, x8, :lo12:gFakeVectors\n"
        "    msr  vbar_el1, x8\n"
        "    ret\n");

int main(int argc, char **argv) {
    uint64_t el;

    (void)argc;
    (void)argv;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    el = (el >> 2) & 3U;

    /*
     * The firmware on this machine runs at EL2 with VHE, so an application it
     * starts runs there too, and a trap taken at EL2 is delivered to
     * VBAR_EL2 - the firmware's own table. Every vector this project takes
     * over is an EL1 one, because that is the level a kernel runs at, so a
     * stand-in that never leaves EL2 can only ever exercise the replacement:
     * the exception path is unreachable from it. It is no longer reachable
     * from here either, and that is why this refuses rather than reports a
     * pass it did not earn
     */
    if (el != 2) {
        printf("FAKEK: FAIL expected EL2, entered at EL%lu\n",
               (unsigned long)el);
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    /*
     * VBAR_EL1 while there is still an EL2 to write it from, then the drop
     */
    fakeInstallVectors();
    {
        uint64_t hcr;
        uint64_t vbar;
        uint64_t sctlr;

        __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        printf("FAKEK: el2 up hcr=%lx vbar=%lx sctlr=%lx\n",
               (unsigned long)(hcr & 0xffffffffUL),
               (unsigned long)(vbar & 0xffffffffUL),
               (unsigned long)(sctlr & 0xffffffffUL));
    }
    enterEL1();
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/*
 * The EL1 half, which is the half that matters: a trap taken here goes to
 * VBAR_EL1, which is the table the driver wrote its stubs into
 */
static void el1Main(void) {
    outStr("FAKEK: el1 up\n");

    /*
     * Before anything else: the site written out is still the load it was
     * built as. If it is not, the pass that replaces instructions ran and the
     * rest of this would be reporting on that instead
     */
    check("site untouched", fakeSiteWordCode[0], 0xb8bfc000U);

    check("wide", loadWide((const void *)&gWide), gWide);
    check("word", loadWord((const void *)&gWord), gWord);
    check("half", loadHalf((const void *)&gHalf), gHalf);
    check("byte", loadByte((const void *)&gByte), gByte);
    check("plain site", fakeSiteWord(&gWord), gWord);

    /*
     * What is deliberately not here: the same widths at an address their
     * width does not divide
     *
     * On the hardware this is for, such a load is still an undefined
     * instruction, because the processor does not implement the instruction
     * at all and the encoding is refused before the address is looked at.
     * QEMU implements the RCpc loads as full acquire loads and therefore
     * checks the alignment first, so here the exception is a data abort
     * instead - and a data abort is not something the handler can stand in
     * for, so it hands it back. The case is real and it is covered by the
     * user-mode demo, where the machine decides; it cannot be covered here,
     * and a check that hung on it would be reporting QEMU's choice rather
     * than this project's behaviour
     */

    /* The two registers an encoding can name that are not general ones */
    loadDropped((const void *)&gWide);
    check("dropped", gWide, 0x0123456789abcdefULL);
    check("from stack", fakeLoadFromStack(0x5a5a1234U), 0x5a5a1234U);

    /* Twice, so a site that is only handled on its first exception is still
     * seen to work the second time */
    check("wide again", loadWide((const void *)&gWide), gWide);
    check("word again", loadWord((const void *)&gWord), gWord);

    outStr(failed == 0 ? "FAKEK: PASS\n" : "FAKEK: FAIL\n");

    /*
     * Park rather than return. There is nothing to return to: the level that
     * called this is the one it left. Parking is what the machine's monitor
     * expects - it has already read the line above
     */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
