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
 * continuation are the only things that vary, and assembling this at build
 * time would mean carrying a second copy of it into the driver.
 */
#define US_STUB_READ_ESR 0xD5385210U    /* mrs  x16, esr_el1       */
#define US_STUB_EC_SHIFT 0xD35AFE10U    /* lsr  x16, x16, #26      */
#define US_STUB_CBNZ_X16 0xB5000000U    /* cbnz x16, <offset>      */
#define US_STUB_JUMP_X16 0xD61F0200U    /* br   x16                */

/* How far the continuation has moved from the slot's first instruction,
 * which is where the original branch was. A relative branch has to be
 * adjusted by that distance to still arrive at the same place. */
#define US_STUB_BRANCH_MOVE ((int32_t)US_STUB_CONTINUATION * 4)

void usEncodeVectorStub(uint32_t out[US_STUB_WORDS], uint64_t target,
                        uint32_t originalBranch) {
    int32_t offset;

    out[0] = US_STUB_READ_ESR;
    out[1] = US_STUB_EC_SHIFT;
    /* Forward to the continuation, which is where the exception class says
     * this is not ours. The offset is in instructions, from this one. */
    offset = (int32_t)US_STUB_CONTINUATION - 2;
    out[2] = US_STUB_CBNZ_X16 | (((uint32_t)offset & 0x7FFFFU) << 5) | (16U << 0);

    out[3] = movz((uint32_t)(target & 0xFFFFU), 0, US_THUNK_REG);
    out[4] = movk((uint32_t)((target >> 16) & 0xFFFFU), 16, US_THUNK_REG);
    out[5] = movk((uint32_t)((target >> 32) & 0xFFFFU), 32, US_THUNK_REG);
    out[6] = movk((uint32_t)((target >> 48) & 0xFFFFU), 48, US_THUNK_REG);
    out[7] = US_STUB_JUMP_X16;

    {
        int32_t displacement = (int32_t)(originalBranch << 6) >> 6;

        if (displacement == 0) {
            /* The slot branched to itself, which is what an unused one holds.
             * Keeping that meaning rather than adjusting it: the place it
             * would now point at is this code. */
            out[US_STUB_CONTINUATION] = US_BRANCH_OPCODE;
        } else {
            int32_t moved = displacement - US_STUB_BRANCH_MOVE / 4;

            out[US_STUB_CONTINUATION] = US_BRANCH_OPCODE
                                        | ((uint32_t)moved & 0x03FFFFFFU);
        }
    }
}
