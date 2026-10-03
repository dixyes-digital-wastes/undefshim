/* Image-local branches and exception stubs */

#include "core/thunk.h"

#define US_MOVZ_OPCODE 0xD2800000U
#define US_MOVK_OPCODE 0xF2800000U
#define US_BR_OPCODE 0xD61F0000U
#define US_STR_PRE16_SP_X16 0xF81F0FF0U
#define US_THUNK_REG 16U
#define US_STUB_REG 18U

static uint32_t movz(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVZ_OPCODE | (((shift / 16U) & 3U) << 21)
           | ((value & 0xFFFFU) << 5) | (reg & 0x1FU);
}

static uint32_t movk(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVK_OPCODE | (((shift / 16U) & 3U) << 21)
           | ((value & 0xFFFFU) << 5) | (reg & 0x1FU);
}

void usEncodeThunk(uint32_t out[US_THUNK_WORDS], uint64_t target) {
    out[0] = US_STR_PRE16_SP_X16;
    out[1] = movz((uint32_t)target, 0, US_THUNK_REG);
    out[2] = movk((uint32_t)(target >> 16), 16, US_THUNK_REG);
    out[3] = movk((uint32_t)(target >> 32), 32, US_THUNK_REG);
    out[4] = movk((uint32_t)(target >> 48), 48, US_THUNK_REG);
    out[5] = US_BR_OPCODE | (US_THUNK_REG << 5);
}

bool usEncodeBranch(uint32_t from, uint32_t to, uint32_t *out) {
    int64_t delta = (int64_t)(int32_t)to - (int64_t)(int32_t)from;

    if (delta < -(int64_t)US_BRANCH_RANGE || delta > (int64_t)(US_BRANCH_RANGE - 4)
        || (delta & 3) != 0) {
        return false;
    }
    *out = US_BRANCH_OPCODE | (((uint32_t)(delta >> 2)) & 0x03FFFFFFU);
    return true;
}

void usEncodeSlotTarget(uint32_t out[US_SLOT_TARGET_WORDS], uint64_t target) {
    out[0] = movz((uint32_t)target, 0, US_STUB_REG);
    out[1] = movk((uint32_t)(target >> 16), 16, US_STUB_REG);
    out[2] = movk((uint32_t)(target >> 32), 32, US_STUB_REG);
    out[3] = movk((uint32_t)(target >> 48), 48, US_STUB_REG);
    out[4] = US_BR_OPCODE | (US_STUB_REG << 5);
}

uint32_t usSlotStubTargetIndex(bool save) {
    return 9U + (save ? 1U : 0U);
}

uint32_t usSlotStubTailIndex(bool save) {
    /* Filter: nine words; destination: five; restore: one or two */
    return (save ? 1U : 0U) + 9U + 5U + (save ? 1U : 2U);
}

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, bool save) {
    uint32_t n = 0;
    uint32_t classBranch;
    uint32_t instructionBranch;
    uint32_t restore;

    if (save) {
        out[n++] = 0xF81F0FF2U; /* str x18, [sp, #-16]! */
    }
    out[n++] = 0xD5385212U; /* mrs x18, esr_el1 */
    out[n++] = 0xD35AFE52U; /* lsr x18, x18, #26 */
    classBranch = n;
    out[n++] = 0;
    out[n++] = 0xD5384032U; /* mrs x18, elr_el1 */
    out[n++] = 0xB9400252U; /* ldr w18, [x18] */
    out[n++] = 0xD34A7652U; /* ubfx x18, x18, #10, #20 */
    out[n++] = 0xD1438A52U; /* sub x18, x18, #0xe2, lsl #12 */
    out[n++] = 0xD13FC252U; /* sub x18, x18, #0xff0 */
    instructionBranch = n;
    out[n++] = 0;

    /* Only x18 is borrowed; the filter leaves live NZCV unchanged */
    usEncodeSlotTarget(out + n, target);
    n += US_SLOT_TARGET_WORDS;

    restore = n;
    if (save) {
        out[n++] = 0xF84107F2U; /* ldr x18, [sp], #16 */
    } else {
        out[n++] = 0xD538D092U; /* mrs x18, tpidr_el1 */
        out[n++] = 0x9274CE52U; /* and x18, x18, #~0xfff */
    }
    out[n++] = tail0;
    out[n++] = tail1;
    while (n < US_SLOT_STUB_WORDS) {
        out[n++] = US_NOP;
    }
    out[classBranch] = 0xB5000012U | ((restore - classBranch) << 5);
    out[instructionBranch] = 0xB5000012U | ((restore - instructionBranch) << 5);
}
