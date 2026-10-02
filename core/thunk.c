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
