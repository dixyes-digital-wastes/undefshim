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

/* --- the vector slot stub ----------------------------------------------- */

/*
 * The stub has to do two opposite things depending on the exception class,
 * and the whole reason it exists is the second one: an exception that is not
 * an undefined instruction must carry on into whatever the slot used to do.
 */
/*
 * The index of the `br x16` that leaves for the payload. Found rather than
 * counted, because the form changes how many instructions come before it and
 * a number that has to be adjusted by hand is a number that will not be.
 */
static uint32_t brIndex(const uint32_t *stub) {
    for (uint32_t i = 0; i < US_SLOT_STUB_WORDS; i++) {
        if (stub[i] == 0xD61F0200U) {   /* br x16 */
            return i;
        }
    }
    return US_SLOT_STUB_WORDS;
}

static void testSlotStub(void) {
    uint32_t stub[US_SLOT_STUB_WORDS];
    uint64_t target = 0x13bc00bb0ULL;
    uint32_t tail0 = 0xD5384112U;   /* mrs x18, sp_el0 */
    uint32_t tail1 = 0x14000010U;
    uint32_t tailIndex = usSlotStubTailIndex(false);

    usEncodeSlotStub(stub, target, tail0, tail1, false);

    /* Nothing is pushed: at this vector there may be no stack, and a write to
     * the one in SP faults inside the exception it was meant to handle. */
    for (int i = 0; i < (int)US_SLOT_STUB_WORDS; i++) {
        ok("the stub touches no stack", (stub[i] & 0xFFC00000U) != 0xF8000000U);
    }
    ok("it reads ESR_EL1", stub[0] == 0xD5385210U);
    ok("it shifts the class down", stub[1] == 0xD35AFE10U);
    ok("and branches on it", (stub[2] & 0xFF000000U) == 0xB5000000U);

    /* The branch has to land on the tail, and the tail has to be the two
     * words it was given, in order: one before the other means an exception
     * that is not ours runs the branch and then the instruction. */
    {
        int32_t to = (int32_t)(((stub[2] >> 5) & 0x7FFFFU) << 13) >> 13;

        eq64("the class branch lands on the tail", (uint64_t)(2 + to), (uint64_t)tailIndex);
        eq64("the tail's first word is the slot's own", stub[tailIndex], tail0);
        eq64("and the second follows it", stub[tailIndex + 1], tail1);
    }

    /*
     * What the instruction is decides which way the second branch goes, and
     * that makes its condition the whole meaning of the stub. A branch whose
     * condition field is left at zero is `b.eq`: it would send every
     * exception that is ours to the tail, and every one that is not to the
     * payload. That is the inverse, and it looks like nothing in the words.
     */
    ok("the instruction branch is conditional", (stub[9] & 0xFF000010U) == 0x54000000U);
    eq64("and its condition is not-equal", (uint64_t)(stub[9] & 0xFU), 1U);
    {
        int32_t to = (int32_t)(((stub[9] >> 5) & 0x7FFFFU) << 13) >> 13;

        eq64("and it lands on the tail too", (uint64_t)(9 + to), (uint64_t)tailIndex);
    }
    /* The first branch is not conditional at all, so it cannot be confused
     * with the second. */
    ok("the class branch has no condition", (stub[2] & 0xFF000000U) == 0xB5000000U);

    /* The address is built from four halves, in order. */
    {
        uint32_t built[US_THUNK_WORDS];
        uint32_t at = brIndex(stub) - 4;   /* the four halves before the br */

        built[0] = US_STR_PRE;
        for (int i = 0; i < 5; i++) {
            built[1 + i] = stub[at + i];
        }
        eq64("the stub reaches the payload", runThunk(built, US_THUNK_WORDS), target);
    }
}

/*
 * The form that saves the registers, for the vector where a stack is
 * available.
 *
 * What it has to do that the other does not is give them back on BOTH ways
 * out. There are two branches to the tail and one fall-through into the
 * payload, and the registers are the kernel's on all three -- its own
 * breakpoint services are `mov x16, #n; brk`, with the number carried in
 * x16 and read by the handler the tail reaches.
 *
 * An earlier version emitted only one restore, on the fall-through. That
 * leaves the tail path running with the stub's values, which is invisible in
 * the words and shows up as a service number that does not exist.
 */
