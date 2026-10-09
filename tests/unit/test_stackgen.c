/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 */

#include "core/stackgen.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define US_CHECK_NAME "test_stackgen"
#include "check.h"

static int64_t signedImmediate(uint32_t value, unsigned width) {
    return (value & (1U << (width - 1))) != 0
           ? (int64_t)value - (INT64_C(1) << width) : (int64_t)value;
}

/* Decode the actual logical immediate, not the expected affinity mask */
static uint64_t logicalImmediate(uint32_t insn) {
    assert(((insn >> 22) & 1U) == 1);
    unsigned length = ((insn >> 10) & 63U) + 1U;
    unsigned rotation = (insn >> 16) & 63U;
    assert(length < 64);
    uint64_t bits = (UINT64_C(1) << length) - 1;
    return rotation == 0 ? bits : (bits >> rotation) | (bits << (64 - rotation));
}

static bool simulate(const uint32_t *code, uint32_t used, uint32_t codeRVA,
                     uint32_t tableRVA, uint32_t readyRVA, uint32_t haltRVA,
                     const uint64_t *stackTop, uint32_t count, uint64_t mpidr,
                     uint64_t *selected) {
    int64_t pc = codeRVA;
    uint64_t x18 = 0;
    unsigned loads = 0;
    for (unsigned step = 0; step < US_STACK_LOOKUP_WORDS * 2; step++) {
        if (pc == readyRVA) {
            assert(loads == 1);
            *selected = x18;
            return true;
        }
        if (pc == haltRVA) {
            assert(loads == 0);
            return false;
        }
        assert(pc >= codeRVA && pc < (int64_t)codeRVA + used * 4);
        assert(((pc - codeRVA) & 3) == 0);
        uint32_t insn = code[(pc - codeRVA) / 4];
        int64_t next = pc + 4;
        if (insn == 0xD53800B2U) {
            x18 = mpidr;
        } else if ((insn & 0xFFC003FFU) == 0x92400252U) {
            x18 &= logicalImmediate(insn);
        } else if ((insn & 0xFFC003FFU) == 0xD2400252U) {
            x18 ^= logicalImmediate(insn);
        } else if ((insn & 0xFF00001FU) == 0xB5000012U) {
            if (x18 != 0) {
                next = pc + signedImmediate((insn >> 5) & 0x7FFFFU, 19) * 4;
            }
        } else if ((insn & 0xFF00001FU) == 0x58000012U) {
            int64_t address = pc + signedImmediate((insn >> 5) & 0x7FFFFU, 19) * 4;
            assert(address >= tableRVA && address < (int64_t)tableRVA + count * 8);
            assert(((address - tableRVA) & 7) == 0);
            x18 = stackTop[(address - tableRVA) / 8];
            loads++;
        } else if ((insn & 0xFC000000U) == 0x14000000U) {
            next = pc + signedImmediate(insn & 0x03FFFFFFU, 26) * 4;
        } else {
            assert(!"unexpected instruction");
        }
        pc = next;
    }
    assert(!"execution did not terminate");
    return false;
}

static void exercise(const uint64_t *ids, uint32_t count, uint32_t codeRVA,
                     uint32_t tableRVA, uint32_t expectedWords) {
    const uint32_t readyRVA = 0x200000;
    const uint32_t haltRVA = 0x200004;
    uint32_t code[US_STACK_LOOKUP_WORDS];
    uint64_t stackTop[US_MAX_CPUS];
    uint32_t used = usGenerateStackLookup(ids, count, codeRVA, tableRVA,
                                         readyRVA, haltRVA, code);
    assert(used == expectedWords);
    /* The table the lookup goes into, which is the only ceiling that holds at
     * every count: the expected number of words above is exact, and it is what
     * says the encoding is not growing per processor */
    assert(used <= US_STACK_LOOKUP_WORDS);
    for (uint32_t i = used; i < US_STACK_LOOKUP_WORDS; i++) {
        assert(code[i] == 0xD503201FU);
    }
    for (uint32_t i = 0; i < count; i++) {
        stackTop[i] = UINT64_C(0xFFFFF00040000000) + i * US_STACK_SIZE;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint64_t selected = 0;
        assert(simulate(code, used, codeRVA, tableRVA, readyRVA, haltRVA,
                        stackTop, count, ids[i], &selected));
        assert(selected == stackTop[i]);
        assert(simulate(code, used, codeRVA, tableRVA, readyRVA, haltRVA,
                        stackTop, count, ids[i] | ~US_MPIDR_AFFINITY_MASK, &selected));
        assert(selected == stackTop[i]);
    }
    /* Every affinity bit must distinguish an unknown CPU */
    for (uint32_t bit = 0; bit < 40; bit++) {
        uint64_t unknown = ids[0] ^ (UINT64_C(1) << bit);
        if (((UINT64_C(1) << bit) & US_MPIDR_AFFINITY_MASK) == 0) {
            continue;
        }
        bool known = false;
        for (uint32_t i = 0; i < count; i++) {
            known |= unknown == ids[i];
        }
        if (!known) {
            uint64_t selected = UINT64_MAX;
            assert(!simulate(code, used, codeRVA, tableRVA, readyRVA, haltRVA,
                             stackTop, count, unknown, &selected));
            assert(selected == UINT64_MAX);
        }
    }
}

