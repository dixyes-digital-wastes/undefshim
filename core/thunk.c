/* Image-local branches and exception stubs */

#include "core/thunk.h"

#define US_MOVZ_OPCODE 0xD2800000U
#define US_MOVK_OPCODE 0xF2800000U
#define US_BR_OPCODE 0xD61F0000U
#define US_STUB_REG 18U

static uint32_t movz(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVZ_OPCODE | (((shift / 16U) & 3U) << 21)
           | ((value & 0xFFFFU) << 5) | (reg & 0x1FU);
}

static uint32_t movk(uint32_t value, unsigned shift, unsigned reg) {
    return US_MOVK_OPCODE | (((shift / 16U) & 3U) << 21)
           | ((value & 0xFFFFU) << 5) | (reg & 0x1FU);
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
 * x18 for EL0, a rebuild from TPIDR_EL1 for the two kernel-mode slots
 */
uint32_t usSlotStubTargetIndex(UsStubSlot slot) {
    return slot == UsStubSlotEL1h ? 11U : slot == UsStubSlotEL0 ? 10U : 9U;
}

uint32_t usSlotStubTailIndex(UsStubSlot slot) {
    return slot == UsStubSlotEL1h ? 18U : 16U;
}

uint32_t usSlotStubTailWords(UsStubSlot slot) {
    return slot == UsStubSlotEL0 ? 3U : 2U;
}

void usEncodeSlotStub(uint32_t *out, uint64_t target, uint32_t tail0,
                      uint32_t tail1, UsStubSlot slot) {
    uint32_t n = 0;
    uint32_t classBranch = 0;
    uint32_t faultBranch = 0;
    uint32_t restore;
    uint32_t targetAt;

    if (slot == UsStubSlotEL1h) {
        /*
         * Everything taken at this slot goes to the payload first
         *
         * This is the slot the payload's own code runs under, so its faults
         * arrive here, and they are not all data aborts: a read of a device
         * answers with an external abort, an instruction fetch that cannot be
         * translated with an instruction abort, and the payload has to be told
         * about any of them, because it is the only thing that knows whether
         * the access was one it asked about. The handler this slot originally
         * held is the fatal one, so looking at the exception first costs
         * nothing: the payload hands it straight back through the tail below
         * when it is not its own
         *
         * So this form is one branch, and the words the other two spend on
         * filters are padding up to the destination, which keeps the layout
         * the same shape for every slot: the destination at its own index, the
         * restore before the tail, the tail last
         */
        faultBranch = n;
        out[n++] = 0;
        while (n < usSlotStubTargetIndex(UsStubSlotEL1h)) {
            out[n++] = US_NOP;
        }
        targetAt = n;
        usEncodeSlotTarget(out + n, target);
        n += US_SLOT_TARGET_WORDS;
        /* An unconditional branch carries its destination in the low 26 bits,
         * unlike the conditional forms whose fields start higher up */
        out[faultBranch] = US_BRANCH_OPCODE
                           | ((targetAt - faultBranch) & 0x03FFFFFFU);
        goto tail;
    }
    if (slot == UsStubSlotEL0) {
        /*
         * User x18 cannot be rebuilt, and this vector's SP_EL1 is the
         * interrupted thread's kernel stack, which the kernel's own entry for
         * it also uses - so the red zone below it is a place to keep one word.
         * The entry reads it back and restores SP before the tail runs
         */
        out[n++] = 0xF81F0FF2U; /* str x18, [sp, #-16]! */
    }
    out[n++] = 0xD5385212U; /* mrs x18, esr_el1 */
    out[n++] = 0xD35AFE52U; /* lsr x18, x18, #26 */
    classBranch = n;
    out[n++] = 0;
    /*
     * Everything undefined comes to the payload, and it is the payload that
     * decides: it reads the instruction itself, and it is the only side that
     * can tell an RCpc load from the acquire load that has replaced one. A
     * filter here would have to accept both - a site that has been replaced
     * still traps on a processor whose caches have not caught up with the
     * write, and what the stub reads then is the acquire load - and getting
     * that wrong sends the exception to the kernel's own handler, which on a
     * kernel address is a bugcheck. Unrelated undefined instructions do reach
     * the payload this way and are handed straight back, which is what the
     * handler did with them anyway
     *
     * The words the filter took are padding, so that every slot keeps the same
     * shape and the destination stays at the index the payload and the boot
     * both know
     */
    while (n < usSlotStubTargetIndex(slot)) {
        out[n++] = US_NOP;
    }
    targetAt = n;
    usEncodeSlotTarget(out + n, target);
    n += US_SLOT_TARGET_WORDS;

tail:

    /*
     * What happens when the exception is not ours. In kernel mode x18 is
     * rebuilt the way the kernel rebuilds it itself, which costs no stack and
     * is the reason an entry whose SP_EL1 is stale cannot fault here; at EL0
     * the word the push put in the red zone comes back and SP with it
     */
    restore = n;
    if (slot == UsStubSlotEL0) {
        out[n++] = 0xF84107F2U; /* ldr x18, [sp], #16 */
    } else {
        out[n++] = 0xD538D092U; /* mrs x18, tpidr_el1 */
        out[n++] = 0x9274CE52U; /* and x18, x18, #~0xfff */
    }
    if (slot == UsStubSlotEL0) {
        /*
         * The first tail word, and so where a handed-back frame branches: put
         * back the interrupted x18 from the word the push left below the SP
         * the entry restored. Both ways of reaching the tail arrive with SP
         * there - the stub's own pop takes it back, and the entry's restore
         * never spent it - so one load serves both
         */
        out[n++] = 0xF85F03F2U; /* ldur x18, [sp, #-16] */
    }
    out[n++] = tail0;
    out[n++] = tail1;
    while (n < US_SLOT_STUB_WORDS) {
        out[n++] = US_NOP;
    }
    /* The two filters exist only in the forms that have them, and the fault
     * branch of the kernel-mode form is patched where it is written */
    /* The kernel-mode form has no class filter: its first word is the branch
     * to the destination, written where it stands */
    if (slot != UsStubSlotEL1h) {
        out[classBranch] = 0xB5000012U | ((restore - classBranch) << 5);
    }
}
