/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What runs when an exception arrives, and the state it needs to do it
 *
 * See payload.h for the interface. This file is the C half; the assembly
 * entry that saves the frame and reaches here is entry.S
 */

#include "common/layout.h"
#include "core/ldapr.h"
#include "core/pan.h"
#include "payload/payload.h"
#include "payload/early.h"
#include "payload/rewrite.h"
#include "payload/selfmap.h"
#include "payload/uart.h"
#include "payload/us_mem.h"

/*
 * Written by the boot after it copies the blob. Named without a leading
 * qualifier because the assembly entry addresses it directly, and the two
 * halves of the payload have to agree on where it is
 */
UsPayloadConfig usPayloadConfigBlock;

/*
 * Where the handler stores the frame it was given, so a nested report can
 * find it. Not used for anything else, and not the ABI: the ABI is the
 * pointer passed in
 */
static UsFrame *gCurrentFrame;

/* How many loads have been carried out, kept here rather than in the pool
 * because the pool is what the host reads and this is only a counter */
static uint64_t gEmulated;

/*
 * What each processor is in the middle of
 *
 * A fault taken inside an emulated access is delivered back here by the entry
 * before anything else can run on that processor, and the question it has to
 * answer is which exception it belongs to: the one being handled, standing in
 * for an instruction of the interrupted code. Neither fact can be recovered
 * afterwards - the nested frame is the handler's own, not the interrupted
 * one - so both are kept per processor, indexed the same way the stacks are
 */
typedef struct UsActive_t {
    UsFrame *frame;    /* the exception the handler was given */
    uint64_t address;  /* what the emulated access is about to read */
    bool     loading;  /* whether one is in flight at all */

    /*
     * An access the handler is making that it expects may be refused
     *
     * The rewrite has to try a store that the mapping might not allow, and
     * learning that from a fault is only useful if the fault comes back here
     * instead of being reported as a fault in a handler. While this is set,
     * a fault on exactly this address is the answer "no", and the handler
     * carries on at the instruction after the one that faulted
     *
     * These are volatile because the handler writes them at a moment the
     * compiler cannot see - between the two halves of the call that armed
     * them, with an exception taken in between. Left plain, the value the
     * arming wrote is forwarded to the reading half, and a store the mapping
     * refused comes back as one that happened
     */
    volatile uint64_t probeAt;
    volatile bool     probing;
    volatile bool     probeRefused;
} UsActive;

static UsActive gActive[US_MAX_CPUS];

static uint64_t currentESR(void) {
    uint64_t esr;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    return esr;
}

static uint64_t currentFAR(void) {
    uint64_t far;

    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    return far;
}

static void usSetELR(uint64_t elr) {
    __asm__ volatile("msr elr_el1, %0" ::"r"(elr));
}

static uint64_t currentSPSR(void) {
    uint64_t spsr;

    __asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
    return spsr;
}

static uint64_t currentVBAR(void) {
    uint64_t vbar;

    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    return vbar;
}

static uint64_t currentELR(void) {
    uint64_t elr;

    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    return elr;
}

/* Says that the frame in flight is finished with, so a later fault is not
 * blamed on it. Every return from the handler goes through here */
static void usPayloadLeave(int cpu) {
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].frame = NULL;
        gActive[cpu].loading = false;
    }
}

static uint64_t currentMPIDR(void) {
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & US_MPIDR_AFFINITY_MASK;
}

/*
 * Which processor this is, as an index
 *
 * Looked up rather than taken out of the register's low byte. That byte is
 * Aff0, the core within a cluster, so the first core of every cluster has the
 * same one: two processors would share a landing pad, a stack and a frame,
 * and the damage would appear wherever the other one happened to be running
 *
 * The list comes from the firmware, through the boot. A processor that is not
 * in it has no index, and the caller has to refuse rather than pick one
 */
static int currentCPU(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t mpidr = currentMPIDR();

    for (uint64_t i = 0; i < cfg->cpuCount && i < US_MAX_CPUS; i++) {
        if (cfg->cpus[i].mpidr == mpidr) {
            return (int)cfg->cpus[i].index;
        }
    }
    return -1;
}

