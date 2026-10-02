/*
 * Checks for the thunk that carries a branch out of an image.
 *
 * The encoder is checked by running what it produced, not by comparing it
 * against constants. The instructions are few and their meaning is exactly
 * what the payload needs, so a small interpreter over the three forms is both
 * shorter than a table of expected words and a stronger statement: it says
 * the thunk loads the address it was given and branches to it, whatever the
 * encoding happens to be.
 */

#include <stdio.h>
#include <string.h>

#include "core/thunk.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eq64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-44s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

/* --- enough of an interpreter to run the thunk -------------------------- */

/*
 * The fixed bits of each form. The shift of a movz/movk and the register of
 * any of them are fields, so they are deliberately outside the masks: a mask
 * that included them would only accept the one encoding the test happens to
 * build, which is the opposite of what running the code is for.
 */
#define MASK_MOVZ 0xFF800000U
#define MASK_MOVK 0xFF800000U
#define MASK_BR 0xFFFFFC1FU
#define OP_MOVZ 0xD2800000U
#define OP_MOVK 0xF2800000U
#define OP_BR 0xD61F0000U

/* str x16, [sp, #-16]! as the assembler encodes it, checked against the
 * toolchain rather than written from the manual. */
#define US_STR_PRE 0xF81F0FF0U

#define IMM16(insn) (((insn) >> 5) & 0xFFFFU)
#define RD(insn) ((insn) & 0x1FU)
#define RN(insn) (((insn) >> 5) & 0x1FU)
#define HW(insn) (((insn) >> 21) & 3U)

/*
 * Returns the address the thunk branches to, or 0 when it is not a thunk:
 * either an instruction that is not one of the three forms, or a branch that
 * does not go through a scratch register.
 */
static uint64_t runThunk(const uint32_t *code, size_t words) {
    uint64_t reg = 0;
    uint64_t pushed = 0;
    bool haveReg = false;

    for (size_t i = 0; i < words; i++) {
        uint32_t insn = code[i];
        unsigned shift = HW(insn) * 16U;

        if (insn == US_STR_PRE) {
            /* The push that keeps x16's value for the entry to read back. */
            pushed = 0xCAFE'0000'0000'0000ULL;
            continue;
        }
        if ((insn & MASK_MOVZ) == OP_MOVZ) {
            if (RD(insn) != 16U) {
                return 0;
            }
            reg = (uint64_t)IMM16(insn) << shift;
            haveReg = true;
        } else if ((insn & MASK_MOVK) == OP_MOVK) {
            if (RD(insn) != 16U || !haveReg) {
                return 0;
            }
            reg = (reg & ~((uint64_t)0xFFFFU << shift)) | ((uint64_t)IMM16(insn) << shift);
        } else if ((insn & MASK_BR) == OP_BR) {
            if (RN(insn) != 16U) {
                return 0;
            }
            /* The entry uses what was pushed; a thunk that pushed nothing has
             * destroyed the register the interrupted code was using. */
            return pushed == 0 ? 0 : reg;
        } else {
            return 0;
        }
    }
    return 0;
}

/*
 * The offset a branch encodes, sign extended. The field is 26 bits and counts
 * instructions, so the sign lives in bit 25 and has to be carried up before
 * the scale is applied.
 */
static int64_t branchOffset(uint32_t insn) {
    return (int64_t)((int32_t)(insn << 6) >> 6) * 4;
}

/* --- the thunk ---------------------------------------------------------- */

static void testThunkReachesAnyAddress(void) {
    static const uint64_t addresses[] = {
        0,
        1,
        0xFFFF,
        0x1'0000,
        0x13bc00000ULL,      /* where a pool has landed */
        0xfffff8027ae81000ULL, /* and where it is reached from */
        0xFFFFFFFFFFFFFFFFULL,
        0x0123456789ABCDEFULL,
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        uint32_t thunk[US_THUNK_WORDS];
        char name[64];

        usEncodeThunk(thunk, addresses[i]);
        snprintf(name, sizeof(name), "the thunk reaches 0x%llx",
                 (unsigned long long)addresses[i]);
        eq64(name, runThunk(thunk, US_THUNK_WORDS), addresses[i]);
    }
}

