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

#include <stdatomic.h>
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
 * The stub decides between two exits using x18 alone. Everything it can be
 * handed is run through a small interpreter, because the decision -- payload
 * or the slot's own tail -- is the behaviour, and the behaviour is what has
 * to be checked rather than the words that encode it.
 */
#define STUB_MRS_ESR   0xD5385212U  /* mrs x18, esr_el1            */
#define STUB_LSR_EC    0xD35AFE52U  /* lsr x18, x18, #26           */
#define STUB_MRS_ELR   0xD5384032U  /* mrs x18, elr_el1            */
#define STUB_LDR_W18   0xB9400252U  /* ldr w18, [x18]              */
#define STUB_UBFX      0xD34A7652U  /* ubfx x18, x18, #10, #20     */
#define STUB_SUB_HI    0xD1438A52U  /* sub x18, x18, #0xe2, lsl 12 */
#define STUB_SUB_LO    0xD13FC252U  /* sub x18, x18, #0xff0        */
#define STUB_PUSH_X18  0xF81F0FF2U  /* str x18, [sp, #-16]!        */
#define STUB_POP_X18   0xF84107F2U  /* ldr x18, [sp], #16          */
#define STUB_MRS_TPIDR 0xD538D092U  /* mrs x18, tpidr_el1          */
#define STUB_AND_TPIDR 0x9274CE52U  /* and x18, x18, #~0xfff       */
#define STUB_CMP_FAULT 0xF100965FU  /* cmp x18, #0x25              */

#define US_EC_DATA_ABORT_SAME_EL (0x25U << 26)

#define IS_CBNZ_X18(insn) (((insn) & 0xFF00001FU) == 0xB5000012U)
#define IS_B_COND(insn) (((insn) & 0xFF000010U) == 0x54000000U)
#define B_COND(insn) ((insn) & 0xFU)

static int32_t branch19(uint32_t insn) {
    return (int32_t)(((insn >> 5) & 0x7FFFFU) << 13) >> 13;
}

/* What the stub did with one exception, as the next code would see it. */
typedef struct StubRun_t {
    bool     reachedPayload;
    uint64_t payloadTarget;
    bool     reachedTail;
    bool     pushed;
} StubRun;

static void runStub(const uint32_t *stub, uint64_t esr, uint32_t insn,
                    StubRun *out) {
    uint64_t x18 = 0xDEAD'0000'BEEFULL;
    bool zero = false;
    size_t pc = 0;

    out->reachedPayload = false;
    out->reachedTail = false;
    out->pushed = false;

    for (;;) {
        uint32_t w = stub[pc];
        unsigned shift = HW(w) * 16U;

        if (w == STUB_PUSH_X18) {
            out->pushed = true;
        } else if (w == STUB_MRS_ESR) {
            x18 = esr;
        } else if (w == STUB_LSR_EC) {
            x18 >>= 26;
        } else if (w == STUB_MRS_ELR) {
            x18 = insn;
        } else if (w == STUB_LDR_W18) {
            x18 &= 0xFFFFFFFFULL;
        } else if (w == STUB_UBFX) {
            x18 = (insn >> 10) & 0xFFFFFULL;
        } else if (w == STUB_SUB_HI) {
            x18 -= 0xE2ULL << 12;
        } else if (w == STUB_SUB_LO) {
            x18 -= 0xFF0ULL;
        } else if (w == STUB_CMP_FAULT) {
            zero = x18 == 0x25ULL;
        } else if (IS_B_COND(w)) {
            if (zero && B_COND(w) == 0U) {
                pc = (size_t)((int32_t)pc + branch19(w));
                continue;
            }
        } else if (IS_CBNZ_X18(w)) {
            if (x18 != 0) {
                pc = (size_t)((int32_t)pc + branch19(w));
                continue;
            }
        } else if ((w & 0xFC000000U) == US_BRANCH_OPCODE) {
            pc = (size_t)((int64_t)pc + branchOffset(w) / 4);
            continue;
        } else if ((w & MASK_MOVZ) == OP_MOVZ && RD(w) == 18U) {
            x18 = (uint64_t)IMM16(w) << shift;
        } else if ((w & MASK_MOVK) == OP_MOVK && RD(w) == 18U) {
            x18 = (x18 & ~((uint64_t)0xFFFFU << shift))
                  | ((uint64_t)IMM16(w) << shift);
        } else if ((w & MASK_BR) == OP_BR && RN(w) == 18U) {
            out->reachedPayload = true;
            out->payloadTarget = x18;
            return;
        } else {
            /* The first word the interpreter does not know is the tail. */
            out->reachedTail = true;
            return;
        }
        pc++;
    }
}