/*
 * Says that a fault on this address is an answer rather than a fault
 *
 * Only one access at a time, and only on the processor making it, which is
 * what a nested fault on this path already is. The flag is read by the
 * handler the entry reaches, so nothing here can be kept in a register across
 * the access: the store is volatile for that reason
 */
static bool probeArm(int cpu, uint64_t at) {
    if (cpu < 0 || cpu >= (int)US_MAX_CPUS) {
        return false;
    }
    gActive[cpu].probeAt = at;
    gActive[cpu].probeRefused = false;
    gActive[cpu].probing = true;
    return true;
}

static bool probeDisarm(int cpu) {
    bool refused = gActive[cpu].probeRefused;

    gActive[cpu].probing = false;
    return !refused;
}

/*
 * Reads a word, saying whether the read happened. A read that faults leaves
 * the value alone rather than zeroing it, so a caller that ignores the answer
 * gets an old value rather than a plausible one
 */
bool usPayloadProbeRead(uint64_t at, uint64_t *value) {
    int cpu = currentCPU();
    uint64_t got;

    if (value == NULL || !probeArm(cpu, at)) {
        return false;
    }
    got = *(const volatile uint64_t *)(uintptr_t)at;
    if (!probeDisarm(cpu)) {
        return false;
    }
    *value = got;
    return true;
}

/* Writes a word, saying whether the store happened */
bool usPayloadProbeWrite(uint64_t at, uint64_t value) {
    int cpu = currentCPU();

    if (!probeArm(cpu, at)) {
        return false;
    }
    *(volatile uint64_t *)(uintptr_t)at = value;
    return probeDisarm(cpu);
}

/* The same, one instruction wide: a site is four bytes, and writing eight
 * would take the instruction after it with it */
bool usPayloadProbeWriteWord(uint64_t at, uint32_t value) {
    int cpu = currentCPU();

    if (!probeArm(cpu, at)) {
        return false;
    }
    *(volatile uint32_t *)(uintptr_t)at = value;
    return probeDisarm(cpu);
}

UsPayloadConfig *usPayloadConfig(void) {
    return &usPayloadConfigBlock;
}

void usPayloadStuck(uint64_t kind) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;

    if (cfg->poolBase == 0) {
        return;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    pool->entry.stuck++;
    pool->entry.stuckKind = kind;
    pool->entry.stuckESR = currentESR();
    pool->entry.stuckELR = currentELR();
    pool->entry.stuckSPSR = currentSPSR();
    pool->entry.stuckVBAR = currentVBAR();
}

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

/*
 * Performs the instruction the frame was trapped on
 *
 * Returns false when it is not one of ours, in which case nothing has been
 * changed and the caller has to answer for the exception the same way it
 * would have without us
 */
static bool emulateLDAPR(UsFrame *frame, int cpu) {
    uint32_t insn = *(const volatile uint32_t *)(uintptr_t)frame->elr;
    UsLDAPRInsn decoded = usLDAPRDecode(insn);
    uint64_t address;
    uint64_t value;
    bool pan;
    bool user = (frame->spsr & 0xFU) == 0U;
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

    /* Rn=31 selects the interrupted stack pointer, not the zero register */
    address = decoded.rn == 31 ? frame->sp : frame->x[decoded.rn];

    /*
     * What is about to be read, and that a read is in flight, are recorded
     * before it happens: if it faults, the entry arrives back here with the
     * nested fault's own account and nothing else to say which access it was
     */
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].address = address;
        gActive[cpu].loading = true;
    }

    /*
     * The access is the interrupted instruction's, so it runs with the PAN
     * that instruction was running with: an exception to EL1 sets PAN, and a
     * load of a user page from a region that had cleared it would otherwise
     * come back as a permission fault the hardware would not have raised
     */
    pan = (frame->spsr & US_SPSR_PAN) != 0;
    if (!pan) {
        usPANOff();
    }
    if (user) {
        value = loadUserAcquire(decoded.kind, address);
    } else if (usLDAPRKindAligned(decoded.kind, address)) {
        value = loadAcquire(decoded.kind, address);
    } else {
        value = loadAcquireUnaligned(decoded.kind, address);
    }
    if (!pan) {
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
     * underneath it sets [scan] el0InPlace to false, and then every RCpc load
     * keeps taking the exception, which is slower and always correct
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

/*
 * The frame on the way out, at the same index the way in used
 *
 * Called from every path that has a frame, including the ones that claim
 * nothing, because those hand the registers back too -- to the kernel's own
 * handler rather than to the code that was interrupted, but from the same
 * copy of them
 */
static void recordHandback(UsFrame *frame) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPoolEntry *e;
    UsPoolTrace *t;
    const uint64_t *src;
    uint64_t at;

    if (cfg->poolBase == 0 || frame == NULL) {
        return;
    }
    e = &((UsPool *)(uintptr_t)cfg->poolBase)->entry;
    at = e->entries - 1;
    t = &e->handback[at % US_POOL_TRACE_SLOTS];

    src = (const uint64_t *)(const void *)frame;
    t->cpu = (uint64_t)(int64_t)currentCPU();
    t->mpidr = currentMPIDR();
    for (uint32_t i = 0; i < US_POOL_TRACE_WORDS; i++) {
        t->words[i] = src[i];
    }
}

