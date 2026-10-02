/*
 * Reading the instruction that faulted.
 *
 * The handler is entered because an instruction is not implemented on this
 * hardware, and what it has to do is whatever that instruction meant. So the
 * first thing it needs is the instruction itself, which is at ELR_EL1: the
 * fault is taken before the instruction runs, and the register still points
 * at it.
 *
 * Decoding it is a pure function, so it lives here rather than with the code
 * that does the load: the interesting part of an emulator is the part that
 * can be checked without a machine.
 */

#ifndef US_LDAPR_H
#define US_LDAPR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The RCpc loads, by the width they move.
 *
 * These are the instructions this whole project exists for: an arm64 CPU
 * without the RCpc extension has no LDAPR, so it takes an undefined
 * instruction exception on one, and the kernel that uses it stops.
 */
typedef enum UsLdaprKind_e {
    UsLdaprNone = 0,
    UsLdaprByte,
    UsLdaprHalf,
    UsLdaprWord,
    UsLdaprXword,
} UsLdaprKind;

typedef struct UsLdaprInsn_t {
    UsLdaprKind kind;   /* UsLdaprNone when this is not one of them */
    uint8_t     rt;     /* destination; 31 means the zero register */
    uint8_t     rn;     /* base address */
} UsLdaprInsn;

/*
 * Identifies the instruction and pulls out its fields.
 *
 * The encoding is fixed except for the two register fields and the size,
 * which is what the mask leaves free. A base of 31 means the zero register
 * rather than a general one, which is a different instruction; it is reported
 * so the caller does not read a register that is not there.
 */
UsLdaprInsn usLdaprDecode(uint32_t insn);

#endif
