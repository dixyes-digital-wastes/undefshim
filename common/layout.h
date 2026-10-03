/*
 * The runtime pool, its layout, and the one thing everything else depends on.
 *
 * The pool is a single contiguous, identity mapped region holding the code
 * that runs in kernelPhase, the data it reads, and one stack per CPU. It has
 * to be identity mapped at every point we are entered, which is why it is one
 * region rather than several: there is one mapping to keep alive instead of
 * several, and one physical address to hand to the page table injector.
 *
 * This header is the single source of the layout. Every offset an assembler
 * needs is derivable from the structures below, so the fields may not be
 * reordered without regenerating whatever consumes them.
 *
 * Nothing here depends on the firmware, so it is equally usable from the
 * driver, from the payload, and from a host test.
 */

#ifndef US_LAYOUT_H
#define US_LAYOUT_H

#include <stdint.h>

#define US_PAGE_SIZE 4096U

/*
 * The bits of MPIDR_EL1 that identify a processor.
 *
 * The register also carries a bit saying whether the value is multiprocessor
 * capable and a bit indicating a thread, and neither is part of a processor's
 * identity. ACPI's copy of the value has those bits as zero, so a comparison
 * between the two is made on the fields they agree on.
 *
 * This is here rather than beside the ACPI reading because three things need
 * it: the reading, the payload's lookup, and the assembly entry, which takes
 * it from a generated header.
 */
#define US_MPIDR_AFFINITY_MASK 0x000000FF00FFFFFFULL

/*
 * The same selection, in the form a logical immediate can hold.
 *
 * The two are not the same word and they are not meant to be. AArch64 encodes
 * the mask of an `and` as a run of ones, so the one above -- chosen because it
 * says which fields are wanted -- cannot be written as an instruction, while
 * its complement can. Writing it out by hand in the entry instead is how the
 * second copy appeared, and the two differ in the bits the registers reserve:
 * this one keeps them, the one above clears them. Both read as zero on either
 * side, which is why the difference is invisible until a processor sets one.
 */
#define US_MPIDR_AFFINITY_MASK_LOGICAL 0xFFFFFFFF00FFFFFFULL

/*
 * One stack per possible CPU rather than per present CPU. The count is not
 * known when the pool is allocated, and finding out costs more than the
 * reservation does: eight stacks is 128 KiB, and having a slot for a CPU that
 * is not there is harmless. The payload picks its slot from MPIDR_EL1 and
 * refuses to run on a CPU with no slot.
 */
#define US_MAX_CPUS 8U

/*
 * Sixteen KiB per CPU. The handler runs with interrupts masked and does not
 * recurse, so this is generous; it is sized so that a panic path can still
 * format a dump on it.
 */
#define US_STACK_SIZE 0x4000U

/* "USPL", used to tell a plausible pool from an uninitialised page. */
#define US_POOL_MAGIC 0x4C505355U

/*
 * What the payload records about being entered.
 *
 * There is no console once the kernel is running: its page tables do not map
 * the serial port, and the first write to it stops the machine rather than
 * printing anything. So the payload leaves a trace in memory instead, and
 * whoever can still read memory -- the host, through the monitor -- reports
 * it. This lives in the pool rather than in the payload itself because the
 * payload sits in memory the firmware keeps for code, which may well be
 * mapped read only by the time anything would want to write here.
 */
#define US_POOL_ENTRY_MAGIC 0x5952544E55504355ULL /* "UCPUNTRY", readable in a dump */

/*
 * One exception, as it was when the handler was entered.
 *
 * The whole frame is kept rather than a chosen few registers. A register
 * that looks wrong is only evidence of something if the ones around it can
 * be checked against what the interrupted code was doing, and which register
 * that will be is not known in advance.
 */
typedef struct UsPoolTrace_t {
    uint64_t cpu;
    /* The whole MPIDR, not the part used to pick a landing pad: the part used
     * is the part that is wrong when two CPUs share one. */
    uint64_t mpidr;
    /* x0 to x30, then sp, elr, spsr, esr and far: the frame, as stored. */
    uint64_t words[36];
} UsPoolTrace;

#define US_POOL_TRACE_WORDS 36U
/*
 * A rolling window, not the first few.
 *
 * What a stop needs explained is the exceptions just before it, since the
 * summary only says where the machine ended up. Keeping the first four
 * answered a question about the boot and nothing about a fault several
 * thousand entries later.
 */
#define US_POOL_TRACE_SLOTS 8U