static void testSlotStub(void) {
    uint32_t stub[US_SLOT_STUB_WORDS];
    uint64_t target = 0x13bc00bb0ULL;
    uint32_t tail0 = 0xD5384112U;   /* mrs x18, sp_el0 */
    uint32_t tail1 = 0x14000010U;
    uint32_t tailIndex = usSlotStubTailIndex(false);
    StubRun run;

    memset(stub, 0xA5, sizeof(stub));
    usEncodeSlotStub(stub, target, tail0, tail1, false);

    /* No stack word anywhere: at this vector the SP the CPU left is not a
     * stack the interrupted code was using. */
    for (uint32_t i = 0; i < US_SLOT_STUB_WORDS; i++) {
        ok("the stub never pushes but the saving form",
           stub[i] != STUB_PUSH_X18);
    }
    ok("it reads ESR_EL1", stub[0] == STUB_MRS_ESR);
    ok("it shifts the class down", stub[1] == STUB_LSR_EC);
    ok("and branches on it", IS_CBNZ_X18(stub[2]));

    /* Both decisions land on the restore that precedes the tail, and the
     * tail is the two words it was given, in order. */
    eq64("the tail is where the index says", (uint64_t)tailIndex, 16U);
    eq64("the class branch lands on the restore",
         (uint64_t)(2 + branch19(stub[2])), (uint64_t)(tailIndex - 2U));
    ok("the instruction branch is conditional", IS_CBNZ_X18(stub[8]));
    eq64("and it lands on the restore too",
         (uint64_t)(8 + branch19(stub[8])), (uint64_t)(tailIndex - 2U));
    eq64("the tail's first word is the slot's own", stub[tailIndex], tail0);
    eq64("and the second follows it", stub[tailIndex + 1], tail1);

    /* x18 is rebuilt from TPIDR on the way to the tail, never popped. */
    eq64("the tail path rebuilds x18", stub[tailIndex - 2U], STUB_MRS_TPIDR);
    eq64("masked to its page", stub[tailIndex - 1U], STUB_AND_TPIDR);

    /* The behaviour: an RCpc load leaves for the payload, anything else --
     * another undefined instruction, or another class entirely -- goes to
     * the tail. */
    runStub(stub, 0, 0xF8BFC22AU /* ldapr x10, [x17] */, &run);
    ok("an RCpc load reaches the payload", run.reachedPayload);
    eq64("at the address it was given", run.payloadTarget, target);

    runStub(stub, 0, 0x00000000U /* an undefined word that is not ours */, &run);
    ok("another undefined instruction reaches the tail", run.reachedTail);
    ok("without a push", !run.pushed);

    runStub(stub, 0x3CULL << 26, 0, &run);
    ok("a breakpoint reaches the tail too", run.reachedTail);

    /* A data abort is the payload's own only where the payload runs; this
     * slot is not that one, so it stays the kernel's. */
    runStub(stub, US_EC_DATA_ABORT_SAME_EL, 0, &run);
    ok("a data abort from this slot reaches the tail", run.reachedTail);
}

/*
 * The saving form, for the one vector with a stack to spend a word on: EL1h.
 * Its single job the other form cannot do is carry x18 through the filter,
 * which destroys it, and give it back on the way to the tail.
 */
