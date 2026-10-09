/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the pool layout
 *
 * The layout is the one thing the driver, the payload and the page table
 * injector all have to agree on, so it is worth checking the arithmetic here
 * rather than discovering it is wrong when the payload takes an exception on a
 * misaligned stack
 */

#include <stdio.h>
#include <string.h>

#include "core/pool.h"

#define US_CHECK_NAME "test_layout"
#include "check.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-34s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

static void testShape(void) {
    /* The pool has to be a whole number of pages, or the firmware cannot hand
     * it over and the stacks would start mid page. It is built for a count, so
     * the check is run at a few of them rather than at one */
    for (uint32_t slots = 1; slots <= US_MAX_CPUS; slots = slots * 2U + 1U) {
        ok("pool bytes are page sized", usPoolBytes(slots) % US_PAGE_SIZE == 0);
        ok("pages match bytes", usPoolPages(slots) * US_PAGE_SIZE == usPoolBytes(slots));
        ok("the header is paid for once",
           usPoolBytes(slots) == US_POOL_HEADER_SIZE + (uint64_t)slots * US_STACK_SIZE);
    }
    ok("header fits in a page", US_POOL_HEADER_SIZE >= sizeof(UsPool));
    ok("header is page sized", US_POOL_HEADER_SIZE % US_PAGE_SIZE == 0);
    ok("stack size is page sized", US_STACK_SIZE % US_PAGE_SIZE == 0);
    ok("stack size is at least a page", US_STACK_SIZE >= US_PAGE_SIZE);

    /* Every stack sits after the header, and they do not overlap */
    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        uint64_t start = US_POOL_STACK_OFFSET(i);
        ok("stack starts after the header", start >= US_POOL_HEADER_SIZE);
        ok("stack ends within the pool",
           start + US_STACK_SIZE <= usPoolBytes(US_MAX_CPUS));
        if (i > 0) {
            ok("stacks do not overlap",
               US_POOL_STACK_OFFSET(i) >= US_POOL_STACK_OFFSET(i - 1) + US_STACK_SIZE);
        }
    }
}

static void testInit(void) {
    /* A deliberately unaligned base: both addresses are page aligned in
     * practice, so anything else is a bug worth refusing */
    UsPool pool;
    ok("rejects an unaligned base",
       !usPoolInitLayout(&pool, 0x1000 + 1, 0x1000 + 1, US_MAX_CPUS));
    ok("rejects a null pool", !usPoolInitLayout(NULL, 0x1000, 0x1000, US_MAX_CPUS));
    ok("rejects no slots", !usPoolInitLayout(&pool, 0x1000, 0x1000, 0));
    ok("rejects more slots than the table holds",
       !usPoolInitLayout(&pool, 0x1000, 0x1000, US_MAX_CPUS + 1));

    ok("accepts an aligned base", usPoolInitLayout(&pool, 0x40000000, 0x40000000,
                                                    US_MAX_CPUS));
    eqU64("magic", pool.magic, US_POOL_MAGIC);
    eqU64("stack slots", pool.stackSlots, US_MAX_CPUS);
    eqU64("self pa", pool.selfPA, 0x40000000);
    eqU64("self va", pool.selfVA, 0x40000000);
    ok("valid after init", usPoolIsValid(&pool));

    /* Alignment is what the CPU requires, not a nicety */
    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        uint64_t top = usPoolStackTop(&pool, i);
        ok("stack top is aligned", (top & (US_STACK_ALIGN - 1)) == 0);
        ok("stack top is above the header", top > 0x40000000 + US_POOL_HEADER_SIZE);
        ok("stack top is within the pool",
           top <= 0x40000000 + usPoolBytes(US_MAX_CPUS));
        eqU64("stack top matches the layout",
              top, 0x40000000 + US_POOL_STACK_OFFSET(i) + US_STACK_SIZE);
    }

    /* Stacks are one per CPU and each has room for at least a page of frame */
    ok("stacks are distinct", usPoolStackTop(&pool, 0) != usPoolStackTop(&pool, 1));
    ok("gap between stacks holds a stack",
       usPoolStackTop(&pool, 1) - usPoolStackTop(&pool, 0) == US_STACK_SIZE);

    ok("out of range slot yields zero", usPoolStackTop(&pool, US_MAX_CPUS) == 0);
    ok("out of range slot is not a pointer", usPoolStackTop(&pool, 999) == 0);

    /*
     * One per processor the machine has rather than per processor it could
     * have: a pool built for four describes four, and the ones past that have
     * no top at all rather than one borrowed from a slot that is not there
     */
    {
        UsPool few;
        ok("a pool can be built for fewer", usPoolInitLayout(&few, 0x50000000, 0x50000000, 4));
        eqU64("and says so", few.stackSlots, 4);
        ok("a smaller pool is valid", usPoolIsValid(&few));
        ok("slots past the count are empty", usPoolStackTop(&few, 4) == 0);
        ok("the last slot it has is there", usPoolStackTop(&few, 3) != 0);
        ok("it costs what it says", usPoolBytes(4) < usPoolBytes(US_MAX_CPUS));
    }
}

