/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The runtime pool, its layout, and the one thing everything else depends on
 *
 * The pool is a single contiguous, identity mapped region holding the code
 * that runs in kernelPhase, the data it reads, and one stack per CPU. It has
 * to be identity mapped at every point we are entered, which is why it is one
 * region rather than several: there is one mapping to keep alive instead of
 * several, and one physical address to hand to the page table injector
 *
 * This header is the single source of the layout. Every offset an assembler
 * needs is derivable from the structures below, so the fields may not be
 * reordered without regenerating whatever consumes them
 *
 * Nothing here depends on the firmware, so it is equally usable from the
 * driver, from the payload, and from a host test
 */

#ifndef US_LAYOUT_H
#define US_LAYOUT_H

#include <stdint.h>

#define US_PAGE_SIZE 4096U

/*
 * The bits of MPIDR_EL1 that identify a processor
 *
 * The register also carries a bit saying whether the value is multiprocessor
 * capable and a bit indicating a thread, and neither is part of a processor's
 * identity. ACPI's copy of the value has those bits as zero, so a comparison
 * between the two is made on the fields they agree on
 *
 * This is here rather than beside the ACPI reading because three things need
 * it: the reading, the payload's lookup, and the assembly entry, which takes
 * it from a generated header
 */
#define US_MPIDR_AFFINITY_MASK 0x000000FF00FFFFFFULL

/*
 * The same selection, in the form a logical immediate can hold
 *
 * The two are not the same word and they are not meant to be. AArch64 encodes
 * the mask of an `and` as a run of ones, so the one above -- chosen because it
 * says which fields are wanted -- cannot be written as an instruction, while
 * its complement can. Writing it out by hand in the entry instead is how the
 * second copy appeared, and the two differ in the bits the registers reserve:
 * this one keeps them, the one above clears them. Both read as zero on either
 * side, which is why the difference is invisible until a processor sets one
 */
#define US_MPIDR_AFFINITY_MASK_LOGICAL 0xFFFFFFFF00FFFFFFULL

/*
 * The most processors this serves, and the size of the tables the payload
 * carries in its own image: the per-CPU state the handler keeps is a fixed
 * array there, so it cannot grow at runtime
 *
 * The pool is not one of those. Its stacks are allocated for the processors
 * the machine actually has, so this is the ceiling on that count rather than
 * the size anything is built to
 *
 * 64 is a single 920 system's maximum (32 in one chip), so a machine this
 * would refuse does not exist. A machine with fewer pays for fewer
 */
#define US_MAX_CPUS 64U

/*
 * Sixteen KiB per CPU. The handler runs with interrupts masked and does not
 * recurse, so this is generous; it is sized so that a panic path can still
 * format a dump on it
 */
#define US_STACK_SIZE 0x4000U

/*
 * Room the entry takes below the payload's own stack while a fault inside the
 * payload is being answered, to hold the registers the answer "carry on" has
 * to give back. Every register a call may destroy, which is x0 to x18 and the
 * link register
 */
#define US_FAULT_SPILL_BYTES 160U

/*
 * Marks for the pool and for the records the payload leaves in it
 *
 * ASCII on purpose, so that a dump reads as words: a reader has to be able to
 * tell a record that was written from a page that is still zero, and a word
 * says that at a glance. Each is US in front of what it marks, and the pool's
 * own is four characters because its field is four bytes while the records'
 * are eight
 */
#define US_POOL_MAGIC 0x4C505355U                /* "USPL" - the pool */

/*
 * What the payload records about being entered
 *
 * There is no console once the kernel is running: its page tables do not map
 * the serial port, and the first write to it stops the machine rather than
 * printing anything. So the payload leaves a trace in memory instead, and
 * whoever can still read memory -- the host, through the monitor -- reports
 * it. This lives in the pool rather than in the payload itself because the
 * payload sits in memory the firmware keeps for code, which may well be
 * mapped read only by the time anything would want to write here
 */
