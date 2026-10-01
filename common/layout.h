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

typedef struct UsPool_t {
    uint32_t magic;
    /* Slots provided, which is US_MAX_CPUS. The payload checks its own CPU
     * index against this before using a stack. */
    uint32_t stackSlots;

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