static void testValidation(void) {
    UsPool pool;
    ok("setup", usPoolInitLayout(&pool, 0x80000000, 0x80000000, US_MAX_CPUS));

    /* The checks have to actually reject, or they are decoration */
    {
        UsPool bad = pool;
        bad.magic = 0;
        ok("rejects a wrong magic", !usPoolIsValid(&bad));
    }
    {
        UsPool bad = pool;
        bad.selfPA = 0x80000001;
        ok("rejects an unaligned self pa", !usPoolIsValid(&bad));
    }
    {
        UsPool bad = pool;
        bad.stackTop[3] += 8;
        ok("rejects a moved stack top", !usPoolIsValid(&bad));
    }
    {
        UsPool bad = pool;
        bad.stackTop[2] = 0;
        ok("rejects a missing stack top", !usPoolIsValid(&bad));
    }
    {
        UsPool bad = pool;
        bad.stackSlots = US_MAX_CPUS + 1;
        ok("rejects too many slots", !usPoolIsValid(&bad));
    }
    {
        /* The layout follows selfVA, so relocating the pool has to relocate
         * the stacks with it or the payload would jump to the old address */
        UsPool moved = pool;
        uint64_t delta = 0x100000000ULL;
        moved.selfVA += delta;
        ok("a relocated header is inconsistent until stacks follow",
           !usPoolIsValid(&moved));

        ok("relocating both stays consistent",
           usPoolInitLayout(&moved, 0x80000000 + delta, 0x80000000, US_MAX_CPUS)
               && usPoolIsValid(&moved));
        eqU64("relocated stack top",
              usPoolStackTop(&moved, 0), usPoolStackTop(&pool, 0) + delta);
    }

    ok("rejects a null pool", !usPoolIsValid(NULL));
}

/*
 * The two spellings of the processor mask
 *
 * They are different words, deliberately: the one that says which fields are
 * wanted is not a logical immediate an `and` can carry, so the entry takes a
 * generated one that is. What has to hold is that the two agree on every
 * value the registers can produce, and that is a statement about the bits
 * where they differ rather than about the words
 */
static void testMPIDRMask(void) {
    static const uint64_t registers[] = {
        0x0000000080000000ULL,  /* RES1, as the register reads */
        0x0000000080000001ULL,
        0x0000000080000101ULL,
        0x0000000081000203ULL,
        0x00000000C0000000ULL,  /* and with the single processor bit set */
        0x00000000FFFFFFFFULL,
    };

    eqU64("they differ only where the registers are reserved",
          US_MPIDR_AFFINITY_MASK ^ US_MPIDR_AFFINITY_MASK_LOGICAL,
          0xFFFFFF0000000000ULL);

    /*
     * The fields that are not part of a processor's identity have to be gone
     * from both, or two names for one processor compare unequal. That is the
     * whole reason the mask exists
     */
    for (int bit = 24; bit <= 31; bit++) {
        ok("neither keeps a non identity bit",
           ((US_MPIDR_AFFINITY_MASK >> bit) & 1U) == 0
               && ((US_MPIDR_AFFINITY_MASK_LOGICAL >> bit) & 1U) == 0);
    }
    /* And the affinity fields have to survive in both */
    for (int bit = 0; bit < 24; bit++) {
        ok("both keep the low affinity fields",
           ((US_MPIDR_AFFINITY_MASK >> bit) & 1U) == 1
               && ((US_MPIDR_AFFINITY_MASK_LOGICAL >> bit) & 1U) == 1);
    }
    for (int bit = 32; bit <= 39; bit++) {
        ok("both keep the fourth one",
           ((US_MPIDR_AFFINITY_MASK >> bit) & 1U) == 1
               && ((US_MPIDR_AFFINITY_MASK_LOGICAL >> bit) & 1U) == 1);
    }

    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
        eqU64("and they agree on what a register holds",
              registers[i] & US_MPIDR_AFFINITY_MASK,
              registers[i] & US_MPIDR_AFFINITY_MASK_LOGICAL);
    }
}

/*
 * Rounding an address down to its page, at addresses the machine actually uses
 *
 * A payload is placed wherever the firmware's allocator puts it, and on the
 * machines this runs on that is above 4G: the pool was at 0x1380e0000 and the
 * payload at 0x13bc00000 in the run this was written from. A page mask that
 * loses those bits is therefore not a corner case, and it is silent -- the
 * answer is a page-aligned address either way, just four gigabytes lower
 */
static void testPageAlign(void) {
    /* The addresses that were measured on the machine this came from */
    eqU64("a page address is its own page", usPageAlignDown(0x13bc00000ULL),
          0x13bc00000ULL);
    eqU64("an offset inside a page is cleared",
          usPageAlignDown(0x13bc00818ULL), 0x13bc00000ULL);
    eqU64("the pool's page", usPageAlignDown(0x1380e2000ULL), 0x1380e2000ULL);
    eqU64("a kernel address", usPageAlignDown(0xfffff8023a3c1abcULL),
          0xfffff8023a3c1000ULL);
    /* Both ends of the width, so a mask narrower than the address shows up */
    eqU64("the top of the space", usPageAlignDown(0xffffffffffffffffULL),
          0xfffffffffffff000ULL);
    eqU64("zero", usPageAlignDown(0), 0);
    /* And the property rather than the examples: clearing the offset of a page
     * aligned address changes nothing, at any alignment of the address */
    for (uint64_t bits = 12; bits < 64; bits += 7) {
        uint64_t address = (UINT64_C(1) << bits) | 0xfffULL;

        if (usPageAlignDown(address) != (UINT64_C(1) << bits)) {
            failures++;
            checks++;
            usCheckFail("bit %llu does not survive the page mask\n",
                        (unsigned long long)bits);
        }
        checks++;
    }
}

int main(void) {
    testShape();
    testInit();
    testValidation();
    testMPIDRMask();
    testPageAlign();

    return usCheckSummary(checks, failures);
}