static void rejected(const uint64_t *ids, uint32_t count, uint32_t codeRVA,
                     uint32_t tableRVA, uint32_t readyRVA, uint32_t haltRVA) {
    uint32_t code[US_STACK_LOOKUP_WORDS];
    uint32_t before[US_STACK_LOOKUP_WORDS];
    memset(code, 0xA5, sizeof(code));
    memcpy(before, code, sizeof(code));
    assert(usGenerateStackLookup(ids, count, codeRVA, tableRVA,
                                readyRVA, haltRVA, code) == 0);
    assert(memcmp(before, code, sizeof(code)) == 0);
}

int main(void) {
    const uint64_t twoClusters[] = {0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103};
    exercise(twoClusters, 8, 0x100004, 0x100800, 61);
    const uint64_t sparse[] = {
        UINT64_C(0x8000800080), UINT64_C(0x0100010001), UINT64_C(0xFE00DCBA98),
        UINT64_C(0xFF00FFFFFF), UINT64_C(0x4000000010), UINT64_C(0x0800004000),
        UINT64_C(0x0200000004), UINT64_C(0x0000008000),
    };
    uint32_t sparseWords = 49;
    for (unsigned i = 0; i < 8; i++) {
        sparseWords += (uint32_t)__builtin_popcountll(sparse[i]);
    }
    exercise(sparse, 8, 0x100004, 0x0FF800, sparseWords);
    /*
     * The sparse shape at every count it has ids for, which is eight: the loop
     * below used to run to US_MAX_CPUS, and that was the same number until the
     * ceiling was raised for a machine with more processors -- at which point
     * it read past the end of this array and took whatever was there for an
     * affinity id
     */
    for (uint32_t count = 1; count <= 8U; count++) {
        uint32_t words = 6U * count + 1U;

        for (uint32_t i = 0; i < count; i++) {
            words += (uint32_t)__builtin_popcountll(sparse[i]);
        }
        exercise(sparse, count, 0x100004, 0x100800, words);
    }
    /*
     * And the most processors this supports, which is the case the ceiling
     * exists for: distinct ids inside the affinity mask, each one the pattern
     * of its own index spread over the low bits. The ids are generated rather
     * than written out because there have to be US_MAX_CPUS of them and they
     * have to stay distinct as that number changes
     */
    uint64_t many[US_MAX_CPUS];
    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        many[i] = 0;
        for (uint32_t bit = 0; bit < 6U; bit++) {
            if ((i & (1U << bit)) != 0) {
                many[i] |= UINT64_C(1) << (2U * bit);
            }
        }
    }
    for (uint32_t count = 1; count <= US_MAX_CPUS; count++) {
        uint32_t words = 6U * count + 1U;

        for (uint32_t i = 0; i < count; i++) {
            words += (uint32_t)__builtin_popcountll(many[i]);
        }
        exercise(many, count, 0x100004, 0x100800, words);
    }
    uint64_t dense[US_MAX_CPUS];
    dense[0] = US_MPIDR_AFFINITY_MASK;
    for (unsigned i = 1; i < US_MAX_CPUS; i++) {
        dense[i] = US_MPIDR_AFFINITY_MASK ^ (UINT64_C(1) << (i - 1));
    }
    exercise(dense, 8, 0x100000, 0x100800, 298);
    const uint64_t zero[] = {0};
    exercise(zero, 1, 0x100004, 0x100800, 7);
    const uint64_t duplicate[] = {0, 0};
    const uint64_t unnormalized[] = {UINT64_C(1) << 24};
    const uint64_t highReserved[] = {UINT64_C(1) << 63};
    rejected(duplicate, 2, 0x100000, 0x100800, 0x200000, 0x200004);
    rejected(unnormalized, 1, 0x100000, 0x100800, 0x200000, 0x200004);
    rejected(highReserved, 1, 0x100000, 0x100800, 0x200000, 0x200004);
    rejected(zero, 0, 0x100000, 0x100800, 0x200000, 0x200004);
    /* One more than the ceiling, named by the ceiling: this was written as 9,
     * which was over the limit while the limit was 8 and is under it now */
    rejected(zero, US_MAX_CPUS + 1U, 0x100000, 0x100800,
             0x200000, 0x200004);
    rejected(NULL, 1, 0x100000, 0x100800, 0x200000, 0x200004);
    assert(usGenerateStackLookup(zero, 1, 0x100000, 0x100800,
                                0x200000, 0x200004, NULL) == 0);
    rejected(zero, 1, 0x100002, 0x100800, 0x200000, 0x200004);
    rejected(zero, 1, 0x100000, 0x100804, 0x200000, 0x200004);
    rejected(zero, 1, 0x100000, 0x100800, 0x200002, 0x200004);
    rejected(zero, 1, 0x100000, 0x100800, 0x200000, 0x200006);
    /* Literal and branch distances, at and beyond signed endpoints */
    uint32_t code[US_STACK_LOOKUP_WORDS];
    assert(usGenerateStackLookup(zero, 1, 0x100000, 0x10,
                                0x100020, 0x100024, code) == 7);
    rejected(zero, 1, 0x100000, 0x08, 0x100020, 0x100024);
    assert(usGenerateStackLookup(zero, 1, 0x100004, 0x200010,
                                0x100020, 0x100024, code) == 7);
    rejected(zero, 1, 0x100004, 0x200018, 0x100020, 0x100024);
    assert(usGenerateStackLookup(zero, 1, 0x08000000, 0x08000800,
                                0x14, 0x18, code) == 7);
    rejected(zero, 1, 0x08000000, 0x08000800, 0x10, 0x18);
    rejected(zero, 1, 0x08000000, 0x08000800, 0x14, 0x14);
    assert(usGenerateStackLookup(zero, 1, 0x100000, 0x100800,
                                0x08100010, 0x08100014, code) == 7);
    rejected(zero, 1, 0x100000, 0x100800, 0x08100014, 0x08100014);
    rejected(zero, 1, 0x100000, 0x100800, 0x08100010, 0x08100018);
    /* A later candidate failure must also leave the whole output unchanged */
    rejected(twoClusters, 8, 0x100000, 0x10, 0x200000, 0x200004);
    rejected(zero, 1, 0xFFFFFFFC, 0xFFFFF000, 0xFFFFF004, 0xFFFFF008);
    rejected(twoClusters, 8, 0xFFFFF000, 0xFFFFFFF8, 0xFFFFF800, 0xFFFFF804);
    rejected(zero, 1, 0xF0000000, 0xF0000800, 0x10000000, 0xF0001000);
    assert(usGenerateStackLookup(zero, 1, 0xF0000004, 0xF0000800,
                                0xF0001000, 0xF0001004, code) == 7);
    assert(usGenerateStackLookup(zero, 1, 0x7FFFFFF0, 0x80000800,
                                0x80001000, 0x80001004, code) == 7);
    /*
     * The last code address the table still fits below. This was written as
     * 0xFFFFFB00, which was that address while the table was 320 words and is
     * past the end of the address space now that it is 1536
     */
    {
        uint32_t lastCode =
            (uint32_t)(UINT64_C(0x100000000)
                       - (uint64_t)US_STACK_LOOKUP_WORDS * 4U);

        assert(usGenerateStackLookup(zero, 1, lastCode, 0xFFFFFFF8,
                                     0xFFFFFFFC, 0xFFFFFA00, code) == 7);
    }
    /* The first ready branch fits, but the next one crosses the lower bound */
    rejected(twoClusters, 8, 0x08000000, 0x08000800, 0x14, 0x08001000);
    return usCheckPassed("native interpreter and input/bounds checks passed");
}
