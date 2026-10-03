/*
 * What runs when an exception arrives, and the state it needs to do it.
 *
 * See payload.h for the interface. This file is the C half; the assembly
 * entry that saves the frame and reaches here is entry.S.
 */

#include "common/layout.h"
#include "core/ldapr.h"
#include "payload/payload.h"
#include "payload/selfmap.h"
#include "payload/uart.h"
#include "payload/us_mem.h"

/*
 * Written by the boot after it copies the blob. Named without a leading
 * qualifier because the assembly entry addresses it directly, and the two
 * halves of the payload have to agree on where it is.
 */
UsPayloadConfig usPayloadConfigBlock;

/*
 * One frame per CPU, filled by the entry before it switches stacks.
 *
 * The entry cannot use a stack it has not switched to yet, so the frame goes
 * here first. Interrupts are masked at entry and the handler does not
 * recurse, so one pad per CPU is enough, and the index comes from the CPU
 * affinity field rather than an assumption that this is running on the first
 * CPU.
 */
UsFrame usPayloadLanding[US_MAX_CPUS];

/*
 * Where the handler stores the frame it was given, so a nested report can
 * find it. Not used for anything else, and not the ABI: the ABI is the
 * pointer passed in.
 */
static UsFrame *gCurrentFrame;

/* How many loads have been carried out, kept here rather than in the pool
 * because the pool is what the host reads and this is only a counter. */
static uint64_t gEmulated;

static uint64_t currentMpidr(void) {
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & US_MPIDR_AFFINITY_MASK;
}

/*
 * Which processor this is, as an index.
 *
 * Looked up rather than taken out of the register's low byte. That byte is
 * Aff0, the core within a cluster, so the first core of every cluster has the
 * same one: two processors would share a landing pad, a stack and a frame,
 * and the damage would appear wherever the other one happened to be running.
 *
 * The list comes from the firmware, through the boot. A processor that is not
 * in it has no index, and the caller has to refuse rather than pick one.
 */
static int currentCpu(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t mpidr = currentMpidr();

    for (uint64_t i = 0; i < cfg->cpuCount && i < US_MAX_CPUS; i++) {
        if (cfg->cpus[i].mpidr == mpidr) {
            return (int)cfg->cpus[i].index;
        }
    }
    return -1;
}

UsPayloadConfig *usPayloadConfig(void) {
    return &usPayloadConfigBlock;
}

/*
 * The exceptions this is willing to take responsibility for.
 *
 * An undefined instruction is the whole point: on hardware without the
 * extension, every LDAPR is one. A breakpoint is accepted as well because it
 * is how the deployment checks get here without planting anything that would
 * corrupt a running kernel, and because answering "not mine" to a breakpoint
 * is worse than reporting it.
 */
static int isHandledClass(uint32_t ec) {
    return ec == US_EC_UNKNOWN || ec == US_EC_BRK64;
}

/*
 * Carries out one of the RCpc loads.
 *
 * The instruction that faulted is at ELR_EL1, because the exception is taken
 * before it runs. Reading it there and doing what it says is the whole of the
 * emulation.
 *
 * The substitutes are the acquire loads rather than ordinary ones. LDAPR only
 * promises release consistency, and these promise more, so a replacement that
 * is at least as strong cannot turn a correct program into an incorrect one.
 * The alternative -- a plain load -- would be weaker, and would be wrong in a
 * way that only shows up under concurrency.
 *
 * A destination of 31 is the zero register: the value is read and thrown
 * away, which is what the instruction does, and it is not written anywhere.
 */