/*
 * One carried-out load, for the rolling record of the last few.
 *
 * The value write-back is the step whose mistakes are not visible at the
 * point they are made: a wrong address makes a wrong value, the value goes
 * into a register, and the next use of that register is where it shows --
 * which is somewhere else entirely. Keeping the last few anyway is what makes
 * the two connectable, and keeping the first one only answered a question
 * about the boot.
 */
typedef struct UsPoolEmu_t {
    uint64_t elr;      /* the instruction the exception was taken on */
    uint64_t insn;     /* the load, as it was decoded */
    uint64_t rt;       /* where the value went */
    uint64_t address;
    uint64_t value;
} UsPoolEmu;

#define US_POOL_EMU_SLOTS 8U

typedef struct UsPoolEntry_t {
    uint64_t magic;
    uint64_t entries;    /* how many times the handler was entered */
    uint64_t handled;    /* how many of those it claimed */
    uint64_t lastEsr;
    uint64_t lastElr;
    uint64_t lastFar;
    uint64_t lastCpu;
    uint64_t lastSp;
    /* The instruction that faulted, so the host can see what was emulated
     * rather than only how often. */
    uint64_t lastInsn;

    /*
     * The first load that was carried out: the instruction, the address it
     * read, and what came back.
     *
     * An emulator that returns the wrong value does not fail where it is;
     * it fails wherever the value is next used, which is a fault with no
     * obvious connection to this code. Keeping the first one makes that
     * connection visible from outside, where nothing else can be.
     */
    uint64_t emuInsn;
    uint64_t emuAddr;
    uint64_t emuValue;
    uint64_t emuElr;
    /* The two registers the address was built from, so an address that looks
     * wrong can be traced to which part of it was wrong. */
    uint64_t emuX0;
    uint64_t emuX9;

    /* Every entry, in arrival order, up to the ring's size. */
    UsPoolTrace trace[US_POOL_TRACE_SLOTS];

    /* The last few loads carried out, newest last once the ring has filled. */
    uint64_t emuCount;
    UsPoolEmu emu[US_POOL_EMU_SLOTS];

    /*
     * The same frames again, on the way out.
     *
     * What the handler received is not the same question as what it hands
     * back, and only the first was being recorded. The second is the one that
     * can be wrong: every register the interrupted code continues with came
     * from here, so a register that is right on the way in and wrong in the
     * code that ran next is wrong in between -- and between is this side.
     *
     * A pair per exception, at the same index, so the two can be compared
     * without having to match them up by hand.
     */
    UsPoolTrace handback[US_POOL_TRACE_SLOTS];
} UsPoolEntry;

typedef struct UsPool_t {
    uint32_t magic;
    /* Slots provided, which is US_MAX_CPUS. The payload checks its own CPU
     * index against this before using a stack. */
    uint32_t stackSlots;

    /*
     * Written by the payload on every entry. Zeroed by the boot, so a magic
     * that is present is proof the payload ran rather than a leftover.
     */
    UsPoolEntry entry;

    /*
     * Physical address of this pool. At bootPhase the pool is identity mapped
     * and this equals its address as a pointer; after the address space is
     * rebuilt it does not, and this is the value the injector has to map.
     * It is written once and never relocated.
     */
    uint64_t selfPa;

    /*
     * Address the pool is reached by. Updated whenever the pool is moved to a
     * different address, so the payload can work out where it is rather than
     * assuming the mapping it was entered through.
     */
    uint64_t selfVa;

    /*
     * Top of each CPU's stack, that is the address stack grows down from, and
     * already aligned. A zero entry means that CPU has no stack.
     */
    uint64_t stackTop[US_MAX_CPUS];
} UsPool;

/* The pool header occupies whole pages, so everything after it starts aligned. */
#define US_POOL_HEADER_SIZE ((sizeof(UsPool) + US_PAGE_SIZE - 1) & ~(uint64_t)(US_PAGE_SIZE - 1))

/* Total bytes the pool needs: header plus every CPU's stack. */
#define US_POOL_BYTES (US_POOL_HEADER_SIZE + (uint64_t)US_MAX_CPUS * US_STACK_SIZE)

/* Pages to ask the firmware for. */
#define US_POOL_PAGES (US_POOL_BYTES / US_PAGE_SIZE)

/* Stack slot n starts here, relative to the pool base. */
#define US_POOL_STACK_OFFSET(n) (US_POOL_HEADER_SIZE + (uint64_t)(n) * US_STACK_SIZE)

/*
 * AArch64 requires the stack pointer to be sixteen byte aligned at every
 * public interface, so the alignment is part of the layout, not a detail.
 */
#define US_STACK_ALIGN 16U

#endif