#define US_POOL_ENTRY_MAGIC 0x005952544E455355ULL /* "USENTRY" - the record */

/*
 * Set in the entry record when the payload stopped on purpose
 *
 * Written last, after everything it explains, so that a reader which finds it
 * has found a complete record: the host samples memory while the machine may
 * still be writing, and a flag written first would let it read a half-written
 * one and believe it
 *
 * It is a different magic rather than a field beside the count, because what
 * the two describe is different in kind: the ordinary record is a machine at
 * work, and this one is a machine that has stopped, and a reader that has to
 * look at a field to tell them apart will eventually not
 */
#define US_POOL_PANIC_MAGIC 0x0043494E41505555ULL /* "USPANIC" - it stopped */

/*
 * One exception, as it was when the handler was entered
 *
 * The whole frame is kept rather than a chosen few registers. A register
 * that looks wrong is only evidence of something if the ones around it can
 * be checked against what the interrupted code was doing, and which register
 * that will be is not known in advance
 */
typedef struct UsPoolTrace_t {
    uint64_t cpu;
    /* The whole MPIDR, not the part used to pick a landing pad: the part used
     * is the part that is wrong when two CPUs share one */
    uint64_t mpidr;
    /* x0 to x30, then sp, elr, spsr, esr and far: the frame, as stored */
    uint64_t words[36];
} UsPoolTrace;

#define US_POOL_TRACE_WORDS 36U
/*
 * A rolling window, not the first few
 *
 * What a stop needs explained is the exceptions just before it, since the
 * summary only says where the machine ended up. Keeping the first four
 * answered a question about the boot and nothing about a fault several
 * thousand entries later
 */
#define US_POOL_TRACE_SLOTS 8U

/*
 * One carried-out load, for the rolling record of the last few
 *
 * The value write-back is the step whose mistakes are not visible at the
 * point they are made: a wrong address makes a wrong value, the value goes
 * into a register, and the next use of that register is where it shows --
 * which is somewhere else entirely. Keeping the last few anyway is what makes
 * the two connectable, and keeping the first one only answered a question
 * about the boot
 */
typedef struct UsPoolEmu_t {
    uint64_t elr;      /* the instruction the exception was taken on */
    uint64_t insn;     /* the load, as it was decoded */
    uint64_t rt;       /* where the value went */
    uint64_t address;
    uint64_t value;
} UsPoolEmu;

#define US_POOL_EMU_SLOTS 8U

/*
 * One attempt at replacing an RCpc load where it stands, for the record the
 * payload leaves behind. The result is a UsRewriteResult; zero means the slot
 * has not been used
 */
typedef struct UsPoolRewrite_t {
    uint64_t site;
    uint64_t insn;
    uint64_t descriptorVA;
    uint64_t descriptor;
    uint64_t result;
} UsPoolRewrite;

#define US_POOL_REWRITE_SLOTS 8U

/* How many distinct trapping addresses are remembered */
#define US_STATS_SLOTS 96U

typedef struct UsPoolStat_t {
    uint64_t va;      /* the address the exception was taken at */
    uint64_t count;   /* how many times */
} UsPoolStat;

typedef struct UsPoolStats_t {
    uint64_t magic;
    uint64_t offered;   /* every entry the handler saw */
    uint64_t recorded;  /* how many of those found or took a slot */
    uint64_t overflows; /* seen when the table was full and the address new */
    UsPoolStat slots[US_STATS_SLOTS];
} UsPoolStats;

#define US_POOL_STATS_MAGIC 0x0053544154535355ULL   /* "USSTATS" - trap counts */

