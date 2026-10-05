/*
 * Reading the instruction that faulted
 *
 * The handler is entered because an instruction is not implemented on this
 * hardware, and what it has to do is whatever that instruction meant. So the
 * first thing it needs is the instruction itself, which is at ELR_EL1: the
 * fault is taken before the instruction runs, and the register still points
 * at it
 *
 * Decoding it is a pure function, so it lives here rather than with the code
 * that does the load: the interesting part of an emulator is the part that
 * can be checked without a machine
 */

#ifndef US_LDAPR_H
#define US_LDAPR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The RCpc loads, by the width they move
 *
 * These are the instructions this whole project exists for: an arm64 CPU
 * without the RCpc extension has no LDAPR, so it takes an undefined
 * instruction exception on one, and the kernel that uses it stops
 */
typedef enum UsLDAPRKind_e {
    UsLDAPRNone = 0,
    UsLDAPRByte,
    UsLDAPRHalf,
    UsLDAPRWord,
    UsLDAPRXword,
} UsLDAPRKind;

typedef struct UsLDAPRInsn_t {
    UsLDAPRKind kind;   /* UsLDAPRNone when this is not one of them */
    uint8_t     rt;     /* destination; 31 means the zero register */
    uint8_t     rn;     /* base address */
} UsLDAPRInsn;

/*
 * Whether an address is aligned for the width of this access
 *
 * The acquire loads are single-copy atomic, and single-copy atomicity is what
 * requires an address to be naturally aligned; the RCpc loads are not
 * single-copy atomic and so may be read at any alignment. Anything that stands
 * in for one of these with the other has to allow for that, because a fault
 * the instruction would not have taken is not one its handler can be given
 */
static inline bool usLDAPRKindAligned(UsLDAPRKind kind, uint64_t address) {
    switch (kind) {
    case UsLDAPRByte:
        return true;
    case UsLDAPRHalf:
        return (address & 1U) == 0;
    case UsLDAPRWord:
        return (address & 3U) == 0;
    default:
        return (address & 7U) == 0;
    }
}

/*
 * Identifies the instruction and pulls out its fields
 *
 * The encoding is fixed except for the two register fields and the size,
 * which is what the mask leaves free. A base of 31 means the zero register
 * rather than a general one, which is a different instruction; it is reported
 * so the caller does not read a register that is not there
 */
UsLDAPRInsn usLDAPRDecode(uint32_t insn);

/*
 * Identifies the acquire load, which is what the four above are replaced with
 *
 * A site that has been replaced still traps on a processor whose caches have
 * not caught up with the write: the exception is the old instruction's, but
 * the memory holds the new one, and reading it is the only way to tell. What
 * that case means is the same load, carried out once more, so it decodes into
 * the same shape
 */
bool usLDARDecode(uint32_t insn, UsLDAPRInsn *out);

/*
 * The acquire load that does the same job, more strongly
 *
 * An RCpc load orders releases; the acquire loads order everything, so a
 * substitute made of one cannot be weaker than what it replaces and cannot
 * turn a correct program incorrect. That is the whole argument for this
 * substitution, and it is why the replacement is not a plain load
 *
 * The two encodings differ only in their fixed bits, so the register fields
 * are carried across untouched. Returns false when the instruction is not one
 * of the four, in which case nothing is written
 */
bool usLDAPRToLDAR(uint32_t insn, uint32_t *out);

#endif
