/*
 * Replacing an RCpc load where it stands, on the exception that trapped on it
 *
 * Carrying out the load on every exception is correct and far too slow. An
 * exception to EL1 costs orders of magnitude more than the load it stands in
 * for, and the kernel reads an LDAPR inside its own spin loops, so the
 * difference is between a boot that finishes and one that never reaches the
 * desktop
 *
 * So the instruction is replaced the first time it is trapped on. The
 * substitute is the acquire load with the same operands (core/ldapr.h), which
 * is at least as strong an ordering as the RCpc load and runs at the same
 * privilege level, so the permission the page had is the permission the
 * substitute runs with and nothing has to be emulated afterwards
 *
 * The obstacle is that the page is read-only: the kernel's own text is mapped
 * that way, and a store into it faults. What can make it writable is the
 * descriptor that translates it, and the kernel maps its own descriptors into
 * its address space for exactly this reason -- finding one is arithmetic in
 * core/pgtable.h, given the base the kernel chose for that mapping. The base
 * is not a constant: it is the variable MmPteBase, whose address the image
 * tells us (see UsPayloadStub.descriptorBaseRva), and the literal the kernel's
 * own MiGetPteAddress uses is the x64 one, which on this 47-bit address space
 * does not translate at all
 *
 * Every step that can be refused is refused safely. The stores go through the
 * probe in payload.h, so a mapping that does not allow one answers "no"
 * instead of becoming a fault inside a handler, and the site is left exactly
 * as it was: the trap path is still there, and it still works
 */

#include "core/ldapr.h"
#include "core/pan.h"
#include "core/pgtable.h"
#include "core/translate.h"
#include "payload/payload.h"
#include "payload/rewrite.h"

/* The base of the kernel's descriptor mapping, read once per machine */
static uint64_t gDescriptorBase;
static bool     gDescriptorBaseTried;

/* Whether a read the walk made was refused, which makes the answer unknown
 * rather than negative */
typedef struct UsRead_t {
    bool refused;
} UsRead;

/*
 * The stack bank the accesses that may fault are made under
 *
 * A fault inside the payload is answered by the entry only when it is taken
 * through the EL1h slot: that stub is the one carrying the fault branch, and
 * the payload's code runs under that slot only when the exception that entered
 * it was itself taken at EL1h. A kernel trap usually is not - the kernel runs
 * thread code on SP_EL0, so its code is at EL1t and so is the payload while it
 * handles it - and a fault taken at EL1t goes to the kernel's own handler,
 * which is not something a payload address can be reported to
 *
 * So the accesses are made with the other bank selected. SP_EL1 is set below
 * the stack in use rather than at its top, because the frames of the handler
 * are live there and a second stack growing down from the same top would write
 * over them. Nothing else uses this bank, and interrupts are masked from the
 * entry onwards, so the switch is the only thing that has to be right
 */
typedef struct UsProbeBank_t {
    uint64_t restoreSp;   /* zero when no switch was made */
} UsProbeBank;

/* How far below the stack in use the probing stack starts. Enough for the few
 * frames these calls need, and far inside the 16KB the CPU has */
#define US_PROBE_STACK_BYTES 0x1000U

static UsProbeBank probeBankEnter(void) {
    UsProbeBank bank = { 0 };
    uint64_t spsel;
    uint64_t sp;

    __asm__ volatile("mrs %0, spsel" : "=r"(spsel));
    if (spsel != 0) {
        return bank;              /* already on the bank that answers faults */
    }
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    /* The bank just selected may hold anything at all; it is pointed somewhere
     * safe before anything is pushed, and no memory is touched in between */
    __asm__ volatile("msr spsel, #1" ::: "memory");
    __asm__ volatile("mov sp, %0" ::"r"(sp - US_PROBE_STACK_BYTES));
    bank.restoreSp = sp;
    return bank;
}