static void testSlotStubKeepingRegisters(void) {
    uint32_t stub[US_SLOT_STUB_WORDS];
    uint64_t target = 0x13bc00bb0ULL;
    uint32_t tailIndex = usSlotStubTailIndex(true);
    uint32_t targetIndex = usSlotStubTargetIndex(true);
    uint32_t classAt = 4U;
    uint32_t faultAt = 3U;
    uint32_t instructionAt = 10U;
    StubRun run;

    memset(stub, 0xA5, sizeof(stub));
    usEncodeSlotStub(stub, target, 0xD5384112U, 0x14000010U, true);

    /* This form is reached at the EL1h vector, where an entry can arrive with
     * a stale SP_EL1 - the kernel's own EL1t handler runs there for its first
     * instructions. Touching memory on the way in would fault there, with the
     * faulting store's address unchanged, which is an endless loop. */
    for (uint32_t i = 0; i < US_SLOT_STUB_WORDS; i++) {
        ok("the saving form never touches the stack",
           stub[i] != STUB_PUSH_X18 && stub[i] != STUB_POP_X18);
    }
    eq64("the tail is where the index says", (uint64_t)tailIndex, 18U);
    eq64("the destination is too", (uint64_t)targetIndex, 11U);

    /* x18 is rebuilt the way the kernel rebuilds it, on the way to the tail. */
    eq64("the tail path reads TPIDR_EL1", stub[tailIndex - 2U], STUB_MRS_TPIDR);
    eq64("and masks it to its page", stub[tailIndex - 1U], STUB_AND_TPIDR);

    /* The three branches land where the indices say. */
    eq64("the class branch lands on the rebuild",
         (uint64_t)(classAt + branch19(stub[classAt])), (uint64_t)(tailIndex - 2U));
    eq64("so does the instruction branch",
         (uint64_t)(instructionAt + branch19(stub[instructionAt])),
         (uint64_t)(tailIndex - 2U));
    eq64("and the fault branch lands on the destination",
         (uint64_t)(faultAt + branch19(stub[faultAt])), (uint64_t)targetIndex);

    /* The way in still reaches the payload, with x18 spent on the way. */
    runStub(stub, 0, 0xF8BFC22AU, &run);
    ok("an RCpc load still reaches the payload", run.reachedPayload);
    eq64("at the address it was given", run.payloadTarget, target);

    /* And so does a fault of the payload's own: this is the slot its code
     * runs under, so a data abort taken here is the payload's to explain. */
    runStub(stub, US_EC_DATA_ABORT_SAME_EL, 0xF8BFC22AU, &run);
    ok("a data abort reaches the payload from this slot", run.reachedPayload);
    eq64("with the same destination", run.payloadTarget, target);

    runStub(stub, 0, 0x00000000U, &run);
    ok("another undefined instruction reaches the tail", run.reachedTail);

    runStub(stub, 0x3CULL << 26, 0, &run);
    ok("another class reaches the tail as well", run.reachedTail);
}

static void testSlotTargetEncoding(void) {
    static const uint64_t targets[] = {
        0, 1, 0x13bc00bb0ULL, 0xfffff8027ae81000ULL,
        0x0123456789ABCDEFULL, 0xFFFFFFFFFFFFFFFFULL,
    };

    eq64("the stub is sized for the saving form", US_SLOT_STUB_WORDS, 20U);
    eq64("the off-path target is five words", US_SLOT_TARGET_WORDS, 5U);
    eq64("the runtime reserves twenty-five words", US_SLOT_RUNTIME_WORDS, 25U);
    eq64("the runtime size is a hundred bytes", US_SLOT_RUNTIME_BYTES, 100U);

    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        uint32_t target[US_SLOT_TARGET_WORDS + 1U];
        StubRun run;

        target[US_SLOT_TARGET_WORDS] = 0xA5A5A5A5U;
        usEncodeSlotTarget(target, targets[t]);
        eq64("target encoding stays within five words",
             target[US_SLOT_TARGET_WORDS], 0xA5A5A5A5U);
        eq64("the target byte size matches", US_SLOT_TARGET_BYTES,
             US_SLOT_TARGET_WORDS * sizeof(uint32_t));
        for (unsigned i = 0; i < 4U; i++) {
            eq64("target immediate and shift encode x18", target[i],
                 (i == 0 ? OP_MOVZ : OP_MOVK) | (i << 21)
                 | ((uint32_t)((targets[t] >> (16U * i)) & 0xFFFFU) << 5)
                 | 18U);
        }
        eq64("the target ends with BR x18", target[4], OP_BR | (18U << 5));
        runStub(target, 0, 0, &run);
        ok("the standalone target reaches its destination", run.reachedPayload);
        eq64("including every high-VA half", run.payloadTarget, targets[t]);

        for (unsigned saving = 0; saving < 2U; saving++) {
            bool save = saving != 0;
            uint32_t stub[US_SLOT_RUNTIME_WORDS];
            uint32_t index = usSlotStubTargetIndex(save);

            memset(stub, 0xA5, sizeof(stub));
            usEncodeSlotStub(stub, targets[t], 0xD5384112U, 0x14000010U, save);
            eq64("the target starts after the filter", index, 9U + 2U * saving);
            ok("the stub uses the shared target encoding",
               memcmp(stub + index, target, US_SLOT_TARGET_BYTES) == 0);
            for (unsigned i = US_SLOT_STUB_WORDS; i < US_SLOT_RUNTIME_WORDS; i++) {
                eq64("encoding leaves off-path space untouched",
                     stub[i], 0xA5A5A5A5U);
            }
            runStub(stub, 0, 0xF8BFC22AU, &run);
            ok("the stub still reaches the payload", run.reachedPayload);
            eq64("the stub also encodes high VA", run.payloadTarget, targets[t]);
            ok("no form touches the stack", !run.pushed);
        }
    }
}

