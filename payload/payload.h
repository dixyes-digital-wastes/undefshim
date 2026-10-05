/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The payload's side of the interface
 *
 * The payload is a position independent blob: linked at zero, copied to a
 * page aligned address, and entered there. Nothing in it may name an absolute
 * address, which is what the link check enforces. It runs with no firmware
 * and no floating point, so everything it needs it carries with it
 *
 * Its layout inside the blob is described by a generated header rather than
 * by symbols, because the driver embeds the blob as bytes and has no way to
 * link against it
 */

#ifndef US_PAYLOAD_H
#define US_PAYLOAD_H

#include <stdint.h>

/* For the number of processors the pool has room for: the configuration
 * block and the pool have to agree about it, and the pool's layout is the
 * one that says */
#include "common/layout.h"
/* For the shape of the stub at each synchronous slot, which is what the SPSR
 * of the exception being handled says to look for */
#include "core/thunk.h"

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

    /*
     * Where the entry is to go when this exception is not ours to resume but
     * the kernel's own handler's to finish: the address of the tail of the
     * slot the exception was taken through, which replays that slot's
     * original instruction and continues into the handler that was there
     *
     * The payload writes it and says so with its return value; the entry
     * branches there instead of returning, with every register restored to
     * what the interrupted code had, which is what those handlers expect
     */
    uint64_t landing;

    /* Keeps the stride a multiple of sixteen, which the ABI requires of the
     * stack pointer at every call and is what the entry's own alignment
     * arithmetic assumes */
    uint64_t reserved;
} UsFrame;

/* ESR_EL1's exception class, and the classes that matter here */
#define US_ESR_EC(esr) (((esr) >> 26) & 0x3FU)

#define US_EC_UNKNOWN 0x00U  /* an undefined instruction: the case this is for */
#define US_EC_DATA_ABORT_SAME_EL 0x25U  /* a fault the handler took itself */
#define US_EC_DATA_ABORT_LOWER 0x24U    /* the same fault, taken from EL0 */

/*
 * The exception class a fault really belongs to
 *
 * An emulated access is carried out at EL1 even when the instruction it stands
 * in for was EL0's, so a fault on it arrives as EC 0x25. The kernel decides
 * what to do with a fault from the exception class, and for an access the user
 * made the answer is 0x24; the rest of the syndrome - the fault status, and
 * the direction - is the same in both
 */
static inline uint64_t usESRAsLowerEL(uint64_t esr) {
    return (esr & ~(UINT64_C(0x3F) << 26))
           | ((uint64_t)US_EC_DATA_ABORT_LOWER << 26);
}

/*
 * Which synchronous slot an interrupted SPSR names
 *
 * An exception is taken through the slot that matches the stack pointer in use
 * at the time, so the SPSR says which one it was: EL1h is the entry at 0x200,
 * EL0 the one at 0x400, and EL1t - like anything unexpected - the entry at
 * zero
 */
static inline UsStubSlot usSlotOfSPSR(uint64_t spsr) {
    switch (spsr & 0xFU) {
    case 5U:
        return UsStubSlotEL1h;
    case 0U:
        return UsStubSlotEL0;
    default:
        return UsStubSlotEL1t;
    }
}

/*
 * Runs one exception
 *
 * 1 resumes the updated frame at its ELR. 2 hands the frame to the kernel's
 * own handler: the entry restores every register and branches to the frame's
 * landing, which is where the slot the exception came through used to lead.
 * 0 means nothing could be worked out and the entry stops
 */
int usPayloadHandle(UsFrame *frame);

/*
 * Runs when an exception arrives while the payload itself is on the stack,
 * which is what a fault on an emulated access looks like from the entry
 *
 * The emulated access stands in for one instruction of the interrupted code,
 * so a fault on it is that instruction's fault: the frame being handled is
 * what the kernel has to be given, and the nested fault's own ESR and FAR
 * already describe it. A fault anywhere else in the payload is a fault inside
 * a handler and is reported as one - except one that a probe asked for, which
 * is an answer rather than a fault
 *
 * Answers with the frame to restore, NULL when there is none, or
 * US_PAYLOAD_RESUME when the handler's own code is to carry on
 */
UsFrame *usPayloadFault(void);

/*
 * Records that the entry cannot go on, and why
 *
 * The entry reaches this when it cannot answer at all: no destination for the
 * exception, or a CPU it has no stack for. Both end in the entry's own halt,
 * which from outside is a machine that stopped with nothing to say, so what it
 * knew at that moment is written where the host can read it
 */
void usPayloadStuck(uint64_t kind);

#define US_STUCK_NO_DESTINATION 1U
#define US_STUCK_NO_STACK 2U