static void probeBankLeave(const UsProbeBank *bank) {
    if (bank->restoreSp == 0) {
        return;
    }
    __asm__ volatile("mov sp, %0" ::"r"(bank->restoreSp));
    __asm__ volatile("msr spsel, #0" ::: "memory");
}

static uint64_t currentVbar(void) {
    uint64_t vbar;

    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    return vbar;
}

static uint64_t readProbed(void *ctx, uint64_t at) {
    UsRead *state = ctx;
    uint64_t value = 0;

    if (!usPayloadProbeRead(at, &value)) {
        state->refused = true;
    }
    return value;
}

/*
 * Where the kernel keeps its descriptor base
 *
 * The value is checked before it is used: the base of a mapping is canonical
 * in the kernel half and aligned to the region it describes, and anything else
 * is not it. A wrong base would only ever be read from, and the reads are
 * refused safely, so being wrong here costs a rewrite rather than a machine
 */
static uint64_t descriptorBase(uint64_t *triedOut, uint64_t *valueOut) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t vbar = currentVbar();
    uint64_t tablePa = 0;
    uint64_t count = cfg->stubCount;

    *triedOut = 0;
    *valueOut = 0;
    if (gDescriptorBaseTried) {
        *triedOut = gDescriptorBase;
        return gDescriptorBase;
    }
    if (count > US_PAYLOAD_MAX_STUBS) {
        count = US_PAYLOAD_MAX_STUBS;
    }

    /*
     * Only the table VBAR names is considered, and only a stub of it that has
     * a base to offer. The rest of the stubs belong to other images, whose
     * base is not this one: the boot addresses a stub carries are turned into
     * runtime addresses by subtracting the difference between the two, and
     * doing that with another image's table names an address in neither
     *
     * The kernel takes its first traps before it has installed its own vector
     * table, so the table in force is sometimes the loader's. Those attempts
     * find nothing here, and that is the right answer: the site will trap
     * again, and by then the table will be the kernel's
     */
    (void)usTranslateAddress(vbar, false, &tablePa);
    if (tablePa == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        const UsPayloadStub *stub = &cfg->stubs[i];
        uint64_t imageVa;
        uint64_t value = 0;

        if (stub->tablePa != tablePa || stub->descriptorBaseRva == 0
            || stub->imageAddress == 0
            || stub->tableAddress < stub->imageAddress
            || vbar < stub->tableAddress - stub->imageAddress) {
            continue;
        }
        /*
         * The stub's addresses are the ones the boot used, which stop being
         * the addresses the kernel runs at once it has built its own tables.
         * What survives is the difference between them: the table VBAR names
         * is this image's table, so the image's base is that difference below
         * it
         */
        imageVa = vbar - (stub->tableAddress - stub->imageAddress);
        *triedOut = imageVa + stub->descriptorBaseRva;
        if (!usPayloadProbeRead(*triedOut, &value)) {
            continue;
        }
        *valueOut = value;
        if ((value & 0xFFF) == 0
            && ((value >> 47) == 0x1FFFF || (value >> 48) == 0xFFFF)) {
            gDescriptorBase = value;
            gDescriptorBaseTried = true;
            return value;
        }
    }
    return 0;
}

/* A change to a translation, and the fetch of an instruction that followed a
 * change to memory. Both are the sequences the architecture asks for; the
 * scope is inner shareable because the other processors are reading the same
 * tables and instructions */
