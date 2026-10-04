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

/*
 * Where the destination and the tail are, counted out for each of the three
 * layouts the encoder writes. They differ by the fault branch, which only the
 * EL1h form carries, and by what happens on the way out: a push and pop of
 * x18 for EL0, a rebuild from TPIDR_EL1 for the two kernel-mode slots.
 */
uint32_t usSlotStubTargetIndex(UsStubSlot slot) {
    return slot == UsStubSlotEl1h ? 11U : slot == UsStubSlotEl0 ? 10U : 9U;
}

uint32_t usSlotStubTailIndex(UsStubSlot slot) {
    return slot == UsStubSlotEl1h ? 18U : 16U;
}

uint32_t usSlotStubTailWords(UsStubSlot slot) {
    return slot == UsStubSlotEl0 ? 3U : 2U;
}

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, UsStubSlot slot) {
    uint32_t n = 0;
    uint32_t classBranch;
    uint32_t faultBranch = 0;
    uint32_t instructionBranch;
    uint32_t restore;
    uint32_t targetAt;

    if (slot == UsStubSlotEl0) {
        /*
         * User x18 cannot be rebuilt, and this vector's SP_EL1 is the
         * interrupted thread's kernel stack, which the kernel's own entry for
         * it also uses - so the red zone below it is a place to keep one word.
         * The entry reads it back and restores SP before the tail runs.
         */
        out[n++] = 0xF81F0FF2U; /* str x18, [sp, #-16]! */
    }
    out[n++] = 0xD5385212U; /* mrs x18, esr_el1 */
    out[n++] = 0xD35AFE52U; /* lsr x18, x18, #26 */
    if (slot == UsStubSlotEl1h) {
        /*
         * A data abort taken at this slot is a fault of the payload's own
         * stack, because this is the slot its code runs under. It is sent to
         * the payload, which knows whether it was the emulated access and
         * answers with what to do about it; the handler this slot originally
         * held is fatal by design and is reached only through the tail below.
         */
        out[n++] = 0xF100965FU; /* cmp x18, #0x25 */
        faultBranch = n;
        out[n++] = 0; /* b.eq <destination> */
    }
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
    targetAt = n;
    usEncodeSlotTarget(out + n, target);
    n += US_SLOT_TARGET_WORDS;

    /*
     * What happens when the exception is not ours. In kernel mode x18 is
     * rebuilt the way the kernel rebuilds it itself, which costs no stack and
     * is the reason an entry whose SP_EL1 is stale cannot fault here; at EL0
     * the word the push put in the red zone comes back and SP with it.
     */
    restore = n;
    if (slot == UsStubSlotEl0) {
        out[n++] = 0xF84107F2U; /* ldr x18, [sp], #16 */
    } else {
        out[n++] = 0xD538D092U; /* mrs x18, tpidr_el1 */
        out[n++] = 0x9274CE52U; /* and x18, x18, #~0xfff */
    }
    if (slot == UsStubSlotEl0) {
        /*
         * The first tail word, and so where a handed-back frame branches: put
         * back the interrupted x18 from the word the push left below the SP
         * the entry restored. Both ways of reaching the tail arrive with SP
         * there - the stub's own pop takes it back, and the entry's restore
         * never spent it - so one load serves both.
         */
        out[n++] = 0xF85F03F2U; /* ldur x18, [sp, #-16] */
    }
    out[n++] = tail0;
    out[n++] = tail1;
    while (n < US_SLOT_STUB_WORDS) {
        out[n++] = US_NOP;
    }
    out[classBranch] = 0xB5000012U | ((restore - classBranch) << 5);
    out[instructionBranch] = 0xB5000012U | ((restore - instructionBranch) << 5);
    if (faultBranch != 0) {
        out[faultBranch] = 0x54000000U | (((targetAt - faultBranch) & 0x7FFFFU) << 5);
    }
}
