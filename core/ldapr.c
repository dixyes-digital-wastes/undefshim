/*
 * Decoding the RCpc loads, see ldapr.h.
 *
 * The four instructions differ only in the size field, so they are one
 * pattern with the size read out of it rather than four patterns. That also
 * makes the size the thing under test, which is where a copy-pasted table of
 * opcodes would go wrong.
 */

#include <stddef.h>

#include "core/ldapr.h"

/*
 *   size 111000 101111 111100 00 Rn Rt
 *
 * Everything but the size and the two registers is fixed. The size is the
 * only field that differs between the four instructions, so it is left out of
 * the mask and read out of the instruction; masking it in would need four
 * patterns and would make a copy-paste error in the table invisible.
 */
#define US_LDAPR_FIXED 0x38BFC000U
#define US_LDAPR_MASK 0x3FFFFC00U

/* Bits 31..30 are the size, and they are the only thing that differs between
 * the four widths, in both families. */
static UsLdaprKind sizeOf(uint32_t insn) {
    switch ((insn >> 30) & 3U) {
    case 0:
        return UsLdaprByte;
    case 1:
        return UsLdaprHalf;
    case 2:
        return UsLdaprWord;
    default:
        return UsLdaprXword;
    }
}

/* The two register fields sit in the same place in both families. */
static UsLdaprInsn fieldsOf(uint32_t insn, UsLdaprKind kind) {
    UsLdaprInsn out = { 0 };

    out.kind = kind;
    out.rt = (uint8_t)(insn & 0x1FU);
    out.rn = (uint8_t)((insn >> 5) & 0x1FU);
    return out;
}

UsLdaprInsn usLdaprDecode(uint32_t insn) {
    if ((insn & US_LDAPR_MASK) != US_LDAPR_FIXED) {
        return fieldsOf(0, UsLdaprNone);
    }
    return fieldsOf(insn, sizeOf(insn));
}

/*
 * The acquire loads, by width.
 *
 * The two encodings put the size in the top two bits and the two registers in
 * the bottom ten, all in the same places, so the substitution carries those
 * across and replaces only what is between them. Doing it this way rather
 * than by table means a width cannot be paired with the wrong replacement.
 */
#define US_LDAPR_SIZE_MASK 0xC0000000U
#define US_LDAPR_REG_MASK 0x3FFU

#define US_LDAR_FIXED 0x08DFFC00U
#define US_LDAR_MASK 0x3FFFFC00U

bool usLdarDecode(uint32_t insn, UsLdaprInsn *out) {
    if (out == NULL || (insn & US_LDAR_MASK) != US_LDAR_FIXED) {
        return false;
    }
    *out = fieldsOf(insn, sizeOf(insn));
    return true;
}

bool usLdaprToLdar(uint32_t insn, uint32_t *out) {
    if (usLdaprDecode(insn).kind == UsLdaprNone) {
        return false;
    }
    *out = US_LDAR_FIXED | (insn & US_LDAPR_SIZE_MASK) | (insn & US_LDAPR_REG_MASK);
    return true;
}