/*
 * One record per trapping address, so that which sites are still costing an
 * exception can be read out afterwards. It is deliberately dumb: look the
 * address up, count it, and take the first free slot when it is new. A probe
 * would be better, but the trap path is not the place for one
 */
static void usStatsRecord(uint64_t va) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;

    if (cfg->poolBase == 0) {
        return;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    if (pool->stats.magic != US_POOL_STATS_MAGIC) {
        if (pool->stats.magic != 0) {
            return;
        }
        pool->stats.magic = US_POOL_STATS_MAGIC;
    }
    pool->stats.offered++;
    for (uint32_t i = 0; i < US_STATS_SLOTS; i++) {
        if (pool->stats.slots[i].va == va) {
            pool->stats.slots[i].count++;
            pool->stats.recorded++;
            return;
        }
        if (pool->stats.slots[i].va == 0) {
            pool->stats.slots[i].va = va;
            pool->stats.slots[i].count = 1;
            pool->stats.recorded++;
            return;
        }
    }
    pool->stats.overflows++;
}

int usPayloadHandle(UsFrame *frame) {
    if (usPayloadConfig()->statsEnabled != 0) {
        usStatsRecord(frame->elr);
    }
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t vbar;
    uint32_t ec;
    int cpu;

    gCurrentFrame = frame;
    cpu = currentCPU();

    /*
     * The exception in flight, recorded before anything that can fault: a
     * fault here is delivered to the entry, which has to tell it from the
     * handler's own and knows nothing about this frame otherwise
     */
    if (cpu >= 0 && cpu < (int)US_MAX_CPUS) {
        gActive[cpu].frame = frame;
        gActive[cpu].loading = false;
    }

    /*
     * The trace goes down first, and before anything that can fault. It is
     * the only evidence this code ran: the console is unreachable from here,
     * and a fault in the payload would otherwise leave nothing behind but a
     * machine that stopped
     */
    if (cfg->poolBase != 0) {
        UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
        uint64_t at = pool->entry.entries;

        pool->entry.magic = US_POOL_ENTRY_MAGIC;
        pool->entry.entries++;
        pool->entry.lastCPU = (uint64_t)(int64_t)cpu;
        pool->entry.lastSP = frame != NULL ? frame->sp : 0;
        if (frame != NULL) {
            pool->entry.lastESR = frame->esr;
            pool->entry.lastELR = frame->elr;
            pool->entry.lastFAR = frame->far;
        }

        /* Kept apart from the summary so that two exceptions arriving at once
         * read as two events rather than as one that cannot have happened
         *
         * The slot is the entry count modulo the ring, so what is kept is the
         * most recent arrivals. Which slot holds the newest is worked out
         * from the count, so the order survives the wrap */
        if (frame != NULL) {
            UsPoolTrace *t = &pool->entry.trace[at % US_POOL_TRACE_SLOTS];
            const uint64_t *src = (const uint64_t *)(const void *)frame;

            t->cpu = (uint64_t)(int64_t)cpu;
            t->mpidr = currentMPIDR();
            for (uint32_t i = 0; i < US_POOL_TRACE_WORDS; i++) {
                t->words[i] = src[i];
            }
        }
    }

    /*
     * The console is only usable before the kernel has its own page tables.
     * Printing from here would fault on the write and the fault would not be
     * survivable, so the quiet flag is obeyed rather than discovered
     */
    if (cfg->quiet == 0) {
        usUARTInit(cfg->uartBase, (uint32_t)cfg->uartKind, (uint32_t)cfg->uartWidth);
    }

    if (frame == NULL) {
        if (cfg->quiet == 0) {
            usUARTPuts("payload: no-frame\n");
        }
        return 0;
    }

    vbar = currentVBAR();
    usPayloadPublish(vbar);
    if (cfg->poolBase != 0) {
        ((UsPool *)(uintptr_t)cfg->poolBase)->entry.lastInsn =
            *(const volatile uint32_t *)(uintptr_t)frame->elr;
    }

    ec = (uint32_t)US_ESR_EC(frame->esr);
    if (cfg->quiet == 0) {
        usUARTPuts("payload: ec=");
        usUARTPutHex(ec);
        usUARTPuts(" elr=");
        usUARTPutHex(frame->elr);
        usUARTPuts(" far=");
        usUARTPutHex(frame->far);
        usUARTPuts(" cpu=");
        usUARTPutDec((uint64_t)(int64_t)cpu);
        usUARTPuts("\n");
    }

    if (ec == US_EC_UNKNOWN && emulateLDAPR(frame, cpu)) {
        if (cfg->poolBase != 0) {
            ((UsPool *)(uintptr_t)cfg->poolBase)->entry.handled++;
        }
        if (cfg->quiet == 0) {
            usUARTPuts("payload: emulated\n");
        }
        /* Claimed: the entry resumes at the instruction after this one */
        usPayloadLeave(cpu);
        recordHandback(frame);
        return 1;
    }

    /*
     * Everything else is not ours to carry out, and the honest answer is the
     * one that leaves the kernel to deal with it exactly as it would have.
     * That is not a halt: the entry can hand the frame to the handler the
     * slot originally held, with the registers and the exception state as
     * they were, which is indistinguishable from the exception never having
     * been taken by us. The addresses are the only thing that has to be
     * worked out, and the SPSR says which slot it was
     */
    if (cfg->quiet == 0) {
        usUARTPuts("payload: not-ours\n");
    }
    if (usPayloadSlotTail(vbar, frame->spsr, &frame->landing)
        || usPayloadSlotTailCached(vbar, frame->spsr, &frame->landing)
        || usPayloadSlotTailLast(frame->spsr, &frame->landing)) {
        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;

            pool->entry.handedBack++;
            pool->entry.handbackESR = frame->esr;
            pool->entry.handbackELR = frame->elr;
        }
        usPayloadLeave(cpu);
        recordHandback(frame);
        return 2;
    }
    usPayloadLeave(cpu);
    recordHandback(frame);
    return 0;
}