/*
 * What an access the handler expects may be refused answers with
 *
 * The rewrite has to store into a page that the mapping may not allow, and the
 * only honest answer to whether it does is to try it. While a probe is armed,
 * a fault on exactly that address comes back to the handler as "no" instead of
 * being reported as a fault in a handler, and the call says it did not happen.
 * The handler then carries on at the instruction after the one that faulted
 *
 * One access at a time, and only on the processor making it. A refused read
 * leaves the value alone; a refused store changes nothing
 */
#define US_PAYLOAD_RESUME ((UsFrame *)(uintptr_t)1)

bool usPayloadProbeRead(uint64_t at, uint64_t *value);
bool usPayloadProbeWrite(uint64_t at, uint64_t value);
/* One instruction wide: a site is four bytes, and a store of eight would take
 * the instruction after it with it */
bool usPayloadProbeWriteWord(uint64_t at, uint32_t value);

/*
 * Says hello on the serial port, and nothing else
 *
 * It exists so that "the blob was copied to executable memory and runs" can
 * be checked on its own, before anything depends on being entered through an
 * exception. Everything it needs it reads from the configuration block, so a
 * silent answer means the block was not written rather than that the copy
 * failed
 */
void usPayloadSelfTest(void);

/*
 * One processor, as the boot found it described
 *
 * The index is carried rather than worked out from the position, because the
 * code that needs it runs before there is a stack to work anything out on
 */
typedef struct UsPayloadCPU_t {
    uint64_t mpidr;   /* the affinity fields only */
    uint64_t index;
} UsPayloadCPU;

#define US_PAYLOAD_MAX_STUBS 32U

typedef struct UsPayloadStub_t {
    uint64_t address;
    uint64_t tableAddress;
    uint64_t imageAddress;
    uint64_t tablePA;
    uint64_t addressPA;
    uint32_t targetIndex;
    uint32_t published;
    /*
     * Where this image keeps the base of its own descriptor mapping, as an
     * RVA, or zero when it has none to offer
     *
     * Reading the descriptor that translates a page needs that base, it is
     * not the constant the image's MiGetPteAddress uses (that one only works
     * on a 48-bit address space, and this kernel runs a 47-bit one), and the
     * only thing that knows where the variable is, is the image: hence an
     * offset from the image's base, passed with the stub
     */
    uint32_t descriptorBaseRVA;
    uint32_t reserved;   /* keeps the stride a multiple of eight */
} UsPayloadStub;

/*
 * Everything the payload cannot work out for itself
 *
 * The payload has no way to find the pool: its address changes when the
 * kernel rebuilds the address space, so nothing about it can be baked in. The
 * boot writes this block into the blob's data area after copying it, and the
 * payload reaches it through a pointer derived from the blob's own address
 */
typedef struct UsPayloadConfig_t {
    /* Where to write. A direct MMIO address, since there is no firmware */
    uint64_t uartBase;
    uint64_t uartKind;   /* 1 pl011, 2 uart8250, 0 silent */
    uint64_t uartWidth;  /* bits per access: 8 or 32 */

    /* Stack tops indexed by the firmware CPU index, not a raw affinity byte */
    uint64_t stackTop[8];

    /* Address the blob was entered at, written by the boot. The only way the
     * payload can locate its own data */
    uint64_t selfVA;

    /*
     * The pool, for the trace of what happened. It is reachable at this
     * address from the moment the kernel is running, which is not true of
     * anything else the boot placed
     */
    uint64_t poolBase;

    /*
     * Whether to keep quiet
     *
     * The serial port is reachable during boot and is not once the kernel has
     * built its own page tables, where writing to it faults and the fault is
     * not survivable. The exception path is entered in exactly that state, so
     * it has to be told not to print rather than finding out
     */
    uint64_t quiet;

    /*
     * Whether an instruction EL0 executed may be replaced where it stands
     *
     * The kernel's own instructions are never replaced while it runs: the
     * integrity check reports that, and a list applied before it starts is
     * where those belong. User code is somebody else's, and replacing a load
     * there is what makes the demo run at full speed
     */
    uint64_t el0InPlace;

    /* Whether to count where traps are taken; see [stats] enabled */
    uint64_t statsEnabled;

    /* Firmware affinity-to-index mapping used to generate the stack lookup */
    uint64_t cpuCount;
    /* One entry per processor plus the firmware mapping's terminator */
    UsPayloadCPU cpus[US_MAX_CPUS + 1U];

    /* Immutable physical ranges used to validate their runtime aliases */
    uint64_t selfPA;
    uint64_t selfBytes;
    uint64_t poolPA;
    uint64_t entryOffset;

    uint64_t highVA;
    uint64_t highPoolVA;
    uint64_t stubCount;
    UsPayloadStub stubs[US_PAYLOAD_MAX_STUBS];
} UsPayloadConfig;

/*
 * The configuration block the boot fills in after copying the blob. Reached
 * through the blob's own address, so nothing has to be linked against it
 */
UsPayloadConfig *usPayloadConfig(void);

#endif