static void invalidateTranslation(uint64_t va) {
    uint64_t arg = va >> 12;

    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vaae1, %0" ::"r"(arg) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static void publishInstruction(uint64_t at) {
    __asm__ volatile("dc cvau, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("ic ivau, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* The record the host reads, since nothing here can print */
static UsPoolRewrite *attemptSlot(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;

    if (cfg->poolBase == 0) {
        return NULL;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    return &pool->entry.rewrite[pool->entry.rewriteCount % US_POOL_REWRITE_SLOTS];
}

/* How far this attempt has got, written into the slot it will end in */
static void step(uint64_t site, uint64_t insn, uint64_t where) {
    UsPoolRewrite *slot = attemptSlot();

    if (slot == NULL) {
        return;
    }
    if (slot->site != site || slot->insn != insn) {
        slot->site = site;
        slot->insn = insn;
        slot->descriptorVa = 0;
        slot->descriptor = 0;
    }
    slot->result = where;
}

static void record(uint64_t site, uint64_t insn, const UsLeaf *leaf, uint64_t result) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool;
    UsPoolRewrite *slot = attemptSlot();

    if (cfg->poolBase == 0 || slot == NULL) {
        return;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    slot->site = site;
    slot->insn = insn;
    slot->descriptorVa = leaf != NULL ? leaf->descriptorVa : slot->descriptorVa;
    slot->descriptor = leaf != NULL ? leaf->descriptor : slot->descriptor;
    slot->result = result;
    pool->entry.rewriteCount++;
    if (result == UsRewriteWritten) {
        pool->entry.rewriteWritten++;
    }
}

UsRewriteResult usRewriteSite(uint64_t site) {
    UsRead state = { false };
    UsPayloadConfig *cfg = usPayloadConfig();
    UsLeaf leaf;
    uint64_t base;
    uint64_t baseTried = 0;
    uint64_t baseValue = 0;
    uint64_t original;
    uint64_t now = 0;
    uint32_t insn;
    uint32_t replacement;
    bool relaxed = false;
    bool written = false;
    bool restored = true;
    UsProbeBank bank;

    if (site == 0 || (site & 3) != 0 || cfg->poolBase == 0) {
        return UsRewriteRefused;
    }
    step(site, 0, UsRewriteReadingBase);

    /*
     * Everything below this point reads and writes addresses whose mapping is
     * not known in advance, so it runs on the bank whose faults come back
     *
     * PAN goes off for the same stretch. The sites are as often in user code
     * as in the kernel - the demo this exists for is an EL0 program - and
     * reading or writing a user page at EL1 is what PAN stops. The exception
     * being handled came from EL0, so the access being replaced is one EL0 is
     * allowed to make; making it with PAN clear is the same thing the kernel
     * does when it copies to a user address. What PAN is left as does not
     * matter: the entry restores the interrupted state from the SPSR, and
     * sets PAN on the way out for a hand-back, which is what a real entry to
     * EL1 does
     */
    bank = probeBankEnter();
    usPanOff();

    /*
     * The instruction as it is now. A site another processor has already
     * replaced reads as the acquire load, and the only work left is this
     * processor's own stale copy: the caller has carried the load out, and
     * the instruction has to be fetched again here
     */
    if (!usPayloadProbeRead(site, &now)) {
        usPanOn();
        probeBankLeave(&bank);
        record(site, 0, NULL, UsRewriteRefused);
        return UsRewriteRefused;
    }
    insn = (uint32_t)now;
    if (!usLdaprToLdar(insn, &replacement)) {
        UsLdaprInsn acquire = { 0 };

        if (usLdarDecode(insn, &acquire)) {
            publishInstruction(site);
            usPanOn();
            probeBankLeave(&bank);
            record(site, insn, NULL, UsRewriteAlready);
            return UsRewriteAlready;
        }
        usPanOn();
        probeBankLeave(&bank);
        record(site, insn, NULL, UsRewriteNotRcpc);
        return UsRewriteNotRcpc;
    }
    step(site, insn, UsRewriteReadingBase);

    base = descriptorBase(&baseTried, &baseValue);
    if (base == 0) {
        /* What it tried and what came back, which is the difference between
         * "nothing was read" and "what was read was not a base". */
        UsPoolRewrite *slot = attemptSlot();

        if (slot != NULL) {
            slot->descriptorVa = baseTried;
            slot->descriptor = baseValue;
        }
        usPanOn();
        probeBankLeave(&bank);
        record(site, insn, NULL, UsRewriteNoBase);
        return UsRewriteNoBase;
    }

    step(site, insn, UsRewriteWalking);
    leaf = usLeafFind(base, readProbed, &state, site);
    if (state.refused || !leaf.found) {
        usPanOn();
        probeBankLeave(&bank);
        record(site, insn, NULL, UsRewriteUnmapped);
        return UsRewriteUnmapped;
    }

    /*
     * Whatever produced the base - the configuration stating a build's
     * address, or the image itself - the walk it drives has to agree with the
     * hardware. The site's own address is the cheapest thing to check it on:
     * the translation the hardware performs for it must name the same
     * physical page the descriptor we are about to edit names. A wrong base
     * cannot pass this, and the attempt is abandoned exactly as it is when
     * there is no base at all, so a configuration written for another build
     * costs nothing but the rewrite
     */
    {
        uint64_t hardwarePa = 0;

        if (!usTranslateAddress(site, false, &hardwarePa)
            || (hardwarePa & ~(uint64_t)(leaf.size - 1U))
               != (leaf.pa & ~(uint64_t)(leaf.size - 1U))) {
            usPanOn();
            probeBankLeave(&bank);
            record(site, insn, &leaf, UsRewriteNoBase);
            return UsRewriteNoBase;
        }
    }
    original = leaf.descriptor;

    /*
     * The descriptor has to be writable for this to be possible at all, and
     * the page it lives on is a page table: the kernel writes those all the
     * time, so it normally is. The probe is the value it already holds, so a
     * refusal leaves everything as it was
     */
    if ((original & US_PTE_AP2) != 0) {
        step(site, insn, UsRewriteProbing);
        if (!usPayloadProbeWrite(leaf.descriptorVa, original)) {
            usPanOn();
            probeBankLeave(&bank);
            record(site, insn, &leaf, UsRewriteReadOnly);
            return UsRewriteReadOnly;
        }
        step(site, insn, UsRewriteClearing);
        if (!usPayloadProbeWrite(leaf.descriptorVa, original & ~US_PTE_AP2)) {
            (void)usPayloadProbeWrite(leaf.descriptorVa, original);
            usPanOn();
            probeBankLeave(&bank);
            record(site, insn, &leaf, UsRewriteReadOnly);
            return UsRewriteReadOnly;
        }
        relaxed = true;
    }

    /* The table changed, so this processor's translation of the site is stale
     * and would still refuse the store */
    invalidateTranslation(site);

    step(site, insn, UsRewriteStoring);
    written = usPayloadProbeWriteWord(site, replacement);

    if (written) {
        step(site, insn, UsRewritePublishing);
        publishInstruction(site);
    }

    /*
     * The permission goes back before anything else: a page left writable is
     * worse than an instruction left unreplaced. Only if the descriptor still
     * holds what this attempt put there, though: it belongs to the kernel, and
     * if the kernel has changed it in the meantime - these are the tables a
     * running system edits - then its value is the one to keep, and writing
     * the old one back would undo its change
     */
    if (relaxed) {
        uint64_t underlying = 0;
        bool ours = true;

        step(site, insn, UsRewriteRestoring);
        if (usPayloadProbeRead(leaf.descriptorVa, &underlying)
            && underlying != (original & ~US_PTE_AP2)) {
            ours = false;
        }
        if (ours) {
            restored = usPayloadProbeWrite(leaf.descriptorVa, original);
        }
        invalidateTranslation(site);
    }
    usPanOn();
    probeBankLeave(&bank);
    if (!restored) {
        record(site, insn, &leaf, UsRewriteStuck);
        return UsRewriteStuck;
    }
    record(site, insn, &leaf, written ? UsRewriteWritten : UsRewriteRefused);
    return written ? UsRewriteWritten : UsRewriteRefused;
}