static void testSlotStubKeepingRegisters(void) {
    uint32_t stub[US_SLOT_STUB_WORDS];
    uint64_t target = 0x13bc00bb0ULL;
    const uint32_t PUSH_PAIR = 0xA9BF47F0U;  /* stp x16, x17, [sp, #-16]! */
    const uint32_t PUSH_X18 = 0xF81F0FF2U;   /* str x18, [sp, #-16]!      */
    const uint32_t POP_X18 = 0xF84107F2U;    /* ldr x18, [sp], #16        */
    const uint32_t POP_PAIR = 0xA8C147F0U;   /* ldp x16, x17, [sp], #16   */
    uint32_t tailIndex = usSlotStubTailIndex(true);
    uint32_t pairPops = 0;
    uint32_t x18Pops = 0;
    uint32_t enter;

    usEncodeSlotStub(stub, target, 0xD5384112U, 0x14000010U, true);

    /* All three of the registers the entry spends, not just two of them: the
     * entry destroys x18 as well, and a register the entry destroys has to
     * come back. */
    eq64("the saving form saves the pair", stub[0], PUSH_PAIR);
    eq64("and the third register", stub[1], PUSH_X18);

    for (uint32_t i = 0; i < US_SLOT_STUB_WORDS; i++) {
        if (stub[i] == POP_PAIR) {
            pairPops++;
        }
        if (stub[i] == POP_X18) {
            x18Pops++;
        }
    }
    eq64("and gives the pair back twice", pairPops, 2U);
    eq64("and the third register twice", x18Pops, 2U);

    /*
     * Both ways out restore, and they restore in the reverse order of the
     * save, because a stack does. Only one of the two was written in an
     * earlier version, and the tail then ran with the stub's values -- which
     * is invisible in the words.
     */
    eq64("the first way out restores the third", stub[12], POP_X18);
    eq64("and then the pair", stub[13], POP_PAIR);
    enter = tailIndex - 2U;
    eq64("the second way out restores the third", stub[enter], POP_X18);
    eq64("and then the pair", stub[enter + 1U], POP_PAIR);

    /* Both branches land on the second restore, not past it. */
    {
        int32_t a = (int32_t)(((stub[4] >> 5) & 0x7FFFFU) << 13) >> 13;
        int32_t b = (int32_t)(((stub[11] >> 5) & 0x7FFFFU) << 13) >> 13;

        eq64("the class branch lands on it", (uint64_t)(4 + a), (uint64_t)enter);
        eq64("the instruction branch lands on it", (uint64_t)(11 + b),
             (uint64_t)enter);
    }
    eq64("the tail is the slot's own words", stub[tailIndex], 0xD5384112U);
    eq64("and the word after it", stub[tailIndex + 1], 0x14000010U);

    /* The address still reaches the payload, which is what the fall-through
     * is for. */
    {
        uint32_t built[US_THUNK_WORDS];
        uint32_t at = brIndex(stub) - 4;

        built[0] = US_STR_PRE;
        for (int i = 0; i < 5; i++) {
            built[1 + i] = stub[at + i];
        }
        eq64("and it still reaches the payload", runThunk(built, US_THUNK_WORDS), target);
    }

    /*
     * The layout the entry reads these back through. The two pushes leave
     * x18 lowest, then x16 and x17, and the constants published beside the
     * stub have to say so: an entry reading the wrong slot gets another
     * register's value with nothing to indicate it.
     */
    eq64("the save area is the two pushes", US_STUB_SAVE_BYTES, 32U);
    eq64("x18 is at the bottom", US_STUB_SAVE_X18, 0U);
    eq64("then x16", US_STUB_SAVE_X16, 16U);
    eq64("then x17", US_STUB_SAVE_X17, 24U);

    /* The longer form is the one the array has to fit. */
    ok("the array is sized for the longer form",
       usSlotStubTailIndex(true) + 2U <= US_SLOT_STUB_WORDS);
}

int main(void) {
    testThunkReachesAnyAddress();
    testThunkShape();
    testBranchRange();
    testBranchAlignment();
    testSlotStub();
    testSlotStubKeepingRegisters();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
