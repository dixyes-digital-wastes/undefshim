/*
 * Encoding a way out of an image, see thunk.h.
 */

#include "core/thunk.h"

/*
 * movz and movk.
 *
 * Both take a sixteen bit immediate and a shift that is a multiple of
 * sixteen, which together are how a wide constant gets into a register
 * without a load. movz clears the rest of the register, so it goes first, and
 * the movk's add the rest.
 */
#define US_MOVZ_OPCODE 0xD2800000U
#define US_MOVK_OPCODE 0xF2800000U
#define US_BR_OPCODE 0xD61F0000U

/* str x16, [sp, #-16]! -- pre-indexed, so the stack grows by sixteen before
 * the store, which is what makes the entry's offsets work out. */
#define US_STR_PRE16_SP_X16 0xF81F0FF0U

/* The register the thunk goes through. x16 and x17 are the platform's
 * intra-procedure-call scratch registers, which is what this is. */
#define US_THUNK_REG 16U

static uint32_t movz(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVZ_OPCODE | (((shift / 16U) & 3U) << 21) | ((value & 0xFFFFU) << 5)
           | (reg & 0x1FU);
}

static uint32_t movk(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVK_OPCODE | (((shift / 16U) & 3U) << 21) | ((value & 0xFFFFU) << 5)
           | (reg & 0x1FU);
}

void usEncodeThunk(uint32_t out[US_THUNK_WORDS], uint64_t target) {
    /* x16 carries the address, so it goes on the interrupted stack first and
     * the entry reads it back from there. */
    out[0] = US_STR_PRE16_SP_X16;
    out[1] = movz((uint32_t)(target & 0xFFFFU), 0, US_THUNK_REG);
    out[2] = movk((uint32_t)((target >> 16) & 0xFFFFU), 16, US_THUNK_REG);
    out[3] = movk((uint32_t)((target >> 32) & 0xFFFFU), 32, US_THUNK_REG);
    out[4] = movk((uint32_t)((target >> 48) & 0xFFFFU), 48, US_THUNK_REG);
    out[5] = US_BR_OPCODE | (US_THUNK_REG << 5);
}

bool usEncodeBranch(uint32_t from, uint32_t to, uint32_t *out) {
    int64_t delta = (int64_t)(int32_t)to - (int64_t)(int32_t)from;

    if (delta < -(int64_t)US_BRANCH_RANGE || delta > (int64_t)(US_BRANCH_RANGE - 4)) {
        return false;
    }
    if ((delta & 3) != 0) {
        return false;
    }
    *out = US_BRANCH_OPCODE | (((uint32_t)(delta >> 2)) & 0x03FFFFFFU);
    return true;
}

/*
 * The stub, instruction by instruction.
 *
 * The encodings are the ones the assembler produces for the same source; they
 * are written here rather than assembled because the payload's address and the
 * tail are the only things that vary, and assembling this at build time would
 * mean carrying a second copy of it into the driver.
 *
 * The two forms differ in one pair of instructions and in where everything
 * else lands, so the emitter counts as it goes instead of the instruction
 * indices being written out. The last version of this had the indices written
 * out by hand and the offsets were right; before that one was wrong by two,
 * and the difference is invisible in the words -- it shows up as a handler
 * that never runs.
 */
#define US_STUB_READ_ESR 0xD5385210U    /* mrs  x16, esr_el1              */
#define US_STUB_EC_SHIFT 0xD35AFE10U    /* lsr  x16, x16, #26             */
#define US_STUB_READ_ELR 0xD5384030U    /* mrs  x16, elr_el1              */
#define US_STUB_LOAD_INS 0xB9400211U    /* ldr  w17, [x16]                */
#define US_STUB_MASK_INS 0x12164E31U    /* and  w17, w17, #0x3ffffc00     */
#define US_STUB_LDAPR_LO 0x52980010U    /* movz w16, #0xc000              */
#define US_STUB_LDAPR_HI 0x72A717F0U    /* movk w16, #0x38bf, lsl #16     */
#define US_STUB_COMPARE 0x6B10023FU     /* cmp  w17, w16                  */
#define US_STUB_JUMP_X16 0xD61F0200U    /* br   x16                       */

/* The saving form's pair, and the third register, which comes before them. */
#define US_STUB_PUSH_X16_X17 0xA9BF47F0U /* stp x16, x17, [sp, #-16]!     */
#define US_STUB_PUSH_X18 0xF81F0FF2U     /* str x18, [sp, #-16]!          */
#define US_STUB_POP_X16_X17 0xA8C147F0U  /* ldp x16, x17, [sp], #16       */
#define US_STUB_POP_X18 0xF84107F2U      /* ldr x18, [sp], #16            */