/*
 * The entry on the CPU's own stack, entered when the payload itself faults
 *
 * The frame of the exception being handled is what the kernel has to be
 * given, because the emulated access stands in for one instruction of the
 * interrupted code: a fault on that access is that instruction's fault, and
 * the kernel's page fault handler is what resolves it and retries it
 *
 * What comes back here is the nested fault's own account, which is already
 * the right exception class and status - nothing has to be made up - and the
 * entry it came from, which says which slot's handler is to finish it
 *
 * The answer is the frame to restore; the entry branches to its landing.
 * NULL means there is nothing to restore and the entry stops
 */
UsFrame *usPayloadFault(void) {
    int cpu = currentCPU();
    UsFrame *outer;
    uint64_t vbar;

    /* The access may have cleared PAN; the handler that is about to run
     * would have been entered with it set */
    usPANOn();

    if (cpu < 0 || cpu >= (int)US_MAX_CPUS) {
        return NULL;
    }
    outer = gActive[cpu].frame;
    if (outer == NULL) {
        return NULL;
    }
    vbar = currentVBAR();

    outer->esr = currentESR();
    outer->far = currentFAR();

    /*
     * An access the handler asked to be told about. ELR still names the
     * instruction that faulted, so moving past it is the whole recovery: the
     * entry puts the handler's own registers back and returns to it. What was
     * refused is recorded, and the caller reads that rather than the fault
     */
    if (gActive[cpu].probing && outer->far == gActive[cpu].probeAt) {
        usSetELR(currentELR() + 4);
        gActive[cpu].probeRefused = true;
        gActive[cpu].probing = false;
        return US_PAYLOAD_RESUME;
    }

    if (gActive[cpu].loading && outer->far == gActive[cpu].address) {
        /* The emulated access: the kernel's handler for the slot the
         * exception was taken through is where its instruction's fault
         * belongs, in the context it was interrupted from */
        if (usSlotOfSPSR(outer->spsr) == UsStubSlotEL0) {
            /* The instruction was the user's, so the fault has to read as one
             * the user took; the fault status and the direction are the same
             * either way */
            outer->esr = usESRAsLowerEL(outer->esr);
        }
        gActive[cpu].loading = false;
        if (!usPayloadSlotTail(vbar, outer->spsr, &outer->landing)
            && !usPayloadSlotTailCached(vbar, outer->spsr, &outer->landing)
            && !usPayloadSlotTailLast(outer->spsr, &outer->landing)) {
            return NULL;
        }
    } else {
        /*
         * Something of ours, not the interrupted instruction: that is a fault
         * inside a handler, which the kernel treats as fatal, and it is
         * reported where it happened - at our code - rather than blamed on
         * the instruction the handler was standing in for
         */
        UsPayloadConfig *cfg = usPayloadConfig();
        UsPool *pool;

        outer->elr = currentELR();
        outer->spsr = currentSPSR();
        pool = cfg->poolBase != 0 ? (UsPool *)(uintptr_t)cfg->poolBase : NULL;
        if (pool != NULL) {
            pool->entry.nestedFaults++;
            pool->entry.nestedESR = outer->esr;
            pool->entry.nestedFAR = outer->far;
            pool->entry.nestedELR = outer->elr;
            pool->entry.nestedSelfVA = cfg->selfVA;
            pool->entry.nestedPoolBase = cfg->poolBase;
            pool->entry.nestedStackTop = cfg->stackTop[cpu];
            pool->entry.nestedCPU = (uint64_t)(int64_t)cpu;
        }
        gActive[cpu].loading = false;
        if (!usPayloadSlotTail(vbar, outer->spsr, &outer->landing)) {
            return NULL;
        }
    }
    /* What was handed back, like every other way out of the handler. The
     * frame is finished with: the entry branches away from here and never
     * returns to the handler that owned it */
    usPayloadLeave(cpu);
    recordHandback(outer);
    return outer;
}

