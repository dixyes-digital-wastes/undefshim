/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The RCpc loads, done the way the interrupted code would have run them
 *
 * See emulate.h for the interface. There are three ways to do one, and which
 * is taken is not a preference: the interrupted exception level decides
 * whether the load may be privileged at all, and the address decides whether
 * the acquire form can be used on it
 */

#include "payload/emulate.h"

#include "common/layout.h"
#include "core/ldapr.h"
#include "core/pan.h"
#include "core/translate.h"
#include "payload/payload.h"
#include "payload/rewrite.h"
#include "payload/state.h"

/* How many loads have been carried out, kept here rather than in the pool
 * because the pool is what the host reads and this is only a counter */
static uint64_t gEmulated;

/*
 * Carries out one of the RCpc loads
 *
 * The instruction that faulted is at ELR_EL1, because the exception is taken
 * before it runs. Reading it there and doing what it says is the whole of the
 * emulation
 *
 * The substitutes are the acquire loads rather than ordinary ones. LDAPR only
 * promises release consistency, and these promise more, so a replacement that
 * is at least as strong cannot turn a correct program into an incorrect one.
 * The alternative -- a plain load -- would be weaker, and would be wrong in a
 * way that only shows up under concurrency
 *
 * A destination of 31 is the zero register: the value is read and thrown
 * away, which is what the instruction does, and it is not written anywhere
 */
