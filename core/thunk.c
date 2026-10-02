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
 * The two branches to the tail are the assembler's own displacements: the
 * tail is word 15, so the first moves 13 instructions and the second 6.
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

#define US_STUB_BRANCH_EC 0xB5000000U   /* cbnz x16, <tail>               */
/* b.<cond> <tail>. The condition is in the bottom four bits and it is the
 * whole difference between this and a branch to the tail for every exception:
 * NE sends the ones that are not ours away, and anything else sends the ones
 * that are. Leaving the field at zero makes it EQ, which is the inverse. */
#define US_STUB_BRANCH_NE 0x54000000U   /* b.ne <tail>                    */
#define US_STUB_COND_NE 1U

void usEncodeSlotStub(uint32_t out[US_SLOT_STUB_WORDS], uint64_t target,
                      uint32_t tail0, uint32_t tail1) {
    int32_t toTail = (int32_t)US_SLOT_STUB_CONTINUATION;

    out[0] = US_STUB_READ_ESR;
    out[1] = US_STUB_EC_SHIFT;
    out[2] = US_STUB_BRANCH_EC | (((uint32_t)(toTail - 2) & 0x7FFFFU) << 5) | 16U;

    out[3] = US_STUB_READ_ELR;
    out[4] = US_STUB_LOAD_INS;
    out[5] = US_STUB_MASK_INS;
    out[6] = US_STUB_LDAPR_LO;
    out[7] = US_STUB_LDAPR_HI;
    out[8] = US_STUB_COMPARE;
    out[9] = US_STUB_BRANCH_NE | (((uint32_t)(toTail - 9) & 0x7FFFFU) << 5)
             | US_STUB_COND_NE;

    out[10] = movz((uint32_t)(target & 0xFFFFU), 0, US_THUNK_REG);
    out[11] = movk((uint32_t)((target >> 16) & 0xFFFFU), 16, US_THUNK_REG);
    out[12] = movk((uint32_t)((target >> 32) & 0xFFFFU), 32, US_THUNK_REG);
    out[13] = movk((uint32_t)((target >> 48) & 0xFFFFU), 48, US_THUNK_REG);
    out[14] = US_STUB_JUMP_X16;

    out[US_SLOT_STUB_CONTINUATION] = tail0;
    out[US_SLOT_STUB_CONTINUATION + 1U] = tail1;
}