#define US_STUB_BRANCH_EC 0xB5000000U   /* cbnz x16, <tail>               */
/*
 * b.<cond> <tail>. The condition is in the bottom four bits and it is the
 * whole difference between this and a branch to the tail for every exception:
 * NE sends the ones that are not ours away, and anything else sends the ones
 * that are. Leaving the field at zero makes it EQ, which is the inverse.
 */
#define US_STUB_BRANCH_NE 0x54000000U   /* b.ne <tail>                    */
#define US_STUB_COND_NE 1U

/* A branch's immediate counts instructions from the branch itself. */
static uint32_t branchTo(uint32_t fromIndex, uint32_t toIndex, uint32_t opcode,
                         uint32_t extra) {
    int32_t d = (int32_t)toIndex - (int32_t)fromIndex;

    return opcode | (((uint32_t)d & 0x7FFFFU) << 5) | extra;
}

uint32_t usSlotStubTailIndex(bool save) {
    /*
     * Everything before the first tail word: the save, three to reach the
     * class test, seven for the instruction test, the first restore, five for
     * the address and the branch, and the restore the tail begins with.
     *
     * Both restores are counted because there are two ways out and the
     * registers belong to the kernel on both. Writing only the first leaves
     * the tail running with the stub's values, which is invisible in the
     * words -- that was an earlier version of this.
     */
    return (save ? 2U : 0U) + 3U + 7U + (save ? 2U : 0U) + 5U + (save ? 2U : 0U);
}

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, bool save) {
    uint32_t n = 0;
    uint32_t classBranch;
    uint32_t instructionBranch;
    uint32_t enter;

    if (save) {
        /* On the interrupted stack, which the vector this form is used on
         * guarantees is a stack. The layout is published in the header,
         * because the entry reads these back and the two have to agree. */
        out[n++] = US_STUB_PUSH_X16_X17;
        out[n++] = US_STUB_PUSH_X18;
    }

    out[n++] = US_STUB_READ_ESR;
    out[n++] = US_STUB_EC_SHIFT;
    classBranch = n;
    out[n++] = 0;   /* filled in once the tail's position is known */

    out[n++] = US_STUB_READ_ELR;
    out[n++] = US_STUB_LOAD_INS;
    out[n++] = US_STUB_MASK_INS;
    out[n++] = US_STUB_LDAPR_LO;
    out[n++] = US_STUB_LDAPR_HI;
    out[n++] = US_STUB_COMPARE;
    instructionBranch = n;
    out[n++] = 0;

    if (save) {
        /* The first way out: into the payload. Done before the branch so the
         * payload is entered with the interrupted stack pointer rather than
         * one save area below it -- the entry records that value and returns
         * through it. */
        out[n++] = US_STUB_POP_X18;
        out[n++] = US_STUB_POP_X16_X17;
    }

    out[n++] = movz((uint32_t)(target & 0xFFFFU), 0, US_THUNK_REG);
    out[n++] = movk((uint32_t)((target >> 16) & 0xFFFFU), 16, US_THUNK_REG);
    out[n++] = movk((uint32_t)((target >> 32) & 0xFFFFU), 32, US_THUNK_REG);
    out[n++] = movk((uint32_t)((target >> 48) & 0xFFFFU), 48, US_THUNK_REG);
    out[n++] = US_STUB_JUMP_X16;

    enter = n;
    if (save) {
        /* The second way out: into the slot's own behaviour, which is the
         * kernel's code and is entitled to what was in these registers. It
         * runs before the slot's own words, so it is written before them, and
         * this is where the branches land. */
        out[n++] = US_STUB_POP_X18;
        out[n++] = US_STUB_POP_X16_X17;
    }

    /*
     * The two tail words, at the index usSlotStubTailIndex reports. That
     * function counts the same instructions this emits, and what keeps the two
     * in step is the unit check: it finds the tail words where the index says
     * they are, so a count that drifts fails there rather than in a slot.
     */
    out[n++] = tail0;
    out[n++] = tail1;

    out[classBranch] = branchTo(classBranch, enter, US_STUB_BRANCH_EC,
                                US_THUNK_REG);
    out[instructionBranch] = branchTo(instructionBranch, enter,
                                      US_STUB_BRANCH_NE, US_STUB_COND_NE);
}