static uint64_t loadAcquire(UsLdaprKind kind, uint64_t address) {
    uint64_t value = 0;

    switch (kind) {
    case UsLdaprByte:
        __asm__ volatile("ldarb %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLdaprHalf:
        __asm__ volatile("ldarh %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    case UsLdaprWord:
        __asm__ volatile("ldar %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    default:
        __asm__ volatile("ldar %x0, [%1]" : "=r"(value) : "r"(address) : "memory");
        break;
    }
    return value;
}

/*
 * Performs the instruction the frame was trapped on.
 *
 * Returns false when it is not one of ours, in which case nothing has been
 * changed and the caller has to answer for the exception the same way it
 * would have without us.
 */
static bool emulateLdapr(UsFrame *frame) {
    uint32_t insn = *(const volatile uint32_t *)(uintptr_t)frame->elr;
    UsLdaprInsn decoded = usLdaprDecode(insn);
    uint64_t address;
    uint64_t value;

    if (decoded.kind == UsLdaprNone) {
        return false;
    }

    /* A base of 31 would be the zero register, which is not an address this
     * instruction can be given; the encoding is reserved and not decoded. */
    if (decoded.rn == 31) {
        return false;
    }

    address = frame->x[decoded.rn];
    value = loadAcquire(decoded.kind, address);

    if (decoded.rt < 31) {
        frame->x[decoded.rt] = value;
    }

    /*
     * The first one is kept, because an emulator that returns the wrong value
     * does not fail here: it fails wherever that value is used next, and the
     * two are only connectable if this is written down.
     */
    if (gEmulated == 0) {
        UsPayloadConfig *cfg = usPayloadConfig();

        if (cfg->poolBase != 0) {
            UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;

            pool->entry.emuInsn = insn;
            pool->entry.emuAddr = address;
            pool->entry.emuValue = value;
            pool->entry.emuElr = frame->elr;
            pool->entry.emuX0 = frame->x[0];
            pool->entry.emuX9 = frame->x[9];
        }
    }

    /* The same, for the last few rather than only the first. */
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

    /* The value is in the frame, and the frame is what the entry restores, so
     * moving past the instruction is all that is left to do. Doing it here
     * rather than in assembly keeps the two halves of the emulation together:
     * a resume that ran the instruction again would fault again, forever. */
    frame->elr += 4;
    return true;
}

int usPayloadHandle(UsFrame *frame) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint32_t ec;
    int cpu;

    gCurrentFrame = frame;
    cpu = currentCpu();

    /*
     * The trace goes down first, and before anything that can fault. It is
     * the only evidence this code ran: the console is unreachable from here,
     * and a fault in the payload would otherwise leave nothing behind but a
     * machine that stopped.
     */
    if (cfg->poolBase != 0) {
        UsPool *pool = (UsPool *)(uintptr_t)cfg->poolBase;
        uint64_t at = pool->entry.entries;

        pool->entry.magic = US_POOL_ENTRY_MAGIC;
        pool->entry.entries++;
        pool->entry.lastCpu = (uint64_t)(int64_t)cpu;
        pool->entry.lastSp = frame != NULL ? frame->sp : 0;
        if (frame != NULL) {
            pool->entry.lastEsr = frame->esr;
            pool->entry.lastElr = frame->elr;
            pool->entry.lastFar = frame->far;
            pool->entry.lastInsn = *(const volatile uint32_t *)(uintptr_t)frame->elr;
        }

        /* Kept apart from the summary so that two exceptions arriving at once
         * read as two events rather than as one that cannot have happened.
         *
         * The slot is the entry count modulo the ring, so what is kept is the
         * most recent arrivals. Which slot holds the newest is worked out
         * from the count, so the order survives the wrap. */
        if (frame != NULL) {
            UsPoolTrace *t = &pool->entry.trace[at % US_POOL_TRACE_SLOTS];
            const uint64_t *src = (const uint64_t *)(const void *)frame;

            t->cpu = (uint64_t)(int64_t)cpu;
            t->mpidr = currentMpidr();
            for (uint32_t i = 0; i < US_POOL_TRACE_WORDS; i++) {
                t->words[i] = src[i];
            }
        }
    }

    /*
     * The console is only usable before the kernel has its own page tables.
     * Printing from here would fault on the write and the fault would not be
     * survivable, so the quiet flag is obeyed rather than discovered.
     */
    if (cfg->quiet == 0) {
        usUartInit(cfg->uartBase);
    }

    if (frame == NULL) {
        if (cfg->quiet == 0) {
            usUartPuts("US-PAYLOAD no-frame\n");
        }
        return 0;
    }

    ec = (uint32_t)US_ESR_EC(frame->esr);
    if (cfg->quiet == 0) {
        usUartPuts("US-PAYLOAD ec=");
        usUartPutHex(ec);
        usUartPuts(" elr=");
        usUartPutHex(frame->elr);
        usUartPuts(" far=");
        usUartPutHex(frame->far);
        usUartPuts(" cpu=");
        usUartPutDec((uint64_t)(int64_t)cpu);
        usUartPuts("\n");
    }

    if (!isHandledClass(ec)) {
        if (cfg->quiet == 0) {
            usUartPuts("US-PAYLOAD not-mine\n");
        }
        return 0;
    }

    if (ec == US_EC_UNKNOWN && emulateLdapr(frame)) {
        if (cfg->poolBase != 0) {
            ((UsPool *)(uintptr_t)cfg->poolBase)->entry.handled++;
        }
        if (cfg->quiet == 0) {
            usUartPuts("US-PAYLOAD emulated\n");
        }
        /* Claimed: the entry resumes at the instruction after this one. */
        return 1;
    }

    /*
     * Not something this can carry out. Saying so is the honest answer, and
     * the one that leaves the kernel to deal with it exactly as it would
     * have; claiming it would resume at an instruction that was never done.
     */
    if (cfg->quiet == 0) {
        usUartPuts("US-PAYLOAD reached\n");
    }
    return 0;
}

/* Only used to prove the blob was copied and is executable before anything
 * depends on it. */
void usPayloadSelfTest(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsSelfMap self;

    usUartInit(cfg->uartBase);
    usUartPuts("US-PAYLOAD alive cpu=");
    usUartPutDec((uint64_t)currentCpu());
    usUartPuts(" frame=");
    usUartPutDec(sizeof(UsFrame));
    usUartPuts("\n");

    /*
     * Where the tables say this very code can be reached from.
     *
     * There are none here. The firmware runs its own regime, and in this one
     * both translation base registers read as zero: an address is its own
     * physical address and no table describes it. So this reports the answer
     * for that case, and the search itself gets its exercise at the handover,
     * where the kernel's tables are in force and the question actually
     * matters.
     */
    self = usSelfMapFind(cfg->selfVa, US_POOL_BYTES, cfg->selfVa);
    usUartPuts("US-PAYLOAD selfmap ");
    if (self.found) {
        usUartPuts("va=");
        usUartPutHex(self.va);
        usUartPuts(" pa=");
        usUartPutHex(self.pa);
        usUartPuts(" size=");
        usUartPutHex(self.size);
    } else {
        usUartPuts("none");
    }
    usUartPuts(" probes=");
    usUartPutDec(self.probes);
    usUartPuts(self.exhausted ? " truncated" : " complete");
    usUartPuts("\n");
}