/*
 * The firmware's frame buffer, and the payload's own way of reaching it
 *
 * The screen is the one channel that outlives the kernel taking over: the
 * serial port is not mapped by the kernel's tables, and the frame buffer is
 * memory the firmware keeps because the machine has no graphics driver and
 * goes on drawing the desktop into it. So it is where a failure with nothing
 * else left to say it is reported
 *
 * Nothing about that is free. The address the firmware gave is in the
 * firmware's own space, and reaching the region from the kernel means a
 * mapping the payload makes for itself, in the kernel's tables, at an address
 * the kernel never chose. That mapping is written once, at the handover, and
 * the record here is what says where it is and whether it is still good: the
 * kernel may take the address back at any time, and the only way to find out
 * is to ask the tables again before each use
 *
 * Written by the payload at the handover, never before: until then the pool
 * is reached through the firmware's tables and the aliases mean nothing
 */
typedef struct UsPoolFB_t {
    uint64_t magic;
    uint64_t pa;        /* where the frame buffer is, physically */
    uint64_t bytes;     /* how much of it the firmware described */
    uint64_t aliasVA;   /* where the payload mapped it, or zero when it has no
                         * mapping and nothing can be drawn */
    uint64_t blocks;    /* how many block entries that mapping took */
    uint64_t attrIndex; /* the MAIR attribute it was made with */
    uint64_t drawn;     /* how many times text has been drawn on it */
    /*
     * Why there is no mapping, when there is none
     *
     * A build that refuses is the ordinary outcome on a machine this was not
     * expected to work on, and the difference between the reasons is the
     * difference between a machine that has no screen and one this code got
     * wrong. Left as zero when the mapping was made, so a reader that finds a
     * non-zero alias can ignore it
     */
    uint64_t reason;
    uint64_t detail;    /* the number the reason is about */
    /*
     * What the payload knew about the kernel when it tried: where the image
     * is, and the value it found for the base of the kernel's own descriptor
     * mapping
     *
     * Recorded whether or not the mapping was made, and that is the point:
     * the base is what a reader needs to walk the kernel's tables at all, and
     * the only side that can work it out is the one that is running. A host
     * that has to look at the tables has no other way in
     */
    uint64_t kernelBase;
    uint64_t mpteBase;
    uint32_t width;
    uint32_t height;
    uint32_t stride;    /* in pixels, not bytes */
    uint32_t blueFirst; /* which end of a pixel is blue */
} UsPoolFB;

#define US_POOL_FB_MAGIC 0x0000004246505355ULL   /* "USPFB" - the frame buffer */

/*
 * Why the frame buffer could not be mapped, in the order the build checks
 *
 * These are the answers to "why is there no screen", which is otherwise a
 * silent refusal: every one of them is a different problem, from a machine
 * that simply has no frame buffer to one whose tables could not be walked at
 * all
 */
#define US_FB_OK 0U
#define US_FB_NO_KERNEL 1U        /* the kernel's base is unknown */
#define US_FB_NO_BASE_RVA 2U      /* the boot did not leave a descriptor base */
#define US_FB_NO_BASE 3U          /* the descriptor base could not be read */
#define US_FB_NO_ATTR 4U          /* no MAIR entry holds a non-cacheable byte */
#define US_FB_NO_SLOT 5U          /* no free entry was found to map into */

