/*
 * The payload's side of the interface.
 *
 * The payload is a position independent blob: linked at zero, copied to a
 * page aligned address, and entered there. Nothing in it may name an absolute
 * address, which is what the link check enforces. It runs with no firmware
 * and no floating point, so everything it needs it carries with it.
 *
 * Its layout inside the blob is described by a generated header rather than
 * by symbols, because the driver embeds the blob as bytes and has no way to
 * link against it.
 */

#ifndef US_PAYLOAD_H
#define US_PAYLOAD_H

#include <stdint.h>

/* For the number of processors the pool has room for: the configuration
 * block and the pool have to agree about it, and the pool's layout is the
 * one that says. */
#include "common/layout.h"

/*
 * What an exception left behind, in the order the entry stores it.
 *
 * The vector table is entered with SP set to the interrupted stack, so the
 * interrupted SP is not recoverable afterwards unless the entry records it
 * before switching. Everything else is read out of the system registers,
 * which keep their values until something writes them.
 *
 * The layout is generated into payload/layout_defs.inc so the assembly entry
 * does not have to be kept in step by hand.
 */
typedef struct UsFrame_t {
    uint64_t x[31];   /* x0 to x30, the last being the link register */
    uint64_t sp;      /* the stack pointer the exception interrupted */
    uint64_t elr;     /* ELR_EL1, where execution was */
    uint64_t spsr;    /* SPSR_EL1, the state it was in */
    uint64_t esr;     /* ESR_EL1, why it trapped */
    uint64_t far;     /* FAR_EL1, the address it trapped on, if any */
} UsFrame;

/* ESR_EL1's exception class, and the classes that matter here. */
#define US_ESR_EC(esr) (((esr) >> 26) & 0x3FU)
#define US_ESR_IL(esr) (((esr) >> 25) & 1U)

#define US_EC_UNKNOWN 0x00U  /* an undefined instruction: the case this is for */
#define US_EC_BRK64 0x3CU    /* a breakpoint, used for probes */

/*
 * Runs one exception.
 *
 * Returns nonzero when the exception was handled and execution can continue
 * from where it was, zero when it was not. A zero answer means the caller has
 * to hand the exception to whoever would have handled it instead.
 */
int usPayloadHandle(UsFrame *frame);

/*
 * Says hello on the serial port, and nothing else.
 *
 * It exists so that "the blob was copied to executable memory and runs" can
 * be checked on its own, before anything depends on being entered through an
 * exception. Everything it needs it reads from the configuration block, so a
 * silent answer means the block was not written rather than that the copy
 * failed.
 */
void usPayloadSelfTest(void);

/*
 * Everything the payload cannot work out for itself.
 *
 * The payload has no way to find the pool: its address changes when the
 * kernel rebuilds the address space, so nothing about it can be baked in. The
 * boot writes this block into the blob's data area after copying it, and the
 * payload reaches it through a pointer derived from the blob's own address.
 */
/*
 * One processor, as the boot found it described.
 *
 * The index is carried rather than worked out from the position, because the
 * code that needs it runs before there is a stack to work anything out on.
 */
typedef struct UsPayloadCpu_t {
    uint64_t mpidr;   /* the affinity fields only */
    uint64_t index;
} UsPayloadCpu;

typedef struct UsPayloadConfig_t {
    /* Where to write. A direct MMIO address, since there is no firmware. */
    uint64_t uartBase;

    /*
     * Top of the stack the entry switches to, per CPU, indexed by the
     * affinity field. A zero entry means that CPU has no stack, which the
     * entry refuses rather than running on whatever was there.
     */
    uint64_t stackTop[8];

    /* Address the blob was entered at, written by the boot. The only way the
     * payload can locate its own data. */
    uint64_t selfVa;

    /*
     * Where an exception handler the kernel installed would have run, for
     * forwarding. Zero when there is none, in which case an unhandled
     * exception is fatal rather than forwarded.
     */
    uint64_t forwardTarget;

    /*
     * The pool, for the trace of what happened. It is reachable at this
     * address from the moment the kernel is running, which is not true of
     * anything else the boot placed.
     */
    uint64_t poolBase;

    /*
     * Whether to keep quiet.
     *
     * The serial port is reachable during boot and is not once the kernel has
     * built its own page tables, where writing to it faults and the fault is
     * not survivable. The exception path is entered in exactly that state, so
     * it has to be told not to print rather than finding out.
     */
    uint64_t quiet;

    /*
     * Which processor is which, as the firmware described it.
     *
     * The low byte of MPIDR_EL1 is Aff0, the core within a cluster, so it
     * repeats across clusters: on a machine of two clusters the first core of
     * each has the same low byte. Indexing per-CPU state by it gives two
     * processors one landing pad and one stack. Looking the value up in this
     * list gives each of them its own place.
     */
    uint64_t cpuCount;
    /*
     * One entry per processor, and one more holding the terminator. The
     * terminator is an index that is not a position any processor can have,
     * which is how the entry's lookup knows it has reached the end without
     * keeping a counter in a register it does not have to spare.
     */
    UsPayloadCpu cpus[US_MAX_CPUS + 1U];
} UsPayloadConfig;

/*
 * The configuration block the boot fills in after copying the blob. Reached
 * through the blob's own address, so nothing has to be linked against it.
 */
UsPayloadConfig *usPayloadConfig(void);

#endif