static void testThunkShape(void) {
    uint32_t thunk[US_THUNK_WORDS];

    usEncodeThunk(thunk, 0x13bc00000ULL);

    /* x16 is pushed first, because the code that was interrupted is entitled
     * to it and the entry reads it back from the stack. An earlier version
     * branched through x16 without saving it, which replaced a register of
     * the interrupted code with the payload's address. */
    ok("the first instruction saves x16", thunk[0] == US_STR_PRE);
    /* A movz clears the register, so it has to come next; a br has to be
     * last, or the instructions after it are never reached. */
    ok("then a movz", (thunk[1] & MASK_MOVZ) == OP_MOVZ);
    ok("then three movk", (thunk[2] & MASK_MOVK) == OP_MOVK
                          && (thunk[3] & MASK_MOVK) == OP_MOVK
                          && (thunk[4] & MASK_MOVK) == OP_MOVK);
    ok("and a br last", (thunk[5] & MASK_BR) == OP_BR);

    /* The shifts are the point of the encoding: two halves at the same place
     * would leave the address short and the error would be a branch to a
     * plausible wrong address. */
    ok("the shifts are 0, 16, 32, 48",
       HW(thunk[1]) == 0 && HW(thunk[2]) == 1 && HW(thunk[3]) == 2 && HW(thunk[4]) == 3);

    /* A one bit address is the case that tells movz from itself: a rule that
     * left the register uninitialised would still pass for zero. */
    {
        uint32_t one[US_THUNK_WORDS];

        usEncodeThunk(one, 1U);
        eq64("a one bit address survives", runThunk(one, US_THUNK_WORDS), 1U);
    }
    ok("and the size is what it says", US_THUNK_BYTES == sizeof(thunk));
}

/* --- the branch --------------------------------------------------------- */

static void testBranchRange(void) {
    uint32_t word;

    ok("a zero branch is b .", usEncodeBranch(0x1000, 0x1000, &word));
    eq64("which encodes to the opcode alone", word, US_BRANCH_OPCODE);

    ok("forward within range", usEncodeBranch(0x1000, 0x2000, &word));
    eq64("one instruction per four bytes",
         (uint64_t)branchOffset(word), (uint64_t)0x1000);

    ok("backward within range", usEncodeBranch(0x2000, 0x1000, &word));
    eq64("and the offset is negative",
         (uint64_t)branchOffset(word), (uint64_t)(int64_t)-0x1000);

    /* The two ends of the reach, and just past them. */
    ok("the positive limit is reachable",
       usEncodeBranch(0, (uint32_t)(US_BRANCH_RANGE - 4), &word));
    eq64("and it decodes back",
         (uint64_t)branchOffset(word), (uint64_t)(US_BRANCH_RANGE - 4));
    ok("just past it is not",
       !usEncodeBranch(0, US_BRANCH_RANGE, &word));
    ok("the negative limit is reachable",
       usEncodeBranch((uint32_t)US_BRANCH_RANGE, 0, &word));
    eq64("and it decodes back too",
         (uint64_t)branchOffset(word), (uint64_t)(int64_t)-(int64_t)US_BRANCH_RANGE);
    ok("just past it is not",
       !usEncodeBranch((uint32_t)US_BRANCH_RANGE + 4, 0, &word));
}

static void testBranchAlignment(void) {
    uint32_t word;

    /* A branch is a word offset, so an unaligned target cannot be expressed.
     * Refusing is the only honest answer: rounding would branch somewhere
     * else and the fault would surface in unrelated code. */
    ok("an unaligned target is refused", !usEncodeBranch(0x1000, 0x1002, &word));
    ok("an unaligned source is refused", !usEncodeBranch(0x1002, 0x1000, &word));
}

int main(void) {
    testThunkReachesAnyAddress();
    testThunkShape();
    testBranchRange();
    testBranchAlignment();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