typedef struct UsPoolEntry_t {
    uint64_t magic;
    uint64_t entries;    /* how many times the handler was entered */
    uint64_t handled;    /* how many of those it claimed */
    /* How many of the rest were given to the kernel's own handler, and what
     * the last of those was. A machine that stopped on an exception the
     * payload never claimed says so here, and the exception state says which
     * one it was */
    uint64_t handedBack;
    uint64_t handbackESR;
    uint64_t handbackELR;
    /*
     * The paths that end without an answer, which are the ones a machine that
     * stopped cannot otherwise explain: the entry stopping because it could
     * not even name a destination, and a fault inside the payload that was not
     * one it had asked about
     */
    uint64_t stuck;
    uint64_t stuckKind;
    uint64_t stuckESR;
    uint64_t stuckELR;
    uint64_t stuckSPSR;   /* which slot it came through */
    uint64_t stuckVBAR;   /* and the table that was in force */
    uint64_t nestedFaults;
    uint64_t nestedESR;
    uint64_t nestedFAR;
    uint64_t nestedELR;
    /* Where the payload and its pool were when that happened, so the two
     * addresses above can be read as offsets into code and data that exist
     * somewhere to be looked at */
    uint64_t nestedSelfVA;
    uint64_t nestedPoolBase;
    uint64_t nestedStackTop;
    uint64_t nestedCPU;
    uint64_t lastESR;
    uint64_t lastELR;
    uint64_t lastFAR;
    uint64_t lastCPU;
    uint64_t lastSP;
    /* The instruction that faulted, so the host can see what was emulated
     * rather than only how often */
    uint64_t lastInsn;

    /*
     * The first load that was carried out: the instruction, the address it
     * read, and what came back
     *
     * An emulator that returns the wrong value does not fail where it is;
     * it fails wherever the value is next used, which is a fault with no
     * obvious connection to this code. Keeping the first one makes that
     * connection visible from outside, where nothing else can be
     */
    uint64_t emuInsn;
    uint64_t emuAddr;
    uint64_t emuValue;
    uint64_t emuELR;
    /* The two registers the address was built from, so an address that looks
     * wrong can be traced to which part of it was wrong */
    uint64_t emuX0;
    uint64_t emuX9;

    /* Every entry, in arrival order, up to the ring's size */
    UsPoolTrace trace[US_POOL_TRACE_SLOTS];

    /* The last few loads carried out, newest last once the ring has filled */
    uint64_t emuCount;
    UsPoolEmu emu[US_POOL_EMU_SLOTS];

    /*
     * The same frames again, on the way out
     *
     * What the handler received is not the same question as what it hands
     * back, and only the first was being recorded. The second is the one that
     * can be wrong: every register the interrupted code continues with came
     * from here, so a register that is right on the way in and wrong in the
     * code that ran next is wrong in between -- and between is this side
     *
     * A pair per exception, at the same index, so the two can be compared
     * without having to match them up by hand
     */
    UsPoolTrace handback[US_POOL_TRACE_SLOTS];

    /*
     * The last few attempts at replacing an instruction where it stands
     *
     * Whether that works is the difference between a boot that finishes and
     * one that only looks busy, and it is not something the machine can say
     * any other way: each record is one site, what was there, where its
     * descriptor was found, and what came of the store
     */
    uint64_t rewriteCount;    /* attempts, all outcomes */
    uint64_t rewriteWritten;  /* sites that now hold the substitute */
    UsPoolRewrite rewrite[US_POOL_REWRITE_SLOTS];

    /*
     * The kernel's driver load call, as it arrived at the payload
     *
     * Whether the hook is in place at all is one question and whether anything
     * comes through it is another, and only the second one says the mechanism
     * works: a thunk nothing reaches and a thunk that was never written look
     * the same from every other angle. The count answers the first, and the
     * image answers the second - an address that names a PE with a sensible
     * size is a driver that was really being loaded
     */
    uint64_t driverLoads;   /* how many calls were diverted to us */
    uint64_t driverImage;   /* the image the last one was loading */
    uint64_t driverSize;    /* its SizeOfImage, from its own header */
    uint64_t driverEntry;   /* and its entry point, as an RVA */
    /*
     * What the replacement did to those images: how many RCpc loads were
     * found in them, and how many now hold the substitute. Kept apart because
     * the two failures look nothing alike - no sites at all means the image
     * was not scanned, sites that were not written mean the pages would not
     * take it
     */
    uint64_t driverSites;
    uint64_t driverWritten;
    /*
     * Pages of those images that could not be read and were left as they
     * were, which is what a walk over an image whose pages are not all
     * resident has to do: those pages keep trapping, which is slower and
     * correct
     */
    uint64_t driverSkipped;

    /*
     * Set when the payload stopped on purpose, and what it knew
     *
     * The report itself goes to the serial port and to the screen, and neither
     * can be relied on: the port is not mapped once the kernel is running, and
     * the frame buffer is only there on a machine that has one the payload
     * managed to map. This is the third copy, in memory the host can read
     * through the monitor whatever else is true
     *
     * panicEntries is how many exceptions the handler had been entered for by
     * then, which is the one piece of context nothing else in this record
     * carries and which says whether the stop was on the boot path or long
     * after it
     */
    uint64_t panic;
    uint64_t panicCode;
    uint64_t panicDetail;
    uint64_t panicELR;
    uint64_t panicESR;
    uint64_t panicFAR;
    uint64_t panicSPSR;
    uint64_t panicEntries;

} UsPoolEntry;

