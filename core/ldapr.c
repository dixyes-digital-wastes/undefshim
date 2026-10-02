/*
 * Decoding the RCpc loads, see ldapr.h.
 *
 * The four instructions differ only in the size field, so they are one
 * pattern with the size read out of it rather than four patterns. That also
 * makes the size the thing under test, which is where a copy-pasted table of
 * opcodes would go wrong.
 */

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

UsLdaprInsn usLdaprDecode(uint32_t insn) {
    UsLdaprInsn out = { 0 };

    if ((insn & US_LDAPR_MASK) != US_LDAPR_FIXED) {
        return out;
    }

    /* Bits 31..30 are the size, and they are the only thing that differs. */
    switch ((insn >> 30) & 3U) {
    case 0:
        out.kind = UsLdaprByte;
        break;
    case 1:
        out.kind = UsLdaprHalf;
        break;
    case 2:
        out.kind = UsLdaprWord;
        break;
    default:
        out.kind = UsLdaprXword;
        break;
    }

    out.rt = (uint8_t)(insn & 0x1FU);
    out.rn = (uint8_t)((insn >> 5) & 0x1FU);
    return out;
}