/* Host model only: runtime cache maintenance is outside the encoder. */
static void runPublishedStub(const _Atomic uint32_t *runtime, uint64_t esr,
                             uint32_t insn, StubRun *run) {
    uint32_t snapshot[US_SLOT_RUNTIME_WORDS];

    for (unsigned i = 0; i < US_SLOT_RUNTIME_WORDS; i++) {
        snapshot[i] = atomic_load_explicit(runtime + i, memory_order_acquire);
    }
    runStub(snapshot, esr, insn, run);
}

static void testSlotTargetPublication(void) {
    const uint64_t lowTarget = 0x13bc00bb0ULL;
    const uint64_t highTarget = 0xfffff8027ae81000ULL;

    for (unsigned saving = 0; saving < 2U; saving++) {
        bool save = saving != 0;
        uint32_t original[US_SLOT_STUB_WORDS];
        uint32_t target[US_SLOT_TARGET_WORDS];
        _Atomic uint32_t runtime[US_SLOT_RUNTIME_WORDS];
        uint32_t index = usSlotStubTargetIndex(save);
        uint32_t branch = 0;
        StubRun run;

        usEncodeSlotStub(original, lowTarget, 0xD5384112U, 0x14000010U, save);
        usEncodeSlotTarget(target, highTarget);
        for (unsigned i = 0; i < US_SLOT_RUNTIME_WORDS; i++) {
            atomic_init(runtime + i, i < US_SLOT_STUB_WORDS ? original[i] : US_NOP);
        }
        ok("the publication branch can reach the off-path sequence",
           usEncodeBranch(index * 4U, US_SLOT_STUB_BYTES, &branch));
        eq64("the branch lands exactly after the old tail",
             index * 4U + branchOffset(branch), US_SLOT_STUB_BYTES);

        /* No prefix of the staged sequence is reachable before publication. */
        for (unsigned staged = 0; staged <= US_SLOT_TARGET_WORDS; staged++) {
            if (staged != 0) {
                atomic_store_explicit(runtime + US_SLOT_STUB_WORDS + staged - 1U,
                                      target[staged - 1U], memory_order_relaxed);
            }
            runPublishedStub(runtime, 0, 0xF8BFC22AU, &run);
            ok("staging still enters the low-VA payload", run.reachedPayload);
            eq64("staging cannot expose a partial target", run.payloadTarget, lowTarget);
            ok("staging keeps that true", !run.pushed);
        }

        atomic_store_explicit(runtime + index, branch, memory_order_release);
        runPublishedStub(runtime, 0, 0xF8BFC22AU, &run);
        ok("atomic publication enters the high-VA payload", run.reachedPayload);
        eq64("publication reaches the complete high VA", run.payloadTarget, highTarget);
        ok("publication keeps that true", !run.pushed);
        for (unsigned i = 0; i < US_SLOT_STUB_WORDS; i++) {
            if (i != index) {
                eq64("publication preserves filter, MOVK, restore and tail",
                     atomic_load_explicit(runtime + i, memory_order_relaxed), original[i]);
            }
        }

        runPublishedStub(runtime, 0, 0, &run);
        ok("published stub still rejects unrelated undefined instructions", run.reachedTail);
        ok("the rejected instruction still touches nothing", !run.pushed);
        runPublishedStub(runtime, 0x3CULL << 26, 0xF8BFC22AU, &run);
        ok("published stub still rejects unrelated exception classes", run.reachedTail);
        ok("the rejected class still touches nothing", !run.pushed);
    }
}

int main(void) {
    testThunkReachesAnyAddress();
    testThunkShape();
    testBranchRange();
    testBranchAlignment();
    testSlotStub();
    testSlotStubKeepingRegisters();
    testSlotTargetEncoding();
    testSlotTargetPublication();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