typedef struct UsPool_t {
    uint32_t magic;
    /* Slots provided, which is the processor count the pool was built for.
     * The payload checks its own CPU index against this before using a stack */
    uint32_t stackSlots;

    /*
     * Written by the payload on every entry. Zeroed by the boot, so a magic
     * that is present is proof the payload ran rather than a leftover
     */
    UsPoolEntry entry;

    /*
     * Where the handler has been spending its time: one record per address a
     * trap was taken at, counted. The trap path is the only place that knows,
     * and user mode cannot read kernel memory, so the count has to be kept
     * here for something else to read
     *
     * It is an open table: a trap looks the address up, and takes the first
     * free slot when it is not there. Nothing is evicted, so a busy address
     * keeps its slot and the ones seen after the table fills are counted
     * rather than recorded
     */
    UsPoolStats stats;

    /* The frame buffer, and the payload's own mapping of it */
    UsPoolFB fb;

    /*
     * Physical address of this pool. At bootPhase the pool is identity mapped
     * and this equals its address as a pointer; after the address space is
     * rebuilt it does not, and this is the value the injector has to map.
     * It is written once and never relocated
     */
    uint64_t selfPA;

    /*
     * Address the pool is reached by. Updated whenever the pool is moved to a
     * different address, so the payload can work out where it is rather than
     * assuming the mapping it was entered through
     */
    uint64_t selfVA;

    /*
     * Top of each CPU's stack, that is the address stack grows down from, and
     * already aligned. A zero entry means that CPU has no stack
     *
     * One per slot the pool was built for; the rest are zero, and a zero slot
     * is what the entry treats as "no stack for this processor"
     */
    uint64_t stackTop[US_MAX_CPUS];
} UsPool;

/* The pool header occupies whole pages, so everything after it starts aligned */
#define US_POOL_HEADER_SIZE ((sizeof(UsPool) + US_PAGE_SIZE - 1) & ~(uint64_t)(US_PAGE_SIZE - 1))

/* Stack slot n starts here, relative to the pool base */
#define US_POOL_STACK_OFFSET(n) (US_POOL_HEADER_SIZE + (uint64_t)(n) * US_STACK_SIZE)

/*
 * What a pool for `slots` stacks costs
 *
 * The count is the machine's rather than the most it could have: the boot
 * allocates one stack per processor it found, so a machine with a few of them
 * does not pay for the many it could have had. Both parts are whole pages, so
 * the total is a whole number of pages exactly, with nothing to round
 *
 * The macro and the function are the same arithmetic: the first because an
 * assembly offset and an array size need a constant expression, the second
 * because the count is a runtime value everywhere else
 */
#define US_POOL_BYTES_FOR(slots) (US_POOL_HEADER_SIZE + (uint64_t)(slots) * US_STACK_SIZE)
#define US_POOL_PAGES_FOR(slots) (US_POOL_BYTES_FOR(slots) / US_PAGE_SIZE)

static inline uint64_t usPoolBytes(uint32_t slots) {
    return US_POOL_BYTES_FOR(slots);
}

static inline uint64_t usPoolPages(uint32_t slots) {
    return US_POOL_PAGES_FOR(slots);
}

/*
 * AArch64 requires the stack pointer to be sixteen byte aligned at every
 * public interface, so the alignment is part of the layout, not a detail
 */
#define US_STACK_ALIGN 16U

#endif