/* Only used to prove the blob was copied and is executable before anything
 * depends on it */
void usPayloadSelfTest(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsSelfMap self;

    usUARTInit(cfg->uartBase, (uint32_t)cfg->uartKind, (uint32_t)cfg->uartWidth);
    usUARTPuts("payload: alive cpu=");
    usUARTPutDec((uint64_t)currentCPU());
    usUARTPuts(" frame=");
    usUARTPutDec(sizeof(UsFrame));
    usUARTPuts("\n");

    /*
     * Where the tables say this very code can be reached from
     *
     * There are none here. The firmware runs its own regime, and in this one
     * both translation base registers read as zero: an address is its own
     * physical address and no table describes it. So this reports the answer
     * for that case, and the search itself gets its exercise at the handover,
     * where the kernel's tables are in force and the question actually
     * matters
     */
    self = usSelfMapFind(cfg->selfVA, US_POOL_BYTES, cfg->selfVA);
    usUARTPuts("payload: selfmap ");
    if (self.found) {
        usUARTPuts("va=");
        usUARTPutHex(self.va);
        usUARTPuts(" pa=");
        usUARTPutHex(self.pa);
        usUARTPuts(" size=");
        usUARTPutHex(self.size);
    } else {
        usUARTPuts("none");
    }
    usUARTPuts(" probes=");
    usUARTPutDec(self.probes);
    usUARTPuts(self.exhausted ? " truncated" : " complete");
    usUARTPuts("\n");
}