static uint64_t loadAcquire(UsLDAPRKind kind, uint64_t address) {
    uint64_t value = 0;

    switch (kind) {
    case UsLDAPRByte:
        __asm__ volatile("ldarb %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRHalf:
        __asm__ volatile("ldarh %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRWord:
        __asm__ volatile("ldar %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    default:
        __asm__ volatile("ldar %x0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    }
    return value;
}

/*
 * The same loads for an address the acquire form would refuse
 *
 * What the aligned form adds is single-copy atomicity, and that is what an
 * acquire load is for; LDAPR is weaker and says nothing about it, so the same
 * strength can be had at any alignment from a load that takes one, with the
 * barrier that makes it an acquire behind it. It costs one instruction more,
 * and only the addresses the aligned form cannot reach pay it
 */
static uint64_t loadAcquireUnaligned(UsLDAPRKind kind, uint64_t address) {
    uint64_t value = 0;

    switch (kind) {
    case UsLDAPRHalf:
        __asm__ volatile("ldurh %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRWord:
        __asm__ volatile("ldur %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRXword:
        __asm__ volatile("ldur %x0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    default:
        __asm__ volatile("ldurb %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    }
    __asm__ volatile("dmb ishld" ::: "memory");
    return value;
}

/*
 * The same loads, done as the interrupted code would have been allowed to
 *
 * An EL0 instruction has to be carried out with EL0's own permissions: from
 * EL1 a plain load would succeed on a page the user may not touch and hand the
 * value back, which is the one thing the acceptance criterion for EL0 forbids.
 * The unprivileged loads (LDTR and its widths) make the hardware do that check
 * itself, so a page the user may not read faults here exactly as it would have
 * there - and what faults is a real access, not a guess
 *
 * An acquire load is the strength being stood in for, and the unprivileged
 * form with a barrier behind it is that strength: DMB ISHLD keeps later
 * accesses from being observed before this one, which is what makes a load an
 * acquire. LDAR would say it in one instruction but is privileged, so it would
 * answer the wrong question
 */
static uint64_t loadUserAcquire(UsLDAPRKind kind, uint64_t address) {
    uint64_t value = 0;

    switch (kind) {
    case UsLDAPRByte:
        __asm__ volatile("ldtrb %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRHalf:
        __asm__ volatile("ldtrh %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLDAPRWord:
        __asm__ volatile("ldtr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    default:
        __asm__ volatile("ldtr %x0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    }
    __asm__ volatile("dmb ishld" ::: "memory");
    return value;
}

bool usPayloadEmulate(UsFrame *frame, int cpu) {
    uint64_t pa;
    uint64_t address;
    uint64_t value;
    uint32_t insn;
    UsLDAPRInsn decoded;
    bool pan;
    bool user = (frame->spsr & 0xFU) == 0U;

    /*
     * The instruction comes out of the address the exception names, and that
     * address belongs to the interrupted code. A user page can be taken away
     * between the trap and this read, and a read that faults here arrives as a
     * fault of the payload's own with nothing to say which access it was. So
     * the translation is asked for first. An address that has none is not ours
     * to answer for, and the entry hands the exception to the handler the slot
     * originally held, which is what a page that went away needs
     */
    if (!usTranslateAddress(frame->elr, false, &pa)) {
        return false;
    }
    insn = *(const volatile uint32_t *)(uintptr_t)frame->elr;
    decoded = usLDAPRDecode(insn);
    /*
     * A site that has already been replaced still traps on a processor whose
     * caches have not caught up with the write. The exception is the old
     * instruction's, the memory holds the new one, and what it means is the
     * same load: reading it here is how the two are told apart, and refusing
     * it would hand the kernel an exception it cannot explain
     */
    bool replaced = decoded.kind == UsLDAPRNone;

    if (replaced && !usLDARDecode(insn, &decoded)) {
        return false;
    }
    if (replaced) {
        /* The substitute is in memory and this processor ran the old word:
         * the site is covered, and this is the only path that says so */
        UsPoolTrapCPU *acc = trapAccount(cpu);

        if (acc != NULL) {
            acc->stale++;
        }
    }

    /* Rn=31 selects the interrupted stack pointer, not the zero register */
    address = decoded.rn == 31 ? frame->sp : frame->x[decoded.rn];

    /*
     * What is about to be read, and that a read is in flight, are recorded
     * before it happens: if it faults, the entry arrives back here with the
     * nested fault's own account and nothing else to say which access it was
     */
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].address = address;
        gActive[cpu].loadBytes = usLDAPRKindBytes(decoded.kind);
        gActive[cpu].loading = true;
    }

    /*
     * The access is the interrupted instruction's, so it runs with the PAN
     * that instruction was running with. An entry to EL1 may have replaced
     * that value with its own, which is what SCTLR_EL1.SPAN decides, and the
     * SPSR says what to put back; where the entry left PAN alone there is
     * nothing to do, and putting it back would be the one way to leave the
     * kernel with a PAN it never set - nothing in it writes PAN at all
     */
    pan = (frame->spsr & US_SPSR_PAN) != 0;
    if (usPANSetOnEntry()) {
        if (!pan) {
            usPANOff();
        }
    }
    if (user) {
        value = loadUserAcquire(decoded.kind, address);
    } else if (usLDAPRKindAligned(decoded.kind, address)) {
        value = loadAcquire(decoded.kind, address);
    } else {
        value = loadAcquireUnaligned(decoded.kind, address);
    }
    if (usPANSetOnEntry() && !pan) {
        usPANOn();
    }
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].loading = false;
    }

    if (decoded.rt < 31) {
        frame->x[decoded.rt] = value;
    }

    /*
     * The first one is kept, because an emulator that returns the wrong value
     * does not fail here: it fails wherever that value is used next, and the
     * two are only connectable if this is written down
     */
    if (gEmulated == 0) {
        UsPayloadConfig *cfg = usPayloadConfig();

        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;

            pool->entry.emuInsn = insn;
            pool->entry.emuAddr = address;
            pool->entry.emuValue = value;
            pool->entry.emuELR = frame->elr;
            pool->entry.emuX0 = frame->x[0];
            pool->entry.emuX9 = frame->x[9];
        }
    }

    /* The same, for the last few rather than only the first */
    {
        UsPayloadConfig *cfg = usPayloadConfig();

        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
            UsPoolEmu *e = &pool->entry.emu[gEmulated % US_POOL_EMU_SLOTS];

            e->elr = frame->elr;
            e->insn = insn;
            e->rt = decoded.rt;
            e->address = address;
            e->value = value;
            pool->entry.emuCount = gEmulated + 1;
        }
    }
    gEmulated++;

    /*
     * Only instructions EL0 executed are replaced in place
     *
     * Writing the kernel's own text at run time is what the kernel's
     * integrity check reports as a modified function or .pdata, so the
     * kernel's instructions are patched before it runs, from a list applied
     * at boot, or not at all. Until such a list is in place, the kernel's
     * RCpc loads keep taking the exception
     *
     * Whether even the user-mode replacement is wanted is the configuration's
     * business: a machine that should not have foreign code patched
     * underneath it sets [ldapr] el0InplaceRewrite to false, and then every
     * RCpc load keeps taking the exception, which is slower and always correct
     */
    if (usPayloadConfig()->el0InPlace != 0
        && usSlotOfSPSR(frame->spsr) == UsStubSlotEL0) {
        (void)usRewriteSite(frame->elr, decoded.kind, address);
    }

    /* The value is in the frame, and the frame is what the entry restores, so
     * moving past the instruction is all that is left to do. Doing it here
     * rather than in assembly keeps the two halves of the emulation together:
     * a resume that ran the instruction again would fault again, forever */
    frame->elr += 4;
    return true;
}
