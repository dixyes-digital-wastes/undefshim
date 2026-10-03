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
 * Interrupted state, saved on the CPU's own stack
 *
 * sp records the bank selected by SPSR_EL1, independently of the entry SP_EL1
 * The entry saves its SP_EL1 above this frame without changing the C layout
 * Assembly offsets are generated from this structure
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
 * Returns nonzero to resume the updated frame, zero when it was not handled
 * The synchronous entry halts on an unclaimed frame
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

#define US_PAYLOAD_MAX_STUBS 32U

typedef struct UsPayloadStub_t {
    uint64_t address;
    uint64_t tableAddress;
    uint64_t imageAddress;
    uint64_t tablePa;
    uint64_t addressPa;
    uint32_t targetIndex;
    uint32_t published;
} UsPayloadStub;

typedef struct UsPayloadConfig_t {
    /* Where to write. A direct MMIO address, since there is no firmware. */
    uint64_t uartBase;

    /* Stack tops indexed by the firmware CPU index, not a raw affinity byte */
    uint64_t stackTop[8];

    /* Address the blob was entered at, written by the boot. The only way the
     * payload can locate its own data. */
    uint64_t selfVa;

    /* Reserved forwarding address; the synchronous entry does not use it */
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

    /* Firmware affinity-to-index mapping used to generate the stack lookup */
    uint64_t cpuCount;
    /* One entry per processor plus the firmware mapping's terminator */
    UsPayloadCpu cpus[US_MAX_CPUS + 1U];

    /* Immutable physical ranges used to validate their runtime aliases */
    uint64_t selfPa;
    uint64_t selfBytes;
    uint64_t poolPa;
    uint64_t entryOffset;

    uint64_t highVa;
    uint64_t highPoolVa;
    uint64_t stubCount;
    UsPayloadStub stubs[US_PAYLOAD_MAX_STUBS];
} UsPayloadConfig;

/*
 * The configuration block the boot fills in after copying the blob. Reached
 * through the blob's own address, so nothing has to be linked against it.
 */
UsPayloadConfig *usPayloadConfig(void);

#endif
